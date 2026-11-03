#include "SuperEmbedderExample.h"

#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SuperEmbedderAsyncActions.h"
#include "SuperEmbedderBlueprintLibrary.h"
#include "SuperEmbedderVectorSet.h"

DEFINE_LOG_CATEGORY_STATIC(LogSuperEmbedderExample, Log, All);

namespace
{
	constexpr int32 kMaxLines = 40;

	FString Seconds(double Since)
	{
		return FString::Printf(TEXT("%.1f s"), FPlatformTime::Seconds() - Since);
	}
}

ASuperEmbedderExample::ASuperEmbedderExample()
{
	PrimaryActorTick.bCanEverTick = false;

	const TCHAR* Places[][2] = {
		{ TEXT("forge"), TEXT("A blacksmith's forge where swords and axes are hammered from hot iron.") },
		{ TEXT("library"), TEXT("A quiet library of old books, scrolls and star maps.") },
		{ TEXT("harbor"), TEXT("A harbor where fishing boats unload their nets at dawn.") },
		{ TEXT("tavern"), TEXT("A noisy tavern serving ale, stew and fresh bread.") },
		{ TEXT("garden"), TEXT("An herb garden of mint, sage and healing flowers.") },
		{ TEXT("stable"), TEXT("A stable where travelers rent horses for the road.") },
		{ TEXT("chapel"), TEXT("A small stone chapel with candles and a bell tower.") },
		{ TEXT("market"), TEXT("A market square of fruit stalls, cloth and spices.") },
	};
	for (const auto& Place : Places)
	{
		FSuperEmbedderDocumentRow Row;
		Row.Id = Place[0];
		Row.Text = Place[1];
		Rows.Add(Row);
	}
	Queries = { TEXT("Where can I buy a sword?"), TEXT("I want something to eat and drink."), TEXT("I need a horse for my journey.") };
	ExpectedTopIds = { TEXT("forge"), TEXT("tavern"), TEXT("stable") };
}

ASuperEmbedderExample* ASuperEmbedderExample::Find(UWorld* World)
{
	if (World != nullptr)
	{
		for (TActorIterator<ASuperEmbedderExample> It(World); It; ++It)
		{
			return *It;
		}
	}
	return nullptr;
}

void ASuperEmbedderExample::BeginPlay()
{
	Super::BeginPlay();
	bExitWhenDone = FParse::Param(FCommandLine::Get(), TEXT("SuperEmbedderExampleExit"));
	StartSeconds = FPlatformTime::Seconds();
	Phase = ESuperEmbedderExamplePhase::Opening;
	Say(TEXT("Opening the encoder from Project Settings (tens of seconds on the published model)..."));

	// Open SuperEmbedder Encoder. Empty paths: Project Settings > Plugins > SuperEmbedder.
	USuperEmbedderOpenEncoderAction* Open = USuperEmbedderOpenEncoderAction::OpenSuperEmbedderEncoder(this, FString(), FString());
	Open->Opened.AddDynamic(this, &ASuperEmbedderExample::OnOpened);
	Open->Failed.AddDynamic(this, &ASuperEmbedderExample::OnOpenFailed);
	Open->Activate();
}

void ASuperEmbedderExample::EndPlay(const EEndPlayReason::Type Reason)
{
	// The world's tickets are cancelled by the subsystem when the world ends; the handle lets the encoder close.
	if (Handle != nullptr)
	{
		Handle->Release();
		Handle = nullptr;
	}
	Super::EndPlay(Reason);
}

void ASuperEmbedderExample::OnOpened(const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle* InHandle)
{
	Handle = InHandle;
	Say(FString::Printf(TEXT("Encoder Ready after %s: %d dims, fingerprint %s..."), *Seconds(StartSeconds), Result.Dims, *Result.FingerprintHex.Left(16)));

	Phase = ESuperEmbedderExamplePhase::Baking;
	Say(FString::Printf(TEXT("Baking %d rows into a vector set..."), Rows.Num()));
	const double BakeStart = FPlatformTime::Seconds();
	StartSeconds = BakeStart;

	// Encode Documents (Async): every row's vector, in row order, in one Completed.
	USuperEmbedderEncodeDocumentsAction* Encode = USuperEmbedderEncodeDocumentsAction::EncodeDocumentsAsync(this, Rows);
	Encode->Completed.AddDynamic(this, &ASuperEmbedderExample::OnDocumentsEncoded);
	Encode->Refused.AddDynamic(this, &ASuperEmbedderExample::OnDocumentsNotEncoded);
	Encode->Cancelled.AddDynamic(this, &ASuperEmbedderExample::OnDocumentsNotEncoded);
	Encode->Activate();
}

void ASuperEmbedderExample::OnOpenFailed(const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle* InHandle)
{
	Fail(FString::Printf(TEXT("The encoder did not open: %s %s. Put model.sslm and tokenizer.sslm in Content/SuperEmbedder/, or set their paths in Project Settings."),
		*USuperEmbedderBlueprintLibrary::StatusToString(Result.Status), *Result.Message));
}

void ASuperEmbedderExample::OnDocumentsEncoded(const FSuperEmbedderDocumentsResult& Result)
{
	// New Vector Set, then Append Documents: the run-time bake. The editor bakes the same rows to an asset.
	Set = USuperEmbedderBlueprintLibrary::NewVectorSet(this);
	TArray<FString> Texts;
	for (const FSuperEmbedderDocumentRow& Row : Rows)
	{
		Texts.Add(Row.Text);
	}
	FString Message;
	const ESuperEmbedderStatus Appended = Set->AppendDocuments(Result, Texts, Message);
	if (Appended != ESuperEmbedderStatus::Ok)
	{
		Fail(FString::Printf(TEXT("Append Documents refused: %s %s"), *USuperEmbedderBlueprintLibrary::StatusToString(Appended), *Message));
		return;
	}
	Say(FString::Printf(TEXT("Baked %d rows in %s (%d dims each)."), Set->Num(), *Seconds(StartSeconds), Set->GetDims()));

	Phase = ESuperEmbedderExamplePhase::Querying;
	FixedQueriesLeft = Queries.Num();
	FixedQueryIndex = 0;
	// The fixed queries run first, ahead of any Ask made while the set was baking.
	Pending.Insert(Queries, 0);
	if (FixedQueriesLeft == 0)
	{
		FinishFixedQueries();
	}
	RunNextQuery();
}

void ASuperEmbedderExample::OnDocumentsNotEncoded(const FSuperEmbedderDocumentsResult& Result)
{
	Fail(FString::Printf(TEXT("Encode Documents did not complete: %s %s"), *USuperEmbedderBlueprintLibrary::StatusToString(Result.Status), *Result.Message));
}

void ASuperEmbedderExample::Ask(const FString& Text)
{
	if (Phase == ESuperEmbedderExamplePhase::Failed)
	{
		Say(TEXT("The example failed earlier; nothing to ask."));
		return;
	}
	Pending.Add(Text);
	if (Set != nullptr)
	{
		RunNextQuery();
	}
}

void ASuperEmbedderExample::RunNextQuery()
{
	if (bQueryInFlight || Pending.Num() == 0 || Set == nullptr)
	{
		return;
	}
	InFlight = Pending[0];
	Pending.RemoveAt(0);
	bQueryInFlight = true;
	StartSeconds = FPlatformTime::Seconds();

	// Query Text Top K (Async): encodes the text as a query, then ranks the set's rows by cosine.
	USuperEmbedderQueryTextTopKAction* Query = USuperEmbedderQueryTextTopKAction::QueryTextTopKAsync(this, Set, InFlight, K, TArray<FString>());
	Query->Completed.AddDynamic(this, &ASuperEmbedderExample::OnQueryAnswered);
	Query->Refused.AddDynamic(this, &ASuperEmbedderExample::OnQueryNotAnswered);
	Query->Cancelled.AddDynamic(this, &ASuperEmbedderExample::OnQueryNotAnswered);
	Query->Activate();
}

void ASuperEmbedderExample::OnQueryAnswered(const FSuperEmbedderTopKResult& Result)
{
	bQueryInFlight = false;
	Say(FString::Printf(TEXT("\"%s\" (%s, query digest %s...):"), *InFlight, *Seconds(StartSeconds),
		*USuperEmbedderBlueprintLibrary::GetVectorDigest(Result.Query).Left(16)));

	// The structural checks the exit code gates on: min(K, rows) hits, best first, each a cosine.
	bool bWellFormed = Result.Hits.Num() == FMath::Min(K, Set->Num());
	float Previous = 2.0f;
	for (const FSuperEmbedderHit& Hit : Result.Hits)
	{
		FString Text;
		Set->GetText(Hit.Id, Text);
		Say(FString::Printf(TEXT("    %.4f  %-8s %s"), Hit.Score, *Hit.Id, *Text));
		bWellFormed &= FMath::IsFinite(Hit.Score) && Hit.Score >= -1.0f && Hit.Score <= 1.0f && Hit.Score <= Previous;
		Previous = Hit.Score;
	}

	if (FixedQueriesLeft > 0)
	{
		const int32 Index = FixedQueryIndex++;
		if (ExpectedTopIds.IsValidIndex(Index) && Result.Hits.Num() > 0)
		{
			Say(FString::Printf(TEXT("    expected first: %s (%s)"), *ExpectedTopIds[Index],
				Result.Hits[0].Id == ExpectedTopIds[Index] ? TEXT("agrees") : TEXT("differs")));
		}
		bAllChecksPassed &= bWellFormed;
		if (--FixedQueriesLeft == 0)
		{
			FinishFixedQueries();
		}
	}
	if (!bWellFormed)
	{
		Say(TEXT("    the ranking is not well formed"));
	}
	Answered.Broadcast(InFlight, Result.Hits);
	RunNextQuery();
}

void ASuperEmbedderExample::OnQueryNotAnswered(const FSuperEmbedderTopKResult& Result)
{
	bQueryInFlight = false;
	Say(FString::Printf(TEXT("\"%s\" was not answered: %s %s"), *InFlight, *USuperEmbedderBlueprintLibrary::StatusToString(Result.Status), *Result.Message));
	if (FixedQueriesLeft > 0)
	{
		++FixedQueryIndex;
		bAllChecksPassed = false;
		if (--FixedQueriesLeft == 0)
		{
			FinishFixedQueries();
		}
	}
	RunNextQuery();
}

void ASuperEmbedderExample::FinishFixedQueries()
{
	Phase = ESuperEmbedderExamplePhase::Ready;
	Say(bAllChecksPassed
		? TEXT("SuperEmbedderExample: PASS. Type SuperEmbedderExample.Ask <text> in the console to query again.")
		: TEXT("SuperEmbedderExample: FAIL (see above)."));
	if (bExitWhenDone)
	{
		FPlatformMisc::RequestExitWithStatus(false, bAllChecksPassed ? 0 : 1);
	}
}

void ASuperEmbedderExample::Fail(const FString& Why)
{
	Phase = ESuperEmbedderExamplePhase::Failed;
	bAllChecksPassed = false;
	Say(Why);
	Say(TEXT("SuperEmbedderExample: FAIL."));
	if (bExitWhenDone)
	{
		FPlatformMisc::RequestExitWithStatus(false, 1);
	}
}

void ASuperEmbedderExample::Say(const FString& Line)
{
	UE_LOG(LogSuperEmbedderExample, Display, TEXT("%s"), *Line);
	Lines.Add(Line);
	if (Lines.Num() > kMaxLines)
	{
		Lines.RemoveAt(0, Lines.Num() - kMaxLines);
	}
}
