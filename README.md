# SuperEmbedder for Unreal Engine

**Text to embedding vectors inside your game, on the player's CPU.** SuperEmbedder-Unreal is the Unreal
Engine 5.8 integration of [SuperEmbedder](https://github.com/dansupergameprogrammer/SuperEmbedder), a
library that turns text into embedding vectors whose bytes do not depend on the compiler, the optimisation
level or the CPU's vector width. The plugin runs every encode off the game thread, keeps the vectors in
vector sets you can bake in the editor, and ranks them by cosine similarity, so your game can find the
rows nearest in meaning to what the player typed.

It needs no server, API key or internet connection. The model files ship with your game.

> **Status: 1.0.** Built and tested on Windows x64 with UE 5.8 (a source build of the engine) and MSVC.
> The plugin's build is **not a certified SuperEmbedder build** (see [Certified or not](#certified-or-not)):
> its vectors were checked bit-identical to SuperEmbedder 1.0.0's command-line tool on the published model,
> but they carry the uncertified fingerprint. Certifying the plugin's build is planned for 1.1. The
> [changelog](CHANGELOG.md) lists what 1.0 contains.

---

## What it is for

- **Matching what the player says to what your game knows.** Bake your quest lines, item descriptions,
  dialogue topics or lore entries into a vector set, then rank them against free text: *"I need a horse
  for my journey"* finds the stable, though the row never says "horse for my journey".
- **Search in your own tools.** Rank DataTable rows or documents by meaning in the editor.
- **Reproducible results.** The same text, on the same build, gives the same vector bytes every time, so a
  ranking can be reproduced in a bug report or an automated test.

## What is in 1.0

- **Encoding off the game thread.** Queries and documents encode on the plugin's worker threads (1 to 4).
  Every request returns a ticket and gets exactly one result, *Completed*, *Refused* or *Cancelled*, on a
  later game-thread tick: never inside the call, refusals and cache hits included. Refusals are named
  (`QueryTooLong`, `QueueFull`, `InvalidUtf8`, ...).
- **Two lanes.** With two or more workers, one worker only runs queries, so a query never waits behind a
  bake.
- **Vector sets** (`USuperEmbedderVectorSet`): rows of id, text and vector, saved as an asset or made at
  run time; exact cosine top-k, synchronous or on a worker. A saved set's rows are checked against a digest
  on every load, and a damaged set loads empty with a named status.
- **The editor bake.** Right-click a DataTable to bake it into a vector set asset, or bake from a JSONL file
  with a console command or a commandlet. A validate commandlet checks every set in the project.
- **An exact encode cache.** A repeated text is served from memory with the same bytes (4,096 entries by
  default), and the cache can be saved to a file and loaded at startup.
- **Blueprint and C++.** Async Blueprint nodes for every encode and ranking, and a C++ API with callbacks.
- **Fingerprint checks.** Every vector carries the fingerprint of the encoder that made it. The helpers
  refuse to compare vectors from different encoders, so a set baked with another model (or by a certified `semb` build) cannot be
  ranked against by mistake.
- **Runs beside SuperSLM-Unreal.** The plugin compiles its own renamed copy of the SuperSLM engine, so it
  can be enabled in the same game as [SuperSLM-Unreal](https://github.com/dansupergameprogrammer/SuperSLM-Unreal),
  monolithic builds included. See [Running beside SuperSLM-Unreal](#running-beside-superslm-unreal).
- **An optional MCP toolset** (`Plugins/SuperEmbedderUnrealMCP`), through which an MCP client can check
  the encoder, encode a text and query a vector set from the editor.
- **A small example project** (`ExampleProject`).

Not in 1.0: a GPU path (SuperEmbedder's GPU support is not used by the plugin), a model asset or import
step (the plugin reads the model files from disk), certification of the plugin's build, and a mode in
which the plugin shares one engine copy with SuperSLM-Unreal.

## Requirements

- Unreal Engine 5.8. Tested on a source build of the engine; a launcher install has not been tested.
- Windows x64 and the Visual Studio toolchain UE 5.8 requires (tested with MSVC 14.44). The plugin's
  modules are Win64-only. A C++ project: the plugin compiles from source.
- The engine-free parts (the encode service, the cache, vector sets, similarity, and the renamed engine
  copy) also build and pass their tests on Linux x64 with GCC 13.3, outside Unreal. That is a host test of
  those parts, not a Linux build of the plugin.
- The SuperEmbedder 1.0.0 model and tokenizer, about 638 MB together (see [Model files](#model-files)).
- For the MCP toolset only: the engine's Experimental `ToolsetRegistry` and `ModelContextProtocol`
  plugins, which in practice means an engine built from GitHub source.

## Model files

The plugin needs two files from the
[SuperEmbedder 1.0.0 release](https://github.com/dansupergameprogrammer/SuperEmbedder/releases/tag/v1.0.0).
They are release downloads, not files in this repository. Check their SHA-256 before use:

| File | Size | SHA-256 |
|---|---:|---|
| `qwen3-embedding-0.6b-1p5.sslm` (model) | 634,489,808 bytes | `0be28bf42637264df85481eb925c46f674da7db97fd7601c6732881ad8f99fbf` |
| `qwen3-embedding-0.6b-tokenizer.sslm` (tokenizer) | 3,465,492 bytes | `2ba15e621e28f4f58046a89a4e4b1e3e6ea8c4e1a4f5397238634b589082b9d3` |

```
certutil -hashfile qwen3-embedding-0.6b-1p5.sslm SHA256
certutil -hashfile qwen3-embedding-0.6b-tokenizer.sslm SHA256
```

The model is a converted form of [Qwen3-Embedding-0.6B](https://huggingface.co/Qwen/Qwen3-Embedding-0.6B)
by the Qwen team (Alibaba Cloud), Apache-2.0; SuperEmbedder's NOTICE gives the source revision and what the
conversion changed. Its vectors have 1,024 dimensions.

## Install

1. Copy `Plugins/SuperEmbedderUnreal` into your project's `Plugins/` folder. It depends on no other
   plugin.
2. Enable it (Edit > Plugins > *SuperEmbedder For Unreal Engine*, or add
   `{ "Name": "SuperEmbedderUnreal", "Enabled": true }` to your `.uproject`'s `Plugins`), regenerate
   project files and build.
3. Put the two files in `<YourProject>/Content/SuperEmbedder/` as `model.sslm` and `tokenizer.sslm` (the
   default paths), or anywhere and set the paths in Project Settings > Plugins > SuperEmbedder. Relative
   paths are relative to the project directory. Keep the model out of source control (ignore it, or use
   LFS).
4. **Stage the folder loose, not in the pak**: the library reads the files itself. Add `SuperEmbedder` to
   Project Settings > Packaging > *Additional Non-Asset Directories to Copy*, which writes this line to
   `Config/DefaultGame.ini`:

   ```ini
   [/Script/UnrealEd.ProjectPackagingSettings]
   +DirectoriesToAlwaysStageAsNonUFS=(Path="SuperEmbedder")
   ```

[Getting Started](docs/GETTING_STARTED.md) covers the settings (the files' SHA-256, opening at startup,
workers, token bounds, queue bounds, the cache) and packaging in full.

## Quick start

**Opening the encoder** maps the model and runs SuperEmbedder's self-checks on a worker. On the published
model it takes tens of seconds (20 to 38 s in the maintainer's runs), so open it early, for example at a
loading screen, or set *Open On Startup*. Hold the handle it returns for as long as you encode; the
encoder closes when the last handle is released.

### Blueprint

1. **Open SuperEmbedder Encoder** with empty paths (Project Settings are used). On **Opened**, store the
   *Handle* in a variable.
2. **New Vector Set**, then **Encode Documents (Async)** with an array of rows (`Id`, `Text`). On
   **Completed**, call **Append Documents** on the set with the result and the rows' texts.
3. **Query Text Top K (Async)** with the set, the player's text and `K`. On **Completed**, read **Hits**:
   each has an `Id`, a `Score` (cosine, best first) and its row index.

Each async node also has **Refused** (with a *Status* and a *Message*) and **Cancelled** pins, and a Cancel
on its *Async Action* pin. There is no blocking Blueprint encode.

### C++

Add `"SuperEmbedderUnreal"` to your module's `PublicDependencyModuleNames`.

```cpp
#include "SuperEmbedderSubsystem.h"
#include "SuperEmbedderBlueprintLibrary.h"
#include "SuperEmbedderVectorSet.h"

// In your class: UPROPERTY() TObjectPtr<USuperEmbedderEncoderHandle> Handle;
//                UPROPERTY() TObjectPtr<USuperEmbedderVectorSet> Places;

void AMyActor::BeginPlay()
{
    Super::BeginPlay();
    USuperEmbedderSubsystem* Embedder = USuperEmbedderSubsystem::Get();

    // Empty paths: Project Settings. The callback runs on the game thread on a later tick.
    Handle = Embedder->OpenEncoder(FString(), FString(),
        [this](const FSuperEmbedderOpenResult& Result, USuperEmbedderEncoderHandle*)
        {
            if (Result.Status != ESuperEmbedderStatus::Ok) { /* Result.Message says why */ return; }
            BakePlaces();
        });
}

void AMyActor::BakePlaces()
{
    TArray<FSuperEmbedderDocumentRow> Rows;
    FSuperEmbedderDocumentRow& Smithy = Rows.AddDefaulted_GetRef();
    Smithy.Id = TEXT("forge");
    Smithy.Text = TEXT("A blacksmith's forge where swords and axes are hammered from hot iron.");
    FSuperEmbedderDocumentRow& Stable = Rows.AddDefaulted_GetRef();
    Stable.Id = TEXT("stable");
    Stable.Text = TEXT("A stable where travelers rent horses for the road.");

    USuperEmbedderSubsystem::Get()->EncodeDocuments(Rows, this,
        [this, Rows](const FSuperEmbedderDocumentsResult& Result)
        {
            if (Result.Outcome != ESuperEmbedderOutcome::Completed) { return; }
            TArray<FString> Texts;
            for (const FSuperEmbedderDocumentRow& Row : Rows) { Texts.Add(Row.Text); }
            Places = USuperEmbedderBlueprintLibrary::NewVectorSet(this);
            FString Message;
            Places->AppendDocuments(Result, Texts, Message);
        });
}

void AMyActor::Ask(const FString& PlayerText)
{
    USuperEmbedderSubsystem::Get()->QueryTextTopK(Places, PlayerText, /*K*/ 3, /*Exclude*/ {}, this,
        [](const FSuperEmbedderTopKResult& Result)
        {
            for (const FSuperEmbedderHit& Hit : Result.Hits)
            {
                UE_LOG(LogTemp, Display, TEXT("%.4f %s"), Hit.Score, *Hit.Id);
            }
        });
}
```

`EncodeText` and `TopKAsync` encode a query and rank a set against a vector you already hold; `Cancel` and
`GetProgress` take a ticket. `EncodeTextBlocking` is for a thread of your own and refuses on the game thread.
Every call is listed in [Blueprint and C++ API](docs/API.md).

## Vector sets: bake and query

A vector set holds rows of id, text and vector, all from one encoder. Bake the sets your game ships with in
the editor and save them as assets:

- **From a DataTable.** Right-click the table > *Bake SuperEmbedder Vector Set*. It writes
  `<Table>_VectorSet` beside the table. The row name is the id, and the row's string or text property named
  `Text` is the text (`FSuperEmbedderTextRow` is such a row struct). The bake shows progress with a Cancel
  button and writes the asset only when every row has encoded.
- **From JSONL**, one `{"id": "...", "text": "..."}` object per line: the console command
  `SuperEmbedder.BakeJsonl <file> </Game/Path/Name>`, or the commandlet
  `-run=SuperEmbedderBake -Input=<file or table path> -Output=</Game/Path/Name>` for a build script.
- **At run time.** *New Vector Set*, then *Append* or *Append Documents*; *Save To Bytes* and *Load From
  Bytes* keep it in your own save data.

Ranking is exact and brute force: every row is scored. That suits hundreds to a few thousand rows; a vector set
is not an index. For larger collections, [SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss)
searches banks baked by SuperEmbedder's `semb` tool (the plugin does not read or write those banks in 1.0).

Query a set with **Query Text Top K (Async)** (encode the text, then rank), **Top K (Async)** (rank against a
vector you hold), or the synchronous **Top K** and **Cosine** on the game thread for small sets. Ties rank the
lower row first. `-run=SuperEmbedderValidate` (or `SuperEmbedder.ValidateSets` in the console) lists every set
whose rows fail their digest or whose fingerprint differs from the encoder's, and exits 1 if any does.

**Bake with the plugin, not the `semb` command-line tool.** A set is tied to the fingerprint of the encoder
that baked it. The plugin's build is uncertified, so its fingerprint differs from a certified `semb` build's,
and a bank baked by a certified `semb` build is refused by the plugin (and the reverse), even though the vector
values are the same. See [Certified or not](#certified-or-not).

## Certified or not

SuperEmbedder **certifies** specific builds: on a certified build, the same text gives byte-identical vectors on
every certified machine, and its fingerprint names the certified set. 1.0.0's certified builds are MSVC 19.33
and clang-cl 18.1.8 at the AVX2 tier ([SuperEmbedder's determinism statement](https://github.com/dansupergameprogrammer/SuperEmbedder/blob/main/docs/DETERMINISM.md)).

The plugin is compiled by Unreal's build tool with the engine's toolchain, which is not one of those builds,
so **the plugin's build is uncertified in 1.0**:

- **What was checked.** On the published model, on Windows x64 (MSVC 14.44, UE 5.8), the plugin's query
  vectors for three texts were bit-identical to the SuperEmbedder 1.0.0 command-line tool's (full SHA-256
  digests of the vectors equal) in a packaged monolithic game that also contained SuperSLM-Unreal 1.4.0, and
  the packaged example's query vectors were too. The plugin's document vectors were checked the same way at
  the previous engine pin. [What 1.0 covers](docs/STATUS.md) has the details.
- **What it means for you.** The plugin's fingerprint is the uncertified one
  (`011c5a315985be6a614e18ef45f4f14535c2a0f632de3068577b72399cb79513` on the published model, the same as an
  uncertified `semb` build reports). Sets baked by the plugin are queried by the plugin. SuperEmbedder builds
  every output value in integer code, so that it does not depend on the compiler or the CPU, but for the
  plugin's build that is checked on one machine, not certified. And because every uncertified build shares one
  fingerprint for a model, the plugin **cannot tell** a set baked by another uncertified build or machine from
  its own: if two builds ever disagreed, nothing would refuse the mix. The fingerprint check does refuse sets
  from another model, and from a certified build.
- **What is planned.** Certifying the plugin's build is planned for 1.1.

## Running beside SuperSLM-Unreal

SuperEmbedder runs on the SuperSLM engine, and so does
[SuperSLM-Unreal](https://github.com/dansupergameprogrammer/SuperSLM-Unreal). Two copies of one engine in one
game would collide at link time in a monolithic build, or worse, silently share code between two different
engine versions. This plugin avoids both: it compiles its own copy of the engine (SuperSLM v1.18.1, the
release SuperEmbedder 1.0.0 pins) under a generated header that renames every engine identifier with a
`semb_` prefix. No vendored file is edited, and the renaming changes link names only, never the vectors.

Tested with SuperSLM-Unreal 1.4.0 (which vendors the same SuperSLM release, the case where an unrenamed link
collides on every engine symbol): a packaged monolithic game with both plugins links with no duplicate symbol,
and this plugin's vectors there were bit-identical to the command-line tool's. Symbol checks on the compiled objects found no strong symbol and no engine-named symbol shared
between the two copies, and every shared standard-library body equal, in Development, DebugGame, Debug and
Shipping. To enable both, put both plugins in your project's `Plugins/` folder; nothing else is needed.

Sharing one engine copy between the two plugins is not in 1.0. [Running beside SuperSLM-Unreal](docs/COEXISTENCE.md)
explains the rename and the checks behind it.

## The MCP toolset

`Plugins/SuperEmbedderUnrealMCP` is an optional editor plugin, disabled by default, that registers five tools
with the engine's Experimental `ToolsetRegistry`, reachable from an MCP client through the engine's Experimental
`ModelContextProtocol` server: `EncoderStatus`, `EncodeText` (a digest and a summary, never the whole vector),
`QueryText` (top-k against a named set), `ListSets` and `ValidateSets`. The tools are read-only: none writes a
vector set or the cache. See its [README](Plugins/SuperEmbedderUnrealMCP/README.md) for setup.

## The example

`ExampleProject` is the smallest complete use of the plugin, written in C++ through the same functions the
Blueprint nodes call. A game mode spawns one actor into the engine's empty Entry map, which opens the encoder,
bakes eight short descriptions of places in a town (a forge, a library, a harbor, a tavern, ...) into a run-time
vector set, and ranks them against three questions, such as *"Where can I buy a sword?"*. The results and their
scores are drawn on screen and logged; type `SuperEmbedderExample.Ask <text>` in the console to ask another. The
project carries no `.uasset` or map.

To run it: put the two model files in `ExampleProject/Content/SuperEmbedder/` as `model.sslm` and
`tokenizer.sslm`, open `ExampleProject/ExampleProject.uproject` (it finds the plugins in this repository's
`Plugins/` folder), build when prompted, and press Play. [Getting Started](docs/GETTING_STARTED.md#the-example)
has the command line for a headless run that exits 0 when every step passes.

## Known limitations

- Windows x64 only; CPU only.
- The plugin's build is uncertified (above). Sets baked by a certified `semb` build cannot be queried by the
  plugin.
- Queries and document rows are bounded at 256 tokens each by default (up to 512 in Project Settings); a longer
  text is refused, not truncated.
- Opening the encoder takes tens of seconds and holds the model in memory while any handle is held.
- No model asset: the files are read from disk and must be staged as a non-asset directory.
- Only English retrieval has been measured, by SuperEmbedder.
- Encoding speed in the plugin has not been measured in a form published here. SuperEmbedder's README has the
  library's own measurements on a certified build.

## Documentation

| | |
|---|---|
| [Getting Started](docs/GETTING_STARTED.md) | Install, settings, packaging, the example. |
| [Blueprint and C++ API](docs/API.md) | Every node, call, console command and commandlet, and the statuses. |
| [Running beside SuperSLM-Unreal](docs/COEXISTENCE.md) | The renamed engine copy and the checks behind it. |
| [What 1.0 covers](docs/STATUS.md) | What was built and tested, on what, and what was not. |

## Contributing, security and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for building and testing, and [SECURITY.md](SECURITY.md) to report a
vulnerability privately.

Apache License 2.0, the same as SuperEmbedder and SuperSLM. See [LICENSE](LICENSE) and [NOTICE](NOTICE). The
model files are distributed by the SuperEmbedder release under their source checkpoint's license.

Unreal and Unreal Engine are trademarks or registered trademarks of Epic Games, Inc. in the United States of
America and elsewhere. This project is not affiliated with, endorsed by or sponsored by Epic Games, Inc. or
Alibaba Cloud.
