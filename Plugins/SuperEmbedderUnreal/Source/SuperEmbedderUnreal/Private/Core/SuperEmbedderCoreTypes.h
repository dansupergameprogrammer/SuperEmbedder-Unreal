#pragma once

// The engine-free cores' shared vocabulary: the named statuses every core returns, the two encode kinds, and
// the vector that carries where it came from (proposal rev 9, sections 2.2 and 2.3). C++17, standard library
// only, no exceptions, no RTTI, no Unreal header. The UE layer mirrors EStatus as ESuperEmbedderStatus,
// value for value (SuperEmbedderTypes.h), and a static_assert there pins the count.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SuperEmbedderUe
{
	// Append-only: existing values never move (the UE enum mirrors them by value).
	enum class EStatus : uint8_t
	{
		Ok = 0,
		EncoderNotReady = 1,           // an encode before Ready (Opening, Failed, Closed, Closing)
		QueueFull = 2,                 // MaxPendingQueries or MaxPendingDocumentTickets already waiting
		QueryTooLong = 3,              // over MaxQueryTokens, or over the byte ceiling at enqueue
		DocumentTooLong = 4,           // a row over MaxDocumentTokens, or over the byte ceiling
		InvalidText = 5,               // an unpaired UTF-16 surrogate (FString input)
		InvalidUtf8 = 6,
		EmptyText = 7,                 // empty after conversion
		DuplicateId = 8,
		EmptyTicket = 9,               // a documents ticket with no rows
		InvalidId = 10,                // an empty id, invalid UTF-8, or longer than kMaxIdBytes
		UnknownTicket = 11,
		GameThreadBlockingRefused = 12,
		WorkerThreadBlockingRefused = 13, // a blocking encode from one of the service's own workers
		DifferentModelOpen = 14,
		ModelInUse = 15,
		ModelFileMissing = 16,
		TokenizerFileMissing = 17,
		NotAnSslmArtifact = 18,        // the file does not begin with the .sslm magic
		ArtifactShaMismatch = 19,      // a configured expected SHA-256 differs
		OpenFailed = 20,               // the library refused the open; the detail names its status
		EncodeFailed = 21,             // the library refused an encode; the detail names its status
		FingerprintMismatch = 22,
		DimsMismatch = 23,
		UncheckedVector = 24,
		ZeroVector = 25,
		NonFiniteVector = 26,
		InvalidK = 27,
		UnknownId = 28,
		WrongThread = 29,
		SetCorrupt = 30,               // a set's row digest does not match its rows
		BadMagic = 31,
		UnsupportedVersion = 32,
		Truncated = 33,
		Malformed = 34,                // framing that is not truncation: trailing bytes, a bad flag, bad rows
		EntryDigestMismatch = 35,      // a cache entry's own digest fails
		ShuttingDown = 36,
		InvalidSetting = 37,
		EncoderClosing = 38,           // an open while the last handle's close is still running
		Cancelled = 39,                // the outcome's status when a ticket is cancelled
		MissingField = 40,             // bake input: a JSONL object without "id" or "text"
		NonStringField = 41,           // bake input: "id" or "text" is not a JSON string
		EmptyInput = 42,               // bake input: no rows at all
		UnknownHandle = 43,
		NullSet = 44,                  // a similarity call or a ranking given no vector set
		Count = 45,
	};

	const char* StatusName(EStatus Status);

	enum class EKind : uint8_t
	{
		Query = 0,
		Document = 1,
	};

	const char* KindName(EKind Kind);

	using FFingerprint = std::array<uint8_t, 32>;

	std::string HexOf(const uint8_t* Data, size_t Size);
	inline std::string HexOf(const FFingerprint& Fp) { return HexOf(Fp.data(), Fp.size()); }

	// The longest id a row may carry, in UTF-8 bytes.
	constexpr size_t kMaxIdBytes = 256;

	// An encoded vector and its provenance (section 2.3, audit Ms-13). bChecked is true only for a vector an
	// encode produced (or a set row read back): it then carries the encoder fingerprint and the kind. A
	// vector built from raw floats is unchecked, and every similarity helper refuses it UncheckedVector,
	// except Cosine between two unchecked vectors (re-audit G-28, cell V-11).
	struct FEmbedding
	{
		std::vector<float> Values;
		FFingerprint Fingerprint{};
		EKind Kind = EKind::Query;
		bool bChecked = false;

		// The explicitly named unchecked constructor (the README documents it).
		static FEmbedding Unchecked(std::vector<float> InValues)
		{
			FEmbedding V;
			V.Values = static_cast<std::vector<float>&&>(InValues);
			return V;
		}
		// What an encode produces. The service and the set reader are its only production callers.
		static FEmbedding Checked(std::vector<float> InValues, const FFingerprint& InFingerprint, EKind InKind)
		{
			FEmbedding V;
			V.Values = static_cast<std::vector<float>&&>(InValues);
			V.Fingerprint = InFingerprint;
			V.Kind = InKind;
			V.bChecked = true;
			return V;
		}
	};

	// SHA-256 of the vector's float32 values in little-endian order: the proposal's section 10.1 vector digest.
	std::string VectorDigestHex(const std::vector<float>& Values);

	// Little-endian framing helpers shared by the set and cache formats.
	void PutU32(std::vector<uint8_t>& Out, uint32_t Value);
	void PutBytes(std::vector<uint8_t>& Out, const void* Data, size_t Size);
	void PutFloatsLe(std::vector<uint8_t>& Out, const float* Values, size_t Count);

	// A bounds-checked reader over a byte span. Every read past the end sets bTruncated and returns zeros.
	struct FByteReader
	{
		const uint8_t* Data = nullptr;
		size_t Size = 0;
		size_t Pos = 0;
		bool bTruncated = false;

		bool Has(size_t N) const { return !bTruncated && Size - Pos >= N && Pos <= Size; }
		uint32_t U32();
		uint8_t U8();
		bool Bytes(void* Out, size_t N);
		bool FloatsLe(float* Out, size_t Count);
		size_t Remaining() const { return Pos <= Size ? Size - Pos : 0; }
	};
}
