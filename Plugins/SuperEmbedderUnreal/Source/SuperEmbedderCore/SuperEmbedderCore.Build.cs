using System;
using System.IO;
using System.Text.RegularExpressions;
using UnrealBuildTool;

// SuperEmbedderCore (proposal rev 9, section 2.5): the vendored SuperEmbedder library and ITS OWN renamed copy
// of the SuperSLM engine, compiled from source with the engine's toolchain through one wrapper .cpp per
// vendored source (Private/Vendored/), the SuperSLMUnreal pattern. No prebuilt library is consumed.
//
// Own-copy mode is the only mode in plugin 1.0 (section 3.1). The SuperSLM plugin vendors a different engine
// release, so two engine copies meet in one monolithic link; the generated rename header, force-included into
// every translation unit here, gives this copy's every engine identifier the semb_ prefix. No vendored file is
// edited. This module depends on no other plugin.
public class SuperEmbedderCore : ModuleRules
{
	public SuperEmbedderCore(ReadOnlyTargetRules Target) : base(Target)
	{
		// The pins (design section 2.5): no PCH, no unity, precise floating point.
		// Unity would merge vendored TUs whose anonymous-namespace helpers collide; a PCH would compile UE
		// headers under the rename macros.
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		// The engine's bit-equality across its SIMD tiers depends on exact mul-then-add rounding, and
		// SuperEmbedder's own build asserts the precise-FP flag on every target (cmake/BuildFlags.cmake).
		FPSemantics = FPSemanticsMode.Precise;
		// Both libraries use ordinary try/catch/throw (each C entry point narrows exceptions to a status).
		bEnableExceptions = true;
		// Both vendored CMake projects set CMAKE_CXX_STANDARD 20.
		CppStandard = CppStandardVersion.Cpp20;

		string ThirdParty = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "ThirdParty"));
		string Generated = Path.Combine(ModuleDirectory, "Private", "Generated");

		// Consumers see semb.h only. The engine headers stay private: a consumer compiles without the rename,
		// so it must never name an engine identifier.
		PublicIncludePaths.Add(Path.Combine(ThirdParty, "SuperEmbedder", "include"));
		PrivateIncludePaths.Add(Path.Combine(ThirdParty, "SuperSLM", "include"));
		PrivateIncludePaths.Add(Generated);

		PublicDependencyModuleNames.Add("Core");

		// semb.h's SEMB_API: dllexport while this module compiles and dllimport for its consumers in a modular
		// build, where each module is a DLL; empty in a monolithic build, where everything is one image.
		if (Target.LinkType == TargetLinkType.Modular)
		{
			PrivateDefinitions.Add("SEMB_BUILDING_LIBRARY=1");
			PublicDefinitions.Add("SEMB_SHARED=1");
		}

		// SuperEmbedder's build descriptor (certification.cpp). "ubt" matches no certified cell, so plugin 1.0
		// opens uncertified, as the proposal states (certification lands in 1.1).
		PrivateDefinitions.Add("SEMB_BUILD_CONFIGURATION=\"ubt-" + Target.Configuration + "\"");
		PrivateDefinitions.Add("SEMB_BUILD_SYSTEM=\"ubt\"");

		// The rename header. The list depends on which words the toolchain's standard library and SDK use,
		// so the header is derived per preprocessor (ci/generate_rename_header.py) and the MSVC one is
		// derived with cl /P on Windows. A missing header fails here, by name: UBT only warns when a
		// force-include path does not resolve, and an unrenamed engine copy links beside the SuperSLM plugin's
		// with duplicate symbols, or silently merges weak ones.
		string Flavor = Target.Platform == UnrealTargetPlatform.Win64 ? "msvc" : "gcc";
		string RenameHeaderName = "SuperEmbedderCoreRename." + Flavor + ".h";
		string RenameHeader = Path.Combine(Generated, RenameHeaderName);
		if (!File.Exists(RenameHeader))
		{
			throw new BuildException("SuperEmbedderCore: SEMB_UE_RENAME_HEADER_MISSING: " + RenameHeader +
				" does not exist. Derive it with ci/generate_rename_header.py --preprocessor " + Flavor +
				" (see CONTRIBUTING.md, Moving the pins).");
		}
		ForceIncludeFiles.Add(RenameHeader);
		// The module's own UE source includes the header a second time to remove the macros before any UE
		// header (Private/SuperEmbedderCoreModule.cpp).
		PrivateDefinitions.Add("SUPEREMBEDDERCORE_RENAME_HEADER=\"" + RenameHeaderName + "\"");

		CheckEngineBuildRecord(ThirdParty, Generated);
	}

	// The engine identity record (section 3.1) is the engine tag, the engine commit, "ubt-own-copy" and the
	// plugin version. ci/generate_semb_config.py writes it into both sides of the link check; this refuses a
	// generated header that no longer states the vendored tag or the .uplugin's version (re-run the generator).
	private void CheckEngineBuildRecord(string ThirdParty, string Generated)
	{
		string Tag = null, Commit = null;
		foreach (string Line in File.ReadAllLines(Path.Combine(ThirdParty, "SuperSLM", "VENDORED_VERSION.txt")))
		{
			string T = Line.Trim();
			if (Tag == null && T.StartsWith("Tag:")) { Tag = T.Substring(4).Trim(); }
			else if (Commit == null && T.StartsWith("Commit:")) { Commit = T.Substring(7).Trim(); }
		}
		Match Version = Regex.Match(File.ReadAllText(Path.Combine(PluginDirectory, "SuperEmbedderUnreal.uplugin")),
			"\"VersionName\"\\s*:\\s*\"([^\"]*)\"");
		if (Tag == null || Commit == null || !Version.Success)
		{
			throw new BuildException("SuperEmbedderCore: could not read the engine Tag/Commit or the plugin VersionName");
		}
		string Expected = "ubt-own-copy;engine=" + Tag + ";engine_commit=" + Commit + ";plugin=" + Version.Groups[1].Value;
		foreach (string Name in new string[] { "semb_engine_pin.h", "superslm_build_record.inc" })
		{
			string Text = File.ReadAllText(Path.Combine(Generated, Name));
			if (!Text.Contains("\"" + Expected + "\""))
			{
				throw new BuildException("SuperEmbedderCore: SEMB_UE_BUILD_RECORD_STALE: " + Name + " does not carry " +
					Expected + ". Re-run ci/generate_semb_config.py.");
			}
		}
	}
}
