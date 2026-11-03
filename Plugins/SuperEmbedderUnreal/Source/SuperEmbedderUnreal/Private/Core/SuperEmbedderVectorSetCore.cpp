#include "SuperEmbedderVectorSetCore.h"

#include "SuperEmbedderSha256.h"
#include "SuperEmbedderText.h"

#include <cmath>
#include <cstring>

namespace SuperEmbedderUe
{
	namespace
	{
		constexpr uint8_t kSetMagic[4] = {'S', 'E', 'V', 'S'};

		void DigestRow(FSha256& S, const FSetRow& R)
		{
			S.UpdateU32(static_cast<uint32_t>(R.Id.size()));
			S.Update(R.Id.data(), R.Id.size());
			S.UpdateU32(static_cast<uint32_t>(R.Text.size()));
			S.Update(R.Text.data(), R.Text.size());
			std::vector<uint8_t> Le;
			Le.reserve(R.Values.size() * 4);
			PutFloatsLe(Le, R.Values.data(), R.Values.size());
			S.Update(Le.data(), Le.size());
		}
	}

	EStatus CheckRowValues(const std::vector<float>& Values, double& OutNorm)
	{
		double Sum = 0.0;
		for (float V : Values)
		{
			if (!std::isfinite(V))
			{
				return EStatus::NonFiniteVector;
			}
			Sum += static_cast<double>(V) * static_cast<double>(V);
		}
		if (Values.empty() || Sum == 0.0)
		{
			return EStatus::ZeroVector;
		}
		OutNorm = std::sqrt(Sum);
		return EStatus::Ok;
	}

	FVectorSetCore::FVectorSetCore()
		: Rows(std::make_shared<std::vector<FSetRowPtr>>())
	{
	}

	void FVectorSetCore::MutableRows()
	{
		// A live snapshot shares the list: copy the pointers, never the rows.
		if (Rows.use_count() > 1)
		{
			Rows = std::make_shared<std::vector<FSetRowPtr>>(*Rows);
		}
	}

	void FVectorSetCore::Reindex()
	{
		IndexById.clear();
		IndexById.reserve(Rows->size());
		for (size_t I = 0; I < Rows->size(); ++I)
		{
			IndexById.emplace((*Rows)[I]->Id, I);
		}
	}

	EStatus FVectorSetCore::Append(const std::string& Id, const std::string& Text, const FEmbedding& Vector)
	{
		if (!Vector.bChecked)
		{
			return EStatus::UncheckedVector;
		}
		if (CheckId(Id) != EStatus::Ok)
		{
			return EStatus::InvalidId;
		}
		if (bHasFingerprint && Vector.Fingerprint != Fingerprint)
		{
			return EStatus::FingerprintMismatch;
		}
		if (bHasFingerprint && Vector.Values.size() != Dims)
		{
			return EStatus::DimsMismatch;
		}
		double Norm = 0.0;
		const EStatus Values = CheckRowValues(Vector.Values, Norm);
		if (Values != EStatus::Ok)
		{
			return Values;
		}
		if (Contains(Id))
		{
			return EStatus::DuplicateId;
		}
		std::shared_ptr<FSetRow> R = std::make_shared<FSetRow>();
		R->Id = Id;
		R->Text = Text;
		R->Values = Vector.Values;
		R->Norm = Norm;
		MutableRows();
		if (!bHasFingerprint)
		{
			bHasFingerprint = true;
			Fingerprint = Vector.Fingerprint;
			Dims = static_cast<uint32_t>(Vector.Values.size());
		}
		IndexById.emplace(Id, Rows->size());
		Rows->push_back(static_cast<std::shared_ptr<FSetRow>&&>(R));
		return EStatus::Ok;
	}

	EStatus FVectorSetCore::Remove(const std::string& Id)
	{
		auto It = IndexById.find(Id);
		if (It == IndexById.end())
		{
			return EStatus::UnknownId;
		}
		const size_t Index = It->second;
		MutableRows();
		Rows->erase(Rows->begin() + static_cast<std::ptrdiff_t>(Index));
		Reindex();
		// The set keeps its fingerprint and dims when its last row goes: it still belongs to that encoder.
		return EStatus::Ok;
	}

	void FVectorSetCore::Reset()
	{
		Rows = std::make_shared<std::vector<FSetRowPtr>>();
		IndexById.clear();
		bHasFingerprint = false;
		Fingerprint = FFingerprint{};
		Dims = 0;
	}

	int64_t FVectorSetCore::IndexOf(const std::string& Id) const
	{
		auto It = IndexById.find(Id);
		return It == IndexById.end() ? -1 : static_cast<int64_t>(It->second);
	}

	FEmbedding FVectorSetCore::RowVector(size_t Index) const
	{
		return FEmbedding::Checked(Row(Index).Values, Fingerprint, EKind::Document);
	}

	FSetSnapshot FVectorSetCore::Snapshot() const
	{
		FSetSnapshot S;
		S.Rows = Rows;
		S.Fingerprint = Fingerprint;
		S.bHasFingerprint = bHasFingerprint;
		S.Dims = Dims;
		return S;
	}

	FFingerprint FVectorSetCore::ComputeRowDigest() const
	{
		FSha256 S;
		S.UpdateU32(Dims);
		S.UpdateU32(static_cast<uint32_t>(Rows->size()));
		for (const FSetRowPtr& R : *Rows)
		{
			DigestRow(S, *R);
		}
		return S.Final();
	}

	std::vector<uint8_t> FVectorSetCore::Serialize() const
	{
		std::vector<uint8_t> Out;
		PutBytes(Out, kSetMagic, 4);
		PutU32(Out, kSetFileVersion);
		PutU32(Out, bHasFingerprint ? 1u : 0u);
		PutBytes(Out, Fingerprint.data(), Fingerprint.size());
		PutU32(Out, Dims);
		PutU32(Out, static_cast<uint32_t>(Rows->size()));
		const FFingerprint Digest = ComputeRowDigest();
		PutBytes(Out, Digest.data(), Digest.size());
		for (const FSetRowPtr& R : *Rows)
		{
			PutU32(Out, static_cast<uint32_t>(R->Id.size()));
			PutBytes(Out, R->Id.data(), R->Id.size());
			PutU32(Out, static_cast<uint32_t>(R->Text.size()));
			PutBytes(Out, R->Text.data(), R->Text.size());
			PutFloatsLe(Out, R->Values.data(), R->Values.size());
		}
		return Out;
	}

	EStatus FVectorSetCore::Deserialize(const uint8_t* Data, size_t Size, FVectorSetCore& Out, std::string& Detail)
	{
		if (Size < 4)
		{
			return EStatus::Truncated;
		}
		if (std::memcmp(Data, kSetMagic, 4) != 0)
		{
			return EStatus::BadMagic;
		}
		if (Size < kSetFileHeaderBytes)
		{
			return EStatus::Truncated;
		}
		FByteReader R{Data, Size, 4};
		const uint32_t Version = R.U32();
		if (Version != kSetFileVersion)
		{
			Detail = "version " + std::to_string(Version);
			return EStatus::UnsupportedVersion;
		}
		const uint32_t Flags = R.U32();
		FFingerprint Fp{};
		R.Bytes(Fp.data(), 32);
		const uint32_t SetDims = R.U32();
		const uint32_t Count = R.U32();
		FFingerprint StoredDigest{};
		R.Bytes(StoredDigest.data(), 32);
		if ((Flags & ~1u) != 0 || (Count > 0 && (SetDims == 0 || (Flags & 1u) == 0)))
		{
			Detail = "flags " + std::to_string(Flags) + ", dims " + std::to_string(SetDims);
			return EStatus::Malformed;
		}
		std::vector<std::shared_ptr<FSetRow>> Loaded;
		Loaded.reserve(Count < 1000000u ? Count : 1000000u);
		for (uint32_t I = 0; I < Count; ++I)
		{
			std::shared_ptr<FSetRow> Row = std::make_shared<FSetRow>();
			const uint32_t IdLen = R.U32();
			if (R.bTruncated || IdLen > R.Remaining())
			{
				Detail = "row " + std::to_string(I);
				return EStatus::Truncated;
			}
			Row->Id.resize(IdLen);
			R.Bytes(&Row->Id[0], IdLen);
			const uint32_t TextLen = R.U32();
			if (R.bTruncated || TextLen > R.Remaining())
			{
				Detail = "row " + std::to_string(I);
				return EStatus::Truncated;
			}
			Row->Text.resize(TextLen);
			if (TextLen > 0)
			{
				R.Bytes(&Row->Text[0], TextLen);
			}
			Row->Values.resize(SetDims);
			if (!R.FloatsLe(Row->Values.data(), SetDims))
			{
				Detail = "row " + std::to_string(I);
				return EStatus::Truncated;
			}
			Loaded.push_back(static_cast<std::shared_ptr<FSetRow>&&>(Row));
		}
		if (R.Remaining() != 0)
		{
			Detail = std::to_string(R.Remaining()) + " trailing bytes";
			return EStatus::Malformed;
		}
		FSha256 S;
		S.UpdateU32(SetDims);
		S.UpdateU32(Count);
		for (const std::shared_ptr<FSetRow>& Row : Loaded)
		{
			DigestRow(S, *Row);
		}
		if (S.Final() != StoredDigest)
		{
			return EStatus::SetCorrupt;
		}
		FVectorSetCore Result;
		Result.bHasFingerprint = (Flags & 1u) != 0;
		Result.Fingerprint = Fp;
		Result.Dims = SetDims;
		for (size_t I = 0; I < Loaded.size(); ++I)
		{
			std::shared_ptr<FSetRow>& Row = Loaded[I];
			if (CheckId(Row->Id) != EStatus::Ok || Result.Contains(Row->Id) ||
				CheckRowValues(Row->Values, Row->Norm) != EStatus::Ok)
			{
				Detail = "row " + std::to_string(I);
				return EStatus::Malformed;
			}
			Result.IndexById.emplace(Row->Id, Result.Rows->size());
			Result.Rows->push_back(static_cast<std::shared_ptr<FSetRow>&&>(Row));
		}
		Out = static_cast<FVectorSetCore&&>(Result);
		return EStatus::Ok;
	}
}
