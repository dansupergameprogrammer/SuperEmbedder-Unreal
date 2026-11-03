#include "SuperEmbedderAsyncActions.h"

#include "Containers/Ticker.h"

#include "SuperEmbedderSubsystem.h"
#include "SuperEmbedderVectorSet.h"

// Game thread throughout. Each node registers with the game instance (or roots itself when there is none),
// issues one request through the subsystem, and ends on the single delivery the subsystem makes for it.

namespace
{
	template <typename TResult>
	TResult RefusedResult(ESuperEmbedderStatus Status, const FString& Message)
	{
		TResult R;
		R.Outcome = ESuperEmbedderOutcome::Refused;
		R.Status = Status;
		R.Message = Message;
		return R;
	}

	const TCHAR* NoSubsystem = TEXT("no SuperEmbedder subsystem (is the plugin enabled, and the engine running?)");
}

// --- Base ---

void USuperEmbedderAsyncActionBase::Register(const UObject* InWorldContext)
{
	WorldContext = InWorldContext;
	RegisterWithGameInstance(InWorldContext);
	if (!IsRegistered())
	{
		AddToRoot();
		bRooted = true;
	}
}

bool USuperEmbedderAsyncActionBase::BeginFinish()
{
	if (bFinished)
	{
		return false;
	}
	bFinished = true;
	return true;
}

void USuperEmbedderAsyncActionBase::NextTick(TFunction<void()> Body)
{
	TWeakObjectPtr<USuperEmbedderAsyncActionBase> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, Body = MoveTemp(Body)](float) -> bool
	{
		if (WeakThis.IsValid())
		{
			Body();
		}
		return false; // once
	}));
}

void USuperEmbedderAsyncActionBase::Cancel()
{
	if (bFinished)
	{
		return;
	}
	// The subsystem delivers Cancelled for the ticket on a later tick, which ends the node through its pin.
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Ticket.IsValid() && Subsystem != nullptr && Subsystem->Cancel(Ticket))
	{
		return;
	}
	// Nothing in flight (not activated yet, or the subsystem is gone): end without a pin.
	bFinished = true;
	SetReadyToDestroy();
}

bool USuperEmbedderAsyncActionBase::ShouldBroadcastDelegates() const
{
	return !bFinished && (IsRegistered() || bRooted);
}

void USuperEmbedderAsyncActionBase::SetReadyToDestroy()
{
	Super::SetReadyToDestroy();
	if (bRooted)
	{
		bRooted = false;
		RemoveFromRoot();
	}
}

// --- Open ---

USuperEmbedderOpenEncoderAction* USuperEmbedderOpenEncoderAction::OpenSuperEmbedderEncoder(UObject* WorldContextObject, FString InModelFile, FString InTokenizerFile)
{
	USuperEmbedderOpenEncoderAction* Action = NewObject<USuperEmbedderOpenEncoderAction>();
	Action->ModelFile = InModelFile;
	Action->TokenizerFile = InTokenizerFile;
	Action->Register(WorldContextObject);
	return Action;
}

void USuperEmbedderOpenEncoderAction::Activate()
{
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		NextTick([this]()
		{
			if (BeginFinish())
			{
				FSuperEmbedderOpenResult R;
				R.Status = ESuperEmbedderStatus::ShuttingDown;
				R.Message = NoSubsystem;
				Failed.Broadcast(R, nullptr);
				SetReadyToDestroy();
			}
		});
		return;
	}
	TWeakObjectPtr<USuperEmbedderOpenEncoderAction> WeakThis(this);
	Handle = Subsystem->OpenEncoder(ModelFile, TokenizerFile, [WeakThis](const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle* InHandle)
	{
		USuperEmbedderOpenEncoderAction* Self = WeakThis.Get();
		if (Self == nullptr || !Self->BeginFinish())
		{
			return;
		}
		if (Result.Status == ESuperEmbedderStatus::Ok && Result.State == ESuperEmbedderEncoderState::Ready && InHandle != nullptr)
		{
			Self->Opened.Broadcast(Result, InHandle);
		}
		else
		{
			Self->Failed.Broadcast(Result, nullptr);
		}
		Self->Handle = nullptr;
		Self->SetReadyToDestroy();
	});
}

void USuperEmbedderOpenEncoderAction::Cancel()
{
	// An open cannot be cancelled (the library has no cancel); this only gives up this node's handle, so the
	// encoder closes after the open if nothing else holds it. No pin fires.
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	if (Handle != nullptr)
	{
		Handle->Release();
		Handle = nullptr;
	}
	SetReadyToDestroy();
}

// --- Encode Text ---

USuperEmbedderEncodeTextAction* USuperEmbedderEncodeTextAction::EncodeTextAsync(UObject* WorldContextObject, FString InText)
{
	USuperEmbedderEncodeTextAction* Action = NewObject<USuperEmbedderEncodeTextAction>();
	Action->Text = InText;
	Action->Register(WorldContextObject);
	return Action;
}

void USuperEmbedderEncodeTextAction::Activate()
{
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		NextTick([this]() { OnDone(RefusedResult<FSuperEmbedderEncodeResult>(ESuperEmbedderStatus::ShuttingDown, NoSubsystem)); });
		return;
	}
	TWeakObjectPtr<USuperEmbedderEncodeTextAction> WeakThis(this);
	Ticket = Subsystem->EncodeText(Text, WorldContext.Get(), [WeakThis](const FSuperEmbedderEncodeResult& Result)
	{
		if (USuperEmbedderEncodeTextAction* Self = WeakThis.Get())
		{
			Self->OnDone(Result);
		}
	});
}

void USuperEmbedderEncodeTextAction::OnDone(const FSuperEmbedderEncodeResult& Result)
{
	if (!BeginFinish())
	{
		return;
	}
	switch (Result.Outcome)
	{
	case ESuperEmbedderOutcome::Completed: Completed.Broadcast(Result); break;
	case ESuperEmbedderOutcome::Cancelled: Cancelled.Broadcast(Result); break;
	default: Refused.Broadcast(Result); break;
	}
	SetReadyToDestroy();
}

// --- Encode Documents ---

USuperEmbedderEncodeDocumentsAction* USuperEmbedderEncodeDocumentsAction::EncodeDocumentsAsync(UObject* WorldContextObject, const TArray<FSuperEmbedderDocumentRow>& InRows)
{
	USuperEmbedderEncodeDocumentsAction* Action = NewObject<USuperEmbedderEncodeDocumentsAction>();
	Action->Rows = InRows;
	Action->Register(WorldContextObject);
	return Action;
}

void USuperEmbedderEncodeDocumentsAction::Activate()
{
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		NextTick([this]() { OnDone(RefusedResult<FSuperEmbedderDocumentsResult>(ESuperEmbedderStatus::ShuttingDown, NoSubsystem)); });
		return;
	}
	TWeakObjectPtr<USuperEmbedderEncodeDocumentsAction> WeakThis(this);
	Ticket = Subsystem->EncodeDocuments(Rows, WorldContext.Get(), [WeakThis](const FSuperEmbedderDocumentsResult& Result)
	{
		if (USuperEmbedderEncodeDocumentsAction* Self = WeakThis.Get())
		{
			Self->OnDone(Result);
		}
	});
	Rows.Empty(); // the subsystem holds its own UTF-8 copy
}

void USuperEmbedderEncodeDocumentsAction::OnDone(const FSuperEmbedderDocumentsResult& Result)
{
	if (!BeginFinish())
	{
		return;
	}
	switch (Result.Outcome)
	{
	case ESuperEmbedderOutcome::Completed: Completed.Broadcast(Result); break;
	case ESuperEmbedderOutcome::Cancelled: Cancelled.Broadcast(Result); break;
	default: Refused.Broadcast(Result); break;
	}
	SetReadyToDestroy();
}

// --- Top K ---

USuperEmbedderTopKAction* USuperEmbedderTopKAction::TopKAsync(UObject* WorldContextObject, USuperEmbedderVectorSet* InSet, const FSuperEmbedderVector& InQuery, int32 InK, const TArray<FString>& InExcludeIds)
{
	USuperEmbedderTopKAction* Action = NewObject<USuperEmbedderTopKAction>();
	Action->Set = InSet;
	Action->Query = InQuery;
	Action->K = InK;
	Action->ExcludeIds = InExcludeIds;
	Action->Register(WorldContextObject);
	return Action;
}

void USuperEmbedderTopKAction::Activate()
{
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		NextTick([this]() { OnDone(RefusedResult<FSuperEmbedderTopKResult>(ESuperEmbedderStatus::ShuttingDown, NoSubsystem)); });
		return;
	}
	TWeakObjectPtr<USuperEmbedderTopKAction> WeakThis(this);
	Ticket = Subsystem->TopKAsync(Set, Query, K, ExcludeIds, WorldContext.Get(), [WeakThis](const FSuperEmbedderTopKResult& Result)
	{
		if (USuperEmbedderTopKAction* Self = WeakThis.Get())
		{
			Self->OnDone(Result);
		}
	});
	Set = nullptr; // the ranking reads its own snapshot
}

void USuperEmbedderTopKAction::OnDone(const FSuperEmbedderTopKResult& Result)
{
	if (!BeginFinish())
	{
		return;
	}
	switch (Result.Outcome)
	{
	case ESuperEmbedderOutcome::Completed: Completed.Broadcast(Result); break;
	case ESuperEmbedderOutcome::Cancelled: Cancelled.Broadcast(Result); break;
	default: Refused.Broadcast(Result); break;
	}
	SetReadyToDestroy();
}

// --- Query Text Top K ---

USuperEmbedderQueryTextTopKAction* USuperEmbedderQueryTextTopKAction::QueryTextTopKAsync(UObject* WorldContextObject, USuperEmbedderVectorSet* InSet, FString InText, int32 InK, const TArray<FString>& InExcludeIds)
{
	USuperEmbedderQueryTextTopKAction* Action = NewObject<USuperEmbedderQueryTextTopKAction>();
	Action->Set = InSet;
	Action->Text = InText;
	Action->K = InK;
	Action->ExcludeIds = InExcludeIds;
	Action->Register(WorldContextObject);
	return Action;
}

void USuperEmbedderQueryTextTopKAction::Activate()
{
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		NextTick([this]() { OnDone(RefusedResult<FSuperEmbedderTopKResult>(ESuperEmbedderStatus::ShuttingDown, NoSubsystem)); });
		return;
	}
	TWeakObjectPtr<USuperEmbedderQueryTextTopKAction> WeakThis(this);
	// The subsystem holds the set strongly until the encode completes (V-3).
	Ticket = Subsystem->QueryTextTopK(Set, Text, K, ExcludeIds, WorldContext.Get(), [WeakThis](const FSuperEmbedderTopKResult& Result)
	{
		if (USuperEmbedderQueryTextTopKAction* Self = WeakThis.Get())
		{
			Self->OnDone(Result);
		}
	});
	Set = nullptr;
}

void USuperEmbedderQueryTextTopKAction::OnDone(const FSuperEmbedderTopKResult& Result)
{
	if (!BeginFinish())
	{
		return;
	}
	switch (Result.Outcome)
	{
	case ESuperEmbedderOutcome::Completed: Completed.Broadcast(Result); break;
	case ESuperEmbedderOutcome::Cancelled: Cancelled.Broadcast(Result); break;
	default: Refused.Broadcast(Result); break;
	}
	SetReadyToDestroy();
}
