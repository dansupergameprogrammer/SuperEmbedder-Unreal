#include "SuperEmbedderEncodeCache.h"

#include "SuperEmbedderSha256.h"

#include <cstring>

namespace SuperEmbedderUe
{
	namespace
	{
		constexpr uint8_t kCacheMagic[4] = {'S', 'E', 'C', 'A'};

		FFingerprint EntryDigest(uint8_t KindByte, const FFingerprint& TextSha, const std::vector<float>& Values)
		{
			FSha256 S;
			S.Update(&KindByte, 1);
			S.Update(TextSha.data(), TextSha.size());
			std::vector<uint8_t> Le;
			Le.reserve(Values.size() * 4);
			PutFloatsLe(Le, Values.data(), Values.size());
			S.Update(Le.data(), Le.size());
			return S.Final();
		}
	}

	FCacheKey FCacheKey::Of(EKind Kind, const std::string& Utf8)
	{
		FCacheKey K;
		K.Kind = Kind;
		K.TextSha = Sha256Of(Utf8.data(), Utf8.size());
		return K;
	}

	size_t FCacheKeyHash::operator()(const FCacheKey& Key) const
	{
		uint64_t H = 0;
		std::memcpy(&H, Key.TextSha.data(), sizeof(H));
		return static_cast<size_t>(H ^ static_cast<uint64_t>(Key.Kind));
	}

	bool FEncodeCache::BindFingerprint(const FFingerprint& Fp)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		const bool bCleared = bBound && Fp != Fingerprint && !Lru.empty();
		if (!bBound || Fp != Fingerprint)
		{
			Lru.clear();
			Index.clear();
			Dims = 0;
		}
		bBound = true;
		Fingerprint = Fp;
		return bCleared;
	}

	bool FEncodeCache::IsBound() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return bBound;
	}

	bool FEncodeCache::Find(const FCacheKey& Key, std::vector<float>& OutValues)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		auto It = Index.find(Key);
		if (It == Index.end())
		{
			return false;
		}
		Lru.splice(Lru.begin(), Lru, It->second);
		OutValues = It->second->Values;
		return true;
	}

	FEncodeCache::EAcquire FEncodeCache::Acquire(const FCacheKey& Key, std::vector<float>& OutValues)
	{
		std::unique_lock<std::mutex> Lock(Mutex);
		for (;;)
		{
			auto It = Index.find(Key);
			if (It != Index.end())
			{
				Lru.splice(Lru.begin(), Lru, It->second);
				OutValues = It->second->Values;
				return EAcquire::Hit;
			}
			if (Owned.find(Key) == Owned.end())
			{
				Owned.insert(Key);
				return EAcquire::Owner;
			}
			Filled.wait(Lock);
		}
	}

	void FEncodeCache::InsertLocked(const FCacheKey& Key, const std::vector<float>& Values)
	{
		if (MaxEntries == 0)
		{
			return;
		}
		auto It = Index.find(Key);
		if (It != Index.end())
		{
			Lru.splice(Lru.begin(), Lru, It->second);
			It->second->Values = Values;
			return;
		}
		if (Dims == 0)
		{
			Dims = static_cast<uint32_t>(Values.size());
		}
		Lru.push_front(FEntry{Key, Values});
		Index[Key] = Lru.begin();
		while (Lru.size() > MaxEntries)
		{
			Index.erase(Lru.back().Key);
			Lru.pop_back();
		}
	}

	void FEncodeCache::Fill(const FCacheKey& Key, const std::vector<float>& Values)
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Owned.erase(Key);
			if (bBound)
			{
				InsertLocked(Key, Values);
			}
		}
		Filled.notify_all();
	}

	void FEncodeCache::Abandon(const FCacheKey& Key)
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Owned.erase(Key);
		}
		Filled.notify_all();
	}

	size_t FEncodeCache::Num() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Lru.size();
	}

	void FEncodeCache::Clear()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Lru.clear();
		Index.clear();
		Dims = 0;
	}

	std::vector<uint8_t> FEncodeCache::Serialize() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		std::vector<uint8_t> Out;
		if (!bBound)
		{
			return Out;
		}
		PutBytes(Out, kCacheMagic, 4);
		PutU32(Out, kCacheFileVersion);
		PutBytes(Out, Fingerprint.data(), Fingerprint.size());
		PutU32(Out, Dims == 0 ? 1u : Dims);
		PutU32(Out, static_cast<uint32_t>(Lru.size()));
		for (auto It = Lru.rbegin(); It != Lru.rend(); ++It)
		{
			const uint8_t KindByte = static_cast<uint8_t>(It->Key.Kind);
			Out.push_back(KindByte);
			PutBytes(Out, It->Key.TextSha.data(), 32);
			PutFloatsLe(Out, It->Values.data(), It->Values.size());
			const FFingerprint D = EntryDigest(KindByte, It->Key.TextSha, It->Values);
			PutBytes(Out, D.data(), D.size());
		}
		return Out;
	}

	EStatus FEncodeCache::Load(const uint8_t* Data, size_t Size, std::string& Detail, bool& bCleared)
	{
		bCleared = false;
		FByteReader R{Data, Size};
		if (Size < 4)
		{
			return EStatus::Truncated;
		}
		if (std::memcmp(Data, kCacheMagic, 4) != 0)
		{
			return EStatus::BadMagic;
		}
		if (Size < kCacheFileHeaderBytes)
		{
			return EStatus::Truncated;
		}
		R.Pos = 4;
		const uint32_t Version = R.U32();
		if (Version != kCacheFileVersion)
		{
			Detail = "version " + std::to_string(Version);
			return EStatus::UnsupportedVersion;
		}
		FFingerprint FileFp{};
		R.Bytes(FileFp.data(), 32);
		const uint32_t FileDims = R.U32();
		const uint32_t Count = R.U32();
		if (FileDims == 0)
		{
			Detail = "dims 0";
			return EStatus::Malformed;
		}
		std::list<FEntry> Loaded;
		for (uint32_t I = 0; I < Count; ++I)
		{
			FEntry E;
			const uint8_t KindByte = R.U8();
			if (R.bTruncated)
			{
				Detail = "entry " + std::to_string(I);
				return EStatus::Truncated;
			}
			if (KindByte > 1)
			{
				Detail = "entry " + std::to_string(I) + " kind " + std::to_string(KindByte);
				return EStatus::Malformed;
			}
			E.Key.Kind = static_cast<EKind>(KindByte);
			E.Values.resize(FileDims);
			FFingerprint Stored{};
			if (!R.Bytes(E.Key.TextSha.data(), 32) || !R.FloatsLe(E.Values.data(), FileDims) || !R.Bytes(Stored.data(), 32))
			{
				Detail = "entry " + std::to_string(I);
				return EStatus::Truncated;
			}
			if (EntryDigest(KindByte, E.Key.TextSha, E.Values) != Stored)
			{
				Detail = "entry " + std::to_string(I);
				return EStatus::EntryDigestMismatch;
			}
			Loaded.push_back(static_cast<FEntry&&>(E));
		}
		if (R.Remaining() != 0)
		{
			Detail = std::to_string(R.Remaining()) + " trailing bytes";
			return EStatus::Malformed;
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		if (bBound && FileFp != Fingerprint)
		{
			bCleared = true;
			Lru.clear();
			Index.clear();
			Dims = 0;
			return EStatus::Ok;
		}
		bBound = true;
		Fingerprint = FileFp;
		Lru.clear();
		Index.clear();
		Dims = FileDims;
		// Oldest first in the file: inserting each at the front leaves the newest at the front.
		for (FEntry& E : Loaded)
		{
			InsertLocked(E.Key, E.Values);
		}
		return EStatus::Ok;
	}
}
