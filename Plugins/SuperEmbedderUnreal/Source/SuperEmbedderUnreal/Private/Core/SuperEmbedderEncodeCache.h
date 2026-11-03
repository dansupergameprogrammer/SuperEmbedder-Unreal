#pragma once

// The exact encode cache (proposal rev 9, section 2.2, audit G-8). Thread-safe: both lanes' workers read and
// fill it, and the game thread reads it at enqueue.
//
// - The key is (encoder fingerprint, kind, SHA-256 of the UTF-8 text). The cache is bound to one fingerprint
//   at a time, so the stored key is (kind, text hash); binding another fingerprint clears it (K-7).
// - At most MaxEntries entries; the least recently used is evicted (K-3). Find and a hit through Acquire
//   both count as a use.
// - Two misses on one key at once make one encode: the first Acquire becomes the owner, a second waits until
//   the owner Fills (the waiter then hits) or Abandons (the waiter becomes the owner) (K-5).
//
// The cooked file ('SECA', little-endian):
//   [0]  4  magic 'S','E','C','A'
//   [4]  4  version (u32, 1)
//   [8]  32 encoder fingerprint
//   [40] 4  dims (u32)
//   [44] 4  entry count (u32)
//   [48]    entries, oldest use first, each: u8 kind (0 query, 1 document), 32-byte text SHA-256,
//           dims x f32 LE, then a 32-byte entry digest = SHA-256(kind byte, text SHA-256, the f32 LE bytes)
//   nothing may follow the last entry.
// Load checks, in this order, each a named refusal that leaves the cache as it was: Truncated (fewer than 4
// bytes), BadMagic, Truncated (fewer than 48), UnsupportedVersion, Malformed (dims 0, or a kind byte above 1),
// Truncated (an entry runs past the end), EntryDigestMismatch (K-8), Malformed (trailing bytes). A file whose
// fingerprint differs from the bound one loads as an empty cache, reported by bCleared (K-7).

#include "SuperEmbedderCoreTypes.h"

#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SuperEmbedderUe
{
	struct FCacheKey
	{
		EKind Kind = EKind::Query;
		FFingerprint TextSha{};

		static FCacheKey Of(EKind Kind, const std::string& Utf8);
		bool operator==(const FCacheKey& Other) const { return Kind == Other.Kind && TextSha == Other.TextSha; }
	};

	struct FCacheKeyHash
	{
		size_t operator()(const FCacheKey& Key) const;
	};

	constexpr uint32_t kCacheFileVersion = 1;
	constexpr size_t kCacheFileHeaderBytes = 48;

	class FEncodeCache
	{
	public:
		explicit FEncodeCache(size_t InMaxEntries) : MaxEntries(InMaxEntries) {}

		// Binds the cache to an encoder's fingerprint. Returns true when it held entries of another one and so
		// was cleared.
		bool BindFingerprint(const FFingerprint& Fp);
		bool IsBound() const;

		// A plain lookup. Counts as a use.
		bool Find(const FCacheKey& Key, std::vector<float>& OutValues);

		enum class EAcquire : uint8_t
		{
			Hit = 0,   // OutValues holds the entry
			Owner = 1, // the caller must encode, then Fill or Abandon
		};
		// Blocks while another caller owns Key.
		EAcquire Acquire(const FCacheKey& Key, std::vector<float>& OutValues);
		void Fill(const FCacheKey& Key, const std::vector<float>& Values);
		void Abandon(const FCacheKey& Key);

		size_t Num() const;
		size_t GetMaxEntries() const { return MaxEntries; }
		void Clear();

		// The cooked file of the current entries, oldest use first. An unbound cache serializes nothing.
		std::vector<uint8_t> Serialize() const;
		EStatus Load(const uint8_t* Data, size_t Size, std::string& Detail, bool& bCleared);

	private:
		struct FEntry
		{
			FCacheKey Key;
			std::vector<float> Values;
		};
		void InsertLocked(const FCacheKey& Key, const std::vector<float>& Values);

		mutable std::mutex Mutex;
		std::condition_variable Filled;
		size_t MaxEntries = 0;
		bool bBound = false;
		FFingerprint Fingerprint{};
		uint32_t Dims = 0;
		std::list<FEntry> Lru; // front = most recently used
		std::unordered_map<FCacheKey, std::list<FEntry>::iterator, FCacheKeyHash> Index;
		std::unordered_set<FCacheKey, FCacheKeyHash> Owned;
	};
}
