#pragma once

#include "CoreMinimal.h"
#include "Engine/CancellableAsyncAction.h"
#include "SuperEmbedderSubsystem.h"
#include "SuperEmbedderTypes.h"
#include "SuperEmbedderAsyncActions.generated.h"

class USuperEmbedderVectorSet;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSuperEmbedderOpenDone, const FSuperEmbedderOpenResult&, Result, USuperEmbedderEncoderHandle*, Handle);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSuperEmbedderEncodeDone, const FSuperEmbedderEncodeResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSuperEmbedderDocumentsDone, const FSuperEmbedderDocumentsResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSuperEmbedderTopKDone, const FSuperEmbedderTopKResult&, Result);

/**
 * The shared base of the SuperEmbedder nodes. Each request gets exactly one of its pins, on a later tick (never
 * inside the node's own call). Cancel on the node's Async Action pin delivers Cancelled on a later tick.
 * Registered with the game instance when there is one; otherwise (an editor utility) it keeps itself alive
 * until its pin fires.
 */
UCLASS(Abstract)
class SUPEREMBEDDERUNREAL_API USuperEmbedderAsyncActionBase : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	virtual void Cancel() override;
	virtual bool ShouldBroadcastDelegates() const override;
	virtual void SetReadyToDestroy() override;

protected:
	void Register(const UObject* WorldContext);
	/** True once, for the pin that ends this action. */
	bool BeginFinish();
	/** Runs Body on the next core tick if this action is still alive: for a refusal found before any request. */
	void NextTick(TFunction<void()> Body);

	TWeakObjectPtr<const UObject> WorldContext;
	FSuperEmbedderTicket Ticket;
	bool bFinished = false;
	bool bRooted = false;
};

/** Open SuperEmbedder Encoder: Opened fires when the encoder is Ready (20-38 s on the published model), Failed with why. Empty paths use Project Settings. Keep the handle for as long as you encode. */
UCLASS()
class SUPEREMBEDDERUNREAL_API USuperEmbedderOpenEncoderAction : public USuperEmbedderAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Open SuperEmbedder Encoder"))
	static USuperEmbedderOpenEncoderAction* OpenSuperEmbedderEncoder(UObject* WorldContextObject, FString ModelFile, FString TokenizerFile);

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderOpenDone Opened;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderOpenDone Failed;

	virtual void Activate() override;
	virtual void Cancel() override;

private:
	FString ModelFile;
	FString TokenizerFile;
	UPROPERTY(Transient)
	TObjectPtr<USuperEmbedderEncoderHandle> Handle;
};

/** Encode Text (Async): a query vector, on a worker. */
UCLASS()
class SUPEREMBEDDERUNREAL_API USuperEmbedderEncodeTextAction : public USuperEmbedderAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Encode Text (Async)"))
	static USuperEmbedderEncodeTextAction* EncodeTextAsync(UObject* WorldContextObject, FString Text);

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderEncodeDone Completed;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderEncodeDone Refused;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderEncodeDone Cancelled;

	virtual void Activate() override;

private:
	void OnDone(const FSuperEmbedderEncodeResult& Result);
	FString Text;
};

/** Encode Documents (Async): every row's document vector, in row order, in one Completed. A refusal names the row before anything encodes. */
UCLASS()
class SUPEREMBEDDERUNREAL_API USuperEmbedderEncodeDocumentsAction : public USuperEmbedderAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Encode Documents (Async)"))
	static USuperEmbedderEncodeDocumentsAction* EncodeDocumentsAsync(UObject* WorldContextObject, const TArray<FSuperEmbedderDocumentRow>& Rows);

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderDocumentsDone Completed;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderDocumentsDone Refused;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderDocumentsDone Cancelled;

	virtual void Activate() override;

private:
	void OnDone(const FSuperEmbedderDocumentsResult& Result);
	TArray<FSuperEmbedderDocumentRow> Rows;
};

/** Top K (Async): ranks a set's rows against a vector on a worker, over the rows as they are when the node runs. */
UCLASS()
class SUPEREMBEDDERUNREAL_API USuperEmbedderTopKAction : public USuperEmbedderAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Top K (Async)", AutoCreateRefTerm = "ExcludeIds"))
	static USuperEmbedderTopKAction* TopKAsync(UObject* WorldContextObject, USuperEmbedderVectorSet* Set, const FSuperEmbedderVector& Query, int32 K, const TArray<FString>& ExcludeIds);

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderTopKDone Completed;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderTopKDone Refused;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderTopKDone Cancelled;

	virtual void Activate() override;

private:
	void OnDone(const FSuperEmbedderTopKResult& Result);
	UPROPERTY(Transient)
	TObjectPtr<USuperEmbedderVectorSet> Set;
	FSuperEmbedderVector Query;
	int32 K = 5;
	TArray<FString> ExcludeIds;
};

/** Query Text Top K (Async): encodes Text as a query, then ranks the set's rows as they are when the encode completes. */
UCLASS()
class SUPEREMBEDDERUNREAL_API USuperEmbedderQueryTextTopKAction : public USuperEmbedderAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Query Text Top K (Async)", AutoCreateRefTerm = "ExcludeIds"))
	static USuperEmbedderQueryTextTopKAction* QueryTextTopKAsync(UObject* WorldContextObject, USuperEmbedderVectorSet* Set, FString Text, int32 K, const TArray<FString>& ExcludeIds);

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderTopKDone Completed;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderTopKDone Refused;

	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderTopKDone Cancelled;

	virtual void Activate() override;

private:
	void OnDone(const FSuperEmbedderTopKResult& Result);
	UPROPERTY(Transient)
	TObjectPtr<USuperEmbedderVectorSet> Set;
	FString Text;
	int32 K = 5;
	TArray<FString> ExcludeIds;
};
