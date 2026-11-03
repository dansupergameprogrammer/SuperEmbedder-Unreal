#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "SuperEmbedderTypes.h"
#include "SuperEmbedderSubsystem.generated.h"

class USuperEmbedderSubsystem;
class USuperEmbedderVectorSet;
struct FSuperEmbedderSubsystemState;

/** What an open reports. */
USTRUCT(BlueprintType)
struct SUPEREMBEDDERUNREAL_API FSuperEmbedderOpenResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderStatus Status = ESuperEmbedderStatus::Ok;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	ESuperEmbedderEncoderState State = ESuperEmbedderEncoderState::Closed;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	int32 Dims = 0;

	UPROPERTY(BlueprintReadOnly, Category = "SuperEmbedder")
	FString FingerprintHex;
};

using FSuperEmbedderOpenCallback = TFunction<void(const FSuperEmbedderOpenResult&, class USuperEmbedderEncoderHandle*)>;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSuperEmbedderStateChanged, ESuperEmbedderEncoderState, State);

/**
 * One reason the encoder stays open. The encoder closes when the last handle is released (Release, or the
 * handle being garbage collected). Keep it referenced (a UPROPERTY, a Blueprint variable) for as long as you
 * encode.
 */
UCLASS(BlueprintType)
class SUPEREMBEDDERUNREAL_API USuperEmbedderEncoderHandle : public UObject
{
	GENERATED_BODY()

public:
	/** Lets the encoder close if no other handle holds it. Idempotent. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder")
	void Release();

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder")
	bool IsHeld() const { return Handle != 0; }

	virtual void BeginDestroy() override;

private:
	friend class USuperEmbedderSubsystem;
	uint64 Handle = 0;
	TWeakObjectPtr<USuperEmbedderSubsystem> Owner;
};

/**
 * The one encoder per process (proposal rev 9, sections 2.1-2.3). An engine subsystem, so the encoder survives
 * play-in-editor sessions and map changes; a world's requests are cancelled when that world ends.
 *
 * Every encode and every asynchronous ranking returns a ticket, and exactly one callback fires for it on a later
 * game-thread tick (never inside the call), refusals and cache hits included. Nothing here blocks the game
 * thread: the open, the token counts, the encodes and the async rankings run on worker threads.
 */
UCLASS()
class SUPEREMBEDDERUNREAL_API USuperEmbedderSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	static USuperEmbedderSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// --- Encoder ---

	/**
	 * Starts an open (or joins the one in progress with the same files) and returns a handle at once; OnDone fires
	 * on a later tick when the encoder is Ready or the open failed. Empty paths use Project Settings. A different
	 * model while one is open refuses Different Model Open. Returns the handle (null when refused at once; OnDone
	 * still fires, on a later tick, with the refusal). Hold the handle for as long as you encode.
	 */
	USuperEmbedderEncoderHandle* OpenEncoder(const FString& ModelFile, const FString& TokenizerFile, FSuperEmbedderOpenCallback OnDone);

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder")
	ESuperEmbedderEncoderState GetEncoderState() const;

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder")
	bool IsEncoderReady() const { return GetEncoderState() == ESuperEmbedderEncoderState::Ready; }

	/** The last open's outcome and its detail. */
	UFUNCTION(BlueprintPure, Category = "SuperEmbedder")
	FSuperEmbedderOpenResult GetOpenResult() const;

	/** Releases the editor's keep-alive handle (also the console command SuperEmbedder.CloseEncoder). */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder")
	void ReleaseKeepAlive();

	/** Broadcast on the game thread when the encoder's state changes. */
	UPROPERTY(BlueprintAssignable, Category = "SuperEmbedder")
	FSuperEmbedderStateChanged OnEncoderStateChanged;

	// --- Encode (C++; Blueprint uses the async action nodes) ---

	/** A query, encoded on a worker with the library's query composition. WorldContext's world owns the ticket (null: none). */
	FSuperEmbedderTicket EncodeText(const FString& Text, const UObject* WorldContext, FSuperEmbedderEncodeCallback OnDone);

	/** {id, text} rows, validated as a whole on a worker, then encoded one row at a time. */
	FSuperEmbedderTicket EncodeDocuments(const TArray<FSuperEmbedderDocumentRow>& Rows, const UObject* WorldContext, FSuperEmbedderDocumentsCallback OnDone);

	/** Ranks a set's rows against Query on a worker, over a snapshot taken now. Exclude: ids to skip. */
	FSuperEmbedderTicket TopKAsync(USuperEmbedderVectorSet* Set, const FSuperEmbedderVector& Query, int32 K, const TArray<FString>& Exclude,
		const UObject* WorldContext, FSuperEmbedderTopKCallback OnDone);

	/** Encodes Text as a query, then ranks the set's rows as they are when the encode completes. Holds the set until then. */
	FSuperEmbedderTicket QueryTextTopK(USuperEmbedderVectorSet* Set, const FString& Text, int32 K, const TArray<FString>& Exclude,
		const UObject* WorldContext, FSuperEmbedderTopKCallback OnDone);

	/** Cancelled fires once, on a later tick. False for an unknown or already delivered ticket. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder")
	bool Cancel(FSuperEmbedderTicket Ticket);

	/** Documents progress: rows encoded and rows in all. False for an unknown ticket. */
	bool GetProgress(FSuperEmbedderTicket Ticket, int32& OutDone, int32& OutTotal) const;

	/**
	 * Blocks the calling thread for a whole encode. Only from a thread of your own: the game thread is refused
	 * Game Thread Blocking Refused, and the encoder's own workers Worker Thread Blocking Refused. Not exposed to
	 * Blueprint.
	 */
	ESuperEmbedderStatus EncodeTextBlocking(const FString& Text, ESuperEmbedderVectorKind Kind, FSuperEmbedderVector& OutVector, FString& OutMessage);

	// --- Cache ---

	/** Writes the encode cache to Path (relative to the project directory) on a worker. */
	UFUNCTION(BlueprintCallable, Category = "SuperEmbedder")
	void SaveEncodeCache(const FString& Path);

	UFUNCTION(BlueprintPure, Category = "SuperEmbedder")
	int32 GetEncodeCacheEntries() const;

private:
	bool Tick(float DeltaSeconds);
	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	uint64 OwnerOf(const UObject* WorldContext);
	void ReleaseHandle(uint64 Handle);
	friend class USuperEmbedderEncoderHandle;

	TSharedPtr<FSuperEmbedderSubsystemState> State;

	/** The editor's keep-alive (GIsEditor) or bOpenOnStartup's handle. */
	UPROPERTY(Transient)
	TObjectPtr<USuperEmbedderEncoderHandle> KeepAlive;
};
