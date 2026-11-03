#pragma once

#include "CoreMinimal.h"

#include "ToolsetRegistry/ToolsetDefinition.h"

#include "SuperEmbedderToolset.generated.h"

class UToolCallAsyncResultString;

// Maintainer notes, kept out of the class tooltip: the toolset follows SuperFAISSUnrealMCP's shape (a
// UCLASS of reflected AICallable statics) and SuperSLMUnrealMCP's asynchronous results. Do not wrap it in
// a custom #if: UnrealHeaderTool rejects a UCLASS inside a preprocessor block it does not recognize. The
// tool list is the proposal's (rev 9, section 2.4): EncoderStatus, EncodeText, QueryText, ListSets,
// ValidateSets, and no other.

/**
 * SuperEmbedder's MCP toolset, registered with the engine's ToolsetRegistry, which exposes it to MCP
 * clients. Every result is compact JSON. Read-only: no tool writes, saves, appends to or removes from a
 * vector set, and none writes the encode cache file.
 *
 * Threads. The ToolsetRegistry calls every tool on the game thread. EncoderStatus and ListSets answer
 * synchronously; a failure is an {"error": "..."} object. EncodeText, QueryText and ValidateSets return a
 * pending UToolCallAsyncResultString at once and never wait on the game thread: the encodes run on the
 * encoder's own workers. The result is completed on a later game-thread tick, with SetValue and the JSON
 * the tool describes on success, and with SetError and a message naming the refusal on failure.
 *
 * The encoder. The three asynchronous tools need a Ready encoder. When none is Ready they open it from
 * Project Settings > Plugins > SuperEmbedder (or join the open in progress), which takes 20-38 s on the
 * published model, once per editor session: the editor then keeps it open. Call EncoderStatus first to
 * see whether a call will wait for an open. An open that fails completes the call with its reason.
 */
UCLASS()
class SUPEREMBEDDERUNREALMCP_API USuperEmbedderToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * The encoder's state (Closed, Opening, Ready, Failed, Closing), the last open's status and detail, its
	 * dims and encoder fingerprint (64 hex characters), the encode cache's entry count, and the settings an
	 * open reads: the model and tokenizer paths as resolved, whether each file exists, the token bounds,
	 * the concurrency and the cache bound. Opens nothing. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperEmbedder")
	static FString EncoderStatus();

	/**
	 * Encodes Text as a query (the library's query composition), on the encoder's worker, opening the
	 * encoder first if none is Ready. Returns a digest plus a summary, never the whole vector: the vector
	 * digest (SHA-256 of the float32 values, little-endian: equal digests mean bit-identical vectors), the
	 * encoder fingerprint, the dims, the kind, whether the vector came from the exact encode cache, the L2
	 * norm, the minimum, maximum and mean component, and the first 8 components. A refusal is named:
	 * QueryTooLong (over the token bound), InvalidUtf8, EmptyText, QueueFull, EncoderNotReady and so on.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperEmbedder")
	static UToolCallAsyncResultString* EncodeText(const FString& Text);

	/**
	 * Top-K against a named vector set: encodes Text as a query (opening the encoder first if none is
	 * Ready), then ranks the set's rows by exact cosine, best first; equal scores rank the lower row first.
	 * SetPath is an asset's object path (/Game/Lore/LoreSet.LoreSet; the package path /Game/Lore/LoreSet
	 * also resolves) or the object path ListSets reports for a live run-time set. K is 1 to 1000; K above the
	 * row count returns every row. Each hit carries the row id, its text, its cosine score (-1 to 1) and its
	 * row index; the query's digest is returned beside them. A set whose last load failed (SetCorrupt and
	 * the like) is refused by name, and so is a set whose fingerprint differs from the encoder's
	 * (FingerprintMismatch). An unloaded asset is loaded asynchronously. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperEmbedder")
	static UToolCallAsyncResultString* QueryText(const FString& SetPath, const FString& Text, int32 K = 10);

	/**
	 * Lists every vector set: each asset in the project (loaded to read its rows, so the first call can
	 * take a moment) and each live run-time set in memory (made with New Vector Set). For each: its object
	 * path, whether it is an asset, its row count, dims, fingerprint, load status, and, when the encoder is
	 * Ready, whether its fingerprint matches the encoder's (an empty set matches any). Opens nothing.
	 * Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperEmbedder")
	static FString ListSets();

	/**
	 * The validation the -run=SuperEmbedderValidate commandlet runs, opening the encoder first if none is
	 * Ready: loads every vector set asset and lists, by name, each one whose row digest failed on load or
	 * whose fingerprint differs from the encoder's. Returns passed (nothing listed), the counts, and one
	 * report line per set checked. Loads every set asset on the game thread, so its cost grows with the
	 * project's sets. Read-only.
	 */
	UFUNCTION(meta = (AICallable), Category = "SuperEmbedder")
	static UToolCallAsyncResultString* ValidateSets();
};
