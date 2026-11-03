# Running beside SuperSLM-Unreal

SuperEmbedder is built on the SuperSLM engine. So is
[SuperSLM-Unreal](https://github.com/dansupergameprogrammer/SuperSLM-Unreal). This page explains how the two
plugins' engine copies stay apart in one game, and the checks that show it.

## The problem

SuperEmbedder-Unreal vendors SuperSLM **v1.18.1**, the release SuperEmbedder 1.0.0 pins. SuperSLM-Unreal
vendors its own engine (1.4.0 vendors the same v1.18.1). In a modular editor build each plugin is its own DLL,
but a packaged game is usually **monolithic**: every module links into one executable. Two copies of one engine
in one image then go wrong in one of two ways:

- **Strong symbols collide.** Every engine function defined in both copies is a duplicate definition, and the
  link fails.
- **Weak symbols merge silently.** Inline functions, templates and standard-library instantiations that use
  engine types are emitted in both copies as COMDAT (weak) symbols, and the linker keeps one body for both. If
  the two copies were built from different engine versions, one copy runs the other's code, and if an engine
  struct's layout changed between them, it reads memory with the wrong layout. Nothing reports it.

## The rename

`SuperEmbedderCore` compiles its engine copy under a generated header, `SuperEmbedderCoreRename.msvc.h` (and
`.gcc.h` for the host checks), force-included into every translation unit of that module. It `#define`s every
identifier of the engine's own source to a `semb_` name: `superslm` becomes `semb_superslm`, every `sslm_*`
becomes `semb_sslm_*`, and so on. No vendored file is edited; only link names change, so the vectors do not.

The list is derived, not written by hand. `ci/generate_rename_header.py` preprocesses each vendored source with
the real compiler (`cl /P` or `g++ -E`) and takes every identifier in the engine's own preprocessed text, minus:

- C++ keywords, and every identifier that any header outside the two vendored trees uses (the standard library,
  the C runtime, the platform, and on MSVC the Unreal headers the module's own generated translation unit reads);
- the **conflict set**, which it reports: identifiers SuperEmbedder's public header `semb.h` uses (the plugin's
  other modules include it without the rename), attribute words (they are macro-expanded), and names a
  preprocessor directive tests or defines (renaming those would flip an `#ifdef`).

The result: 3,000 names on MSVC (conflict set 104) and 3,104 on GCC (conflict set 10), committed under
`ci/rename/`. Names that a linker directive gives as a string, which no macro reaches, are answered on MSVC with
`/ALTERNATENAME`. The module's own Unreal-facing source re-includes the header with
`SUPEREMBEDDERCORE_RENAME_UNDO` defined, which removes every macro before any Unreal header is read.

The engine's build identity is compiled under the rename too, so a SuperSLM plugin that defines its own cannot
clash with it.

## What was checked

The check names below (R-1, R-1b, R-7, R-8, R-9) are the requirement numbers of the plugin's design and appear in
the scripts and their output.

| Check | What it shows | Tool |
|---|---|---|
| R-7 | The rename changes no line outside the vendored trees: every non-engine header preprocesses the same with and without it. | `ci/host/coexist/check_r7.py` |
| R-1 | No strong symbol is defined in both copies, and no shared weak symbol names a renamed engine identifier. | `ci/host/coexist/check_symbols.py` (GCC), `ci/msvc/coff_coexist.py` (MSVC) |
| R-1b | Every weak symbol the two copies share has the same body, compared instruction by instruction with relocation targets by name. A shared symbol that cannot be compared fails. | `ci/host/coexist/bodycmp.py` (GCC), `ci/msvc/coff_coexist.py` (MSVC) |
| Link | One image with both copies links in both orders with no duplicate definition, and a control link of the two copies without the rename **fails**, so the check is shown able to fail. | `ci/host/coexist/run.sh` |
| Smoke | The renamed build's vectors and fingerprint equal the unrenamed build's and SuperEmbedder's own CMake build's, on a small synthetic model. | `ci/host/coexist/run.sh` |
| R-8, R-9 | A simulated later engine version that grows an engine struct (`sslm_schema_s`, `sslm_kv_pool_s`) still shares no symbol carrying that struct; a control header that leaves the struct's name out **must** produce a shared symbol and a crash under AddressSanitizer. | `ci/host/coexist/grown/` |

### Windows x64, MSVC 14.44 (`cl` 19.44.35229), UE 5.8 source build, against SuperSLM-Unreal 1.4.0

- R-7 (`check_r7.py --preprocessor msvc`): 0 of 37 translation units change.
- Every `SuperEmbedderCore` compile line force-includes the rename header, in every configuration built.
- R-1 and R-1b, the two plugins' objects compared in both orders: 0 strong symbols on both sides, 0 renamed names
  shared, 0 bodies differ, 0 uncompared, in editor Development (978 shared weak symbols), game Development (978),
  DebugGame (872), Debug (888) and Shipping (969). A small number of standard-library template bodies that differ
  only by inlining, and name only standard-library types, are exempt and listed by the tool (13 in editor
  Development, 12 in game Development and Shipping, none in DebugGame, 52 in Debug).
- The control, without the rename (Shipping): 316 strong collisions, 254 renamed names shared. The check refuses.
- R-8 and R-9 under `/fsanitize=address` at `/Od` and `/O2`: pass, and every required control fails.
- A packaged monolithic Development game with both plugins links, and this plugin's query vectors there are
  bit-identical to the SuperEmbedder 1.0.0 command-line tool's on the published model.

### Linux x86-64, GCC 13.3.0 (host checks, outside Unreal)

`ci/host/coexist/run.sh` at `-O0` and `-O2` against SuperSLM-Unreal 1.4.0's vendored engine: R-7 0 of 37
translation units change; R-1 0 strong and 0 engine-named weak symbols shared (1,704 shared weak symbols at
`-O0`, 82 at `-O2`); R-1b every shared weak symbol compared, 0 not compared, 0 bodies differ; both link orders 0
multiple definitions; the control without the rename 322 multiple definitions; the smoke's vector digests and fingerprint equal the unrenamed build's and
SuperEmbedder's own CMake build's. The same checks also passed earlier at `-O0` under AddressSanitizer, and
against SuperSLM-Unreal 1.2.0's engine (v1.13.1). `ci/host/coexist/grown/run_grown.py --toolchain gcc`: R-8 and
R-9 pass, and every required control fails.

## Running the checks yourself

The host checks need Python 3, a C++20 GCC, `nm` and `objdump`, and two inputs from outside this repository:

- **SuperSLM-Unreal's vendored engine**, the plain copy to compare against. By default the scripts look for
  `Plugins/SuperSLMUnreal/Source/ThirdParty/SuperSLM` beside this plugin; set `SUPERSLM_PLUGIN_TREE` to that
  directory of a SuperSLM-Unreal checkout otherwise.
- **SuperEmbedder's synthetic model and tokenizer pair** (`u1_pair_model.sslm`, `u1_pair_tokenizer.sslm`,
  `u1_pair_tokenizer_no_endoftext.sslm`), made by the SuperEmbedder repository at tag `v1.0.0`:

  ```sh
  python3 tests/fixtures/regenerate_tokenizer_pair_fixture.py --lock superslm.lock --out-dir <pair dir>
  ```

  It clones the pinned SuperSLM release to run its converter, so it needs network access the first time.

Then, from `Plugins/SuperEmbedderUnreal`:

```sh
python3 ci/generate_rename_header.py --plugin-root . --preprocessor gcc --cxx g++ --check
ci/host/coexist/run.sh --pair-dir <pair dir> [--opts "-O0 -O2"] [--asan]
python3 ci/host/coexist/grown/run_grown.py --toolchain gcc --out <dir>
```

The GCC rename header is derived with GCC 13.3; another GCC version can derive a different list, which
`--check` reports. The MSVC legs of R-8 and R-9 run with `--toolchain msvc` from a Windows prompt with the Visual
Studio C++ tools; `ci/host/coexist/grown/README.md` gives the command line.

## What 1.0 does not do

**Sharing one engine copy** between the two plugins, so a game that uses both carries the engine once, is not in
1.0.
