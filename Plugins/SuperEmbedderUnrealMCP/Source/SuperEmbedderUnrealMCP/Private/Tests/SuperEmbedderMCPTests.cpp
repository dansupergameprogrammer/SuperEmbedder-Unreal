// The MCP toolset's automation cells (proposal rev 9, section 10: P-1 to P-3). The tool handlers are plain
// reflected statics, so the cells call them directly, as the SuperFAISS and SuperSLM toolsets' suites do; the
// protocol layer is the engine's to test.
//
// - SyncTools: EncoderStatus and ListSets answer with the documented fields, open nothing, and ListSets
//   names a live run-time set.
// - Refusals: the arguments a QueryText refuses before any encode (K out of range, an empty or unknown set
//   path) complete at once with a named error.
// - EncodeAndQuery (needs the model in Project Settings; a missing file fails the cell by name): EncodeText
//   twice gives one digest and a summary, never the whole vector; QueryText against a run-time set of three
//   rows returns them ranked, best first, with their texts; ValidateSets answers.
//
// Asynchronous tools return a pending UToolCallAsyncResultString, so a cell is a chain of latent commands
// that waits one frame at a time for bIsComplete, with a deadline that turns a result that never completes
// into a named failure, never a hang or a pass. A success completes with SetValue (a JSON object) and an empty
// Error; a failure with SetError and no Value.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SuperEmbedderBlueprintLibrary.h"
#include "SuperEmbedderSettings.h"
#include "SuperEmbedderSubsystem.h"
#include "SuperEmbedderToolset.h"
#include "SuperEmbedderVectorSet.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	// The open takes 20-38 s on the published model; three short encodes and a three-row documents ticket
	// add a few seconds each on the CPU.
	constexpr double kEncoderTimeoutSeconds = 300.0;

	TSharedPtr<FJsonObject> ParseTool(const FString& Result)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Result);
		if (!FJsonSerializer::Deserialize(Reader, Object))
		{
			return nullptr;
		}
		return Object;
	}

	struct FAwaitedTool
	{
		TStrongObjectPtr<UToolCallAsyncResultString> Pending;
		double DeadlineSeconds = 0.0;
		bool bCompleted = false;
		FString Value;
		FString Error;
	};

	void EnqueueStep(TFunction<void()> Step)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Step]() { Step(); return true; }));
	}

	/** Starts a tool in one latent command and waits for its result in the next. */
	void EnqueueToolAwait(FAutomationTestBase* Test, const FString& Label, TFunction<UToolCallAsyncResultString*()> Start,
		TSharedRef<FAwaitedTool> Out, double TimeoutSeconds)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Label, Start, Out, TimeoutSeconds]()
		{
			UToolCallAsyncResultString* Result = Start();
			if (Result == nullptr)
			{
				Test->AddError(FString::Printf(TEXT("%s: the tool returned no result object"), *Label));
				return true;
			}
			Out->Pending.Reset(Result);
			Out->DeadlineSeconds = FPlatformTime::Seconds() + TimeoutSeconds;
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Label, Out, TimeoutSeconds]()
		{
			if (!Out->Pending.IsValid())
			{
				return true;
			}
			if (Out->Pending->bIsComplete)
			{
				Out->Value = Out->Pending->Value;
				Out->Error = Out->Pending->Error;
				Out->bCompleted = true;
				Out->Pending.Reset();
				return true;
			}
			if (FPlatformTime::Seconds() > Out->DeadlineSeconds)
			{
				Test->AddError(FString::Printf(TEXT("%s: not complete after %.0f s"), *Label, TimeoutSeconds));
				Out->Pending.Reset();
				return true;
			}
			return false;
		}));
	}

	TSharedPtr<FJsonObject> ExpectToolSuccess(FAutomationTestBase* Test, const FString& Label, const FAwaitedTool& Tool)
	{
		if (!Tool.bCompleted)
		{
			return nullptr;
		}
		if (!Tool.Error.IsEmpty())
		{
			Test->AddError(FString::Printf(TEXT("%s failed: %s"), *Label, *Tool.Error));
			return nullptr;
		}
		TSharedPtr<FJsonObject> Object = ParseTool(Tool.Value);
		if (!Object.IsValid())
		{
			Test->AddError(FString::Printf(TEXT("%s: the value is not a JSON object: %s"), *Label, *Tool.Value));
		}
		return Object;
	}

	/** A refusal that completes inside the call: no encoder, no frame. */
	void ExpectImmediateError(FAutomationTestBase* Test, const FString& Label, UToolCallAsyncResultString* Result, const FString& Contains)
	{
		if (!Test->TestNotNull(*FString::Printf(TEXT("%s returns a result object"), *Label), Result))
		{
			return;
		}
		Test->TestTrue(*FString::Printf(TEXT("%s completes at once"), *Label), Result->bIsComplete);
		Test->TestTrue(*FString::Printf(TEXT("%s is an error naming %s (got '%s')"), *Label, *Contains, *Result->Error),
			Result->Error.Contains(Contains));
		Test->TestTrue(*FString::Printf(TEXT("%s carries no value"), *Label), Result->Value.IsEmpty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperEmbedderMCPSyncToolsTest,
	"SuperEmbedder.MCP.SyncTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperEmbedderMCPSyncToolsTest::RunTest(const FString& Parameters)
{
	const USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
	if (!TestNotNull(TEXT("the SuperEmbedder subsystem exists"), Subsystem))
	{
		return false;
	}
	const ESuperEmbedderEncoderState Before = Subsystem->GetEncoderState();

	const TSharedPtr<FJsonObject> Status = ParseTool(USuperEmbedderToolset::EncoderStatus());
	if (!TestTrue(TEXT("EncoderStatus is a JSON object"), Status.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("EncoderStatus is not an error"), Status->HasField(TEXT("error")));
	for (const TCHAR* Field : { TEXT("state"), TEXT("ready"), TEXT("openStatus"), TEXT("dims"), TEXT("fingerprint"), TEXT("cacheEntries"), TEXT("settings") })
	{
		TestTrue(*FString::Printf(TEXT("EncoderStatus has %s"), Field), Status->HasField(Field));
	}
	const TSharedPtr<FJsonObject>* Settings = nullptr;
	if (TestTrue(TEXT("EncoderStatus.settings is an object"), Status->TryGetObjectField(TEXT("settings"), Settings)))
	{
		TestEqual(TEXT("EncoderStatus.settings.maxQueryTokens"), static_cast<int32>((*Settings)->GetNumberField(TEXT("maxQueryTokens"))),
			GetDefault<USuperEmbedderSettings>()->MaxQueryTokens);
		TestEqual(TEXT("EncoderStatus.settings.modelFile is resolved"), (*Settings)->GetStringField(TEXT("modelFile")),
			USuperEmbedderSettings::ResolvePath(GetDefault<USuperEmbedderSettings>()->ModelFile));
	}

	TStrongObjectPtr<USuperEmbedderVectorSet> Live(USuperEmbedderBlueprintLibrary::NewVectorSet(nullptr));
	const TSharedPtr<FJsonObject> Listed = ParseTool(USuperEmbedderToolset::ListSets());
	if (!TestTrue(TEXT("ListSets is a JSON object"), Listed.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Sets = nullptr;
	if (TestTrue(TEXT("ListSets.sets is an array"), Listed->TryGetArrayField(TEXT("sets"), Sets)))
	{
		TestEqual(TEXT("ListSets.count equals the array's length"), static_cast<int32>(Listed->GetNumberField(TEXT("count"))), Sets->Num());
		const TSharedPtr<FJsonObject>* Found = nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Sets)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (Value->TryGetObject(Entry) && (*Entry)->GetStringField(TEXT("path")) == Live->GetPathName())
			{
				Found = Entry;
			}
		}
		if (TestNotNull(TEXT("ListSets names the live run-time set"), Found))
		{
			TestFalse(TEXT("the live set is not an asset"), (*Found)->GetBoolField(TEXT("asset")));
			TestEqual(TEXT("the live set has no rows"), static_cast<int32>((*Found)->GetNumberField(TEXT("rows"))), 0);
			TestEqual(TEXT("the live set's load status"), (*Found)->GetStringField(TEXT("loadStatus")), LexToString(ESuperEmbedderStatus::Ok));
		}
	}
	TestTrue(TEXT("the synchronous tools open nothing"), Subsystem->GetEncoderState() == Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperEmbedderMCPRefusalsTest,
	"SuperEmbedder.MCP.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperEmbedderMCPRefusalsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<USuperEmbedderVectorSet> Live(USuperEmbedderBlueprintLibrary::NewVectorSet(nullptr));
	const FString LivePath = Live->GetPathName();
	ExpectImmediateError(this, TEXT("QueryText K=0"), USuperEmbedderToolset::QueryText(LivePath, TEXT("a door"), 0), TEXT("InvalidK"));
	ExpectImmediateError(this, TEXT("QueryText K=-1"), USuperEmbedderToolset::QueryText(LivePath, TEXT("a door"), -1), TEXT("InvalidK"));
	ExpectImmediateError(this, TEXT("QueryText K=1001"), USuperEmbedderToolset::QueryText(LivePath, TEXT("a door"), 1001), TEXT("InvalidK"));
	ExpectImmediateError(this, TEXT("QueryText with no set path"), USuperEmbedderToolset::QueryText(TEXT(""), TEXT("a door"), 5), TEXT("SetPath is empty"));
	ExpectImmediateError(this, TEXT("QueryText with an unknown set"),
		USuperEmbedderToolset::QueryText(TEXT("/Game/SuperEmbedderMCPTests/NoSuchSet"), TEXT("a door"), 5), TEXT("no vector set at"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSuperEmbedderMCPEncodeAndQueryTest,
	"SuperEmbedder.MCP.EncodeAndQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSuperEmbedderMCPEncodeAndQueryTest::RunTest(const FString& Parameters)
{
	const FString ModelFile = USuperEmbedderSettings::ResolvePath(GetDefault<USuperEmbedderSettings>()->ModelFile);
	const FString TokenizerFile = USuperEmbedderSettings::ResolvePath(GetDefault<USuperEmbedderSettings>()->TokenizerFile);
	if (!IFileManager::Get().FileExists(*ModelFile) || !IFileManager::Get().FileExists(*TokenizerFile))
	{
		AddError(FString::Printf(TEXT("the model files named in Project Settings are missing: %s, %s"), *ModelFile, *TokenizerFile));
		return false;
	}

	FAutomationTestBase* Test = this;
	const TSharedRef<FAwaitedTool> First = MakeShared<FAwaitedTool>();
	const TSharedRef<FAwaitedTool> Second = MakeShared<FAwaitedTool>();
	EnqueueToolAwait(Test, TEXT("EncodeText #1"), []() { return USuperEmbedderToolset::EncodeText(TEXT("a lantern by the door")); }, First, kEncoderTimeoutSeconds);
	EnqueueToolAwait(Test, TEXT("EncodeText #2"), []() { return USuperEmbedderToolset::EncodeText(TEXT("a lantern by the door")); }, Second, kEncoderTimeoutSeconds);
	EnqueueStep([Test, First, Second]()
	{
		const TSharedPtr<FJsonObject> A = ExpectToolSuccess(Test, TEXT("EncodeText #1"), *First);
		const TSharedPtr<FJsonObject> B = ExpectToolSuccess(Test, TEXT("EncodeText #2"), *Second);
		if (!A.IsValid() || !B.IsValid())
		{
			return;
		}
		Test->TestEqual(TEXT("EncodeText gives one digest for one text"), A->GetStringField(TEXT("digest")), B->GetStringField(TEXT("digest")));
		Test->TestEqual(TEXT("EncodeText digest is SHA-256 hex"), A->GetStringField(TEXT("digest")).Len(), 64);
		Test->TestEqual(TEXT("EncodeText fingerprint is 64 hex"), A->GetStringField(TEXT("fingerprint")).Len(), 64);
		Test->TestEqual(TEXT("EncodeText kind"), A->GetStringField(TEXT("kind")), FString(TEXT("Query")));
		Test->TestTrue(TEXT("EncodeText dims are positive"), A->GetNumberField(TEXT("dims")) > 0);
		Test->TestTrue(TEXT("EncodeText l2Norm is positive"), A->GetNumberField(TEXT("l2Norm")) > 0.0);
		Test->TestEqual(TEXT("EncodeText shows 8 leading components, not the vector"), A->GetArrayField(TEXT("head")).Num(), 8);
		Test->TestFalse(TEXT("EncodeText carries no values array"), A->HasField(TEXT("values")));
	});

	// A run-time set of three rows, through the documents lane, then ranked through QueryText.
	struct FSetBox
	{
		TStrongObjectPtr<USuperEmbedderVectorSet> Set;
		bool bDone = false;
		double Deadline = 0.0;
	};
	const TSharedRef<FSetBox> Box = MakeShared<FSetBox>();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Box]()
	{
		USuperEmbedderSubsystem* Subsystem = USuperEmbedderSubsystem::Get();
		Box->Set.Reset(USuperEmbedderBlueprintLibrary::NewVectorSet(nullptr));
		Box->Deadline = FPlatformTime::Seconds() + kEncoderTimeoutSeconds;
		TArray<FSuperEmbedderDocumentRow> Rows;
		const TCHAR* Texts[][2] = {
			{ TEXT("forge"), TEXT("The blacksmith's forge, hot with hammered iron and swords.") },
			{ TEXT("library"), TEXT("A quiet library of old books and maps.") },
			{ TEXT("harbor"), TEXT("A harbor of fishing boats and nets at dawn.") },
		};
		TArray<FString> RowTexts;
		for (const auto& Row : Texts)
		{
			FSuperEmbedderDocumentRow R;
			R.Id = Row[0];
			R.Text = Row[1];
			Rows.Add(R);
			RowTexts.Add(R.Text);
		}
		Subsystem->EncodeDocuments(Rows, nullptr, [Test, Box, RowTexts](const FSuperEmbedderDocumentsResult& Result)
		{
			Box->bDone = true;
			if (Result.Outcome != ESuperEmbedderOutcome::Completed)
			{
				Test->AddError(FString::Printf(TEXT("EncodeDocuments: %s %s"), *LexToString(Result.Status), *Result.Message));
				return;
			}
			FString Message;
			const ESuperEmbedderStatus Appended = Box->Set->AppendDocuments(Result, RowTexts, Message);
			Test->TestTrue(FString::Printf(TEXT("AppendDocuments: %s %s"), *LexToString(Appended), *Message), Appended == ESuperEmbedderStatus::Ok);
		});
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Box]()
	{
		if (Box->bDone)
		{
			return true;
		}
		if (FPlatformTime::Seconds() > Box->Deadline)
		{
			Test->AddError(TEXT("EncodeDocuments: not complete in time"));
			return true;
		}
		return false;
	}));

	const TSharedRef<FAwaitedTool> Query = MakeShared<FAwaitedTool>();
	EnqueueToolAwait(Test, TEXT("QueryText"), [Box]()
	{
		return USuperEmbedderToolset::QueryText(Box->Set.IsValid() ? Box->Set->GetPathName() : FString(), TEXT("where can I buy a sword"), 10);
	}, Query, kEncoderTimeoutSeconds);
	EnqueueStep([Test, Box, Query]()
	{
		const TSharedPtr<FJsonObject> Object = ExpectToolSuccess(Test, TEXT("QueryText"), *Query);
		if (!Object.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>& Hits = Object->GetArrayField(TEXT("hits"));
		Test->TestEqual(TEXT("QueryText with K above the row count returns every row"), Hits.Num(), 3);
		double Previous = 2.0;
		for (const TSharedPtr<FJsonValue>& Value : Hits)
		{
			const TSharedPtr<FJsonObject> Hit = Value->AsObject();
			const double Score = Hit->GetNumberField(TEXT("score"));
			Test->TestTrue(TEXT("QueryText ranks best first"), Score <= Previous);
			Test->TestTrue(TEXT("QueryText scores are cosines"), Score >= -1.0 && Score <= 1.0);
			Test->TestTrue(TEXT("QueryText hits carry their row text"), !Hit->GetStringField(TEXT("text")).IsEmpty());
			Previous = Score;
		}
		Test->TestEqual(TEXT("QueryText queryDigest is SHA-256 hex"), Object->GetStringField(TEXT("queryDigest")).Len(), 64);
	});

	const TSharedRef<FAwaitedTool> Validate = MakeShared<FAwaitedTool>();
	EnqueueToolAwait(Test, TEXT("ValidateSets"), []() { return USuperEmbedderToolset::ValidateSets(); }, Validate, kEncoderTimeoutSeconds);
	EnqueueStep([Test, Validate, Box]()
	{
		const TSharedPtr<FJsonObject> Object = ExpectToolSuccess(Test, TEXT("ValidateSets"), *Validate);
		if (Object.IsValid())
		{
			Test->TestEqual(TEXT("ValidateSets.checked equals its report lines"),
				static_cast<int32>(Object->GetNumberField(TEXT("checked"))), Object->GetArrayField(TEXT("report")).Num());
			Test->TestEqual(TEXT("ValidateSets.passed iff nothing is listed"), Object->GetBoolField(TEXT("passed")),
				static_cast<int32>(Object->GetNumberField(TEXT("listed"))) == 0);
		}
		Box->Set.Reset();
	});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
