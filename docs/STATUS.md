# What 1.0 covers

SuperEmbedder-Unreal 1.0.0 vendors SuperEmbedder **v1.0.0** (`d326b42`) and SuperSLM **v1.18.1**
(`c3c14b16690a0c0c54de781f6f30380d88e62bd1`), the engine SuperEmbedder 1.0.0 pins. Both are vendored unmodified,
as library subsets, under `Plugins/SuperEmbedderUnreal/Source/ThirdParty/`, each with a `VENDORED_VERSION.txt` and
a `VENDORED_MANIFEST.sha256`.

## Built and tested

**Windows x64**: UE 5.8 built from source, MSVC 14.44.35207 (`cl` 19.44.35229), with the published model files.

- The editor (Development and DebugGame) builds with 0 errors and 0 warnings from either plugin. Game targets were
  built in Development, DebugGame, Debug and Shipping for the symbol checks.
- The example passes its headless check in the editor and as a packaged Development game.
- A packaged monolithic Development game with this plugin and SuperSLM-Unreal 1.4.0 links. Its query vectors for
  three texts are bit-identical to the SuperEmbedder 1.0.0 command-line tool's (full SHA-256 digests equal), and
  the packaged example's query vectors equal the tool's too.
- The MCP toolset builds with 0 warnings, and its automation tests (`SuperEmbedder.MCP.SyncTools`, `.Refusals`,
  `.EncodeAndQuery`) pass headless on the published model.
- The coexistence checks against SuperSLM-Unreal 1.4.0, in every configuration: see
  [Running beside SuperSLM-Unreal](COEXISTENCE.md).
- At the previous engine pin (SuperSLM v1.15.0), document vectors were checked too: the editor bake, and in the
  packaged monolithic game a cooked vector set and *Encode Documents*, were bit-identical to that release's
  command-line tool. SuperEmbedder reports the same document and query vectors at v1.18.1 as at v1.15.0.

**Linux x86-64**, GCC 13.3.0, outside Unreal:

- The engine-free cores (the encode service and its two lanes, the cache, vector sets and their saved form,
  similarity, the JSONL reader, text rules), built with strict warnings and linked against the vendored library
  under the rename header: 56 of 56 tests pass with GCC, 56 of 56 under AddressSanitizer with
  UndefinedBehaviorSanitizer, and 56 of 56 under ThreadSanitizer, with no sanitizer report. Their real-encode
  tests compare vector digests with SuperEmbedder's own CMake build on a synthetic model.
- The coexistence host checks and the grown-struct checks pass ([Running beside SuperSLM-Unreal](COEXISTENCE.md)).

The plugin's own Unreal code (the subsystem, the Blueprint nodes, vector set assets, the bake) is exercised by the
example, the MCP toolset's automation tests and the console commands on Windows. It has no automation test suite
of its own in 1.0.

## Not certified

The plugin's build is not a certified SuperEmbedder build. Its fingerprint on the published model is the
uncertified one, `011c5a315985be6a614e18ef45f4f14535c2a0f632de3068577b72399cb79513`, the same as an uncertified
`semb` build reports. Sets baked by a certified `semb` build are refused by the plugin, and the reverse. The vector
values were checked equal to the command-line tool's on one machine; nothing is claimed for other machines or
toolchains. Every uncertified build shares that one fingerprint for a model, so the plugin cannot refuse a set or
cache baked by another uncertified build or on another machine, even if their bytes differed. Certification of the
plugin's build is planned for 1.1.

## Not tested

- A launcher (installed) UE 5.8. Every build above used an engine built from source.
- Any platform but Windows x64. The modules are Win64-only.
- Hardware beyond the maintainer's development machines.
- Encoding speed inside the plugin, in a form published here. [SuperEmbedder's README](https://github.com/dansupergameprogrammer/SuperEmbedder#readiness)
  has the library's own measurements, on a certified build.
- Running beside SuperSLM-Unreal versions other than 1.4.0 in a packaged game. (The host checks also passed against
  SuperSLM-Unreal 1.2.0's engine, SuperSLM v1.13.1.)

## Not in 1.0

- A GPU path. The plugin compiles no GPU source.
- A model asset and import step: the plugin reads the model files from disk.
- A shared engine copy with SuperSLM-Unreal.
- Unreal Insights trace channels or stat groups.
- Latent (non-async-action) Blueprint nodes.
