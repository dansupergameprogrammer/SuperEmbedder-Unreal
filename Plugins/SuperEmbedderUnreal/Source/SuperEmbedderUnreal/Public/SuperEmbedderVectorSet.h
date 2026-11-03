#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SuperEmbedderTypes.h"
#include "SuperEmbedderVectorSet.generated.h"

namespace SuperEmbedderUe
{
	class FVectorSetCore;
}

/**
 * A vector set (proposal rev 9, section 2.3): ids, source text, float32 rows, dims, the encoder fingerprint and a
 * row digest. Plain data: a set holds no encoder open, and it outlives the encoder safely.
 *
 * A set belongs to the game thread: Append, Remove and the similarity helpers refuse Wrong Thread elsewhere.
 * Every read from disk (cooked or saved) recomputes the row digest where the engine serializes the asset, on the
 * async loading thread for a streamed load, and a mismatch leaves the set empty with Load Status Set Corrupt.
 * Bake one in the editor (right-click a DataTable, or SuperEmbedder.BakeJsonl), or build one at run time.
 */
UCLASS(BlueprintType)
class SUPEREMBEDDERUNREAL_API USuperEmbedderVectorSet : public UObject
{
	GENERATED_BODY()

public:
	USuperEmbedderVectorSet();

	/** Adds a row. Refuses Unchecked Vector, Invalid Id, Fingerprint Mismatch, Dims Mismatch, Non Finite Vector, Zero Vector or Duplicate Id. The first row of an empty set gives it its fingerprint. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set")
	ESuperEmbedderStatus Append(const FString& Id, const FString& Text, const FSuperEmbedderVector& Vector);

	/** Adds every row of a completed Encode Documents result, with Texts[i] as row i's text (Texts may be empty). All or nothing: the first refusal names the row and adds none. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set")
	ESuperEmbedderStatus AppendDocuments(const FSuperEmbedderDocumentsResult& Documents, const TArray<FString>& Texts, FString& OutMessage);

	/** Removes a row. Refuses Unknown Id. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set")
	ESuperEmbedderStatus Remove(const FString& Id);

	/** Removes every row and the fingerprint. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set")
	void Reset();

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder|Vector Set")
	int32 Num() const;

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder|Vector Set")
	bool Contains(const FString& Id) const;

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder|Vector Set")
	TArray<FString> GetIds() const;

	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set")
	ESuperEmbedderStatus GetText(const FString& Id, FString& OutText) const;

	/** The row as a checked document vector of this set's fingerprint. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set")
	ESuperEmbedderStatus GetRowVector(const FString& Id, FSuperEmbedderVector& OutVector) const;

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder|Vector Set")
	int32 GetDims() const;

	/** The encoder fingerprint, 64 hex characters; empty for a set that has never had a row. */
	UFUNCTION(BlueprintPure, Category = "SuperEmbedder|Vector Set")
	FString GetFingerprintHex() const;

	/** Ok, or why the last load from disk or bytes was refused (Set Corrupt, Bad Magic, Unsupported Version, Truncated, Malformed). */
	UFUNCTION(BlueprintPure, Category = "SuperEmbedder|Vector Set")
	ESuperEmbedderStatus GetLoadStatus() const { return LoadStatus; }

	/** The set's saved form ('SEVS' version 1). */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set")
	ESuperEmbedderStatus SaveToBytes(TArray<uint8>& OutBytes) const;

	/** Replaces the rows with a saved form, only when it reads clean; a refusal leaves the set as it was. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder|Vector Set")
	ESuperEmbedderStatus LoadFromBytes(const TArray<uint8>& Bytes, FString& OutMessage);

	//~ UObject
	virtual void Serialize(FArchive& Ar) override;

	/** For the module's own C++ (the subsystem, the similarity helpers). Never null. */
	const SuperEmbedderUe::FVectorSetCore& GetCore() const { return *Core; }

	/** True on the game thread (or with no game thread yet, as in a commandlet's startup). */
	static bool IsOwnerThread();

protected:
	/** Shown in the editor only; the rows themselves are saved through Serialize. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "SuperEmbedder")
	int32 RowCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "SuperEmbedder")
	int32 Dims = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "SuperEmbedder")
	FString Fingerprint;

	UPROPERTY(VisibleAnywhere, Transient, Category = "SuperEmbedder")
	ESuperEmbedderStatus LoadStatus = ESuperEmbedderStatus::Ok;

	UPROPERTY(VisibleAnywhere, Transient, Category = "SuperEmbedder")
	FString LoadMessage;

private:
	void RefreshSummary();

	TSharedPtr<SuperEmbedderUe::FVectorSetCore> Core;
};
