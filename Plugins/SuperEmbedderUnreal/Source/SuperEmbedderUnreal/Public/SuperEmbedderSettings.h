#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SuperEmbedderSettings.generated.h"

/**
 * Project Settings > Plugins > SuperEmbedder. Saved to Config/DefaultGame.ini under
 * [/Script/SuperEmbedderUnreal.SuperEmbedderSettings]. The encoder settings are read once, when the engine
 * starts the subsystem; change them and restart. The model paths are read at each open.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "SuperEmbedder"))
class SUPEREMBEDDERUNREAL_API USuperEmbedderSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** The .sslm model file. Relative paths are relative to the project directory. Stage its folder as a non-asset directory (not in the pak): the library reads the file itself. */
	UPROPERTY(config, EditAnywhere, Category = "Model")
	FString ModelFile = TEXT("Content/SuperEmbedder/model.sslm");

	/** The tokenizer .sslm file that ships with the model. Relative paths are relative to the project directory. */
	UPROPERTY(config, EditAnywhere, Category = "Model")
	FString TokenizerFile = TEXT("Content/SuperEmbedder/tokenizer.sslm");

	/** Optional: the model file's expected SHA-256 (64 hex characters). Checked off the game thread before the open; empty skips the check. Hashing the published model takes a few seconds. */
	UPROPERTY(config, EditAnywhere, Category = "Model")
	FString ModelSha256;

	/** Optional: the tokenizer file's expected SHA-256. */
	UPROPERTY(config, EditAnywhere, Category = "Model")
	FString TokenizerSha256;

	/** Open the encoder from these files when the engine starts, and keep it open for the whole process. The open takes 20-38 s on the published model and never runs on the game thread. */
	UPROPERTY(config, EditAnywhere, Category = "Model")
	bool bOpenOnStartup = false;

	/** Worker threads and concurrent encodes, 1-4. At 2 or more, one worker only ever runs queries, so a query never waits behind documents. */
	UPROPERTY(config, EditAnywhere, Category = "Encoding", meta = (ClampMin = "1", ClampMax = "4"))
	int32 MaxConcurrency = 1;

	/** The longest query, in body tokens counted by the encoder's own tokenizer, 1-512. */
	UPROPERTY(config, EditAnywhere, Category = "Encoding", meta = (ClampMin = "1", ClampMax = "512"))
	int32 MaxQueryTokens = 256;

	/** The longest document row, in tokens, 1-512. Also bounds how long shutdown waits: one row per worker. */
	UPROPERTY(config, EditAnywhere, Category = "Encoding", meta = (ClampMin = "1", ClampMax = "512"))
	int32 MaxDocumentTokens = 256;

	/** The byte ceiling, checked before any tokenizing: the longest query or row in UTF-8 bytes. */
	UPROPERTY(config, EditAnywhere, Category = "Encoding", meta = (ClampMin = "1", ClampMax = "65536"))
	int32 MaxTextBytes = 4096;

	/** How many queries may wait at once; one more is refused Queue Full. */
	UPROPERTY(config, EditAnywhere, Category = "Encoding", meta = (ClampMin = "1", ClampMax = "100000"))
	int32 MaxPendingQueries = 64;

	/** How many documents requests may be outstanding at once; one more is refused Queue Full. */
	UPROPERTY(config, EditAnywhere, Category = "Encoding", meta = (ClampMin = "1", ClampMax = "1000"))
	int32 MaxPendingDocumentTickets = 8;

	/** Exact encode cache entries (0 disables it). 4,096 entries of the published model's 1,024 floats is about 16 MB. */
	UPROPERTY(config, EditAnywhere, Category = "Cache", meta = (ClampMin = "0", ClampMax = "1000000"))
	int32 MaxCacheEntries = 4096;

	/** Optional: a cache file (written by Save Encode Cache) loaded off the game thread at startup. A file for another encoder is cleared; a damaged file is refused by name and the game runs with a cold cache. Relative to the project directory. */
	UPROPERTY(config, EditAnywhere, Category = "Cache")
	FString CacheFile;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** A settings path made absolute: relative paths are taken from the project directory. Empty stays empty. */
	static FString ResolvePath(const FString& Path);
};
