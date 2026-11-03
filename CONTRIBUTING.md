# Contributing to SuperEmbedder-Unreal

## Scope

This repository is the Unreal Engine 5.8 integration of SuperEmbedder. The library lives in the
[SuperEmbedder repository](https://github.com/dansupergameprogrammer/SuperEmbedder) and the engine under it in the
[SuperSLM repository](https://github.com/dansupergameprogrammer/SuperSLM). Both are vendored, at pinned release
tags, under `Plugins/SuperEmbedderUnreal/Source/ThirdParty/`.

**Do not edit the vendored trees.** A change to SuperEmbedder or SuperSLM is made upstream, released, and brought
in by moving the pin (see *Moving the pins* below). A change that would be easier with a change to an upstream API
is proposed upstream on its own merits.

## Layout

| Path | What it is |
|---|---|
| `Plugins/SuperEmbedderUnreal/Source/SuperEmbedderCore/` | The vendored library and its renamed engine copy: one wrapper `.cpp` per compiled vendored source under `Private/Vendored/`, and the generated configuration and rename headers under `Private/Generated/` (committed). No precompiled header, no unity build, precise floating point, C++20. |
| `Plugins/SuperEmbedderUnreal/Source/SuperEmbedderUnreal/Private/Core/` | The engine-free cores: standard C++ only, no exceptions, no RTTI, no Unreal header. The encode service, cache, vector set, similarity, JSONL reader, SHA-256 and text rules. |
| `Plugins/SuperEmbedderUnreal/Source/SuperEmbedderUnreal/` (the rest) | The Unreal layer over the cores: the subsystem, handle, vector set asset, async actions, Blueprint library and settings. |
| `Plugins/SuperEmbedderUnreal/Source/SuperEmbedderUnrealEditor/` | The bake, validation, commandlets, the DataTable menu entry and console commands. |
| `Plugins/SuperEmbedderUnreal/ci/` | The generators and checks below. |
| `Plugins/SuperEmbedderUnrealMCP/` | The optional MCP toolset. |
| `ExampleProject/` | The example. |

## Building

Unreal Engine 5.8 on Windows x64 with its Visual Studio toolchain. Open `ExampleProject/ExampleProject.uproject`,
which finds the plugins in `Plugins/` through `"AdditionalPluginDirectories": ["../Plugins"]`, and build when
prompted, or:

```
<UE>\Engine\Build\BatchFiles\Build.bat ExampleProjectEditor Win64 Development -Project="<repo>\ExampleProject\ExampleProject.uproject" -WaitMutex
```

To build the MCP toolset, enable `SuperEmbedderUnrealMCP` and `ModelContextProtocol` in the example's
`.uproject`, on an engine that has the Experimental `ToolsetRegistry` and `ModelContextProtocol` plugins.

## Testing

**Checks that need only Python** (any OS), from the repository root:

```sh
python3 Plugins/SuperEmbedderUnreal/ci/check_vendored_tree_matches_manifest.py --plugin-root Plugins/SuperEmbedderUnreal
python3 Plugins/SuperEmbedderUnreal/ci/check_vendored_wrappers_complete.py --plugin-root Plugins/SuperEmbedderUnreal
python3 Plugins/SuperEmbedderUnreal/ci/generate_semb_config.py --plugin-root Plugins/SuperEmbedderUnreal --check
python3 -m pytest Plugins/SuperEmbedderUnrealMCP/ci/tests ExampleProject/tests
```

They check that the vendored trees match their manifests, that every compiled vendored source has exactly one
wrapper, that the committed configuration headers are current, that the MCP toolset's surface matches its README,
and the example's layout.

**The host suites** (Linux, GCC 13.3, CMake), which need SuperEmbedder's synthetic model pair and, for the
coexistence checks, SuperSLM-Unreal's vendored engine; [Running beside SuperSLM-Unreal](docs/COEXISTENCE.md#running-the-checks-yourself)
says how to get both:

```sh
cd Plugins/SuperEmbedderUnreal
ci/host/cores/run.sh --pair-dir <pair dir> [--only gcc|asan|tsan]      # the cores: 56 tests, three times
ci/host/coexist/run.sh --pair-dir <pair dir>                           # the coexistence checks
python3 ci/host/coexist/grown/run_grown.py --toolchain gcc --out <dir> # the grown-struct checks
python3 ci/generate_rename_header.py --plugin-root . --preprocessor gcc --cxx g++ --check
```

The cores' tests carry, beside each name, the requirement numbers of the plugin's design they cover (`Q-5`,
`K-1`, `S-2`, ...). They are labels only.

**On Windows**, in the editor or headless:

- The example's check: `UnrealEditor-Cmd.exe "<repo>\ExampleProject\ExampleProject.uproject" -game -nullrhi -unattended -stdout -SuperEmbedderExampleExit`
  exits 0 on success ([Getting Started](docs/GETTING_STARTED.md#the-example)).
- The MCP toolset's automation tests, with the MCP plugin enabled:
  `-ExecCmds="Automation RunTests SuperEmbedder.MCP;Quit" -unattended -nopause -nullrhi`.
  `SuperEmbedder.MCP.EncodeAndQuery` needs the model files where Project Settings point.
- The MSVC coexistence checks need both plugins built into one project: `ci/msvc/coff_coexist.py` compares
  the two plugins' object files, and `ci/host/coexist/grown/run_grown.py --toolchain msvc` runs the grown-struct
  checks with `cl` and `link`.

## Moving the pins

`ci/revendor.py` writes both vendored trees, their manifests and the wrappers from a SuperEmbedder revision and the
SuperSLM revision that pins it, reading raw git blobs so line endings are not converted. After it:

1. `python3 ci/generate_semb_config.py --plugin-root . ` to regenerate the configuration headers.
2. Re-derive the rename headers: `ci/generate_rename_header.py --preprocessor msvc --cxx cl.exe` from an x64 Native
   Tools prompt (with `--host-tu <SuperEmbedderCore's PerModuleInline.gen.cpp response file> --host-cwd <engine>\Engine\Source`,
   so names Unreal's own headers use stay off the list), and `--preprocessor gcc` on Linux. Review the conflict
   sets, then commit the headers and `ci/rename/`.
3. Run every check above, the MSVC coexistence checks in every configuration, and the example, and record the
   results in the changelog.

A pin move can change the encoder fingerprint, which makes every baked vector set and cache file stale: say so in
the changelog entry.

## Pull requests

Keep a change to one purpose, say what you tested and on what, and add a changelog line under the unreleased
version. New code in `Private/Core/` stays engine-free and gets a host test.
