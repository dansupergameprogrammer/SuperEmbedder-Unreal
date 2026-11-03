#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "SuperEmbedderExample.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ExampleProject, "ExampleProject");

DEFINE_LOG_CATEGORY_STATIC(LogSuperEmbedderExampleCommands, Log, All);

// The example's console command. It works in every build configuration, from the console or from -ExecCmds.
namespace
{
	FAutoConsoleCommandWithWorldAndArgs GAskCommand(
		TEXT("SuperEmbedderExample.Ask"),
		TEXT("SuperEmbedderExample.Ask <text>: ranks the example's vector set against the text and shows the top K rows."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ASuperEmbedderExample* Example = ASuperEmbedderExample::Find(World);
			if (Example == nullptr)
			{
				UE_LOG(LogSuperEmbedderExampleCommands, Warning, TEXT("No example in this world; start the game with the example's game mode."));
				return;
			}
			const FString Text = FString::Join(Args, TEXT(" "));
			if (Text.IsEmpty())
			{
				UE_LOG(LogSuperEmbedderExampleCommands, Warning, TEXT("Usage: SuperEmbedderExample.Ask <text>"));
				return;
			}
			Example->Ask(Text);
		}));
}
