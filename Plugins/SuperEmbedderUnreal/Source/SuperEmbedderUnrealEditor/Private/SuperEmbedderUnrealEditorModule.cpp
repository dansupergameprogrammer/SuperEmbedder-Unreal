// The editor module: the DataTable context-menu bake, and console commands for the bake and validation.
#include "AssetRegistry/AssetData.h"
#include "ContentBrowserMenuContexts.h"
#include "Engine/DataTable.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"

#include "SuperEmbedderBake.h"

#define LOCTEXT_NAMESPACE "SuperEmbedderEditor"

namespace
{
	void LogFinished(ESuperEmbedderStatus, const FString&)
	{
		// FSuperEmbedderBake logs and updates its toast itself; nothing more to do.
	}

	FAutoConsoleCommand GBakeJsonl(
		TEXT("SuperEmbedder.BakeJsonl"),
		TEXT("SuperEmbedder.BakeJsonl <rows.jsonl> </Game/Path/SetName>: bakes a vector set asset from JSONL ({\"id\", \"text\"} per line)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() != 2)
			{
				UE_LOG(LogTemp, Error, TEXT("usage: SuperEmbedder.BakeJsonl <rows.jsonl> </Game/Path/SetName>"));
				return;
			}
			FSuperEmbedderBake::StartFromJsonlFile(Args[0], Args[1], /*bNotify*/ true, &LogFinished);
		}));

	FAutoConsoleCommand GBakeDataTable(
		TEXT("SuperEmbedder.BakeDataTable"),
		TEXT("SuperEmbedder.BakeDataTable </Game/Path/Table.Table> </Game/Path/SetName>: bakes a vector set asset from a DataTable (row name = id, a Text property = text)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() != 2)
			{
				UE_LOG(LogTemp, Error, TEXT("usage: SuperEmbedder.BakeDataTable </Game/Path/Table.Table> </Game/Path/SetName>"));
				return;
			}
			FSuperEmbedderBake::StartFromDataTable(LoadObject<UDataTable>(nullptr, *Args[0]), Args[1], /*bNotify*/ true, &LogFinished);
		}));

	FAutoConsoleCommand GCancelBake(
		TEXT("SuperEmbedder.CancelBake"),
		TEXT("Cancels the running SuperEmbedder bake; nothing is written."),
		FConsoleCommandDelegate::CreateStatic(&FSuperEmbedderBake::Cancel));

	FAutoConsoleCommand GValidateSets(
		TEXT("SuperEmbedder.ValidateSets"),
		TEXT("Lists every vector set whose row digest fails or whose fingerprint differs from the open encoder's."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			TArray<FString> Report;
			const int32 Listed = FSuperEmbedderValidate::ValidateAllSets(Report);
			for (const FString& Line : Report)
			{
				UE_LOG(LogTemp, Display, TEXT("SuperEmbedder: %s"), *Line);
			}
			UE_LOG(LogTemp, Display, TEXT("SuperEmbedder: %d set(s) listed"), FMath::Max(0, Listed));
		}));
}

class FSuperEmbedderUnrealEditorModule final : public IModuleInterface
{
public:
	void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSuperEmbedderUnrealEditorModule::RegisterMenus));
	}

	void ShutdownModule() override
	{
		FSuperEmbedderBake::Cancel();
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.DataTable");
		if (Menu == nullptr)
		{
			return;
		}
		FToolMenuSection& Section = Menu->FindOrAddSection("GetAssetActions");
		Section.AddDynamicEntry("SuperEmbedderBake", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
		{
			const UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
			if (Context == nullptr || Context->SelectedAssets.Num() != 1)
			{
				return;
			}
			const FAssetData Asset = Context->SelectedAssets[0];
			InSection.AddMenuEntry(
				"SuperEmbedderBakeVectorSet",
				LOCTEXT("BakeLabel", "Bake SuperEmbedder Vector Set"),
				LOCTEXT("BakeTip", "Encodes every row (row name = id, the Text property = text) and writes <Table>_VectorSet beside it. Runs in the background; Cancel on the progress toast leaves nothing behind."),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([Asset]()
					{
						const UDataTable* Table = Cast<UDataTable>(Asset.GetAsset());
						const FString PackageName = Asset.PackagePath.ToString() / (Asset.AssetName.ToString() + TEXT("_VectorSet"));
						FSuperEmbedderBake::StartFromDataTable(Table, PackageName, /*bNotify*/ true, &LogFinished);
					}),
					FCanExecuteAction::CreateLambda([]() { return !FSuperEmbedderBake::IsRunning(); })));
		}));
	}
};

IMPLEMENT_MODULE(FSuperEmbedderUnrealEditorModule, SuperEmbedderUnrealEditor)

#undef LOCTEXT_NAMESPACE
