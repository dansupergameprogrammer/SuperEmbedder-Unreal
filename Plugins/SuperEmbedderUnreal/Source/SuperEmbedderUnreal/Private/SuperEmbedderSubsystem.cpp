#include "SuperEmbedderSubsystem.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"

#include "SuperEmbedderBlueprintLibrary.h"
#include "SuperEmbedderConvert.h"
#include "SuperEmbedderSettings.h"
#include "SuperEmbedderVectorSet.h"

#include "SuperEmbedderEncodeService.h"
#include "SuperEmbedderSimilarity.h"
#include "SuperEmbedderVectorSetCore.h"

#include <atomic>
#include <unordered_set>

// Game thread throughout, except the bodies handed to UE::Tasks (a ranking, a cache file read or write), which
// touch only the immutable data they were given, and the service's own workers (FUeWorkerThreads).

namespace
{
	namespace SE = SuperEmbedderUe;
	using namespace SuperEmbedderUeConvert;

	// Ranking tickets live in their own range, so one Cancel() takes either kind. Encode tickets are the
	// service's own (counting from 1) and never reach this bit.
	constexpr uint64 kTopKTicketBit = uint64(1) << 62;

	FAutoConsoleCommand GCloseEncoderCommand(
		TEXT("SuperEmbedder.CloseEncoder"),
		TEXT("Releases the editor's keep-alive handle on the SuperEmbedder encoder; it closes once no other handle holds it."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get())
			{
				Subsystem->ReleaseKeepAlive();
			}
		}));

#if !UE_BUILD_SHIPPING
	// Development checks (editor and non-Shipping builds), one console command per lane:
	//   SuperEmbedder.EncodeText <text>              the query lane
	//   SuperEmbedder.EncodeDocuments <rows.jsonl>   the documents lane ({"id","text"} per line, as the bake reads)
	//   SuperEmbedder.DumpSet </Game/Path/Set>       a vector set asset as loaded (cooked, in a packaged game)
	// Each logs what it read or encoded and every vector's digest (SHA-256 of the float32 values, little-endian: the
	// digest SuperEmbedder's own tools print). An encode opens the encoder from Project Settings when none is Ready,
	// holding a handle until its result is delivered; the handle is held by the callbacks themselves (a shared
	// pointer they capture), never by a static, so nothing outlives the subsystem.
	using FDevHold = TSharedPtr<TStrongObjectPtr<USuperEmbedderEncoderHandle>>;

	void DevRelease(const FDevHold& Hold)
	{
		if (Hold.IsValid() && Hold->IsValid())
		{
			(*Hold)->Release();
			Hold->Reset();
		}
	}

	FString DevOutcome(ESuperEmbedderOutcome Outcome)
	{
		return StaticEnum<ESuperEmbedderOutcome>()->GetNameStringByValue(static_cast<int64>(Outcome));
	}

	/** Runs Body with an encoder Ready: at once if one is, otherwise once one opens from Project Settings. */
	void DevWithEncoder(const FString& Command, TFunction<void(USuperEmbedderSubsystem*, FDevHold)> Body)
	{
		USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
		if (Subsystem == nullptr)
		{
			UE_LOG(LogSuperEmbedder, Error, TEXT("%s: no SuperEmbedder subsystem"), *Command);
			return;
		}
		if (Subsystem->IsEncoderReady())
		{
			Body(Subsystem, nullptr);
			return;
		}
		FDevHold Hold = MakeShared<TStrongObjectPtr<USuperEmbedderEncoderHandle>>();
		USuperEmbedderEncoderHandle* Handle = Subsystem->OpenEncoder(FString(), FString(),
			[Command, Hold, Body](const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle*)
			{
				USuperEmbedderSubsystem* S = USuperEmbedderSubsystem::Get();
				if (S == nullptr || Result.Status != ESuperEmbedderStatus::Ok || Result.State != ESuperEmbedderEncoderState::Ready)
				{
					UE_LOG(LogSuperEmbedder, Error, TEXT("%s: the encoder did not open: %s %s"), *Command,
						*USuperEmbedderBlueprintLibrary::StatusToString(Result.Status), *Result.Message);
					DevRelease(Hold);
					return;
				}
				Body(S, Hold);
			});
		// The open's callback is delivered on a later tick, never inside OpenEncoder, so the hold is set first.
		Hold->Reset(Handle);
	}

	FAutoConsoleCommand GEncodeTextCommand(
		TEXT("SuperEmbedder.EncodeText"),
		TEXT("SuperEmbedder.EncodeText <text>: encodes the text as a query (opening the encoder from Project Settings if none is Ready) and logs the outcome, dims and vector digest. Development builds only."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Text = FString::Join(Args, TEXT(" "));
			if (Text.IsEmpty())
			{
				UE_LOG(LogSuperEmbedder, Error, TEXT("SuperEmbedder.EncodeText: usage: SuperEmbedder.EncodeText <text>"));
				return;
			}
			DevWithEncoder(TEXT("SuperEmbedder.EncodeText"), [Text](USuperEmbedderSubsystem* S, FDevHold Hold)
			{
				S->EncodeText(Text, nullptr, [Text, Hold](const FSuperEmbedderEncodeResult& Result)
				{
					UE_LOG(LogSuperEmbedder, Display,
						TEXT("SuperEmbedder.EncodeText \"%s\": %s status=%s dims=%d digest=%s fingerprint=%s cache=%d%s%s"), *Text,
						*DevOutcome(Result.Outcome), *USuperEmbedderBlueprintLibrary::StatusToString(Result.Status),
						Result.Vector.Values.Num(), *Result.Vector.GetDigestHex(), *Result.Vector.GetFingerprintHex(),
						Result.bFromCache ? 1 : 0, Result.Message.IsEmpty() ? TEXT("") : TEXT(" message="), *Result.Message);
					DevRelease(Hold);
				});
			});
		}));

	FAutoConsoleCommand GEncodeDocumentsCommand(
		TEXT("SuperEmbedder.EncodeDocuments"),
		TEXT("SuperEmbedder.EncodeDocuments <rows.jsonl>: encodes the file's rows ({\"id\", \"text\"} per line) through the documents lane and logs each row's id and vector digest. Development builds only."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Path = FString::Join(Args, TEXT(" "));
			TArray<uint8> Bytes;
			if (Path.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *Path))
			{
				UE_LOG(LogSuperEmbedder, Error, TEXT("SuperEmbedder.EncodeDocuments: %s"),
					Path.IsEmpty() ? TEXT("usage: SuperEmbedder.EncodeDocuments <rows.jsonl>") : *FString::Printf(TEXT("cannot read %s"), *Path));
				return;
			}
			TArray<FSuperEmbedderDocumentRow> Rows;
			FString Message;
			const ESuperEmbedderStatus Parsed = USuperEmbedderBlueprintLibrary::ParseJsonlRows(Bytes, Rows, Message);
			if (Parsed != ESuperEmbedderStatus::Ok)
			{
				UE_LOG(LogSuperEmbedder, Error, TEXT("SuperEmbedder.EncodeDocuments: %s: %s %s"), *Path,
					*USuperEmbedderBlueprintLibrary::StatusToString(Parsed), *Message);
				return;
			}
			DevWithEncoder(TEXT("SuperEmbedder.EncodeDocuments"), [Rows, Path](USuperEmbedderSubsystem* S, FDevHold Hold)
			{
				S->EncodeDocuments(Rows, nullptr, [Path, Hold](const FSuperEmbedderDocumentsResult& Result)
				{
					UE_LOG(LogSuperEmbedder, Display, TEXT("SuperEmbedder.EncodeDocuments %s: %s status=%s rows=%d%s%s"), *Path,
						*DevOutcome(Result.Outcome), *USuperEmbedderBlueprintLibrary::StatusToString(Result.Status),
						Result.Vectors.Num(), Result.Message.IsEmpty() ? TEXT("") : TEXT(" message="), *Result.Message);
					for (int32 I = 0; I < Result.Vectors.Num(); ++I)
					{
						UE_LOG(LogSuperEmbedder, Display, TEXT("SuperEmbedder.EncodeDocuments row %d id=%s dims=%d digest=%s fingerprint=%s"), I,
							Result.Ids.IsValidIndex(I) ? *Result.Ids[I] : TEXT("?"), Result.Vectors[I].Values.Num(),
							*Result.Vectors[I].GetDigestHex(), *Result.Vectors[I].GetFingerprintHex());
					}
					DevRelease(Hold);
				});
			});
		}));

	FAutoConsoleCommand GDumpSetCommand(
		TEXT("SuperEmbedder.DumpSet"),
		TEXT("SuperEmbedder.DumpSet </Game/Path/SetName>: loads the vector set asset and logs its load status, rows, dims, fingerprint and each row's vector digest. Development builds only."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Path = FString::Join(Args, TEXT(" "));
			USuperEmbedderVectorSet* Set = Path.IsEmpty() ? nullptr : LoadObject<USuperEmbedderVectorSet>(nullptr, *Path);
			if (Set == nullptr)
			{
				UE_LOG(LogSuperEmbedder, Error, TEXT("SuperEmbedder.DumpSet: %s"),
					Path.IsEmpty() ? TEXT("usage: SuperEmbedder.DumpSet </Game/Path/SetName>") : *FString::Printf(TEXT("cannot load %s"), *Path));
				return;
			}
			UE_LOG(LogSuperEmbedder, Display, TEXT("SuperEmbedder.DumpSet %s: load=%s rows=%d dims=%d fingerprint=%s"), *Path,
				*USuperEmbedderBlueprintLibrary::StatusToString(Set->GetLoadStatus()), Set->Num(), Set->GetDims(), *Set->GetFingerprintHex());
			for (const FString& Id : Set->GetIds())
			{
				FSuperEmbedderVector V;
				const ESuperEmbedderStatus S = Set->GetRowVector(Id, V);
				UE_LOG(LogSuperEmbedder, Display, TEXT("SuperEmbedder.DumpSet row id=%s status=%s digest=%s"), *Id,
					*USuperEmbedderBlueprintLibrary::StatusToString(S), *V.GetDigestHex());
			}
		}));
#endif

	// What a ranking reads and writes off the game thread. Holds no UObject.
	struct FRankWork
	{
		SE::FSetSnapshot Snapshot;
		SE::FEmbedding Query;
		int64 K = 0;
		std::unordered_set<std::string> Exclude;
		SE::EStatus Status = SE::EStatus::Ok;
		std::vector<SE::FHit> Hits;
		std::atomic<bool> bDone{false};
	};

	FSuperEmbedderTopKResult MakeTopKResult(ESuperEmbedderOutcome Outcome, ESuperEmbedderStatus Status, const FString& Message)
	{
		FSuperEmbedderTopKResult R;
		R.Outcome = Outcome;
		R.Status = Status;
		R.Message = Message;
		return R;
	}
}

struct FSuperEmbedderSubsystemState
{
	std::shared_ptr<SE::FEncodeService> Service;
	SE::EStatus StartStatus = SE::EStatus::Ok;
	FString StartDetail;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle WorldCleanupHandle;
	ESuperEmbedderEncoderState LastState = ESuperEmbedderEncoderState::Closed;

	TMap<const UWorld*, uint64> Owners;
	uint64 NextOwner = 1;

	// An encode in flight, by the service's ticket.
	struct FPendingEncode
	{
		uint64 Owner = 0;
		FSuperEmbedderEncodeCallback OnQuery;
		FSuperEmbedderDocumentsCallback OnDocuments;
		uint64 TopKTicket = 0; // the Query Text Top K this encode belongs to
	};
	TMap<uint64, FPendingEncode> Encodes;

	// A ranking (Top K Async) or a Query Text Top K, by its own ticket.
	struct FTopKJob
	{
		uint64 Owner = 0;
		FSuperEmbedderTopKCallback OnDone;
		uint64 EncodeTicket = 0;                     // Query Text Top K while it encodes
		TStrongObjectPtr<USuperEmbedderVectorSet> Set; // held while it encodes (V-3)
		int64 K = 0;
		std::unordered_set<std::string> Exclude;
		TSharedPtr<FRankWork, ESPMode::ThreadSafe> Work; // set once the ranking starts
		FSuperEmbedderVector Query;
		bool bHasImmediate = false; // a refusal or a cancel, delivered on the next tick
		FSuperEmbedderTopKResult Immediate;
	};
	TMap<uint64, TSharedPtr<FTopKJob>> TopK;
	uint64 NextTopK = 1;

	struct FPendingOpen
	{
		FSuperEmbedderOpenCallback OnDone;
		TStrongObjectPtr<USuperEmbedderEncoderHandle> Handle;
		bool bImmediate = false;
		FSuperEmbedderOpenResult Immediate;
	};
	TArray<FPendingOpen> Opens;

	TArray<UE::Tasks::FTask> Tasks;
	FString KeepAliveModelKey;

	void WaitForTasks()
	{
		for (UE::Tasks::FTask& Task : Tasks)
		{
			Task.Wait();
		}
		Tasks.Reset();
	}
};

// --- Handle ---

void USuperEmbedderEncoderHandle::Release()
{
	if (Handle != 0)
	{
		const uint64 Released = Handle;
		Handle = 0;
		if (USuperEmbedderSubsystem* Subsystem = Owner.Get())
		{
			Subsystem->ReleaseHandle(Released);
		}
	}
}

void USuperEmbedderEncoderHandle::BeginDestroy()
{
	Release();
	Super::BeginDestroy();
}

// --- Subsystem ---

USuperEmbedderSubsystem* USuperEmbedderSubsystem::Get()
{
	return GEngine != nullptr ? GEngine->GetEngineSubsystem<USuperEmbedderSubsystem>() : nullptr;
}

void USuperEmbedderSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	State = MakeShared<FSuperEmbedderSubsystemState>();

	const USuperEmbedderSettings* Settings = GetDefault<USuperEmbedderSettings>();
	SE::FServiceConfig Config;
	Config.MaxConcurrency = static_cast<uint32_t>(FMath::Max(0, Settings->MaxConcurrency));
	Config.MaxQueryTokens = static_cast<uint32_t>(FMath::Max(0, Settings->MaxQueryTokens));
	Config.MaxDocumentTokens = static_cast<uint32_t>(FMath::Max(0, Settings->MaxDocumentTokens));
	Config.MaxTextBytes = static_cast<uint32_t>(FMath::Max(0, Settings->MaxTextBytes));
	Config.MaxPendingQueries = static_cast<uint32_t>(FMath::Max(0, Settings->MaxPendingQueries));
	Config.MaxPendingDocumentTickets = static_cast<uint32_t>(FMath::Max(0, Settings->MaxPendingDocumentTickets));
	Config.MaxCacheEntries = static_cast<uint32_t>(FMath::Max(0, Settings->MaxCacheEntries));

	std::shared_ptr<const SE::IFileReader> Files = std::make_shared<FUeFileReader>();
	State->Service = std::make_shared<SE::FEncodeService>(Config,
		std::make_shared<SE::FSembEncoderFactory>(Files, /*bReadNonAsciiPaths*/ true),
		std::make_shared<FUeWorkerThreads>(),
		[]() { return IsInGameThread(); });
	std::string Detail;
	State->StartStatus = State->Service->Start(Detail);
	State->StartDetail = FromUtf8(Detail);
	if (State->StartStatus != SE::EStatus::Ok)
	{
		UE_LOG(LogSuperEmbedder, Error, TEXT("SuperEmbedder: the encoder service did not start: %s %s (Project Settings > Plugins > SuperEmbedder)"),
			*LexToString(ToUe(State->StartStatus)), *State->StartDetail);
	}

	State->TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USuperEmbedderSubsystem::Tick), 0.0f);
	State->WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &USuperEmbedderSubsystem::OnWorldCleanup);

	// The cooked cache loads off the game thread; the open's fingerprint clears it if it was baked for another encoder.
	if (!Settings->CacheFile.IsEmpty() && Config.MaxCacheEntries > 0 && State->StartStatus == SE::EStatus::Ok)
	{
		const FString Path = USuperEmbedderSettings::ResolvePath(Settings->CacheFile);
		std::shared_ptr<SE::FEncodeService> Service = State->Service;
		State->Tasks.Add(UE::Tasks::Launch(TEXT("SuperEmbedder.LoadCache"), [Service, Path]()
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
			{
				UE_LOG(LogSuperEmbedder, Warning, TEXT("SuperEmbedder: no encode cache at %s; running with a cold cache"), *Path);
				return;
			}
			std::string LoadDetail;
			bool bCleared = false;
			const SE::EStatus Status = Service->GetCache().Load(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), LoadDetail, bCleared);
			if (Status != SE::EStatus::Ok)
			{
				UE_LOG(LogSuperEmbedder, Warning, TEXT("SuperEmbedder: encode cache %s refused (%s %s); running with a cold cache"), *Path,
					*LexToString(ToUe(Status)), *FromUtf8(LoadDetail));
			}
		}));
	}

	if (Settings->bOpenOnStartup)
	{
		// In the editor OpenEncoder() has already taken the keep-alive; this handle is then dropped and released.
		USuperEmbedderEncoderHandle* Handle = OpenEncoder(FString(), FString(), nullptr);
		if (KeepAlive == nullptr)
		{
			KeepAlive = Handle;
		}
	}
}

void USuperEmbedderSubsystem::Deinitialize()
{
	if (State.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(State->TickHandle);
		FWorldDelegates::OnWorldCleanup.Remove(State->WorldCleanupHandle);
		// Shutdown delivers nothing: it cancels what is queued, waits for the encodes already running (one row
		// or one query per worker), joins the workers and closes the library handle.
		State->Service->Shutdown();
		State->WaitForTasks();
		State->Encodes.Reset();
		State->Opens.Reset();
		State->TopK.Reset();
	}
	if (KeepAlive != nullptr)
	{
		KeepAlive->Handle = 0;
		KeepAlive = nullptr;
	}
	State.Reset();
	Super::Deinitialize();
}

uint64 USuperEmbedderSubsystem::OwnerOf(const UObject* WorldContext)
{
	if (WorldContext == nullptr || GEngine == nullptr)
	{
		return 0;
	}
	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	if (World == nullptr)
	{
		return 0;
	}
	if (const uint64* Found = State->Owners.Find(World))
	{
		return *Found;
	}
	const uint64 Owner = State->NextOwner++;
	State->Owners.Add(World, Owner);
	return Owner;
}

// --- Encoder ---

USuperEmbedderEncoderHandle* USuperEmbedderSubsystem::OpenEncoder(const FString& ModelFile, const FString& TokenizerFile, FSuperEmbedderOpenCallback OnDone)
{
	check(IsInGameThread());
	FSuperEmbedderSubsystemState::FPendingOpen Pending;
	Pending.OnDone = MoveTemp(OnDone);
	if (!State.IsValid())
	{
		return nullptr;
	}

	const USuperEmbedderSettings* Settings = GetDefault<USuperEmbedderSettings>();
	const bool bFromSettings = ModelFile.IsEmpty() && TokenizerFile.IsEmpty();
	SE::FModelSource Source;
	Source.ModelPath = ToUtf8Lossy(USuperEmbedderSettings::ResolvePath(bFromSettings ? Settings->ModelFile : ModelFile));
	Source.TokenizerPath = ToUtf8Lossy(USuperEmbedderSettings::ResolvePath(bFromSettings ? Settings->TokenizerFile : TokenizerFile));
	if (bFromSettings)
	{
		Source.ExpectedModelSha256 = ToUtf8Lossy(Settings->ModelSha256.TrimStartAndEnd().ToLower());
		Source.ExpectedTokenizerSha256 = ToUtf8Lossy(Settings->TokenizerSha256.TrimStartAndEnd().ToLower());
	}

	uint64_t Handle = 0;
	SE::EStatus Status = State->StartStatus;
	if (Status == SE::EStatus::Ok)
	{
		Status = State->Service->Open(Source, Handle);
	}
	USuperEmbedderEncoderHandle* Object = nullptr;
	if (Status == SE::EStatus::Ok)
	{
		Object = NewObject<USuperEmbedderEncoderHandle>(this);
		Object->Handle = Handle;
		Object->Owner = this;
		Pending.Handle.Reset(Object);

		// The editor keeps the encoder open across play-in-editor sessions (section 2.1).
		if (GIsEditor && KeepAlive == nullptr)
		{
			uint64_t KeepHandle = 0;
			if (State->Service->Open(Source, KeepHandle) == SE::EStatus::Ok)
			{
				KeepAlive = NewObject<USuperEmbedderEncoderHandle>(this);
				KeepAlive->Handle = KeepHandle;
				KeepAlive->Owner = this;
			}
		}
	}
	else
	{
		Pending.bImmediate = true;
		Pending.Immediate.Status = ToUe(Status);
		Pending.Immediate.Message = Status == State->StartStatus ? State->StartDetail : FString();
		if (Status == SE::EStatus::DifferentModelOpen)
		{
			Pending.Immediate.Message = TEXT("another model is open; release every handle on it first");
		}
		Pending.Immediate.State = GetEncoderState();
	}
	State->Opens.Add(MoveTemp(Pending));
	return Object;
}

ESuperEmbedderEncoderState USuperEmbedderSubsystem::GetEncoderState() const
{
	return State.IsValid() ? ToUe(State->Service->GetState()) : ESuperEmbedderEncoderState::Closed;
}

FSuperEmbedderOpenResult USuperEmbedderSubsystem::GetOpenResult() const
{
	FSuperEmbedderOpenResult R;
	if (!State.IsValid())
	{
		R.Status = ESuperEmbedderStatus::ShuttingDown;
		return R;
	}
	if (State->StartStatus != SE::EStatus::Ok)
	{
		R.Status = ToUe(State->StartStatus);
		R.Message = State->StartDetail;
		return R;
	}
	std::string Detail;
	R.Status = ToUe(State->Service->GetOpenStatus(&Detail));
	R.Message = FromUtf8(Detail);
	R.State = ToUe(State->Service->GetState());
	R.Dims = static_cast<int32>(State->Service->GetDims());
	SE::FFingerprint Fp{};
	if (R.State == ESuperEmbedderEncoderState::Ready && State->Service->GetFingerprint(Fp))
	{
		R.FingerprintHex = FingerprintHex(Fp);
	}
	return R;
}

void USuperEmbedderSubsystem::ReleaseKeepAlive()
{
	if (KeepAlive != nullptr)
	{
		KeepAlive->Release();
		KeepAlive = nullptr;
	}
}

void USuperEmbedderSubsystem::ReleaseHandle(uint64 Handle)
{
	if (State.IsValid() && Handle != 0)
	{
		State->Service->Release(Handle);
	}
}

// --- Encode ---

FSuperEmbedderTicket USuperEmbedderSubsystem::EncodeText(const FString& Text, const UObject* WorldContext, FSuperEmbedderEncodeCallback OnDone)
{
	check(IsInGameThread());
	FSuperEmbedderTicket Ticket;
	if (!State.IsValid())
	{
		return Ticket;
	}
	const uint64 Owner = OwnerOf(WorldContext);
	std::string Utf8;
	const SE::EStatus Converted = ToUtf8(Text, Utf8);
	const SE::FTicket Id = Converted == SE::EStatus::Ok
		? State->Service->EncodeQuery(MoveTemp(Utf8), Owner)
		: State->Service->Refuse(SE::EKind::Query, Converted, "the query text", Owner);
	FSuperEmbedderSubsystemState::FPendingEncode Pending;
	Pending.Owner = Owner;
	Pending.OnQuery = MoveTemp(OnDone);
	State->Encodes.Add(Id, MoveTemp(Pending));
	Ticket.Id = static_cast<int64>(Id);
	return Ticket;
}

FSuperEmbedderTicket USuperEmbedderSubsystem::EncodeDocuments(const TArray<FSuperEmbedderDocumentRow>& Rows, const UObject* WorldContext, FSuperEmbedderDocumentsCallback OnDone)
{
	check(IsInGameThread());
	FSuperEmbedderTicket Ticket;
	if (!State.IsValid())
	{
		return Ticket;
	}
	const uint64 Owner = OwnerOf(WorldContext);
	// Only a copy into UTF-8 here (G-29); every row rule runs on a worker, in the documents ticket's validation.
	std::vector<SE::FDocumentRow> CoreRows;
	CoreRows.reserve(static_cast<size_t>(Rows.Num()));
	SE::EStatus Refusal = SE::EStatus::Ok;
	std::string RefusalDetail;
	for (int32 I = 0; I < Rows.Num() && Refusal == SE::EStatus::Ok; ++I)
	{
		SE::FDocumentRow Row;
		const SE::EStatus IdStatus = ToUtf8(Rows[I].Id, Row.Id);
		if (IdStatus == SE::EStatus::InvalidText)
		{
			Refusal = SE::EStatus::InvalidId;
			RefusalDetail = "row " + std::to_string(I + 1) + ": an unpaired UTF-16 surrogate in the id";
			break;
		}
		const SE::EStatus TextStatus = ToUtf8(Rows[I].Text, Row.Utf8);
		if (TextStatus == SE::EStatus::InvalidText)
		{
			Refusal = SE::EStatus::InvalidText;
			RefusalDetail = "row " + std::to_string(I + 1) + " (" + Row.Id + ")";
			break;
		}
		// An empty id or text converts to empty and is refused by name in the validation unit.
		CoreRows.push_back(MoveTemp(Row));
	}
	const SE::FTicket Id = Refusal == SE::EStatus::Ok
		? State->Service->EncodeDocuments(MoveTemp(CoreRows), Owner)
		: State->Service->Refuse(SE::EKind::Document, Refusal, RefusalDetail, Owner);
	FSuperEmbedderSubsystemState::FPendingEncode Pending;
	Pending.Owner = Owner;
	Pending.OnDocuments = MoveTemp(OnDone);
	State->Encodes.Add(Id, MoveTemp(Pending));
	Ticket.Id = static_cast<int64>(Id);
	return Ticket;
}

FSuperEmbedderTicket USuperEmbedderSubsystem::TopKAsync(USuperEmbedderVectorSet* Set, const FSuperEmbedderVector& Query, int32 K,
	const TArray<FString>& Exclude, const UObject* WorldContext, FSuperEmbedderTopKCallback OnDone)
{
	check(IsInGameThread());
	FSuperEmbedderTicket Ticket;
	if (!State.IsValid())
	{
		return Ticket;
	}
	TSharedPtr<FSuperEmbedderSubsystemState::FTopKJob> Job = MakeShared<FSuperEmbedderSubsystemState::FTopKJob>();
	Job->Owner = OwnerOf(WorldContext);
	Job->OnDone = MoveTemp(OnDone);
	Job->K = K;
	for (const FString& Id : Exclude)
	{
		Job->Exclude.insert(ToUtf8Lossy(Id));
	}
	const uint64 Id = kTopKTicketBit | State->NextTopK++;
	State->TopK.Add(Id, Job);
	Ticket.Id = static_cast<int64>(Id);

	if (Set == nullptr)
	{
		Job->bHasImmediate = true;
		Job->Immediate = MakeTopKResult(ESuperEmbedderOutcome::Refused, ESuperEmbedderStatus::NullSet, TEXT("no vector set"));
		return Ticket;
	}
	// The snapshot is O(1) and copy-on-write: a later Append or Remove never changes this ranking (V-12).
	TSharedPtr<FRankWork, ESPMode::ThreadSafe> Work = MakeShared<FRankWork, ESPMode::ThreadSafe>();
	Work->Snapshot = Set->GetCore().Snapshot();
	Work->Query = ToCore(Query);
	Work->K = K;
	Work->Exclude = Job->Exclude;
	Job->Work = Work;
	Job->Query = Query;
	State->Tasks.Add(UE::Tasks::Launch(TEXT("SuperEmbedder.TopK"), [Work]()
	{
		Work->Status = SE::TopK(Work->Snapshot, Work->Query, Work->K, Work->Exclude.empty() ? nullptr : &Work->Exclude, Work->Hits);
		Work->bDone.store(true, std::memory_order_release);
	}));
	return Ticket;
}

FSuperEmbedderTicket USuperEmbedderSubsystem::QueryTextTopK(USuperEmbedderVectorSet* Set, const FString& Text, int32 K,
	const TArray<FString>& Exclude, const UObject* WorldContext, FSuperEmbedderTopKCallback OnDone)
{
	check(IsInGameThread());
	FSuperEmbedderTicket Ticket;
	if (!State.IsValid())
	{
		return Ticket;
	}
	TSharedPtr<FSuperEmbedderSubsystemState::FTopKJob> Job = MakeShared<FSuperEmbedderSubsystemState::FTopKJob>();
	Job->Owner = OwnerOf(WorldContext);
	Job->OnDone = MoveTemp(OnDone);
	Job->K = K;
	for (const FString& Id : Exclude)
	{
		Job->Exclude.insert(ToUtf8Lossy(Id));
	}
	const uint64 Id = kTopKTicketBit | State->NextTopK++;
	State->TopK.Add(Id, Job);
	Ticket.Id = static_cast<int64>(Id);

	if (Set == nullptr)
	{
		Job->bHasImmediate = true;
		Job->Immediate = MakeTopKResult(ESuperEmbedderOutcome::Refused, ESuperEmbedderStatus::NullSet, TEXT("no vector set"));
		return Ticket;
	}
	if (K < 1)
	{
		Job->bHasImmediate = true;
		Job->Immediate = MakeTopKResult(ESuperEmbedderOutcome::Refused, ESuperEmbedderStatus::InvalidK, FString::Printf(TEXT("k %d"), K));
		return Ticket;
	}
	Job->Set.Reset(Set);
	const FSuperEmbedderTicket Encode = EncodeText(Text, WorldContext, nullptr);
	Job->EncodeTicket = static_cast<uint64>(Encode.Id);
	if (FSuperEmbedderSubsystemState::FPendingEncode* Pending = State->Encodes.Find(Job->EncodeTicket))
	{
		Pending->TopKTicket = Id;
	}
	return Ticket;
}

bool USuperEmbedderSubsystem::Cancel(FSuperEmbedderTicket Ticket)
{
	check(IsInGameThread());
	if (!State.IsValid() || !Ticket.IsValid())
	{
		return false;
	}
	const uint64 Id = static_cast<uint64>(Ticket.Id);
	if ((Id & kTopKTicketBit) == 0)
	{
		return State->Service->Cancel(Id);
	}
	TSharedPtr<FSuperEmbedderSubsystemState::FTopKJob>* Found = State->TopK.Find(Id);
	if (Found == nullptr || (*Found)->bHasImmediate)
	{
		return false;
	}
	FSuperEmbedderSubsystemState::FTopKJob& Job = **Found;
	if (Job.EncodeTicket != 0)
	{
		// Its Cancelled delivery routes to this job on the next drain.
		return State->Service->Cancel(Job.EncodeTicket);
	}
	// A ranking already running finishes on its worker; its result is discarded.
	Job.Work.Reset();
	Job.bHasImmediate = true;
	Job.Immediate = MakeTopKResult(ESuperEmbedderOutcome::Cancelled, ESuperEmbedderStatus::Cancelled, FString());
	return true;
}

bool USuperEmbedderSubsystem::GetProgress(FSuperEmbedderTicket Ticket, int32& OutDone, int32& OutTotal) const
{
	OutDone = 0;
	OutTotal = 0;
	if (!State.IsValid() || !Ticket.IsValid() || (static_cast<uint64>(Ticket.Id) & kTopKTicketBit) != 0)
	{
		return false;
	}
	size_t Done = 0;
	size_t Total = 0;
	if (!State->Service->GetProgress(static_cast<uint64>(Ticket.Id), Done, Total))
	{
		return false;
	}
	OutDone = static_cast<int32>(Done);
	OutTotal = static_cast<int32>(Total);
	return true;
}

ESuperEmbedderStatus USuperEmbedderSubsystem::EncodeTextBlocking(const FString& Text, ESuperEmbedderVectorKind Kind, FSuperEmbedderVector& OutVector, FString& OutMessage)
{
	OutVector = FSuperEmbedderVector();
	OutMessage.Reset();
	// State is created and reset on the game thread; a blocking caller must not outlive Deinitialize.
	TSharedPtr<FSuperEmbedderSubsystemState> Local = State;
	if (!Local.IsValid())
	{
		return ESuperEmbedderStatus::ShuttingDown;
	}
	std::string Utf8;
	const SE::EStatus Converted = ToUtf8(Text, Utf8);
	if (Converted != SE::EStatus::Ok)
	{
		return ToUe(Converted);
	}
	SE::FEmbedding Vector;
	std::string Detail;
	const SE::EStatus Status = Local->Service->EncodeBlocking(
		Kind == ESuperEmbedderVectorKind::Document ? SE::EKind::Document : SE::EKind::Query, Utf8, Vector, Detail);
	OutMessage = FromUtf8(Detail);
	if (Status == SE::EStatus::Ok)
	{
		OutVector = ToUe(Vector);
	}
	return ToUe(Status);
}

// --- Cache ---

void USuperEmbedderSubsystem::SaveEncodeCache(const FString& Path)
{
	check(IsInGameThread());
	if (!State.IsValid() || Path.IsEmpty())
	{
		return;
	}
	const FString Resolved = USuperEmbedderSettings::ResolvePath(Path);
	std::shared_ptr<SE::FEncodeService> Service = State->Service;
	State->Tasks.Add(UE::Tasks::Launch(TEXT("SuperEmbedder.SaveCache"), [Service, Resolved]()
	{
		const std::vector<uint8_t> Bytes = Service->GetCache().Serialize();
		TArray<uint8> Out;
		Out.SetNumUninitialized(static_cast<int32>(Bytes.size()));
		if (!Bytes.empty())
		{
			FMemory::Memcpy(Out.GetData(), Bytes.data(), Bytes.size());
		}
		if (!FFileHelper::SaveArrayToFile(Out, *Resolved))
		{
			UE_LOG(LogSuperEmbedder, Error, TEXT("SuperEmbedder: could not write the encode cache to %s"), *Resolved);
		}
		else
		{
			UE_LOG(LogSuperEmbedder, Log, TEXT("SuperEmbedder: wrote the encode cache (%d bytes) to %s"), Out.Num(), *Resolved);
		}
	}));
}

int32 USuperEmbedderSubsystem::GetEncodeCacheEntries() const
{
	return State.IsValid() ? static_cast<int32>(State->Service->GetCache().Num()) : 0;
}

// --- Delivery ---

namespace
{
	FSuperEmbedderEncodeResult ToEncodeResult(const SE::FDelivery& D)
	{
		FSuperEmbedderEncodeResult R;
		R.Outcome = ToUe(D.Outcome);
		R.Status = ToUe(D.Status);
		R.Message = FromUtf8(D.Detail);
		R.bFromCache = D.bFromCache;
		if (D.Outcome == SE::EOutcome::Completed && !D.Vectors.empty())
		{
			R.Vector = ToUe(D.Vectors[0]);
		}
		return R;
	}

	FSuperEmbedderDocumentsResult ToDocumentsResult(const SE::FDelivery& D)
	{
		FSuperEmbedderDocumentsResult R;
		R.Outcome = ToUe(D.Outcome);
		R.Status = ToUe(D.Status);
		R.Message = FromUtf8(D.Detail);
		if (D.Outcome == SE::EOutcome::Completed)
		{
			R.Ids.Reserve(static_cast<int32>(D.Ids.size()));
			for (const std::string& Id : D.Ids)
			{
				R.Ids.Add(FromUtf8(Id));
			}
			R.Vectors.Reserve(static_cast<int32>(D.Vectors.size()));
			for (const SE::FEmbedding& V : D.Vectors)
			{
				R.Vectors.Add(ToUe(V));
			}
		}
		return R;
	}
}

bool USuperEmbedderSubsystem::Tick(float DeltaSeconds)
{
	if (!State.IsValid())
	{
		return true;
	}
	// Hold the state for this tick: a callback may reach Deinitialize (an engine exit from a delegate).
	TSharedPtr<FSuperEmbedderSubsystemState> Local = State;

	// Encoder state: pending opens, the state-changed delegate, handles a failed open invalidated.
	const ESuperEmbedderEncoderState Now = ToUe(Local->Service->GetState());
	if (Now != Local->LastState)
	{
		Local->LastState = Now;
		if ((Now == ESuperEmbedderEncoderState::Failed || Now == ESuperEmbedderEncoderState::Closed) && KeepAlive != nullptr)
		{
			KeepAlive->Handle = 0; // a failed open (or a close) discards every handle
			KeepAlive = nullptr;
		}
		OnEncoderStateChanged.Broadcast(Now);
	}
	if (Local->Opens.Num() > 0)
	{
		TArray<FSuperEmbedderSubsystemState::FPendingOpen> Ready;
		for (int32 I = 0; I < Local->Opens.Num();)
		{
			FSuperEmbedderSubsystemState::FPendingOpen& Open = Local->Opens[I];
			if (Open.bImmediate || Now != ESuperEmbedderEncoderState::Opening)
			{
				Ready.Add(MoveTemp(Open));
				Local->Opens.RemoveAt(I);
			}
			else
			{
				++I;
			}
		}
		const FSuperEmbedderOpenResult Result = GetOpenResult();
		for (FSuperEmbedderSubsystemState::FPendingOpen& Open : Ready)
		{
			FSuperEmbedderOpenResult R = Open.bImmediate ? Open.Immediate : Result;
			USuperEmbedderEncoderHandle* Handle = Open.Handle.Get();
			if (!Open.bImmediate && R.State != ESuperEmbedderEncoderState::Ready)
			{
				if (R.Status == ESuperEmbedderStatus::Ok)
				{
					R.Status = ESuperEmbedderStatus::EncoderNotReady;
					R.Message = TEXT("every handle was released before the open finished");
				}
				if (Handle != nullptr)
				{
					Handle->Handle = 0;
				}
			}
			if (Open.OnDone)
			{
				Open.OnDone(R, Handle);
			}
		}
	}

	// Encodes.
	std::vector<SE::FDelivery> Deliveries = Local->Service->Drain();
	for (const SE::FDelivery& D : Deliveries)
	{
		FSuperEmbedderSubsystemState::FPendingEncode Pending;
		if (!Local->Encodes.RemoveAndCopyValue(D.Ticket, Pending))
		{
			continue;
		}
		if (Pending.TopKTicket != 0)
		{
			TSharedPtr<FSuperEmbedderSubsystemState::FTopKJob>* Found = Local->TopK.Find(Pending.TopKTicket);
			if (Found == nullptr)
			{
				continue;
			}
			TSharedPtr<FSuperEmbedderSubsystemState::FTopKJob> Job = *Found;
			Job->EncodeTicket = 0;
			if (D.Outcome != SE::EOutcome::Completed || D.Vectors.empty())
			{
				Job->Set.Reset();
				Job->bHasImmediate = true;
				Job->Immediate = MakeTopKResult(ToUe(D.Outcome), ToUe(D.Status), FromUtf8(D.Detail));
				continue;
			}
			// The rows as they are now, when the encode completed (V-3, V-4); the ranking leaves the game thread.
			TSharedPtr<FRankWork, ESPMode::ThreadSafe> Work = MakeShared<FRankWork, ESPMode::ThreadSafe>();
			Work->Snapshot = Job->Set->GetCore().Snapshot();
			Work->Query = D.Vectors[0];
			Work->K = Job->K;
			Work->Exclude = Job->Exclude;
			Job->Set.Reset();
			Job->Query = ToUe(D.Vectors[0]);
			Job->Work = Work;
			Local->Tasks.Add(UE::Tasks::Launch(TEXT("SuperEmbedder.QueryTextTopK"), [Work]()
			{
				Work->Status = SE::TopK(Work->Snapshot, Work->Query, Work->K, Work->Exclude.empty() ? nullptr : &Work->Exclude, Work->Hits);
				Work->bDone.store(true, std::memory_order_release);
			}));
			continue;
		}
		if (D.Kind == SE::EKind::Document)
		{
			if (Pending.OnDocuments)
			{
				Pending.OnDocuments(ToDocumentsResult(D));
			}
		}
		else if (Pending.OnQuery)
		{
			Pending.OnQuery(ToEncodeResult(D));
		}
	}

	// Rankings. Collected first: a callback may start another, which is delivered on a later tick.
	TArray<TPair<FSuperEmbedderTopKCallback, FSuperEmbedderTopKResult>> Finished;
	for (auto It = Local->TopK.CreateIterator(); It; ++It)
	{
		FSuperEmbedderSubsystemState::FTopKJob& Job = *It.Value();
		if (Job.bHasImmediate)
		{
			Finished.Emplace(MoveTemp(Job.OnDone), Job.Immediate);
			It.RemoveCurrent();
			continue;
		}
		if (Job.Work.IsValid() && Job.Work->bDone.load(std::memory_order_acquire))
		{
			FSuperEmbedderTopKResult R;
			R.Status = ToUe(Job.Work->Status);
			R.Outcome = Job.Work->Status == SE::EStatus::Ok ? ESuperEmbedderOutcome::Completed : ESuperEmbedderOutcome::Refused;
			R.Query = Job.Query;
			if (Job.Work->Status == SE::EStatus::Ok)
			{
				R.Hits.Reserve(static_cast<int32>(Job.Work->Hits.size()));
				for (const SE::FHit& Hit : Job.Work->Hits)
				{
					FSuperEmbedderHit H;
					H.Id = FromUtf8(Hit.Id);
					H.Score = Hit.Score;
					H.RowIndex = static_cast<int32>(Hit.Row);
					R.Hits.Add(MoveTemp(H));
				}
			}
			Finished.Emplace(MoveTemp(Job.OnDone), MoveTemp(R));
			It.RemoveCurrent();
		}
	}
	for (TPair<FSuperEmbedderTopKCallback, FSuperEmbedderTopKResult>& Done : Finished)
	{
		if (Done.Key)
		{
			Done.Key(Done.Value);
		}
	}

	Local->Tasks.RemoveAll([](const UE::Tasks::FTask& Task) { return Task.IsCompleted(); });
	return true;
}

void USuperEmbedderSubsystem::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (!State.IsValid() || World == nullptr)
	{
		return;
	}
	uint64 Owner = 0;
	if (!State->Owners.RemoveAndCopyValue(World, Owner))
	{
		return;
	}
	TSharedPtr<FSuperEmbedderSubsystemState> Local = State;
	// Delivered now, while the world still exists (section 2.1, L-4). The encoder itself keeps running.
	std::vector<SE::FDelivery> Cancelled = Local->Service->CancelOwner(Owner);
	TArray<TPair<FSuperEmbedderTopKCallback, FSuperEmbedderTopKResult>> TopKCancelled;
	for (const SE::FDelivery& D : Cancelled)
	{
		FSuperEmbedderSubsystemState::FPendingEncode Pending;
		if (!Local->Encodes.RemoveAndCopyValue(D.Ticket, Pending))
		{
			continue;
		}
		if (Pending.TopKTicket != 0)
		{
			continue; // the job itself is cancelled below, with the owner's other rankings
		}
		if (D.Kind == SE::EKind::Document)
		{
			if (Pending.OnDocuments)
			{
				Pending.OnDocuments(ToDocumentsResult(D));
			}
		}
		else if (Pending.OnQuery)
		{
			Pending.OnQuery(ToEncodeResult(D));
		}
	}
	for (auto It = Local->TopK.CreateIterator(); It; ++It)
	{
		FSuperEmbedderSubsystemState::FTopKJob& Job = *It.Value();
		if (Job.Owner != Owner)
		{
			continue;
		}
		Job.Set.Reset();
		if (Job.EncodeTicket != 0)
		{
			Local->Encodes.Remove(Job.EncodeTicket);
		}
		TopKCancelled.Emplace(MoveTemp(Job.OnDone),
			MakeTopKResult(ESuperEmbedderOutcome::Cancelled, ESuperEmbedderStatus::Cancelled, TEXT("the world ended")));
		It.RemoveCurrent();
	}
	for (TPair<FSuperEmbedderTopKCallback, FSuperEmbedderTopKResult>& Done : TopKCancelled)
	{
		if (Done.Key)
		{
			Done.Key(Done.Value);
		}
	}
}
