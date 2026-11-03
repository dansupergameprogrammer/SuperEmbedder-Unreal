using UnrealBuildTool;

// The runtime module (proposal rev 9, section 2.5): the subsystem, the encode surface, vector sets, similarity,
// the cache and the Blueprint nodes. A thin UE layer over the engine-free cores in Private/Core/, which the
// host suite (ci/host/cores/) builds and tests on GCC without Unreal.
//
// It sees SuperEmbedder only through semb.h and SuperEmbedderCore's one extra verb (SuperEmbedderCoreExtras.h),
// compiled without the engine rename: no engine identifier is ever named here. It depends on no other plugin.
public class SuperEmbedderUnreal : ModuleRules
{
	public SuperEmbedderUnreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// The cores are plain C++ with file-scope helpers; keep each a translation unit of its own, as the host
		// suite builds them (it also builds them as one unity TU, which compiles clean).
		bUseUnity = false;
		FPSemantics = FPSemanticsMode.Precise;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });
		PrivateDependencyModuleNames.AddRange(new string[] { "SuperEmbedderCore" });

		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "Private", "Core"));
	}
}
