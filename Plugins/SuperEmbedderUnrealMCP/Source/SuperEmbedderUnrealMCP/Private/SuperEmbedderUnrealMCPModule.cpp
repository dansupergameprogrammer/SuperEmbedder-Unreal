#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

#include "SuperEmbedderToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

// Registers the SuperEmbedder toolset with the engine's ToolsetRegistry, from which the MCP plugin's
// adapter discovers it. This module never depends on the MCP plugin itself. Registration waits for
// OnPostEngineInit, because the registry's backing editor subsystem does not exist when the module loads
// (the SuperFAISS toolset found this at its first gate).
//
// Nothing here owns a thread or a queue: every asynchronous tool runs on the SuperEmbedder subsystem's
// own workers, so there is no host to shut down at engine pre-exit. At engine shutdown the subsystem
// cancels its tickets without delivering them; a call still pending then is dropped with the callbacks
// that held it, and its client sees the editor's MCP server go away.
class FSuperEmbedderUnrealMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddLambda([]()
		{
			UToolsetRegistry::RegisterToolsetClass(USuperEmbedderToolset::StaticClass());
		});
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		if (UObjectInitialized())
		{
			UToolsetRegistry::UnregisterToolsetClass(USuperEmbedderToolset::StaticClass());
		}
	}

private:
	FDelegateHandle PostEngineInitHandle;
};

IMPLEMENT_MODULE(FSuperEmbedderUnrealMCPModule, SuperEmbedderUnrealMCP)
