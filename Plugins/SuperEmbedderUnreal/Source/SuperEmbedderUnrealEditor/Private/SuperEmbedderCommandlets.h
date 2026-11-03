#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "SuperEmbedderCommandlets.generated.h"

/**
 * -run=SuperEmbedderBake -Input=<rows.jsonl | /Game/Path/DataTable.DataTable> -Output=/Game/Path/SetName
 * Opens the encoder from Project Settings, bakes through the documents path and saves the set. Exit 0 on a
 * written set, 1 on any refusal (named in the log).
 */
UCLASS()
class USuperEmbedderBakeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	USuperEmbedderBakeCommandlet();
	virtual int32 Main(const FString& Params) override;
};

/**
 * -run=SuperEmbedderValidate
 * Opens the encoder from Project Settings, loads every vector set and lists, by name, each one whose row digest
 * fails or whose fingerprint differs from the encoder's. Exit 1 if any is listed or the encoder fails to open.
 */
UCLASS()
class USuperEmbedderValidateCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	USuperEmbedderValidateCommandlet();
	virtual int32 Main(const FString& Params) override;
};
