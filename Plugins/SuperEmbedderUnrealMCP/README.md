# SuperEmbedder MCP Toolset

MCP tools over SuperEmbedder: the encoder's status, encode a text, top-k against a named vector set, list the
vector sets, and validate them, from a connected MCP client. It ships as its own plugin beside
`SuperEmbedderUnreal`, disabled by default.

## Requirements: read this first

This plugin depends on **Experimental, `NoRedist` engine plugins** that stock engine distributions are expected
not to include:

- `ToolsetRegistry`: the tool-authoring surface this plugin registers with (a hard dependency).
- `ModelContextProtocol`: the MCP server that exposes registered toolsets to clients (used at run time, never
  depended on directly).

In practice that means **a GitHub-source engine build**. Everything else in SuperEmbedder-Unreal works without
them; this plugin stays disabled unless your engine can host it. Like `SuperEmbedderUnreal`, its module is
Win64-only.

## Enabling

1. Confirm your engine tree has `Engine/Plugins/Experimental/ToolsetRegistry` and
   `Engine/Plugins/Experimental/ModelContextProtocol`.
2. Enable `SuperEmbedderUnreal`, `SuperEmbedderUnrealMCP` and `ModelContextProtocol` in your project.
3. Start the editor; the MCP server listens (default port 8000). The toolset registers as
   `SuperEmbedderUnrealMCP.SuperEmbedderToolset`: discover it through the server's `list_toolsets`,
   `describe_toolset` and `call_tool` meta-tools.

## Tools

| Tool | What it does |
|---|---|
| `EncoderStatus` | The encoder's state, the last open's status and detail, dims, fingerprint, the cache's entry count, and the settings an open reads (resolved model and tokenizer paths and whether they exist, token bounds, concurrency, cache bound). Opens nothing |
| `EncodeText` | Encodes a text as a query. Returns a **digest plus a summary**, never the whole vector: the vector digest (SHA-256 of the float32 values, little-endian), fingerprint, dims, kind, whether it came from the encode cache, the L2 norm, min, max, mean and the first 8 components. Refusals are named (`QueryTooLong`, `InvalidUtf8`, `EmptyText`, `QueueFull`, ...) |
| `QueryText` | Encodes a text as a query and returns the top `K` (1-1000) rows of a named vector set by exact cosine, best first, each with id, text, score and row index, plus the query's digest. `SetPath` is an asset's object path (or its package path), or a live run-time set's path as `ListSets` reports it |
| `ListSets` | Every vector set asset in the project and every live run-time set (New Vector Set): path, asset or not, rows, dims, fingerprint, load status, and whether it matches the encoder's fingerprint when the encoder is Ready. Opens nothing |
| `ValidateSets` | What `-run=SuperEmbedderValidate` checks: lists each set asset whose row digest failed on load or whose fingerprint differs from the encoder's. Returns `passed`, the counts and one report line per set |

`EncodeText`, `QueryText` and `ValidateSets` are asynchronous: each returns at once and completes on a later
game-thread tick. They need a Ready encoder and open it from Project Settings when none is: **the first call of
an editor session waits for the open**, which takes tens of seconds on the published model (the editor then keeps
the encoder open). Call `EncoderStatus` first if your client has a short tool timeout. An open that fails
completes the call with its reason.

**Read-only by design**: no tool writes, saves, appends to or removes from a vector set, and none writes the
encode cache file. Baking a set stays in the editor (right-click a DataTable, or `-run=SuperEmbedderBake`).

## Tests

`Private/Tests/SuperEmbedderMCPTests.cpp` (UE automation, editor context):

- `SuperEmbedder.MCP.SyncTools`: `EncoderStatus` and `ListSets` answer with their documented fields, open
  nothing, and `ListSets` names a live run-time set;
- `SuperEmbedder.MCP.Refusals`: `QueryText` refuses `K` out of range and an empty or unknown set path at once;
- `SuperEmbedder.MCP.EncodeAndQuery` (needs the model files in Project Settings, and fails by name without them):
  `EncodeText` twice gives one digest and a summary without the vector, `QueryText` over a three-row run-time set
  returns every row ranked best first with its text, and `ValidateSets` answers.

`ci/tests/test_toolset_surface.py` (Python, any host) checks the tool list, this table, the plugin's dependencies
and that the tools are read-only, from source.

## Licensing

Apache-2.0 (see `LICENSE` and `NOTICE`). Contains no engine code.
