# Getting Started

This page takes you from a clone of this repository to a first ranked query, in your own project and in the
example. The [README](../README.md) has the short version.

## 1. Get the model files

Download the model and tokenizer from the
[SuperEmbedder 1.0.0 release](https://github.com/dansupergameprogrammer/SuperEmbedder/releases/tag/v1.0.0) and
check them:

| File | Size | SHA-256 |
|---|---:|---|
| `qwen3-embedding-0.6b-1p5.sslm` | 634,489,808 bytes | `0be28bf42637264df85481eb925c46f674da7db97fd7601c6732881ad8f99fbf` |
| `qwen3-embedding-0.6b-tokenizer.sslm` | 3,465,492 bytes | `2ba15e621e28f4f58046a89a4e4b1e3e6ea8c4e1a4f5397238634b589082b9d3` |

```
certutil -hashfile qwen3-embedding-0.6b-1p5.sslm SHA256
```

The plugin works with any model and tokenizer pair SuperEmbedder 1.0.0 opens, but only these two files were
tested with it. A different pair gives a different encoder fingerprint, and sets baked with one pair are
refused by an encoder opened on another.

## 2. Install the plugin

1. Copy `Plugins/SuperEmbedderUnreal` into `<YourProject>/Plugins/`.
2. Enable *SuperEmbedder For Unreal Engine* in Edit > Plugins, or add it to the `.uproject`:

   ```json
   "Plugins": [ { "Name": "SuperEmbedderUnreal", "Enabled": true } ]
   ```

3. Regenerate project files and build your editor target (for example from Visual Studio, or
   `<UE>\Engine\Build\BatchFiles\Build.bat <YourProject>Editor Win64 Development -Project="<path>\<YourProject>.uproject" -WaitMutex`).

The plugin has three modules: `SuperEmbedderCore` (the vendored SuperEmbedder library and its renamed engine
copy, built with no precompiled header and no unity build, so the first build of it takes a while),
`SuperEmbedderUnreal` (the runtime API) and `SuperEmbedderUnrealEditor` (the bake, validation and commandlets).
All three are Win64-only.

## 3. Place the model files

By default the plugin opens `Content/SuperEmbedder/model.sslm` and `Content/SuperEmbedder/tokenizer.sslm`,
relative to the project directory. Rename the downloads to those names, or point Project Settings at them.

Add the folder to your ignore file; it is about 638 MB:

```
Content/SuperEmbedder/
```

## 4. Settings

Project Settings > Plugins > SuperEmbedder (`[/Script/SuperEmbedderUnreal.SuperEmbedderSettings]` in
`DefaultGame.ini`). The encoder's settings are read when the engine starts.

| Setting | Default | What it does |
|---|---|---|
| `ModelFile`, `TokenizerFile` | `Content/SuperEmbedder/model.sslm`, `.../tokenizer.sslm` | The two files. Relative paths are relative to the project directory; non-ASCII paths work. |
| `ModelSha256`, `TokenizerSha256` | empty | Expected SHA-256 of each file, checked off the game thread before each open (`ArtifactShaMismatch` on a difference). Empty skips the check. Hashing the model takes a few seconds. |
| `bOpenOnStartup` | false | Open the encoder when the engine starts and keep it open for the whole process. |
| `MaxConcurrency` | 1 | Worker threads, 1 to 4. At 2 or more, one worker only ever runs queries, so a query never waits behind documents. |
| `MaxQueryTokens`, `MaxDocumentTokens` | 256, 256 | The longest query and the longest document row, in tokens counted by the model's tokenizer, 1 to 512. A longer text is refused (`QueryTooLong`, `DocumentTooLong`), never truncated. |
| `MaxTextBytes` | 4096 | A byte ceiling checked before tokenizing. |
| `MaxPendingQueries`, `MaxPendingDocumentTickets` | 64, 8 | How many requests may wait; one more is refused `QueueFull`. |
| `MaxCacheEntries` | 4096 | The exact encode cache's size (0 turns it off). 4,096 entries of 1,024 floats is about 16 MB. |
| `CacheFile` | empty | A cache file written by *Save Encode Cache*, loaded off the game thread at startup. A file for another encoder loads empty; a damaged file is refused by name and the game runs with a cold cache. |

**Opening** maps the model and runs SuperEmbedder's self-checks, on a worker, never on the game thread. It takes
tens of seconds on the published model (20 to 38 s in the maintainer's runs). In the editor the plugin keeps the
encoder open across play-in-editor sessions; `SuperEmbedder.CloseEncoder` releases it.

## 5. Packaging

The library reads the model files itself, so they must be staged as loose files, not cooked into the pak. Add
`SuperEmbedder` to Project Settings > Packaging > *Additional Non-Asset Directories to Copy*:

```ini
[/Script/UnrealEd.ProjectPackagingSettings]
+DirectoriesToAlwaysStageAsNonUFS=(Path="SuperEmbedder")
```

Vector set assets cook like any other asset; their rows are checked against their digest on every load, in the
game as in the editor.

## 6. A first request

The [README's quick start](../README.md#quick-start) has a Blueprint and a C++ walk-through: open the encoder,
encode rows as documents into a vector set, and rank the set against a query. [Blueprint and C++ API](API.md)
lists every call.

To check an install from the console without writing any code (editor and non-Shipping builds):

```
SuperEmbedder.EncodeText Where can I buy a sword?
```

It opens the encoder from Project Settings if none is open, encodes the text as a query, and logs the outcome,
the dimensions, the vector's SHA-256 digest and the encoder fingerprint.

## The example

`ExampleProject/` finds the plugins in this repository's `Plugins/` folder through
`"AdditionalPluginDirectories": ["../Plugins"]`, so it builds from a clone as it stands.

1. Put the two model files in `ExampleProject/Content/SuperEmbedder/` as `model.sslm` and `tokenizer.sslm`
   (that folder is ignored by git).
2. Open `ExampleProject/ExampleProject.uproject` and build when prompted, or:

   ```
   <UE>\Engine\Build\BatchFiles\Build.bat ExampleProjectEditor Win64 Development -Project="<repo>\ExampleProject\ExampleProject.uproject" -WaitMutex
   ```

3. Press Play. The screen shows the encoder opening, the eight rows baking, and the three questions with their
   top three rows and scores. `SuperEmbedderExample.Ask <text>` in the console asks another.

A headless run, which exits 0 when the encoder opened, the rows baked and every query returned its hits ranked
best first, and 1 otherwise:

```
<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<repo>\ExampleProject\ExampleProject.uproject" -game -nullrhi -unattended -stdout -SuperEmbedderExampleExit
```

The log ends with `SuperEmbedderExample: PASS` or `FAIL`. Next to each fixed question it also prints the row a
reader would expect first and whether the ranking agrees; that line is information, not part of the pass.

The example packages like any project; its `DefaultGame.ini` already stages `Content/SuperEmbedder/` loose and
cooks the engine's Entry map, which nothing else references.
