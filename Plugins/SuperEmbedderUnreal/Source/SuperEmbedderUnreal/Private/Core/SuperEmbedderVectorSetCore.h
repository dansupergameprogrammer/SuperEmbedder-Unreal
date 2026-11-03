#pragma once

// The vector set's rows (proposal rev 9, section 2.3): ids, source text, float32 rows, dims, the encoder
// fingerprint, and a row digest over the rows in order. Plain data: a set holds no encoder open.
//
// Ownership. The UObject wrapper allows Append, Remove and every similarity helper only on the game thread
// (WrongThread elsewhere, V-10); this core has no thread notion. Rows are immutable once added and shared,
// so Snapshot() is O(1) and an async ranking over it is unchanged by a later Append or Remove (V-12): a
// mutation while a snapshot is alive copies the row-pointer list (pointers only, never the floats).
//
// Per-row norms are computed when a row is added and stored with it, never cached apart from the rows (V-2).
//
// The saved form ('SEVS', little-endian):
//   [0]  4  magic 'S','E','V','S'
//   [4]  4  version (u32, 1)
//   [8]  4  flags (u32): bit 0 = the set carries a fingerprint; no other bit may be set
//   [12] 32 encoder fingerprint (zero when bit 0 is clear)
//   [44] 4  dims (u32; 0 only for an empty set without a fingerprint)
//   [48] 4  row count (u32)
//   [52] 32 row digest = SHA-256(u32 dims, u32 count, then per row: u32 id bytes, id, u32 text bytes, text,
//           dims x f32 LE)
//   [84]    rows, each: u32 id bytes, id, u32 text bytes, text, dims x f32 LE; nothing may follow the last.
// Read checks, in this order: Truncated (fewer than 4 bytes), BadMagic, Truncated (fewer than 84),
// UnsupportedVersion, Malformed (unknown flag bits, rows without dims), Truncated (a row runs past the end),
// Malformed (trailing bytes), SetCorrupt (the recomputed row digest differs, V-8), then Malformed (an
// invalid or duplicate id, a non-finite or zero row: content no writer of this format produces).

#include "SuperEmbedderCoreTypes.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SuperEmbedderUe
{
	struct FSetRow
	{
		std::string Id;
		std::string Text;
		std::vector<float> Values;
		double Norm = 0.0;
	};
	using FSetRowPtr = std::shared_ptr<const FSetRow>;

	// What a ranking reads: an immutable view of the rows and the set's identity at one moment.
	struct FSetSnapshot
	{
		std::shared_ptr<const std::vector<FSetRowPtr>> Rows;
		FFingerprint Fingerprint{};
		bool bHasFingerprint = false;
		uint32_t Dims = 0;

		size_t Num() const { return Rows ? Rows->size() : 0; }
	};

	constexpr uint32_t kSetFileVersion = 1;
	constexpr size_t kSetFileHeaderBytes = 84;

	// Shared by the set and the similarity helpers: InvalidId, NonFiniteVector, ZeroVector, or Ok with the norm.
	EStatus CheckRowValues(const std::vector<float>& Values, double& OutNorm);

	class FVectorSetCore
	{
	public:
		FVectorSetCore();

		// UncheckedVector, InvalidId, FingerprintMismatch, DimsMismatch, NonFiniteVector, ZeroVector, DuplicateId.
		// The first row of a set without a fingerprint gives the set its fingerprint and dims.
		EStatus Append(const std::string& Id, const std::string& Text, const FEmbedding& Vector);
		// UnknownId.
		EStatus Remove(const std::string& Id);
		void Reset();

		size_t Num() const { return Rows->size(); }
		bool Contains(const std::string& Id) const { return IndexById.find(Id) != IndexById.end(); }
		// -1 when absent.
		int64_t IndexOf(const std::string& Id) const;
		const FSetRow& Row(size_t Index) const { return *(*Rows)[Index]; }
		bool HasFingerprint() const { return bHasFingerprint; }
		const FFingerprint& GetFingerprint() const { return Fingerprint; }
		uint32_t GetDims() const { return Dims; }
		// The row as a checked vector of this set's fingerprint (a document-kind vector).
		FEmbedding RowVector(size_t Index) const;

		FSetSnapshot Snapshot() const;
		FFingerprint ComputeRowDigest() const;

		std::vector<uint8_t> Serialize() const;
		// Replaces Out's contents only on Ok.
		static EStatus Deserialize(const uint8_t* Data, size_t Size, FVectorSetCore& Out, std::string& Detail);

	private:
		void MutableRows();
		void Reindex();

		std::shared_ptr<std::vector<FSetRowPtr>> Rows;
		std::unordered_map<std::string, size_t> IndexById;
		FFingerprint Fingerprint{};
		bool bHasFingerprint = false;
		uint32_t Dims = 0;
	};
}
