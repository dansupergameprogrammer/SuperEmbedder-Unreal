#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SuperEmbedderSubsystem.h"
#include "SuperEmbedderTypes.h"
#include "SuperEmbedderExample.generated.h"

class USuperEmbedderEncoderHandle;
class USuperEmbedderVectorSet;

UENUM(BlueprintType)
enum class ESuperEmbedderExamplePhase : uint8
{
	Opening,
	Baking,
	Querying,
	Ready,
	Failed,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSuperEmbedderExampleAnswered, const FString&, Query, const TArray<FSuperEmbedderHit>&, Hits);

/**
 * The whole example, in one actor. On BeginPlay it:
 *  1. opens the encoder (Open SuperEmbedder Encoder) from Project Settings;
 *  2. encodes Rows as documents (Encode Documents (Async)) and bakes them into a run-time vector set
 *     (New Vector Set, then Append Documents);
 *  3. runs each of Queries against the set (Query Text Top K (Async)) and logs the top K rows with scores.
 * Ask queues one more query at any time after that, and Answered fires for each.
 *
 * It calls the plugin's Blueprint-callable API only, from C++ the way a Blueprint graph calls it: each async
 * node is created by its factory, its pins are bound with AddDynamic to UFUNCTIONs here, and it is activated.
 * A Blueprint subclass placed in a level replaces the one the game mode spawns, and can edit Rows, Queries and
 * K, call Ask, and bind Answered.
 *
 * With -SuperEmbedderExampleExit on the command line it quits when the fixed queries are done: exit code 0
 * when every step completed and every query returned min(K, rows) hits ranked best first, 1 otherwise.
 */
UCLASS(Blueprintable)
class ASuperEmbedderExample : public AActor
{
	GENERATED_BODY()

public:
	ASuperEmbedderExample();

	/** The rows baked into the set: id and text. Self-authored, short, one place each. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperEmbedder Example")
	TArray<FSuperEmbedderDocumentRow> Rows;

	/** The queries run once the set is baked. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperEmbedder Example")
	TArray<FString> Queries;

	/** The row id a reader would expect first for Queries[i]; shown beside the result, never gated on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperEmbedder Example")
	TArray<FString> ExpectedTopIds;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SuperEmbedder Example", meta = (ClampMin = "1"))
	int32 K = 3;

	/** Queues a query against the baked set; it runs after any query already waiting. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder Example")
	void Ask(const FString& Text);

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder Example")
	ESuperEmbedderExamplePhase GetPhase() const { return Phase; }

	/** What the HUD draws: the newest lines last. */
	const TArray<FString>& GetLines() const { return Lines; }

	/** Fires on the game thread for every query answered. */
	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder Example")
	FSuperEmbedderExampleAnswered Answered;

	static ASuperEmbedderExample* Find(UWorld* World);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UFUNCTION()
	void OnOpened(const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle* InHandle);

	UFUNCTION()
	void OnOpenFailed(const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle* InHandle);

	UFUNCTION()
	void OnDocumentsEncoded(const FSuperEmbedderDocumentsResult& Result);

	UFUNCTION()
	void OnDocumentsNotEncoded(const FSuperEmbedderDocumentsResult& Result);

	UFUNCTION()
	void OnQueryAnswered(const FSuperEmbedderTopKResult& Result);

	UFUNCTION()
	void OnQueryNotAnswered(const FSuperEmbedderTopKResult& Result);

	void RunNextQuery();
	void Fail(const FString& Why);
	void Say(const FString& Line);
	void FinishFixedQueries();

	ESuperEmbedderExamplePhase Phase = ESuperEmbedderExamplePhase::Opening;

	/** Keeps the encoder open for as long as the example encodes. */
	UPROPERTY(Transient)
	TObjectPtr<USuperEmbedderEncoderHandle> Handle;

	/** The vector set baked from Rows. */
	UPROPERTY(Transient)
	TObjectPtr<USuperEmbedderVectorSet> Set;

	TArray<FString> Pending;
	FString InFlight;
	bool bQueryInFlight = false;
	int32 FixedQueriesLeft = 0;
	int32 FixedQueryIndex = 0;
	bool bAllChecksPassed = true;
	bool bExitWhenDone = false;
	double StartSeconds = 0.0;
	TArray<FString> Lines;
};
