using UnrealBuildTool;

// The editor module (proposal rev 9, section 2.4): bake a vector set from a DataTable or JSONL through the
// documents encode path, and validate every vector set. Model import (the asset factory and Verify) is not in
// 1.0: the runtime reads the model files named in Project Settings. Depends on the runtime module only.
public class SuperEmbedderUnrealEditor : ModuleRules
{
	public SuperEmbedderUnrealEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;
		FPSemantics = FPSemanticsMode.Precise;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"AssetRegistry",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"ContentBrowser",
			"SuperEmbedderUnreal",
		});
	}
}
