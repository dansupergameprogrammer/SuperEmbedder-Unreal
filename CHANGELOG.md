# Changelog

All notable changes to SuperEmbedder-Unreal are recorded here. Versions follow the `VersionName` in
`Plugins/SuperEmbedderUnreal/SuperEmbedderUnreal.uplugin`. The plugin versions independently of SuperEmbedder;
each entry names the SuperEmbedder and SuperSLM releases it vendors.

## 1.0.1 - 2026-11-07

**Vendored:** SuperEmbedder `v1.0.0` and SuperSLM `v1.18.1`, unchanged. Vector sets baked with 1.0.0 stay valid.

### Changed

- **About 2x faster on Windows machines with AVX-512.** The plugin now builds the vendored engine with
  `SUPERSLM_TILED_AVX512_MSVC=1` and `SUPERSLM_SITES_AVX512_MSVC=1`, the fast AVX-512 kernels that SuperSLM 1.19.1
  turns on by default. On an Intel i7-1165G7 laptop (development `-game` build, 127 queries), a query went from about
  1,270 ms to about 530 ms and the encoder opened in 11-13 s instead of 22-34 s, with identical query digests. CPUs
  without AVX-512 run the same code as before.

### Fixed

- **The plugin builds with shared precompiled headers off.** A project that sets `bUseSharedPCHs = false` failed to
  compile two of the plugin's files, which leaned on the shared precompiled header for `UPackage` and `FAssetData`.
  Each now includes the engine header that declares what it uses. No behavior changes.

## 1.0.0 - 2026-11-03

The first public release.

**Vendored:** SuperEmbedder `v1.0.0` (`d326b42`) and SuperSLM `v1.18.1`
(`c3c14b16690a0c0c54de781f6f30380d88e62bd1`), the engine SuperEmbedder 1.0.0 pins. Both unmodified, as library
subsets.

**Model files:** SuperEmbedder 1.0.0's release assets, `qwen3-embedding-0.6b-1p5.sslm` (SHA-256
`0be28bf42637264df85481eb925c46f674da7db97fd7601c6732881ad8f99fbf`) and `qwen3-embedding-0.6b-tokenizer.sslm`
(`2ba15e621e28f4f58046a89a4e4b1e3e6ea8c4e1a4f5397238634b589082b9d3`).

### Added

- **`SuperEmbedderUnreal`**, the plugin: text to embedding vectors on the CPU, off the game thread.
  - `USuperEmbedderSubsystem`: one encoder per process, opened on a worker and held by
    `USuperEmbedderEncoderHandle`s; queries and documents encoded on 1 to 4 workers, with a worker reserved for
    queries at 2 or more; bounded queues; cancel by ticket, and every request a world made cancelled when that
    world ends; a blocking encode for threads of your own.
  - Exactly one *Completed*, *Refused* or *Cancelled* per request, on a later game-thread tick. Refusals are
    named by `ESuperEmbedderStatus`.
  - An exact encode cache (4,096 entries by default), with a file it can save and load at startup.
  - `USuperEmbedderVectorSet`: rows of id, text and vector, saved as an asset or made at run time, with exact
    cosine top-k (synchronous and async). Saved sets are checked against a row digest on every load.
  - Every vector carries its encoder's fingerprint and kind, and the helpers refuse to mix fingerprints.
  - Async Blueprint nodes: *Open SuperEmbedder Encoder*, *Encode Text (Async)*, *Encode Documents (Async)*,
    *Top K (Async)*, *Query Text Top K (Async)*.
  - Project Settings: the model and tokenizer paths and optional SHA-256, open at startup, workers, token, byte
    and queue bounds, the cache.
  - The editor bake: right-click a DataTable, `SuperEmbedder.BakeJsonl`, `SuperEmbedder.BakeDataTable`, and the
    `SuperEmbedderBake` commandlet; validation with `SuperEmbedder.ValidateSets` and the
    `SuperEmbedderValidate` commandlet.
  - Development console commands: `SuperEmbedder.EncodeText`, `SuperEmbedder.EncodeDocuments`,
    `SuperEmbedder.DumpSet`, `SuperEmbedder.CloseEncoder`.
  - Its own copy of the SuperSLM engine, compiled under a generated rename header, so it can be enabled beside
    SuperSLM-Unreal in one game, monolithic builds included. Tested with SuperSLM-Unreal 1.4.0.
- **`SuperEmbedderUnrealMCP`**, an optional, disabled-by-default editor plugin: the MCP tools `EncoderStatus`,
  `EncodeText`, `QueryText`, `ListSets` and `ValidateSets`, all read-only. It needs the engine's Experimental
  `ToolsetRegistry` and `ModelContextProtocol` plugins.
- **`ExampleProject`**: open the encoder, bake eight rows into a run-time vector set, rank three questions against
  it, and answer more from the console; a headless check mode that exits 0 on success.

### Known limitations

- The plugin's build is not a certified SuperEmbedder build: its vectors carry the uncertified fingerprint, so
  banks baked by a certified `semb` build are refused by the plugin. Certification is planned for 1.1.
- Windows x64 only, CPU only. Tested on a source build of UE 5.8.
- No model asset: the files are read from disk and staged as a non-asset directory.
- No shared engine copy with SuperSLM-Unreal.
