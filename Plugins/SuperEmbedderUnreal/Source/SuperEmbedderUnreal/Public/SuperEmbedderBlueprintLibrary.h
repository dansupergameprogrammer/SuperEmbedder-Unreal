#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SuperEmbedderTypes.h"
#include "SuperEmbedderBlueprintLibrary.generated.h"

class UDataTable;
class USuperEmbedderVectorSet;

/**
 * The synchronous similarity helpers and the row readers. Exact and brute force: the vectors are not unit
 * length (L2 near 82 on the published model), so every score is a cosine, accumulated in double in a fixed order.
 * Cosine and Top K run on the game thread and refuse Wrong Thread elsewhere; for a large set use Top K (Async).
 */
UCLASS()
class SUPEREMBEDDERUNREAL_API USuperEmbedderBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Cosine similarity of two vectors from the same encoder (two unchecked vectors are also allowed). */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Similarity")
	static ESuperEmbedderStatus Cosine(const FSuperEmbedderVector& A, const FSuperEmbedderVector& B, float& OutScore);

	/** The K rows most similar to Query, best first; equal scores rank the lower row first. K above the row count returns every row. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Similarity", meta = (AutoCreateRefTerm = "ExcludeIds"))
	static ESuperEmbedderStatus TopK(USuperEmbedderVectorSet* Set, const FSuperEmbedderVector& Query, int32 K, const TArray<FString>& ExcludeIds, TArray<FSuperEmbedderHit>& OutHits);

	/** A new, empty, transient vector set to Append to at run time. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set", meta = (WorldContext = "WorldContextObject"))
	static USuperEmbedderVectorSet* NewVectorSet(UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder", meta = (DisplayName = "Status To String"))
	static FString StatusToString(ESuperEmbedderStatus Status);

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder")
	static int32 GetVectorDims(const FSuperEmbedderVector& Vector) { return Vector.Values.Num(); }

	/** SHA-256 of the vector's float32 values, little-endian: equal digests mean bit-identical vectors. */
	UFUNCTION(BlueprintPure, Category = "SuperEmbedder")
	static FString GetVectorDigest(const FSuperEmbedderVector& Vector) { return Vector.GetDigestHex(); }

	// --- Row readers (C++; the editor's bake uses them) ---

	/** JSONL bytes, one {"id": ..., "text": ...} object per line. A refusal names its line in OutMessage. */
	static ESuperEmbedderStatus ParseJsonlRows(const TArray<uint8>& Bytes, TArray<FSuperEmbedderDocumentRow>& OutRows, FString& OutMessage);

	/** A DataTable's rows: the row name is the id; the text is the row struct's string or text property named Text. */
	static ESuperEmbedderStatus RowsFromDataTable(const UDataTable* Table, TArray<FSuperEmbedderDocumentRow>& OutRows, FString& OutMessage);

	/** The row rules every bake input meets before an encode: at least one row, valid unique ids, non-empty text. */
	static ESuperEmbedderStatus CheckRows(const TArray<FSuperEmbedderDocumentRow>& Rows, FString& OutMessage);
};
