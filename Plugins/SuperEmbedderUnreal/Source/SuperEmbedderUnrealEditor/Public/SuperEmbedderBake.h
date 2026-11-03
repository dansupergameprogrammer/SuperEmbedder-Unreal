#pragma once

#include "CoreMinimal.h"
#include "SuperEmbedderTypes.h"

class UDataTable;

/**
 * The bake (proposal rev 9, section 2.4): {id, text} rows to a vector set asset, through the documents encode
 * path, so every row rule (ids, duplicates, UTF-8, the token bound) is checked before anything encodes and a
 * refusal names the row. Asynchronous: the open and the encodes run on the encoder's workers, the game thread
 * only polls. The asset is written only when every row has encoded, so a cancel or a refusal leaves nothing
 * behind (an existing asset at the path is left as it was). One bake at a time.
 */
class SUPEREMBEDDERUNREALEDITOR_API FSuperEmbedderBake
{
public:
	using FOnFinished = TFunction<void(ESuperEmbedderStatus Status, const FString& Message)>;

	/** PackageName is a long package name, such as /Game/Lore/LoreSet. bNotify shows a progress toast with Cancel. */
	static bool Start(TArray<FSuperEmbedderDocumentRow> Rows, const FString& PackageName, bool bNotify, FOnFinished OnFinished);
	static bool StartFromJsonlFile(const FString& JsonlPath, const FString& PackageName, bool bNotify, FOnFinished OnFinished);
	static bool StartFromDataTable(const UDataTable* Table, const FString& PackageName, bool bNotify, FOnFinished OnFinished);

	static bool IsRunning();
	static void Cancel();
};

/** Validation (proposal rev 9, section 2.4): every vector set asset whose row digest fails or whose fingerprint differs from the open encoder's. */
struct SUPEREMBEDDERUNREALEDITOR_API FSuperEmbedderValidate
{
	/** Needs a Ready encoder. Returns how many sets were listed; Report holds one line per set checked. */
	static int32 ValidateAllSets(TArray<FString>& OutReport);
};
