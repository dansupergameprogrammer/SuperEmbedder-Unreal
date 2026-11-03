using UnrealBuildTool;

// SuperEmbedder's MCP toolset (proposal rev 9, section 2.4), a sibling plugin in SuperFAISSUnrealMCP's
// shape: disabled by default, with a hard dependency on the Experimental ToolsetRegistry engine plugin,
// which is absent from stock distributions. Leaving the plugin disabled is how it compiles out on a
// stock engine. A preprocessor stub is not possible: UHT forbids reflected types inside preprocessor
// blocks, and the toolset class must be reflected. It never depends on the ModelContextProtocol plugin
// itself: that plugin's adapter discovers the toolset through the registry.
public class SuperEmbedderUnrealMCP : ModuleRules
{
	public SuperEmbedderUnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"Json",
			"SuperEmbedderUnreal",
			"SuperEmbedderUnrealEditor", // FSuperEmbedderValidate
			"ToolsetRegistry",
			"UnrealEd",
		});
	}
}
