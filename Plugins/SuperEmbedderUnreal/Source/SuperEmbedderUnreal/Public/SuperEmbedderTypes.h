#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "SuperEmbedderTypes.generated.h"

// The Blueprint-facing vocabulary (proposal rev 9, sections 2.2 and 2.3). ESuperEmbedderStatus mirrors the
// cores' SuperEmbedderUe::EStatus value for value (a static_assert in SuperEmbedderConvert.cpp pins the count),
// so every refusal reaches Blueprint by the same name the cores and the host suite use.

UENUM(BlueprintType)
enum class ESuperEmbedderStatus : uint8
{
	Ok = 0,
	EncoderNotReady = 1,
	QueueFull = 2,
	QueryTooLong = 3,
	DocumentTooLong = 4,
	InvalidText = 5,
	InvalidUtf8 = 6,
	EmptyText = 7,
	DuplicateId = 8,
	EmptyTicket = 9,
	InvalidId = 10,
	UnknownTicket = 11,
	GameThreadBlockingRefused = 12,
	WorkerThreadBlockingRefused = 13,
	DifferentModelOpen = 14,
	ModelInUse = 15,
	ModelFileMissing = 16,
	TokenizerFileMissing = 17,
	NotAnSslmArtifact = 18,
	ArtifactShaMismatch = 19,
	OpenFailed = 20,
	EncodeFailed = 21,
	FingerprintMismatch = 22,
	DimsMismatch = 23,
	UncheckedVector = 24,
	ZeroVector = 25,
	NonFiniteVector = 26,
	InvalidK = 27,
	UnknownId = 28,
	WrongThread = 29,
	SetCorrupt = 30,
	BadMagic = 31,
	UnsupportedVersion = 32,
	Truncated = 33,
	Malformed = 34,
	EntryDigestMismatch = 35,
	ShuttingDown = 36,
	InvalidSetting = 37,
	EncoderClosing = 38,
	Cancelled = 39,
	MissingField = 40,
	NonStringField = 41,
	EmptyInput = 42,
	UnknownHandle = 43,
	NullSet = 44,
};

UENUM(BlueprintType)
enum class ESuperEmbedderEncoderState : uint8
{
	Closed = 0,
	Opening = 1,
	Ready = 2,
	Failed = 3,
	Closing = 4,
};

UENUM(BlueprintType)
enum class ESuperEmbedderVectorKind : uint8
{
	Query = 0,
	Document = 1,
};

UENUM(BlueprintType)
enum class ESuperEmbedderOutcome : uint8
{
	Completed = 0,
	Refused = 1,
	Cancelled = 2,
};

/**
 * An encoded vector and where it came from: the floats, the encoder fingerprint and the kind. Every similarity
 * helper refuses a vector whose fingerprint differs from the set's (or the other vector's), so a vector held
 * from before a model change cannot meet the new rows. Only an encode or a set row makes a checked vector;
 * Blueprint cannot fill one in (a Make node, if the editor offers one, yields an empty unchecked vector that
 * every helper refuses with Unchecked Vector).
 */
USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderVector
{
	GENERATED_BODY()

	/** The vector's components. Not unit length; the similarity helpers normalize. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	TArray<float> Values;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderVectorKind Kind = ESuperEmbedderVectorKind::Query;

	/** True only for a vector an encode produced or a set row read back. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	bool bChecked = false;

	/** The encoder fingerprint (32 bytes) of a checked vector; empty otherwise. */
	UPROPERTY()
	TArray<uint8> Fingerprint;

	/**
	 * The one way to wrap raw floats: an UNCHECKED vector. It carries no fingerprint, and every helper refuses it
	 * (Unchecked Vector) except Cosine between two unchecked vectors.
	 */
	static FSuperEmbedderVector MakeUnchecked(const TArray<float>& InValues)
	{
		FSuperEmbedderVector V;
		V.Values = InValues;
		return V;
	}

	FString GetFingerprintHex() const;
	/** SHA-256 of the float32 values in little-endian order (the proposal's vector digest). */
	FString GetDigestHex() const;
};

/** One ranked row. */
USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderHit
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	FString Id;

	/** Cosine similarity, -1 to 1. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	float Score = 0.0f;

	/** The row's index in the set when it was ranked. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	int32 RowIndex = INDEX_NONE;
};

/** One {id, text} row of a documents encode or a bake. */
USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderDocumentRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperEmbedder")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperEmbedder")
	FString Text;
};

/** A DataTable row for baking a vector set: the row name is the id, Text is the text. Any row struct with a string or text property named Text also bakes. */
USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderTextRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperEmbedder", meta = (MultiLine = "true"))
	FString Text;
};

/** A ticket names one asynchronous request. Exactly one of Completed, Refused or Cancelled is delivered for it, never inside the call that issued it. */
USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderTicket
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	int64 Id = 0;

	bool IsValid() const { return Id != 0; }
};

USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderEncodeResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderOutcome Outcome = ESuperEmbedderOutcome::Refused;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderStatus Status = ESuperEmbedderStatus::Ok;

	/** The refusal's detail (a row, a token count, a library status name); empty on success. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	FSuperEmbedderVector Vector;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	bool bFromCache = false;
};

USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderDocumentsResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderOutcome Outcome = ESuperEmbedderOutcome::Refused;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderStatus Status = ESuperEmbedderStatus::Ok;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	FString Message;

	/** The row ids, in row order. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	TArray<FString> Ids;

	/** One vector per row, in row order. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	TArray<FSuperEmbedderVector> Vectors;
};

USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderTopKResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderOutcome Outcome = ESuperEmbedderOutcome::Refused;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderStatus Status = ESuperEmbedderStatus::Ok;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	FString Message;

	/** Best first; equal scores rank the lower row first. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	TArray<FSuperEmbedderHit> Hits;

	/** Query Text Top K: the encoded query. */
	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	FSuperEmbedderVector Query;
};

using FSuperEmbedderEncodeCallback = TFunction<void(const FSuperEmbedderEncodeResult&)>;
using FSuperEmbedderDocumentsCallback = TFunction<void(const FSuperEmbedderDocumentsResult&)>;
using FSuperEmbedderTopKCallback = TFunction<void(const FSuperEmbedderTopKResult&)>;

SUPEREMBEDDERUNREAL_API FString LexToString(ESuperEmbedderStatus Status);
