# SuperEmbedder For Unreal Engine (plugin)

The Unreal Engine 5.8 plugin that turns text into SuperEmbedder embedding vectors on the CPU, off the game
thread: an encoder subsystem, vector sets with exact cosine top-k, an exact encode cache, async Blueprint nodes,
and an editor bake and validate. Win64.

This folder is the plugin. The repository's top-level documentation covers it:

- `README.md`: what it does, install, the model files, quick start in Blueprint and C++.
- `docs/GETTING_STARTED.md`, `docs/API.md`: settings, packaging, every node and call.
- `docs/COEXISTENCE.md`: its renamed copy of the SuperSLM engine, which lets it run beside SuperSLM-Unreal.
- `docs/STATUS.md`: what 1.0 covers and what it does not.

## What is here

| Path | What it is |
|---|---|
| `SuperEmbedderUnreal.uplugin` | Modules `SuperEmbedderCore` and `SuperEmbedderUnreal` (Runtime), `SuperEmbedderUnrealEditor` (Editor). No plugin dependency. |
| `Source/SuperEmbedderCore/` | The vendored library and its own renamed copy of the SuperSLM engine: one wrapper `.cpp` per compiled vendored source (`Private/Vendored/`), the generated configuration headers, build record and rename headers (`Private/Generated/`), and one exported verb over the library's internal query token count (`Public/SuperEmbedderCoreExtras.h`). |
| `Source/SuperEmbedderUnreal/Private/Core/` | The engine-free cores (standard C++ only, no exceptions, no RTTI, no Unreal header): the encoder seam over `semb.h`, the encode service and its two lanes, the cache, the vector set, cosine and top-k, the JSONL reader, SHA-256 and text rules. |
| `Source/SuperEmbedderUnreal/Public/`, `Private/` | The Unreal layer: `USuperEmbedderSubsystem`, `USuperEmbedderEncoderHandle`, `USuperEmbedderVectorSet`, the async actions, `USuperEmbedderBlueprintLibrary`, `USuperEmbedderSettings`. |
| `Source/SuperEmbedderUnrealEditor/` | The bake, validation, the two commandlets, the DataTable menu entry and the console commands. |
| `Source/ThirdParty/SuperEmbedder/` | SuperEmbedder v1.0.0, library subset, unmodified, with `VENDORED_VERSION.txt` and `VENDORED_MANIFEST.sha256`. |
| `Source/ThirdParty/SuperSLM/` | SuperSLM v1.18.1, the engine SuperEmbedder 1.0.0 pins, library subset, unmodified, likewise. |
| `ci/generate_semb_config.py` | Writes the generated configuration headers and the build record; `--check` verifies them. |
| `ci/generate_rename_header.py`, `ci/rename/` | Derives the rename list from preprocessed source and writes the rename header (`--preprocessor gcc` or `msvc`); the derived lists and conflict sets. |
| `ci/revendor.py`, `ci/check_vendored_*.py` | Moves the pins; checks the vendored trees against their manifests and the wrappers against the compiled sources. |
| `ci/host/cores/` | The cores' host test suite (GCC, ASan with UBSan, TSan). |
| `ci/host/coexist/`, `ci/msvc/` | The coexistence checks: GCC host checks, the grown-struct fixtures, and the MSVC object comparison. |

## License

Apache-2.0 (see `LICENSE` and `NOTICE`). The vendored trees carry their own `LICENSE` and `NOTICE` files.
