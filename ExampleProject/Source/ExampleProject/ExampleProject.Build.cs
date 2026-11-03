using UnrealBuildTool;

public class ExampleProject : ModuleRules
{
	public ExampleProject(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		// The example drives the plugin through its Blueprint-callable API only (the async action nodes, the
		// vector set and the Blueprint library), called from C++ the way a Blueprint graph calls them. Nothing
		// here reads the plugin's Private/ tree or the vendored headers.
		PrivateDependencyModuleNames.AddRange(new string[] { "SuperEmbedderUnreal" });
	}
}
