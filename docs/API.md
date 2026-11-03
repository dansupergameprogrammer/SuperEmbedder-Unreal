# Blueprint and C++ API

Everything here is in the `SuperEmbedderUnreal` module (headers under
`Plugins/SuperEmbedderUnreal/Source/SuperEmbedderUnreal/Public/`), except the bake, which is editor-only
(`SuperEmbedderUnrealEditor`).

## The rules every request follows

- **Nothing encodes on the game thread.** The open, the token counts, every encode and the async rankings run on
  the plugin's workers.
- **One result per request, never inside the call.** Each request returns a ticket (`FSuperEmbedderTicket`) and
  later gets exactly one of *Completed*, *Refused* or *Cancelled*, on a later game-thread tick. A refusal known at
  once (an empty text, a full queue) and a cache hit are delivered the same way, on the next tick.
- **Refusals are named.** Every result carries an `ESuperEmbedderStatus` and a `Message` with the detail (a row, a
  token count, a library status). *Status To String* prints a status's name.
- **A world's requests end with the world.** A request made with a world context is cancelled when that world is
  cleaned up (the end of a play-in-editor session, a level change), and its *Cancelled* is delivered.
- **Vectors carry their origin.** An `FSuperEmbedderVector` holds the floats, the encoder fingerprint and the kind
  (query or document). Every helper refuses to compare vectors whose fingerprints differ (`FingerprintMismatch`).

## Blueprint nodes

The async nodes each have a Cancel on their *Async Action* pin.

| Node | Pins | What it does |
|---|---|---|
| **Open SuperEmbedder Encoder** (Model File, Tokenizer File) | Opened, Failed; both give the open result and the handle | Opens the encoder, or joins the open in progress with the same files. Empty paths use Project Settings. A different model while one is open is refused `DifferentModelOpen`. Keep the handle. |
| **Encode Text (Async)** (Text) | Completed, Refused, Cancelled | Encodes a query. The result has the vector and whether it came from the cache. |
| **Encode Documents (Async)** (Rows) | Completed, Refused, Cancelled | Validates the rows as a whole (ids unique and valid, every text within bounds), then encodes them one at a time on the documents lane. The result has the ids and one vector per row, in row order. A cancel or the world's end stops it between rows and delivers nothing partial. |
| **Top K (Async)** (Set, Query, K, Exclude Ids) | Completed, Refused, Cancelled | Ranks a set's rows against a query vector on a worker, over a snapshot of the set taken at the call. |
| **Query Text Top K (Async)** (Set, Text, K, Exclude Ids) | Completed, Refused, Cancelled | Encodes the text as a query, then ranks the set as it is when the encode completes. The result has the hits and the encoded query. |

Other Blueprint functions:

| Function | What it does |
|---|---|
| **New Vector Set** | A transient vector set, for a run-time bake. |
| **Cosine** (A, B) | Cosine similarity of two vectors from the same encoder. Game thread. |
| **Top K** (Set, Query, K, Exclude Ids) | The synchronous ranking, on the game thread. Use *Top K (Async)* for a large set. |
| **Status To String** | A status's name. |
| **Get Vector Dims**, **Get Vector Digest** | A vector's length, and the SHA-256 of its float32 values (little-endian), as hex. |
| **Release** (on the handle), **Is Held** | Release a handle; the encoder closes when the last one is released or garbage collected. |
| **Get Encoder State**, **Is Encoder Ready**, **Get Open Result** | The encoder's state (`Closed`, `Opening`, `Ready`, `Failed`, `Closing`) and the last open's outcome. |
| **On Encoder State Changed** | An event on the subsystem, broadcast on the game thread. |
| **Cancel** (Ticket) | Cancels a request; its *Cancelled* fires once, on a later tick. False for an unknown or already delivered ticket. |
| **Save Encode Cache** (Path), **Get Encode Cache Entries** | Writes the cache to a file (off the game thread), for `CacheFile` to load at a later start. |
| **Release Keep Alive** | Releases the editor's own handle (also the console command `SuperEmbedder.CloseEncoder`). |

## Vector sets

`USuperEmbedderVectorSet` holds ids, source texts, rows of float32 values, the dimensions, the encoder fingerprint
and a digest of the rows. It holds no encoder open and can outlive the encoder. A set belongs to the game thread:
*Append*, *Remove* and the synchronous helpers refuse `WrongThread` elsewhere.

| Function | What it does |
|---|---|
| **Append** (Id, Text, Vector) | Adds a row. The first row of an empty set gives the set its fingerprint and dimensions; later rows must match. Refuses an unchecked, zero or non-finite vector, an invalid id (empty, not UTF-8, or over 256 bytes) and a duplicate id. |
| **Append Documents** (Documents, Texts) | Appends a whole *Encode Documents* result, all rows or none. |
| **Remove** (Id), **Reset** | Removes a row; empties the set. |
| **Num**, **Contains**, **Get Ids**, **Get Text**, **Get Row Vector**, **Get Dims**, **Get Fingerprint Hex** | Reads. |
| **Get Load Status** | `Ok`, or why a loaded set came up empty (`SetCorrupt` when its rows fail their digest, or the saved form's own refusals). |
| **Save To Bytes**, **Load From Bytes** | The set's saved form, for your own save data. Loading checks the digest and refuses each kind of damage by name. |

A set saved as an asset is checked on every load, where the engine serializes it (on the async loading thread
for a streamed load).

Ranking is exact: every row is scored. Equal scores rank the lower row first. Each hit has `Id`, `Score` (cosine,
-1 to 1) and `RowIndex`. `K` is 1 or more; fewer rows than `K` gives every row.

## C++

`USuperEmbedderSubsystem::Get()` returns the engine subsystem. Its callbacks are `TFunction`s, called on the game
thread.

```cpp
USuperEmbedderEncoderHandle* OpenEncoder(const FString& ModelFile, const FString& TokenizerFile,
                                         FSuperEmbedderOpenCallback OnDone);
FSuperEmbedderTicket EncodeText(const FString& Text, const UObject* WorldContext, FSuperEmbedderEncodeCallback OnDone);
FSuperEmbedderTicket EncodeDocuments(const TArray<FSuperEmbedderDocumentRow>& Rows, const UObject* WorldContext,
                                     FSuperEmbedderDocumentsCallback OnDone);
FSuperEmbedderTicket TopKAsync(USuperEmbedderVectorSet* Set, const FSuperEmbedderVector& Query, int32 K,
                               const TArray<FString>& Exclude, const UObject* WorldContext, FSuperEmbedderTopKCallback OnDone);
FSuperEmbedderTicket QueryTextTopK(USuperEmbedderVectorSet* Set, const FString& Text, int32 K,
                                   const TArray<FString>& Exclude, const UObject* WorldContext, FSuperEmbedderTopKCallback OnDone);
bool Cancel(FSuperEmbedderTicket Ticket);
bool GetProgress(FSuperEmbedderTicket Ticket, int32& OutDone, int32& OutTotal) const;   // a documents ticket's rows
ESuperEmbedderStatus EncodeTextBlocking(const FString& Text, ESuperEmbedderVectorKind Kind,
                                        FSuperEmbedderVector& OutVector, FString& OutMessage);
void SaveEncodeCache(const FString& Path);
```

- `OpenEncoder` returns the handle at once (null when the open is refused at once; the callback still fires, on
  a later tick, with the refusal). Hold the handle in a `UPROPERTY` for as long as you encode.
- A null `WorldContext` makes a request no world owns (for editor utilities and commandlets).
- `EncodeTextBlocking` waits for the encode on the calling thread. It refuses on the game thread
  (`GameThreadBlockingRefused`) and on the plugin's own workers (`WorkerThreadBlockingRefused`). Use it from a
  thread of your own.
- `FSuperEmbedderVector::MakeUnchecked` wraps raw floats as an unchecked vector. Every helper refuses an unchecked
  vector (`UncheckedVector`), except *Cosine* between two unchecked vectors.

## The editor bake

- **DataTable**: right-click the table > *Bake SuperEmbedder Vector Set*; it writes `<Table>_VectorSet` beside the
  table. The row name is the id and the row's string or text property named `Text` is the text
  (`FSuperEmbedderTextRow` is such a row struct).
- **Console**: `SuperEmbedder.BakeDataTable </Game/Path/Table.Table> </Game/Path/SetName>`,
  `SuperEmbedder.BakeJsonl <file> </Game/Path/SetName>`, `SuperEmbedder.CancelBake`.
- **Commandlet**: `-run=SuperEmbedderBake -Input=<JSONL file | /Game/Path/Table.Table> -Output=/Game/Path/SetName`.

A JSONL input has one `{"id": "...", "text": "..."}` object per line; each refusal names its line. The bake encodes
through the documents lane, shows progress with a Cancel button, and writes the asset only when every row has
encoded, so a cancel or a refusal leaves nothing behind.

**Validation**: `SuperEmbedder.ValidateSets` in the console, or `-run=SuperEmbedderValidate`, lists every vector set
asset whose rows fail their digest or whose fingerprint differs from the encoder's. The commandlet exits 1 when
any does, or when the encoder does not open.

## Development console commands

In the editor and in non-Shipping builds:

| Command | What it does |
|---|---|
| `SuperEmbedder.EncodeText <text>` | Encodes the text as a query, opening the encoder from Project Settings if none is ready, and logs the outcome, dimensions, vector digest and fingerprint. |
| `SuperEmbedder.EncodeDocuments <file>` | Encodes a JSONL file's rows through the documents lane and logs each row's id and vector digest. |
| `SuperEmbedder.DumpSet </Game/Path/SetName>` | Loads a vector set asset and logs its load status, rows, dimensions, fingerprint and each row's digest. |
| `SuperEmbedder.CloseEncoder` | Releases the editor's keep-alive handle. |

## Statuses

`ESuperEmbedderStatus`, in `SuperEmbedderTypes.h`. The ones you are most likely to meet:

| Status | Meaning |
|---|---|
| `EncoderNotReady` | No encoder is open, or it is still opening. |
| `QueueFull` | Too many requests are waiting (`MaxPendingQueries`, `MaxPendingDocumentTickets`). |
| `QueryTooLong`, `DocumentTooLong` | Over the token bound. The text is never truncated. |
| `EmptyText`, `InvalidText`, `InvalidUtf8`, `InvalidId`, `DuplicateId`, `EmptyInput` | The input itself. |
| `ModelFileMissing`, `TokenizerFileMissing`, `NotAnSslmArtifact`, `ArtifactShaMismatch`, `OpenFailed` | The open. The message carries the library's own status. |
| `DifferentModelOpen`, `ModelInUse` | Another model is open, or still held. |
| `FingerprintMismatch`, `DimsMismatch` | Vectors or a set from a different encoder. |
| `UncheckedVector`, `ZeroVector`, `NonFiniteVector` | A vector no encoder produced, or one that cannot be ranked. |
| `SetCorrupt`, `BadMagic`, `UnsupportedVersion`, `Truncated`, `Malformed`, `EntryDigestMismatch` | A saved set or cache file that is damaged or not one. |
| `WrongThread`, `GameThreadBlockingRefused`, `WorkerThreadBlockingRefused` | A call from a thread it does not allow. |
| `Cancelled`, `ShuttingDown`, `EncoderClosing` | The request was cancelled, or the engine or encoder is going away. |
| `InvalidK`, `UnknownId`, `UnknownTicket`, `NullSet` | A bad argument. |
