# SuperEmbedder

SuperEmbedder turns text into embedding vectors whose bytes are identical on every certified build and
machine. It is a C++20 library with a C interface, and a command line tool, `semb`, that bakes a corpus into
a vector bank and encodes queries against it.

It runs [Qwen3-Embedding-0.6B](https://huggingface.co/Qwen/Qwen3-Embedding-0.6B) on the
[SuperSLM](https://github.com/dansupergameprogrammer/SuperSLM) engine, whose forward is integer arithmetic.
It selects the last token's hidden state and builds each output `float32` from its exact integer value, in
integer code. No floating-point operation computes an output element, so the result does not depend on the
compiler, its optimisation level, the CPU's vector width, or the calling thread's floating-point settings.
[docs/POOLING.md](docs/POOLING.md) specifies the construction.

Its banks are read by [SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss), whose search is
bit-identical from the bank onward. Together the two give the same hits for the same text on every
certified machine.

| Project | Role |
|---|---|
| [SuperSLM](https://github.com/dansupergameprogrammer/SuperSLM) | The integer transformer engine SuperEmbedder runs on. SuperEmbedder 1.0 is pinned to SuperSLM v1.18.1 |
| [SuperFAISS](https://github.com/dansupergameprogrammer/superfaiss) | Searches the banks SuperEmbedder bakes, with the query vectors it emits |

**Contents:** [Get the model](#get-the-model) · [Build and install](#build-and-install) ·
[Quick start](#quick-start) · [The library](#the-library) · [The GPU](#the-gpu) ·
[Using a bank with SuperFAISS](#using-a-bank-with-superfaiss) · [Certified platforms](#certified-platforms) ·
[Determinism](#determinism) · [Readiness](#readiness) · [Retrieval quality](#retrieval-quality) ·
[Limits](#limits) · [License](#license)

## Get the model

SuperEmbedder needs two files: the converted model and its tokenizer. **Download them from the
[1.0.0 release](https://github.com/dansupergameprogrammer/SuperEmbedder/releases/tag/v1.0.0)** and check their
SHA-256:

| File | Size | SHA-256 |
|---|---:|---|
| `qwen3-embedding-0.6b-1p5.sslm` | 634,489,808 bytes | `0be28bf42637264df85481eb925c46f674da7db97fd7601c6732881ad8f99fbf` |
| `qwen3-embedding-0.6b-tokenizer.sslm` | 3,465,492 bytes | `2ba15e621e28f4f58046a89a4e4b1e3e6ea8c4e1a4f5397238634b589082b9d3` |

```sh
sha256sum qwen3-embedding-0.6b-1p5.sslm qwen3-embedding-0.6b-tokenizer.sslm   # Linux
certutil -hashfile qwen3-embedding-0.6b-1p5.sslm SHA256                        # Windows
```

**Converting instead.** You can produce the files yourself from the Hugging Face checkpoint at revision
`97b0c614be4d77ee51c0cef4e5f07c00f9eb65b3`, with SuperSLM `1.18.1`'s converter:

```sh
# In a SuperSLM 1.18.1 checkout, after building its verifier (see SuperSLM's quickstart):
python tools/t2700_fused_k_calibration.py flow --checkpoint <checkpoint dir> \
    --out qwen3-embedding-0.6b.sslm --work <scratch dir> \
    --capture build/t2700_fused_k_capture --channel-scale-headroom 1.25 \
    --verifier build/sslm_verify
python tools/convert_tokenizer.py --ckpt <checkpoint dir> --emit qwen3-embedding-0.6b-tokenizer.sslm
```

The model conversion takes about 1.5 hours (5,226 s on an AMD Ryzen 9 3950X).
**A converted file is certified only if its bytes equal the published ones.** Check its SHA-256 against the
table above. A file that differs is a different model: it gives a different encoder fingerprint, its banks
are refused by every build using the published files, and no certification covers it.

## Build and install

Requires CMake 3.21 or later, Ninja, Git, Python 3.10 or later, and a C++20 compiler. Running the tests
requires Python 3.13 and NumPy (`python -m pip install numpy`); use that Python for the recipe below.
On Windows, use a Developer PowerShell with the compiler you mean to use. On Linux and macOS, use a shell:

```sh
# 1. Build and install the pinned engine. This clones SuperSLM at the tag in superslm.lock,
#    builds it with the compiler in your current environment, and records how it was built.
python tools/pin-superslm/pin_superslm.py --lock superslm.lock --prefix build/engine

# 2. Configure, build and test.
cmake -G Ninja -S . -B build/semb -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/build/engine"
cmake --build build/semb
ctest --test-dir build/semb --output-on-failure

# 3. Install.
cmake --install build/semb --prefix "<install dir>"
```

On Windows:

```powershell
# 1. Build and install the pinned engine.
python tools/pin-superslm/pin_superslm.py --lock superslm.lock --prefix build/engine

# 2. Configure, build and test.
cmake -G Ninja -S . -B build/semb -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/build/engine"
cmake --build build/semb
ctest --test-dir build/semb --output-on-failure

# 3. Install.
cmake --install build/semb --prefix "<install dir>"
```

**Build the engine and SuperEmbedder in the same compiler environment.** The engine's tier kernels are
compiled by whatever compiler step 1 finds, and they are part of what a certified cell certifies. Configure
refuses an engine built with a different compiler ID, version or frontend. Encoder open checks the compiler
and standard-library macros and refuses a mismatch (`SEMB_ENGINE_TOOLCHAIN_MISMATCH`).

**Add no compile flags.** Every target is built with `-ffp-contract=off` (GCC, Clang) or `/fp:precise` (MSVC)
and never with fast-math, and the build fails if those are missing. A build with any flag beyond its cell's
recorded set still works, but it is uncertified: see [Certified platforms](#certified-platforms). Flags proven
unable to change code — warning levels, and debug information on MSVC and GCC — are not recorded and do not
affect certification.

**Unset `CL`, `_CL_` and `CCC_OVERRIDE_OPTIONS`.** Each passes options to the compiler that no compile line
shows, so the engine pin tool and the build both refuse to run while any is set
(`SEMB_PIN_AMBIENT_COMPILER_INPUT`, `SEMB_BUILD_AMBIENT_COMPILER_INPUT`), naming the variable.

The compiler environment each certified cell was built in:

| Member | Compiler | Environment |
|---|---|---|
| `msvc19.33-auto` | MSVC 19.33.31631 | Windows 11 Pro 10.0.26300; Visual Studio Community 2022 17.14, an x64 shell from `vcvars64.bat -vcvars_ver=14.33` (MSVC toolset 14.33.31629, `cl` 19.33.31631) |
| `clangcl18.1.8-auto` | clang-cl 18.1.8 | Windows 11 Pro 10.0.26300; Visual Studio Community 2022 17.14, the same `vcvars64.bat -vcvars_ver=14.33` shell (MSVC STL 202205L), with `CC` and `CXX` set to clang-cl 18.1.8 |

### Using the installed library

The install puts a shared library, the headers `superembedder/semb.h` and `superembedder/version.h`, the `semb`
tool, and a CMake package in `<install dir>`. The library exports only the `semb_*` functions declared in
`semb.h`; the engine is linked into it privately. In `version.h`, only the `SEMB_VERSION_*` macros are for
consumers; `superembedder::Version()` is not exported from the shared library.

```cmake
find_package(superembedder 1.0 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE superembedder::superembedder)
```

**Source builds and applications that link SuperEmbedder statically must link the engine SuperEmbedder was
configured against.** The installed shared library carries its engine privately. At every encoder and bank
open, before reading any file, the library checks that engine and refuses a mismatch by name
(`SEMB_ENGINE_BUILD_UNRECORDED`, `SEMB_CPU_TIER_UNSUPPORTED`, `SEMB_ENGINE_TOOLCHAIN_MISMATCH` or
`SEMB_ENGINE_LINK_MISMATCH`; [docs/FORMAT.md](docs/FORMAT.md#53-the-check-at-open) gives the order).

If you build SuperEmbedder from source and link its static library into your application instead, it is certified only where every
translation unit of yours that includes its headers compiles with the certified cell's options.

## Quick start

A corpus is either a `.jsonl` file, one `{"id": "...", "text": "..."}` object per line (blank lines skipped), or
any other text file, one document per line, whose ids are the zero-based line numbers.

```sh
# Bake a corpus into a bank: docs.wvbank.json, docs.wvbank.bin, docs.semb.json, docs.skipped.json
semb bake corpus.jsonl --out docs --model <model>.sslm --tokenizer <tokenizer>.sslm

# Encode one query against that bank. Prints VECTOR_HEX=<lowercase hex of the float32 bytes>
semb query docs "how do I reset the router" --model <model>.sslm --tokenizer <tokenizer>.sslm

# Encode many queries, one per line of stdin, on one open encoder
semb query --stdin docs --model <model>.sslm --tokenizer <tokenizer>.sslm < queries.txt

# Re-encode a corpus and check that it reproduces the bank byte for byte
semb verify docs --model <model>.sslm --tokenizer <tokenizer>.sslm --corpus corpus.jsonl

# Print a bank's manifest
semb inspect docs.semb.json

# Print the version, the engine pin, and this build's certification verdict on this host
semb --version
```

Details a script may depend on:

- **Lines.** A line ends at LF only. In plain-text corpora and stdin queries, a CR is part of the text, so
  CRLF line endings encode each line with a trailing CR. JSONL encodes the decoded `text` member;
  whitespace after the object, including a CR, is ignored. A final line without an LF still counts.
- **Output modes of `semb query`:** `--hex` (the default) prints `VECTOR_HEX=` and the vector's bytes as
  lowercase hex; `--raw` writes the raw `float32` bytes and cannot be combined with `--stdin`; `--top k` prints
  `{"hits":[{"id":"..."}, ...]}`, the bank's `k` nearest rows by an exact cosine scan in double precision.
  **`--top` is a diagnostic, not a search engine**: it checks a bank by brute force. Search with SuperFAISS.
- **Encoder opens.** The commands that encode — `semb bake`, `semb query` (including `--stdin`) and
  `semb verify` — open an encoder. `inspect`, version output and `query --help` do not. One-shot `semb query`
  is a documented diagnostic; `semb query --stdin` reuses one encoder and checked query handle for the stream.
  The [Readiness](#readiness) timing runs from process start through the first query result, including encoder
  and bank open. The library pays encoder-open cost once per handle; that cost is not measured separately here.
- **Bake concurrency.** `semb bake` encodes up to 8 documents at a time, and never more than the corpus has
  documents or the machine has hardware threads. `--max-concurrency N` replaces the 8. The bank's bytes
  do not depend on it.
- **Empty documents.** A document whose text is empty is skipped, not refused. Each one is reported on stderr as
  `skipped "<id>": SEMB_INPUT_EMPTY`, with the id escaped as a JSON string, and listed in `<stem>.skipped.json`
  (`[]` when none). Any other defect in a document, such as an invalid or duplicate id, refuses the bake.
- **stderr also carries one engine line per encoder open**, beginning `preflight: ` — a report the SuperSLM
  engine writes whenever it loads a model. It is expected, it is not an error, and it is not a skip report.
  `semb query --stdin` also writes `query_handle_opens=1` to stderr when it finishes.
- **Errors** print `<where>: SEMB_<NAME> (<number>)` on stderr and exit with status 1. Prefer status names in
  logs and tests; existing status names and numeric values never move.

## The library

`include/superembedder/semb.h` is the whole interface. Every function except `semb_status_name` returns a `semb_status`.

**Encoders.** `semb_encoder_open` opens an encoder from the model and tokenizer files;
`semb_encoder_open_memory` opens one from those files' bytes already in memory, and copies them, so you may
release your buffers when it returns. Opening loads the model and tokenizer and executes the witnesses;
open one encoder per process and reuse it. `semb_encode` encodes one document; `semb_encode_batch` encodes many,
all or nothing; `semb_encode_query` encodes one query, applying the query prefix. `semb_encode` and
`semb_encode_query` return the encoder fingerprint with their result. `semb_encode_batch` returns rows and
the required byte count, with no fingerprint; use `semb_fingerprint_of` to read the encoder's fingerprint.

**Banks and queries.** `semb_bank_open` opens a baked bank and checks it. `semb_query_open` binds an encoder to
a bank, and refuses a bank baked by an encoder with a different fingerprint (`SEMB_FINGERPRINT_MISMATCH`).
`semb_query_encode` then encodes queries for that bank.

**Documents and queries are composed differently.** A document is encoded exactly as given. A query is
prefixed with the fixed instruction [POOLING.md](docs/POOLING.md#1-composition) gives. Use the query functions
for queries and never add the prefix yourself.

**Memory.** `max_concurrency` in the open configuration is the number of encodes that may run on the handle at
once; zero means one, and a call beyond it returns `SEMB_CONCURRENCY_EXCEEDED`. Opening reserves nothing in
proportion to `max_concurrency`, and the handle holds no encode workspace when open returns. Each encode's
workspace is sized to its document: 57,344 bytes per position, for the next power of two at or above the
document's positions, at least 64 and at most the 32,768-position context. It is kept for that slot's next
encode. A workspace above 4,096 positions is released after its encode, and only one such encode runs at a time
per handle. When an encode's workspace cannot grow, that encode returns `SEMB_ALLOCATION_FAILED` and the handle
stays usable. Open returns `SEMB_ALLOCATION_FAILED` when the model, the tokenizer or the witness cannot be
allocated. Measured host memory after open: 635.6 MiB (working set), with a peak of 1,233.7 MiB during the
open, on the `msvc19.33-auto@avx2` build on an AMD Ryzen 9 5950X, CPU device.

**Errors.** No function declared in `semb.h` lets a C++ exception escape: every call except `semb_status_name` returns a `semb_status`.
When a call on a bank, a query, or an encoder opened for the CPU fails because memory cannot be allocated, it
returns `SEMB_ALLOCATION_FAILED`, and every handle the call used stays usable. On an encoder opened for the GPU,
memory exhausted, host or GPU, returns `SEMB_ALLOCATION_FAILED`, and the handle stays usable, with one rare
exception: if the GPU's pending work cannot be confirmed stopped after the failure, the call returns
`SEMB_DEVICE_UNAVAILABLE`, and every GPU encoder in the process may return it from then on. A lost device
returns `SEMB_DEVICE_UNAVAILABLE`, and the handle stops using the GPU; any other GPU failure returns
`SEMB_ENGINE_REFUSED`, and the handle stays usable. When the operating
system or the C runtime cannot open or read a model, tokenizer or bank file, the call returns
`SEMB_ARTIFACT_UNREADABLE`, whatever the reason and whichever device an encoder is opened for, a shortage of
memory included; memory the library allocates to hold a file's bytes falls under the clauses above. Any other
internal failure returns `SEMB_ENGINE_REFUSED`, or `SEMB_ARTIFACT_UNREADABLE` from `semb_bank_open`.

This covers the library's functions. It does not describe what the `semb` tool leaves on disk when a bake fails
partway.

`semb_status_name` gives a status's name. Prefer status names in your own code's logs and tests. The enum is
append-only: existing names and numeric values never move.

## The GPU

On Windows, SuperEmbedder can encode on a Direct3D 12 GPU. **The vectors are the same bytes as the CPU's**, and
the encoder fingerprint does not name the device, so a bank baked on the GPU is queried on the CPU and the
reverse.

**Build.** In the Windows Developer PowerShell, pin the engine with GPU support and configure with the option on:

```powershell
python tools/pin-superslm/pin_superslm.py --lock superslm.lock --prefix build/engine --with-gpu
cmake -G Ninja -S . -B build/semb -DCMAKE_BUILD_TYPE=Release -DSEMB_BUILD_GPU=ON -DCMAKE_PREFIX_PATH="$PWD/build/engine"
```

Configure refuses `SEMB_BUILD_GPU=ON` off Windows, and when the engine install has no GPU library
(`SEMB_ENGINE_GPU_MISSING`). A GPU-enabled build is the same certified cell as the CPU-only build of the same
compiler. The verified GPU toolchain is MSVC.

**Shaders.** The engine's compiled shaders must be in a `shaders` directory beside the executable that loads
SuperEmbedder. The build and `cmake --install` put them beside `semb`; an application that links the library
must ship them beside its own executable.

**Use.** Set `device` to `SEMB_DEVICE_GPU` in `semb_config`, or pass `--device gpu` to `semb bake` or
`semb query`. The default is the CPU. `semb verify` always runs on the CPU. The GPU path is selected through
`semb_config.device`; `semb_encoder_open_memory` always runs on the CPU.

- **No fallback.** If the GPU cannot be used, the open is refused (`SEMB_DEVICE_UNAVAILABLE`); it never runs on the
  CPU instead. A build without GPU support refuses `SEMB_DEVICE_GPU` (`SEMB_DEVICE_UNSUPPORTED`).
- **Every GPU open checks itself.** It encodes 22 witness sequences on the GPU and on the CPU and refuses unless
  every result is byte-identical (`SEMB_DEVICE_DIVERGED`). This makes a GPU open slower than a CPU open.
- **Choosing an adapter.** `SSLM_GPU_ADAPTER_INDEX` selects the GPU. **Set it before the process's first GPU open
  and do not change it.** On a machine with two GPUs, changing it later can split an encoder's work across
  adapters. An invalid value refuses the open, and an open with a corrected value succeeds.
- **`SSLM_GPU_ENABLE_DEBUG_LAYER`** enables Direct3D validation. Set it before the process starts; setting it
  after a GPU is in use removes that device.
- **After a device loss** (a driver reset, a timeout, a removed GPU), every later GPU open in the process is
  refused `SEMB_DEVICE_UNAVAILABLE` until the process restarts.
- **Memory.** A document too long for the free GPU memory is refused `SEMB_ALLOCATION_FAILED`, and the handle
  keeps working. Free memory, or bake that document on the CPU. Measured at 1.18.1 on an AMD Radeon RX 7900 XT:
  an open GPU encoder holds 472.06 MiB of GPU memory, the model and the open's own checks together; a document
  at the 32,768-position context cap peaks at 2,300.31 MiB, about 58,504 bytes per position beside the open
  encoder.
- **Very long documents on slow GPUs.** Windows resets a GPU that takes too long on one submission (2 s by
  default). A document near the 32,768-token context can trigger that reset on a slow GPU, which ends GPU work
  for the process. On a slow GPU, bake such documents on the CPU. On the AMD Radeon RX 7900 XT, a
  32,768-position document and a 32,767-position capture (2 runs) completed with no device reset. The longest
  single submission was not timed, so 1.0 gives no speed below which a GPU is at risk.

**What the GPU buys.** Measured at 1.18.1 on a GPU-enabled MSVC 19.33 build, on an AMD Ryzen 9 5950X
(16 cores) with an AMD Radeon RX 7900 XT (driver 32.0.21030.2001), Windows 11 Pro 10.0.26300. Bake rates are
forward positions per second (each document's tokens plus its end token), whole-process wall time with one
encoder open included. Each corpus is split into thirds by document length:

| Bake, positions/s | Documents | Positions | CPU, 1 worker | CPU, 8 workers | GPU |
|---|---:|---:|---:|---:|---:|
| Certification corpus, shortest third | 79 | 648 | 28.83 | 87.23 | 30.98 |
| Certification corpus, middle third | 80 | 991 | 40.35 | 131.63 | 43.50 |
| Certification corpus, longest third | 80 | 1,327 | 47.53 | 160.62 | 52.68 |
| 80 long FiQA-2018 documents, shortest third | 26 | 1,570 | 55.00 | 179.08 | 59.86 |
| 80 long FiQA-2018 documents, middle third | 27 | 3,089 | 65.57 | 248.13 | 82.80 |
| 80 long FiQA-2018 documents, longest third | 27 | 7,385 | 72.13 | 329.87 | 110.11 |
| One 4,096-token document | 1 | 4,097 | 35.54 (115.28 s) | — | 62.68 (65.36 s) |

| | CPU | GPU |
|---|---:|---:|
| Query on an open handle (100 FiQA-2018 queries) | 0.4096 s | 0.2149 s |
| Encoder open | 9.43 s | 15.41 s |
| One `semb query` process, end to end | 10.00 s | 14.79 s |

**For bakes, the CPU is faster.** `semb bake` uses 8 workers by default, and at 8 workers the CPU bakes about
three times the GPU's rate on these corpora. The GPU is 7% to 53% faster than one CPU worker, so a GPU bake
mainly leaves the CPU free. On one long document the GPU is about 1.8 times one CPU worker. **For queries on a
handle that stays open** — the library, or `semb query --stdin` — the GPU takes about half the CPU's time.
**A single `semb query` process is faster on the CPU** (10.00 s against 14.79 s), because the GPU open costs
more than one query saves.

**What the GPU guarantee rests on**, in three parts:

1. The embedding path is integer arithmetic, which leaves a driver's shader compiler no legal freedom to change
   a result.
2. Identity with the CPU is executed at 1.18.1 on one AMD GPU. At this pin the GPU encode runs the engine's
   multi-row prompt kernels. On an AMD Radeon RX 7900 XT (driver 32.0.21030.2001), a full bake of the
   239-document certification corpus passed the open-time check, and all 239 rows matched the CPU's, with the
   same digest. On the same card, the GPU test suite passed except for one known test issue, since fixed in the
   tests (a timing-record drift in the C64 a/a2 legs, not a vector difference), and a 32,767-token long-context
   correctness capture passed 2 of 2.
3. Every GPU open re-proves byte equality with the CPU on the hardware and driver it is running on.

On a GPU no one has verified, that open-time check covers positions up to the longest witness sequence,
97 positions. Longer positions on that hardware rest on parts 1 and 2 alone, not on an
execution there. GPUs that have been verified are listed under [Verified GPUs](#verified-gpus).

**Known issue: the GPU path is not yet verified on NVIDIA at 1.18.1.** The multi-row path has not been run on
an NVIDIA GPU at this pin. Every GPU open still runs the open-time check and refuses on any difference, but on
NVIDIA nothing beyond those 97 positions has been executed at 1.18.1. Earlier NVIDIA evidence was on the
engine's older one-row path and does not carry over: an NVIDIA RTX 2080 SUPER and an AMD RX 7900 XTX produced
the same bytes at engine 1.7.1 or earlier, and an NVIDIA RTX 5060 was checked at engine 1.17, after the engine
fixed an int64 carry bug in NVIDIA's Blackwell driver.

## Using a bank with SuperFAISS

A SuperEmbedder bank is a SuperFAISS bank: `<stem>.wvbank.json` and `<stem>.wvbank.bin` are SuperFAISS's
interchange format (its `FORMAT.md` §1), `float32`, metric `cosine`, one id per row. The third file,
`<stem>.semb.json`, is SuperEmbedder's manifest; SuperFAISS does not read it.

1. **Bake the search bank** with SuperFAISS's import path (its `docs/INTEGRATION.md` §4): parse the header, then
   `ValidateSourceRows`, `NormalizeRows` (the bank is cosine, and SuperEmbedder's rows are not normalized), and
   `QuantizeRowsInt8` for a bank that is identical across machines, or `PadRowsFloat32`. Then `ValidateBankData`
   at load.
2. **Encode each query** with `semb_query_encode` or `semb_encode_query` in the library, or
   `semb query --raw` / `semb query --stdin` from the command line.
3. **Pass the query to SuperFAISS** as follows: copy its `dims` floats into a 16-byte-aligned `float32` buffer
   of the bank's `paddedDims` (`PaddedDims(dims, quantization)`), with every pad lane zero, and pass that buffer
   to `Query` unchanged. It needs no normalizing: the bank's rows are unit length, so scoring by dot product
   ranks by cosine. This is exactly what the repository's SuperFAISS round-trip test does.

`Exactness::CrossDevice` accepts int8 banks only, and gives identical hits across machines **given identical
bank bytes**. 1.0 has not compared int8 bank bytes baked on two machines, so ship the int8 bank you baked:
re-baking it on another machine, such as a player's, is not guaranteed to give the same bytes.

## Certified platforms

A **cell** is one build of SuperEmbedder running at one CPU tier. The guarantee — same text, same bytes — holds
between any two certified cells. The list is [docs/certified-builds.json](docs/certified-builds.json), and this
table is generated from it.

| Cell | OS | Toolchain | Dispatch | CPU tier | Certified on |
|---|---|---|---|---|---|
| `msvc19.33-auto@avx2` | Windows x64 | MSVC 19.33.31631 | auto | AVX2 | AMD Ryzen 9 5950X |
| `clangcl18.1.8-auto@avx2` | Windows x64 | clang-cl 18.1.8 (MSVC frontend 19.33.31631) | auto | AVX2 | AMD Ryzen 9 5950X |

1.0 certifies what was tested: two cells, one pairwise proof, on one machine. Both are 64-bit Windows builds at
AVX2.

**Not yet certified (planned for a later update):**

- AVX-512 cells for the auto builds.
- Forced-SSE2 builds, for hosts whose best tier is SSE2.
- MSVC 19.44 and GCC 13.3 (Linux) builds.
- NVIDIA GPUs at SuperSLM 1.18.1 (see [the known issue](#the-gpu)).

Adding cells keeps the certification epoch, because the epoch digests only the engine pin and the proof corpus
(`docs/certification-identity.json`), not the list of cells. Banks baked by a 1.0 certified cell stay valid
when those cells are added.

Reading the table:

- **Membership is by exact compiler build.** MSVC 19.33.31631 is a member; another MSVC 19.33 build is not.
- **An auto-dispatching build is certified per tier, and 1.0 certifies AVX2 only.** An `-auto` build picks its
  CPU kernels at run time from the host. On a host whose best tier is AVX-512 or SSE2 it runs a tier with no
  cell, so it is uncertified there. **If you ship a baked bank to players, this matters:** on such a player's
  machine the bank is refused at query open (`SEMB_FINGERPRINT_MISMATCH`) rather than searched with different
  vectors.
- **Check your own build on your own host** with `semb --version`. It prints the SuperEmbedder version, the engine
  pin, the build's descriptor, the tier it dispatched to, the certification line and the SHA-256 of the identity
  file and of the list, the linked engine's build report, and the cell this build is on this host — or
  `uncertified`.

An uncertified build works. It computes a different certification epoch, so its banks and a certified build's
banks refuse each other by name (`SEMB_FINGERPRINT_MISMATCH`) rather than mixing.

### Verified GPUs

A GPU is never a cell: a GPU process takes its build's CPU cell. These GPUs have reproduced their cell's
certified rows on a GPU-enabled build:

| Vendor | Device | Adapter | Driver | Cell |
|---|---|---|---|---|
| `0x1002` (AMD) | `0x744c` | AMD Radeon RX 7900 XT | 32.0.21030.2001 | `msvc19.33-auto@avx2` |

### Checking your build

`docs/certification/reference-bank/` holds a bank of the certification corpus baked by a certified cell.
To check that your build reproduces it, use the `corpus-239-cli.jsonl` corpus file committed beside the bank. Each
row has string `id` and `text` members, with the certification fixture's `label` used as `id`:

```sh
semb verify docs/certification/reference-bank/corpus-239 \
    --model <model>.sslm --tokenizer <tokenizer>.sslm \
    --corpus docs/certification/reference-bank/corpus-239-cli.jsonl
```

`semb verify` exits 0 when every row matches, and otherwise names the mismatch (`SEMB_BANK_DIGEST_MISMATCH`,
`SEMB_CORPUS_DIGEST_MISMATCH`, or `SEMB_FINGERPRINT_MISMATCH` for a build on a different epoch).

## Determinism

**The same text, encoded by two installations whose `encoder_fingerprint` is equal and whose builds are members
of the certified set that fingerprint's `certification_epoch` names, produces byte-identical vectors.**
[docs/DETERMINISM.md](docs/DETERMINISM.md) states what the fingerprint covers, what it does not, and what the
guarantee rests on.

The fingerprint is a SHA-256 over the model's bytes, the tokenizer's bytes, the composition literals, the
executed witness results and the certification epoch. A change to any of these changes it. Every bank
records it, and a query against a bank with a different fingerprint is refused. A code change outside what
the witnesses execute can change output without moving the fingerprint. The certified set and `semb verify`
against the reference bank check for such changes on the certification corpus.

**Epochs and re-baking.** A bank's epoch names a certification line.

- A later release can **add cells** to the line without refusing any existing bank.
- **A new engine pin, a withdrawn cell or a changed proof corpus** starts a new line. Banks from the old line
  are refused by name. **Re-bake them.**

**Threat model:** accidental and stale mismatch. A bundle whose manifest, rows and digests were edited to agree
with each other is not detected. [docs/FORMAT.md](docs/FORMAT.md#6-what-the-checks-do-not-catch) lists each
case.

## Readiness

Measured on the `msvc19.33-auto@avx2` build at the 1.18.1 engine pin, with the 1.0 model and tokenizer, on an
AMD Ryzen 9 5950X, CPU device. Time to ready runs from process start through the first query result,
including process startup, encoder open, bank open and the first encoded query.

| Reading | `msvc19.33-auto@avx2`, AMD Ryzen 9 5950X, CPU device |
|---|---:|
| Time to ready, warm file cache (median of 3 runs) | 9,204 ms |
| Host memory with the encoder open (working set) | 635.6 MiB |
| Peak host memory during open | 1,233.7 MiB |
| Median query, one `--stdin` session, 239 queries | 376.5 ms (289.3 to 512.1 ms) |
| One 256-token document, warm | 2,681 ms (95.48 tokens/s) |
| One 1,024-token document, warm | 12,925 ms (79.23 tokens/s) |
| One 4,096-token document, warm | 95,688 ms (42.81 tokens/s) |

Time to ready from a cold file cache was not measured on this build. On an earlier pre-release build, on an
AMD Ryzen 9 3950X, cold and warm differed by less than 0.1%.

**Plan for these numbers.** On this machine, opening an encoder takes about 9 seconds on the CPU, and a query
on an open encoder about 0.4 s; a slower machine takes longer. Open it once, off the frame — at load time or on
a worker thread — and keep it open. Neither figure is a per-frame budget.

Encode time grows faster than linearly with length. A document at the full 32,768-token context is legal and
takes far longer than the 4,096-token row. A new `semb query` process repeats startup, encoder open,
bank open and its first query after every launch, restart or crash recovery.

## Retrieval quality

How much of a floating-point run's retrieval quality SuperEmbedder keeps, measured as
**R = nDCG@10 of SuperEmbedder ÷ nDCG@10 of the same model run in floating point**, on the same candidate pool,
with a paired 95% bootstrap interval:

| Set | R | 95% interval |
|---|---:|---|
| SciFact | 0.9798 | [0.9630, 0.9965] |
| FiQA-2018 | 0.9654 | [0.9405, 0.9882] |
| SCIDOCS | 0.9383 | [0.9110, 0.9646] |

Measured on an MSVC 19.33.31631 Release build (commit `2001662`) on an AMD Ryzen 9 3950X, over a fixed query-sampled pool per set (SciFact: 300 queries, 411 documents; FiQA-2018: 150 queries, 546 documents; SCIDOCS: 150 queries, 824 documents); the figures are relative to
that pool, not to each set's full leaderboard corpus. They equal the SuperSLM engine's own embedding on the same
pool exactly. English is the only language measured. The 1.0.0 release produces the same document and query vectors as the build measured, so these figures apply to it unchanged.

## Limits

- **A document longer than the context** (32,768 tokens, the end token included) is refused,
  `SEMB_CONTEXT_CAP_EXCEEDED`. It is never truncated. Split long documents yourself.
- **A query is truncated, by design**, to 32,767 tokens after its prefix is applied, and the call returns `SEMB_OK`.
  `semb_interim_query_token_count` — an internal, non-frozen function for integrations that compile
  SuperEmbedder's sources, not exported by the installed library — counts the query's own tokens as composed,
  *before* this truncation, so its count can exceed what is encoded. The two numbers do not disagree.
- **Empty text** is refused (`SEMB_INPUT_EMPTY`); `semb bake` and `semb verify` skip such documents and report
  them (see [Quick start](#quick-start)). A one-token document is valid.
- **Special-token literals.** Text containing a special token's literal, such as `<|endoftext|>`, encodes it as that
  special token, as the reference tokenizer does by default.
- **Bank size.** SuperEmbedder reads banks of at most 10,000,000 rows.
- **Language.** Only English retrieval has been measured.
- **The GPU path is Windows only.**

## License

SuperEmbedder is licensed under the [Apache License 2.0](LICENSE). The release's model and tokenizer downloads
are converted from Qwen3-Embedding-0.6B, also Apache 2.0; see [NOTICE](NOTICE).

Security issues: [SECURITY.md](SECURITY.md). Contributions: [CONTRIBUTING.md](CONTRIBUTING.md) — outside pull
requests are not merged; propose changes as an issue with a gist.
