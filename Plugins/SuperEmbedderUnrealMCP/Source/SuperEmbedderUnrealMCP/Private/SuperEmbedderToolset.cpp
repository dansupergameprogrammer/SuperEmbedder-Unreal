#include "SuperEmbedderToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SuperEmbedderBake.h"
#include "SuperEmbedderSettings.h"
#include "SuperEmbedderSubsystem.h"
#include "SuperEmbedderTypes.h"
#include "SuperEmbedderVectorSet.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

// Every result is compact JSON. Tools are called on the game thread, where the ToolsetRegistry dispatches
// them. EncoderStatus and ListSets answer synchronously, and a failure is an {"error": "..."} object (the
// SuperFAISS toolset's convention). EncodeText, QueryText and ValidateSets return a pending
// UToolCallAsyncResultString and never wait on the game thread (the SuperSLM toolset's convention): the
// subsystem delivers every encode and ranking on a later game-thread tick, and the result is completed
// there, with SetValue on success and SetError on failure, so an MCP client sees a tool error.
namespace
{
	FString ToJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FString JsonError(const FString& Message)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("error"), Message);
		return ToJson(Object);
	}

	// A pending result and the reference that keeps it alive until it completes (the registry's future
	// handler does not keep it alive). Copyable, so the subsystem's TFunction callbacks can capture it.
	using FPendingResult = TStrongObjectPtr<UToolCallAsyncResultString>;

	FPendingResult NewPendingResult()
	{
		return FPendingResult(NewObject<UToolCallAsyncResultString>());
	}

	// The most rows one QueryText returns. Declared, not derived: a ranking of 1,000 rows is already more
	// than an agent reads.
	constexpr int32 kMaxK = 1000;

	// How many leading components EncodeText shows.
	constexpr int32 kHeadComponents = 8;

	FString StateName(ESuperEmbedderEncoderState State)
	{
		return StaticEnum<ESuperEmbedderEncoderState>()->GetNameStringByValue(static_cast<int64>(State));
	}

	FString KindName(ESuperEmbedderVectorKind Kind)
	{
		return Kind == ESuperEmbedderVectorKind::Query ? TEXT("Query") : TEXT("Document");
	}

	FString Refusal(ESuperEmbedderOutcome Outcome, ESuperEmbedderStatus Status, const FString& Message)
	{
		const FString Head = Outcome == ESuperEmbedderOutcome::Cancelled ? FString(TEXT("Cancelled")) : LexToString(Status);
		return Message.IsEmpty() ? Head : FString::Printf(TEXT("%s: %s"), *Head, *Message);
	}

	/** The vector's provenance and a summary of its values; never the whole vector. */
	TSharedRef<FJsonObject> VectorSummary(const FSuperEmbedderVector& Vector)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("digest"), Vector.GetDigestHex());
		Object->SetStringField(TEXT("fingerprint"), Vector.GetFingerprintHex());
		Object->SetStringField(TEXT("kind"), KindName(Vector.Kind));
		Object->SetBoolField(TEXT("checked"), Vector.bChecked);
		const int32 Dims = Vector.Values.Num();
		Object->SetNumberField(TEXT("dims"), Dims);
		if (Dims > 0)
		{
			// Accumulated in double, in index order, so the summary is reproducible for a given vector.
			double SumSquares = 0.0;
			double Sum = 0.0;
			float Min = Vector.Values[0];
			float Max = Vector.Values[0];
			for (const float V : Vector.Values)
			{
				SumSquares += static_cast<double>(V) * static_cast<double>(V);
				Sum += V;
				Min = FMath::Min(Min, V);
				Max = FMath::Max(Max, V);
			}
			Object->SetNumberField(TEXT("l2Norm"), FMath::Sqrt(SumSquares));
			Object->SetNumberField(TEXT("min"), Min);
			Object->SetNumberField(TEXT("max"), Max);
			Object->SetNumberField(TEXT("mean"), Sum / Dims);
			TArray<TSharedPtr<FJsonValue>> Head;
			for (int32 i = 0; i < FMath::Min(Dims, kHeadComponents); ++i)
			{
				Head.Add(MakeShared<FJsonValueNumber>(Vector.Values[i]));
			}
			Object->SetArrayField(TEXT("head"), Head);
		}
		return Object;
	}

	/** One set's summary. EncoderFingerprint is empty when the encoder is not Ready. */
	TSharedRef<FJsonObject> SetSummary(const USuperEmbedderVectorSet& Set, const FString& EncoderFingerprint)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("path"), Set.GetPathName());
		Object->SetBoolField(TEXT("asset"), Set.IsAsset());
		Object->SetNumberField(TEXT("rows"), Set.Num());
		Object->SetNumberField(TEXT("dims"), Set.GetDims());
		Object->SetStringField(TEXT("fingerprint"), Set.GetFingerprintHex());
		Object->SetStringField(TEXT("loadStatus"), LexToString(Set.GetLoadStatus()));
		if (!EncoderFingerprint.IsEmpty())
		{
			Object->SetBoolField(TEXT("matchesEncoder"), Set.Num() == 0 || Set.GetFingerprintHex() == EncoderFingerprint);
		}
		return Object;
	}

	FString ReadyFingerprint(const USuperEmbedderSubsystem* Subsystem)
	{
		if (Subsystem == nullptr)
		{
			return FString();
		}
		const FSuperEmbedderOpenResult Open = Subsystem->GetOpenResult();
		return Open.State == ESuperEmbedderEncoderState::Ready ? Open.FingerprintHex : FString();
	}

	// A handle held for one tool call, so the encoder cannot close under it; released when the call
	// completes. The callbacks hold it (a shared pointer they capture), never a static. In the editor the
	// subsystem's own keep-alive also holds the encoder, so releasing this one does not close it.
	using FHold = TSharedPtr<TStrongObjectPtr<USuperEmbedderEncoderHandle>>;

	void ReleaseHold(const FHold& Hold)
	{
		if (Hold.IsValid() && Hold->IsValid())
		{
			(*Hold)->Release();
			Hold->Reset();
		}
	}

	/**
	 * Runs Body with a Ready encoder: at once if one is Ready, otherwise once an open from Project Settings
	 * (or the open in progress, which OpenEncoder joins) reaches Ready. A failed open completes Result with
	 * its reason and never runs Body. Body releases the hold it is given when its own work is delivered.
	 */
	void WithReadyEncoder(const FPendingResult& Result, TFunction<void(USuperEmbedderSubsystem*, FHold)> Body)
	{
		USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
		if (Subsystem == nullptr)
		{
			Result->SetError(TEXT("no SuperEmbedder subsystem: is the SuperEmbedderUnreal plugin enabled?"));
			return;
		}
		if (Subsystem->IsEncoderReady())
		{
			Body(Subsystem, nullptr);
			return;
		}
		FHold Hold = MakeShared<TStrongObjectPtr<USuperEmbedderEncoderHandle>>();
		USuperEmbedderEncoderHandle* Handle = Subsystem->OpenEncoder(FString(), FString(),
			[Result, Hold, Body](const FSuperEmbedderOpenResult& Open, USuperEmbedderEncoderHandle*)
			{
				USuperEmbedderSubsystem* S = USuperEmbedderSubsystem::Get();
				if (S == nullptr || Open.Status != ESuperEmbedderStatus::Ok || Open.State != ESuperEmbedderEncoderState::Ready)
				{
					Result->SetError(FString::Printf(TEXT("the encoder did not open: %s"),
						*Refusal(ESuperEmbedderOutcome::Refused, Open.Status, Open.Message)));
					ReleaseHold(Hold);
					return;
				}
				Body(S, Hold);
			});
		// The open's callback is delivered on a later tick, never inside OpenEncoder, so the hold is set first.
		Hold->Reset(Handle);
	}

	/**
	 * A set by object path, found in memory or loaded asynchronously; OnResolved runs on the game thread
	 * exactly once. A package path without an object name (/Game/A/Set) is taken as /Game/A/Set.Set.
	 */
	void ResolveSetAsync(const FString& InPath, TFunction<void(USuperEmbedderVectorSet*, const FString&)> OnResolved)
	{
		FString ObjectPath = InPath.TrimStartAndEnd();
		if (ObjectPath.IsEmpty())
		{
			OnResolved(nullptr, TEXT("SetPath is empty"));
			return;
		}
		if (USuperEmbedderVectorSet* Found = FindObject<USuperEmbedderVectorSet>(nullptr, *ObjectPath))
		{
			OnResolved(Found, FString());
			return;
		}
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath = FString::Printf(TEXT("%s.%s"), *ObjectPath, *FPackageName::GetShortName(ObjectPath));
			if (USuperEmbedderVectorSet* Found = FindObject<USuperEmbedderVectorSet>(nullptr, *ObjectPath))
			{
				OnResolved(Found, FString());
				return;
			}
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FPackageName::IsValidLongPackageName(PackageName) || !FPackageName::DoesPackageExist(PackageName))
		{
			OnResolved(nullptr, FString::Printf(TEXT("no vector set at %s (ListSets names them)"), *InPath));
			return;
		}
		LoadPackageAsync(PackageName, FLoadPackageAsyncDelegate::CreateLambda(
			[OnResolved, ObjectPath, InPath](const FName&, UPackage*, EAsyncLoadingResult::Type LoadResult)
			{
				USuperEmbedderVectorSet* Loaded = LoadResult == EAsyncLoadingResult::Succeeded
					? FindObject<USuperEmbedderVectorSet>(nullptr, *ObjectPath)
					: nullptr;
				OnResolved(Loaded, Loaded != nullptr ? FString()
					: FString::Printf(TEXT("no vector set at %s (ListSets names them)"), *InPath));
			}));
	}
}

FString USuperEmbedderToolset::EncoderStatus()
{
	const USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (Subsystem == nullptr)
	{
		return JsonError(TEXT("no SuperEmbedder subsystem: is the SuperEmbedderUnreal plugin enabled?"));
	}
	const FSuperEmbedderOpenResult Open = Subsystem->GetOpenResult();
	const ESuperEmbedderEncoderState State = Subsystem->GetEncoderState();

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("state"), StateName(State));
	Object->SetBoolField(TEXT("ready"), State == ESuperEmbedderEncoderState::Ready);
	Object->SetStringField(TEXT("openStatus"), LexToString(Open.Status));
	Object->SetStringField(TEXT("openMessage"), Open.Message);
	Object->SetNumberField(TEXT("dims"), Open.Dims);
	Object->SetStringField(TEXT("fingerprint"), Open.FingerprintHex);
	Object->SetNumberField(TEXT("cacheEntries"), Subsystem->GetEncodeCacheEntries());

	const USuperEmbedderSettings* Settings = GetDefault<USuperEmbedderSettings>();
	const TSharedRef<FJsonObject> SettingsObject = MakeShared<FJsonObject>();
	const FString ModelFile = USuperEmbedderSettings::ResolvePath(Settings->ModelFile);
	const FString TokenizerFile = USuperEmbedderSettings::ResolvePath(Settings->TokenizerFile);
	SettingsObject->SetStringField(TEXT("modelFile"), ModelFile);
	SettingsObject->SetBoolField(TEXT("modelFileExists"), !ModelFile.IsEmpty() && IFileManager::Get().FileExists(*ModelFile));
	SettingsObject->SetStringField(TEXT("tokenizerFile"), TokenizerFile);
	SettingsObject->SetBoolField(TEXT("tokenizerFileExists"), !TokenizerFile.IsEmpty() && IFileManager::Get().FileExists(*TokenizerFile));
	SettingsObject->SetBoolField(TEXT("openOnStartup"), Settings->bOpenOnStartup);
	SettingsObject->SetNumberField(TEXT("maxConcurrency"), Settings->MaxConcurrency);
	SettingsObject->SetNumberField(TEXT("maxQueryTokens"), Settings->MaxQueryTokens);
	SettingsObject->SetNumberField(TEXT("maxDocumentTokens"), Settings->MaxDocumentTokens);
	SettingsObject->SetNumberField(TEXT("maxCacheEntries"), Settings->MaxCacheEntries);
	Object->SetObjectField(TEXT("settings"), SettingsObject);
	return ToJson(Object);
}

UToolCallAsyncResultString* USuperEmbedderToolset::EncodeText(const FString& Text)
{
	const FPendingResult Result = NewPendingResult();
	WithReadyEncoder(Result, [Result, Text](USuperEmbedderSubsystem* Subsystem, FHold Hold)
	{
		const FSuperEmbedderTicket Ticket = Subsystem->EncodeText(Text, nullptr, [Result, Hold](const FSuperEmbedderEncodeResult& Encoded)
		{
			ReleaseHold(Hold);
			if (Encoded.Outcome != ESuperEmbedderOutcome::Completed)
			{
				Result->SetError(Refusal(Encoded.Outcome, Encoded.Status, Encoded.Message));
				return;
			}
			const TSharedRef<FJsonObject> Object = VectorSummary(Encoded.Vector);
			Object->SetBoolField(TEXT("fromCache"), Encoded.bFromCache);
			Result->SetValue(ToJson(Object));
		});
		if (!Ticket.IsValid())
		{
			ReleaseHold(Hold);
			Result->SetError(TEXT("the SuperEmbedder subsystem is shutting down"));
		}
	});
	return Result.Get();
}

UToolCallAsyncResultString* USuperEmbedderToolset::QueryText(const FString& SetPath, const FString& Text, int32 K)
{
	const FPendingResult Result = NewPendingResult();
	if (K < 1 || K > kMaxK)
	{
		Result->SetError(FString::Printf(TEXT("InvalidK: K is %d; it must be 1 to %d"), K, kMaxK));
		return Result.Get();
	}
	ResolveSetAsync(SetPath, [Result, Text, K](USuperEmbedderVectorSet* Resolved, const FString& Error)
	{
		if (Resolved == nullptr)
		{
			Result->SetError(Error);
			return;
		}
		if (Resolved->GetLoadStatus() != ESuperEmbedderStatus::Ok)
		{
			Result->SetError(FString::Printf(TEXT("%s: the set's last load was refused, so it holds no rows: %s"),
				*LexToString(Resolved->GetLoadStatus()), *Resolved->GetPathName()));
			return;
		}
		// Held across the open, which can take tens of seconds; QueryTextTopK holds it from then on.
		const TStrongObjectPtr<USuperEmbedderVectorSet> Set(Resolved);
		WithReadyEncoder(Result, [Result, Set, Text, K](USuperEmbedderSubsystem* Subsystem, FHold Hold)
		{
			const FSuperEmbedderTicket Ticket = Subsystem->QueryTextTopK(Set.Get(), Text, K, TArray<FString>(), nullptr,
				[Result, Hold, Set](const FSuperEmbedderTopKResult& Ranked)
				{
					ReleaseHold(Hold);
					if (Ranked.Outcome != ESuperEmbedderOutcome::Completed)
					{
						Result->SetError(Refusal(Ranked.Outcome, Ranked.Status, Ranked.Message));
						return;
					}
					const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
					Object->SetStringField(TEXT("set"), Set->GetPathName());
					Object->SetNumberField(TEXT("rows"), Set->Num());
					Object->SetStringField(TEXT("queryDigest"), Ranked.Query.GetDigestHex());
					Object->SetStringField(TEXT("fingerprint"), Ranked.Query.GetFingerprintHex());
					TArray<TSharedPtr<FJsonValue>> Hits;
					for (const FSuperEmbedderHit& Hit : Ranked.Hits)
					{
						const TSharedRef<FJsonObject> HitObject = MakeShared<FJsonObject>();
						HitObject->SetStringField(TEXT("id"), Hit.Id);
						FString RowText;
						if (Set->GetText(Hit.Id, RowText) == ESuperEmbedderStatus::Ok)
						{
							HitObject->SetStringField(TEXT("text"), RowText);
						}
						HitObject->SetNumberField(TEXT("score"), Hit.Score);
						HitObject->SetNumberField(TEXT("rowIndex"), Hit.RowIndex);
						Hits.Add(MakeShared<FJsonValueObject>(HitObject));
					}
					Object->SetArrayField(TEXT("hits"), Hits);
					Result->SetValue(ToJson(Object));
				});
			if (!Ticket.IsValid())
			{
				ReleaseHold(Hold);
				Result->SetError(TEXT("the SuperEmbedder subsystem is shutting down"));
			}
		});
	});
	return Result.Get();
}

FString USuperEmbedderToolset::ListSets()
{
	const FString EncoderFingerprint = ReadyFingerprint(USuperEmbedderSubsystem::Get());

	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	Registry.SearchAllAssets(/*bSynchronousSearch*/ true);
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(USuperEmbedderVectorSet::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses*/ true);

	TArray<TSharedPtr<FJsonValue>> SetValues;
	for (const FAssetData& Asset : Assets)
	{
		if (const USuperEmbedderVectorSet* Set = Cast<USuperEmbedderVectorSet>(Asset.GetAsset()))
		{
			SetValues.Add(MakeShared<FJsonValueObject>(SetSummary(*Set, EncoderFingerprint)));
		}
		else
		{
			const TSharedRef<FJsonObject> Failed = MakeShared<FJsonObject>();
			Failed->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			Failed->SetBoolField(TEXT("asset"), true);
			Failed->SetStringField(TEXT("loadStatus"), TEXT("did not load"));
			SetValues.Add(MakeShared<FJsonValueObject>(Failed));
		}
	}
	// Live run-time sets (New Vector Set), which are not assets. Asset sets already loaded are skipped here:
	// the registry listed them above.
	for (TObjectIterator<USuperEmbedderVectorSet> It; It; ++It)
	{
		const USuperEmbedderVectorSet* Set = *It;
		if (Set->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Set->IsAsset() || !IsValid(Set))
		{
			continue;
		}
		SetValues.Add(MakeShared<FJsonValueObject>(SetSummary(*Set, EncoderFingerprint)));
	}

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("count"), SetValues.Num());
	if (!EncoderFingerprint.IsEmpty())
	{
		Object->SetStringField(TEXT("encoderFingerprint"), EncoderFingerprint);
	}
	Object->SetArrayField(TEXT("sets"), SetValues);
	return ToJson(Object);
}

UToolCallAsyncResultString* USuperEmbedderToolset::ValidateSets()
{
	const FPendingResult Result = NewPendingResult();
	WithReadyEncoder(Result, [Result](USuperEmbedderSubsystem* Subsystem, FHold Hold)
	{
		TArray<FString> Report;
		const int32 Listed = FSuperEmbedderValidate::ValidateAllSets(Report);
		ReleaseHold(Hold);
		if (Listed < 0)
		{
			Result->SetError(Report.Num() > 0 ? Report[0] : FString(TEXT("the encoder is not Ready")));
			return;
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("passed"), Listed == 0);
		Object->SetNumberField(TEXT("checked"), Report.Num());
		Object->SetNumberField(TEXT("listed"), Listed);
		Object->SetStringField(TEXT("encoderFingerprint"), ReadyFingerprint(Subsystem));
		TArray<TSharedPtr<FJsonValue>> Lines;
		for (const FString& Line : Report)
		{
			Lines.Add(MakeShared<FJsonValueString>(Line));
		}
		Object->SetArrayField(TEXT("report"), Lines);
		Result->SetValue(ToJson(Object));
	});
	return Result.Get();
}
