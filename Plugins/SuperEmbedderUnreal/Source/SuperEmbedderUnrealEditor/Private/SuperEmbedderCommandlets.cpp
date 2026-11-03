#include "SuperEmbedderCommandlets.h"

#include "Containers/Ticker.h"
#include "Engine/DataTable.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h"

#include "SuperEmbedderBake.h"
#include "SuperEmbedderSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSuperEmbedderCommandlet, Log, All);

namespace
{
	// A commandlet has no engine loop: the subsystem delivers through the core ticker, so pump it.
	void PumpUntil(const TFunction<bool()>& bDone)
	{
		double Last = FPlatformTime::Seconds();
		while (!bDone() && !IsEngineExitRequested())
		{
			const double Now = FPlatformTime::Seconds();
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(Now - Last));
			Last = Now;
			FPlatformProcess::Sleep(0.02f);
		}
	}

	// Opens from Project Settings and waits. Returns the handle on Ready, null on a refusal (logged).
	USuperEmbedderEncoderHandle* OpenAndWait(USuperEmbedderSubsystem& Subsystem)
	{
		// Shared, not on the stack: a delivery after an early exit must not write to a dead frame.
		TSharedRef<TPair<bool, bool>> Outcome = MakeShared<TPair<bool, bool>>(false, false); // done, ready
		USuperEmbedderEncoderHandle* Handle = Subsystem.OpenEncoder(FString(), FString(), [Outcome](const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle*)
		{
			Outcome->Key = true;
			Outcome->Value = Result.Status == ESuperEmbedderStatus::Ok && Result.State == ESuperEmbedderEncoderState::Ready;
			if (!Outcome->Value)
			{
				UE_LOG(LogSuperEmbedderCommandlet, Error, TEXT("SuperEmbedder: the encoder did not open: %s %s"), *LexToString(Result.Status), *Result.Message);
			}
		});
		TStrongObjectPtr<USuperEmbedderEncoderHandle> Hold(Handle);
		UE_LOG(LogSuperEmbedderCommandlet, Display, TEXT("SuperEmbedder: opening the encoder (20-38 s on the published model)..."));
		PumpUntil([Outcome]() { return Outcome->Key; });
		return Outcome->Value ? Handle : nullptr;
	}
}

USuperEmbedderBakeCommandlet::USuperEmbedderBakeCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 USuperEmbedderBakeCommandlet::Main(const FString& Params)
{
	FString Input;
	FString Output;
	if (!FParse::Value(*Params, TEXT("Input="), Input) || !FParse::Value(*Params, TEXT("Output="), Output))
	{
		UE_LOG(LogSuperEmbedderCommandlet, Error, TEXT("usage: -run=SuperEmbedderBake -Input=<rows.jsonl | DataTable object path> -Output=/Game/Path/SetName"));
		return 1;
	}
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		UE_LOG(LogSuperEmbedderCommandlet, Error, TEXT("SuperEmbedder: no subsystem"));
		return 1;
	}
	TSharedRef<TPair<bool, ESuperEmbedderStatus>> Outcome = MakeShared<TPair<bool, ESuperEmbedderStatus>>(false, ESuperEmbedderStatus::Ok);
	auto OnFinished = [Outcome](ESuperEmbedderStatus InStatus, const FString&)
	{
		Outcome->Value = InStatus;
		Outcome->Key = true;
	};
	const bool bStarted = Input.EndsWith(TEXT(".jsonl"), ESearchCase::IgnoreCase)
		? FSuperEmbedderBake::StartFromJsonlFile(Input, Output, /*bNotify*/ false, OnFinished)
		: FSuperEmbedderBake::StartFromDataTable(LoadObject<UDataTable>(nullptr, *Input), Output, /*bNotify*/ false, OnFinished);
	if (!bStarted)
	{
		return 1;
	}
	PumpUntil([Outcome]() { return Outcome->Key; });
	return Outcome->Key && Outcome->Value == ESuperEmbedderStatus::Ok ? 0 : 1;
}

USuperEmbedderValidateCommandlet::USuperEmbedderValidateCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 USuperEmbedderValidateCommandlet::Main(const FString& Params)
{
	USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		UE_LOG(LogSuperEmbedderCommandlet, Error, TEXT("SuperEmbedder: no subsystem"));
		return 1;
	}
	TStrongObjectPtr<USuperEmbedderEncoderHandle> Handle(OpenAndWait(*Subsystem));
	if (!Handle.IsValid())
	{
		return 1;
	}
	TArray<FString> Report;
	const int32 Listed = FSuperEmbedderValidate::ValidateAllSets(Report);
	for (const FString& Line : Report)
	{
		if (Line.StartsWith(TEXT("LISTED")))
		{
			UE_LOG(LogSuperEmbedderCommandlet, Error, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogSuperEmbedderCommandlet, Display, TEXT("%s"), *Line);
		}
	}
	UE_LOG(LogSuperEmbedderCommandlet, Display, TEXT("SuperEmbedderValidate: %d set(s) checked, %d listed"), Report.Num(), FMath::Max(0, Listed));
	Handle->Release();
	return Listed == 0 ? 0 : 1;
}
