#include "SuperEmbedderBake.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"
#include "Engine/DataTable.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "SuperEmbedderBlueprintLibrary.h"
#include "SuperEmbedderSubsystem.h"
#include "SuperEmbedderVectorSet.h"

#define LOCTEXT_NAMESPACE "SuperEmbedderBake"

DEFINE_LOG_CATEGORY_STATIC(LogSuperEmbedderEditor, Log, All);

namespace
{
	struct FBakeState
	{
		TArray<FSuperEmbedderDocumentRow> Rows;
		FString PackageName;
		bool bNotify = false;
		FSuperEmbedderBake::FOnFinished OnFinished;
		TStrongObjectPtr<USuperEmbedderEncoderHandle> Handle;
		FSuperEmbedderTicket Ticket;
		bool bCancelRequested = false;
		TSharedPtr<SNotificationItem> Toast;
		FTSTicker::FDelegateHandle ProgressTick;
	};

	TSharedPtr<FBakeState> GBake;

	void Finish(ESuperEmbedderStatus Status, const FString& Message)
	{
		TSharedPtr<FBakeState> Bake = GBake;
		GBake.Reset();
		if (!Bake.IsValid())
		{
			return;
		}
		if (Bake->ProgressTick.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Bake->ProgressTick);
		}
		if (Bake->Handle.IsValid())
		{
			Bake->Handle->Release();
			Bake->Handle.Reset();
		}
		const FString Line = Status == ESuperEmbedderStatus::Ok
			? FString::Printf(TEXT("SuperEmbedder bake: wrote %s (%d rows)"), *Bake->PackageName, Bake->Rows.Num())
			: FString::Printf(TEXT("SuperEmbedder bake of %s: %s %s"), *Bake->PackageName, *LexToString(Status), *Message);
		if (Status == ESuperEmbedderStatus::Ok)
		{
			UE_LOG(LogSuperEmbedderEditor, Display, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogSuperEmbedderEditor, Error, TEXT("%s"), *Line);
		}
		if (Bake->Toast.IsValid())
		{
			Bake->Toast->SetText(FText::FromString(Line));
			Bake->Toast->SetCompletionState(Status == ESuperEmbedderStatus::Ok ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			Bake->Toast->SetExpireDuration(Status == ESuperEmbedderStatus::Ok ? 4.0f : 10.0f);
			Bake->Toast->ExpireAndFadeout();
		}
		if (Bake->OnFinished)
		{
			Bake->OnFinished(Status, Message);
		}
	}

	// Writes the asset. Every row is encoded by now; the set is built apart first, so a refusal here leaves any
	// existing asset at the path unchanged.
	void WriteAsset(const FSuperEmbedderDocumentsResult& Result)
	{
		TSharedPtr<FBakeState> Bake = GBake;
		TArray<FString> Texts;
		Texts.Reserve(Bake->Rows.Num());
		for (const FSuperEmbedderDocumentRow& Row : Bake->Rows)
		{
			Texts.Add(Row.Text);
		}
		USuperEmbedderVectorSet* Built = NewObject<USuperEmbedderVectorSet>(GetTransientPackage());
		FString Message;
		ESuperEmbedderStatus Status = Built->AppendDocuments(Result, Texts, Message);
		if (Status != ESuperEmbedderStatus::Ok)
		{
			Finish(Status, Message);
			return;
		}
		TArray<uint8> Bytes;
		Built->SaveToBytes(Bytes);

		const FString& PackageName = Bake->PackageName;
		UPackage* Package = FPackageName::DoesPackageExist(PackageName)
			? LoadPackage(nullptr, *PackageName, LOAD_NoWarn | LOAD_Quiet)
			: CreatePackage(*PackageName);
		if (Package == nullptr)
		{
			Finish(ESuperEmbedderStatus::InvalidSetting, TEXT("could not create or load the package"));
			return;
		}
		Package->FullyLoad();
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		UObject* Existing = FindObject<UObject>(Package, *AssetName);
		USuperEmbedderVectorSet* Set = Cast<USuperEmbedderVectorSet>(Existing);
		if (Existing != nullptr && Set == nullptr)
		{
			Finish(ESuperEmbedderStatus::InvalidSetting, FString::Printf(TEXT("%s already holds a %s"), *PackageName, *Existing->GetClass()->GetName()));
			return;
		}
		const bool bNew = Set == nullptr;
		if (bNew)
		{
			Set = NewObject<USuperEmbedderVectorSet>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		}
		Status = Set->LoadFromBytes(Bytes, Message);
		if (Status != ESuperEmbedderStatus::Ok)
		{
			Finish(Status, Message);
			return;
		}
		if (bNew)
		{
			FAssetRegistryModule::AssetCreated(Set);
		}
		Package->MarkPackageDirty();

		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Set, *Filename, Args))
		{
			Finish(ESuperEmbedderStatus::InvalidSetting, FString::Printf(TEXT("could not save %s"), *Filename));
			return;
		}
		Finish(ESuperEmbedderStatus::Ok, FString());
	}

	void OnDocuments(const FSuperEmbedderDocumentsResult& Result)
	{
		if (!GBake.IsValid())
		{
			return;
		}
		if (Result.Outcome != ESuperEmbedderOutcome::Completed)
		{
			Finish(Result.Status, Result.Message);
			return;
		}
		WriteAsset(Result);
	}

	void BeginEncode()
	{
		USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
		if (Subsystem == nullptr)
		{
			Finish(ESuperEmbedderStatus::ShuttingDown, TEXT("no SuperEmbedder subsystem"));
			return;
		}
		// Owner 0 (no world): a map change in the editor does not cancel a bake.
		GBake->Ticket = Subsystem->EncodeDocuments(GBake->Rows, nullptr, [](const FSuperEmbedderDocumentsResult& Result) { OnDocuments(Result); });
	}

	bool OnProgressTick(float)
	{
		if (!GBake.IsValid() || !GBake->Toast.IsValid())
		{
			return true;
		}
		USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
		int32 Done = 0;
		int32 Total = 0;
		if (Subsystem != nullptr && GBake->Ticket.IsValid() && Subsystem->GetProgress(GBake->Ticket, Done, Total))
		{
			GBake->Toast->SetText(FText::Format(LOCTEXT("Progress", "Baking {0}: {1} of {2} rows"),
				FText::FromString(GBake->PackageName), FText::AsNumber(Done), FText::AsNumber(Total)));
		}
		else if (!GBake->Ticket.IsValid())
		{
			GBake->Toast->SetText(FText::Format(LOCTEXT("Opening", "Baking {0}: opening the encoder"), FText::FromString(GBake->PackageName)));
		}
		return true;
	}
}

bool FSuperEmbedderBake::Start(TArray<FSuperEmbedderDocumentRow> Rows, const FString& PackageName, bool bNotify, FOnFinished OnFinished)
{
	check(IsInGameThread());
	if (GBake.IsValid())
	{
		UE_LOG(LogSuperEmbedderEditor, Error, TEXT("SuperEmbedder bake: one is already running (%s)"), *GBake->PackageName);
		return false;
	}
	GBake = MakeShared<FBakeState>();
	GBake->Rows = MoveTemp(Rows);
	GBake->PackageName = PackageName;
	GBake->bNotify = bNotify;
	GBake->OnFinished = MoveTemp(OnFinished);

	FText Reason;
	if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
	{
		Finish(ESuperEmbedderStatus::InvalidSetting, FString::Printf(TEXT("not a package name: %s"), *Reason.ToString()));
		return false;
	}
	// The cheap rules first, on the game thread; the token bound is checked by the documents ticket, on a worker.
	FString Message;
	const ESuperEmbedderStatus Checked = USuperEmbedderBlueprintLibrary::CheckRows(GBake->Rows, Message);
	if (Checked != ESuperEmbedderStatus::Ok)
	{
		Finish(Checked, Message);
		return false;
	}
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		Finish(ESuperEmbedderStatus::ShuttingDown, TEXT("no SuperEmbedder subsystem"));
		return false;
	}

	if (bNotify)
	{
		FNotificationInfo Info(FText::Format(LOCTEXT("Starting", "Baking {0}"), FText::FromString(PackageName)));
		Info.bFireAndForget = false;
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("Cancel", "Cancel"), LOCTEXT("CancelTip", "Cancel the bake; nothing is written."),
			FSimpleDelegate::CreateStatic(&FSuperEmbedderBake::Cancel), SNotificationItem::CS_Pending));
		GBake->Toast = FSlateNotificationManager::Get().AddNotification(Info);
		if (GBake->Toast.IsValid())
		{
			GBake->Toast->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
	GBake->ProgressTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&OnProgressTick), 0.25f);

	// A handle for the bake's whole length. When the encoder is already Ready this joins it (same files) or, if
	// another model is open, uses that one.
	if (Subsystem->IsEncoderReady())
	{
		BeginEncode();
		return true;
	}
	USuperEmbedderEncoderHandle* Handle = Subsystem->OpenEncoder(FString(), FString(), [](const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle*)
	{
		if (!GBake.IsValid())
		{
			return;
		}
		if (GBake->bCancelRequested)
		{
			Finish(ESuperEmbedderStatus::Cancelled, TEXT("cancelled while the encoder opened"));
			return;
		}
		if (Result.Status != ESuperEmbedderStatus::Ok || Result.State != ESuperEmbedderEncoderState::Ready)
		{
			Finish(Result.Status == ESuperEmbedderStatus::Ok ? ESuperEmbedderStatus::EncoderNotReady : Result.Status, Result.Message);
			return;
		}
		BeginEncode();
	});
	if (GBake.IsValid())
	{
		GBake->Handle.Reset(Handle);
	}
	return true;
}

bool FSuperEmbedderBake::StartFromJsonlFile(const FString& JsonlPath, const FString& PackageName, bool bNotify, FOnFinished OnFinished)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *JsonlPath))
	{
		UE_LOG(LogSuperEmbedderEditor, Error, TEXT("SuperEmbedder bake: cannot read %s"), *JsonlPath);
		if (OnFinished)
		{
			OnFinished(ESuperEmbedderStatus::EmptyInput, FString::Printf(TEXT("cannot read %s"), *JsonlPath));
		}
		return false;
	}
	TArray<FSuperEmbedderDocumentRow> Rows;
	FString Message;
	const ESuperEmbedderStatus Status = USuperEmbedderBlueprintLibrary::ParseJsonlRows(Bytes, Rows, Message);
	if (Status != ESuperEmbedderStatus::Ok)
	{
		UE_LOG(LogSuperEmbedderEditor, Error, TEXT("SuperEmbedder bake: %s: %s %s"), *JsonlPath, *LexToString(Status), *Message);
		if (OnFinished)
		{
			OnFinished(Status, Message);
		}
		return false;
	}
	return Start(MoveTemp(Rows), PackageName, bNotify, MoveTemp(OnFinished));
}

bool FSuperEmbedderBake::StartFromDataTable(const UDataTable* Table, const FString& PackageName, bool bNotify, FOnFinished OnFinished)
{
	TArray<FSuperEmbedderDocumentRow> Rows;
	FString Message;
	const ESuperEmbedderStatus Status = USuperEmbedderBlueprintLibrary::RowsFromDataTable(Table, Rows, Message);
	if (Status != ESuperEmbedderStatus::Ok)
	{
		UE_LOG(LogSuperEmbedderEditor, Error, TEXT("SuperEmbedder bake: %s: %s %s"), *GetPathNameSafe(Table), *LexToString(Status), *Message);
		if (OnFinished)
		{
			OnFinished(Status, Message);
		}
		return false;
	}
	return Start(MoveTemp(Rows), PackageName, bNotify, MoveTemp(OnFinished));
}

bool FSuperEmbedderBake::IsRunning()
{
	return GBake.IsValid();
}

void FSuperEmbedderBake::Cancel()
{
	if (!GBake.IsValid())
	{
		return;
	}
	GBake->bCancelRequested = true;
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (GBake->Ticket.IsValid() && Subsystem != nullptr)
	{
		// Cancelled arrives through OnDocuments on a later tick; the documents ticket stops between rows.
		Subsystem->Cancel(GBake->Ticket);
	}
	// While opening, the open callback finishes the bake as cancelled (an open cannot be cancelled).
}

// --- Validate ---

int32 FSuperEmbedderValidate::ValidateAllSets(TArray<FString>& OutReport)
{
	OutReport.Reset();
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	const FSuperEmbedderOpenResult Open = Subsystem != nullptr ? Subsystem->GetOpenResult() : FSuperEmbedderOpenResult();
	if (Subsystem == nullptr || Open.State != ESuperEmbedderEncoderState::Ready)
	{
		OutReport.Add(TEXT("the encoder is not Ready; open it first"));
		return -1;
	}
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	Registry.SearchAllAssets(/*bSynchronousSearch*/ true);
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(USuperEmbedderVectorSet::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses*/ true);
	int32 Listed = 0;
	for (const FAssetData& Asset : Assets)
	{
		const USuperEmbedderVectorSet* Set = Cast<USuperEmbedderVectorSet>(Asset.GetAsset());
		const FString Name = Asset.GetObjectPathString();
		if (Set == nullptr)
		{
			OutReport.Add(FString::Printf(TEXT("LISTED %s: did not load"), *Name));
			++Listed;
		}
		else if (Set->GetLoadStatus() != ESuperEmbedderStatus::Ok)
		{
			OutReport.Add(FString::Printf(TEXT("LISTED %s: %s"), *Name, *LexToString(Set->GetLoadStatus())));
			++Listed;
		}
		else if (Set->Num() > 0 && Set->GetFingerprintHex() != Open.FingerprintHex)
		{
			OutReport.Add(FString::Printf(TEXT("LISTED %s: FingerprintMismatch (set %s, encoder %s)"), *Name, *Set->GetFingerprintHex(), *Open.FingerprintHex));
			++Listed;
		}
		else
		{
			OutReport.Add(FString::Printf(TEXT("ok %s (%d rows)"), *Name, Set->Num()));
		}
	}
	return Listed;
}

#undef LOCTEXT_NAMESPACE
