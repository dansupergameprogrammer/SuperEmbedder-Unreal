# Grown-struct coexistence fixtures (R-8, R-9)

A later re-vendor can change the layout of an engine struct. When it does, the SuperSLM plugin's plain engine
and SuperEmbedderCore's renamed engine disagree on that struct's layout. If any link name that carries the
struct is shared between the two copies (its constructor or destructor, or a standard-library instantiation
such as `std::vector<sslm_schema_s>`), a monolithic link keeps one body for both copies, and the copy with the
other layout runs it. These fixtures build exactly that re-vendor and show that the rename rule leaves no
such name shared:

| Cell | Grown struct | What it exercises |
|---|---|---|
| R-8 | `sslm_schema_s` | the model's schema handle vector, filled when a two-schema model is mapped |
| R-9 | `sslm_kv_pool_s` | the K/V pool's constructor and destructor, emitted out of line in unoptimized builds (UBT's Debug and DebugGame) |

Everything is generated from the committed vendored tree. Nothing under `Source/` is edited; the grown copy
lives only in the output directory.

## Files

- `make_grown.py`: copies the vendored engine (and the vendored SuperEmbedder, the wrappers and the
  generated configuration headers) to an output directory, inserts one line,
  `unsigned char grown_field[64] = {};`, before the closing `};` of the named struct, and derives the grown
  tree's own rename header with `ci/generate_rename_header.py --plugin-root <out> --lists-dir <out>/rename`.
  It also writes the **control header**: the same header with the grown struct's name left off. If the
  struct's definition is not found exactly once, it stops with `SEMB_GROWN_STRUCT_NOT_FOUND`.
- `two_schema_model.sslm` (135,082 bytes) and its `.sha256`: a small synthetic model with two schemas,
  `grown_accepts_q` and `grown_rejects_q`. The runner checks the hash before it runs.
- `mint_two_schema_model.py`: how the model was made. It runs SuperSLM's own minting tools, so it needs a
  SuperSLM checkout at v1.18.1 (`--superslm <dir>`). `--check` re-mints and compares; minting at the same
  tag reproduces the committed file byte for byte.
- `grown_drv.cpp`, `grown_main.cpp`: the program. The driver is compiled once against each engine copy. Each
  copy maps the model, creates a K/V pool and a sequence, looks up both schemas, binds each one, and
  releases everything.
- `run_grown.py`: the runner. One Python command runs both cases at both optimization levels, under
  AddressSanitizer, with GCC or MSVC. It needs no shell.

## What the runner checks

At each level it builds four object sets: **P**, the SuperSLM plugin's engine, plain; **G**, the grown copy
under its rename header; **C**, the grown copy under the control header; and **U**, the grown copy with no
rename at all.

Required to pass:
- R-1, G vs P: no strong symbol on both sides, and no shared weak or COMDAT symbol that names a listed
  engine identifier. GCC uses `../check_symbols.py`; MSVC uses `ci/msvc/coff_coexist.py`.
- The body check, G vs P. GCC uses `../bodycmp.py`; on MSVC, `coff_coexist.py` does it.
- G and P link into one image in both object orders (plugin objects first, embedder objects first) with no
  duplicate definition.
- Both images run: both copies map the model and bind both schemas with no failure and no sanitizer report.

Required to fail. A check that cannot fail proves nothing, so the runner requires these controls to fail:
- R-1, C vs P, must fail and name a shared symbol that carries the grown struct. For R-8 this holds at
  every level; for R-9, at the unoptimized level only.
- C linked beside P and run must fail in at least one link order, where the original strike failed: R-8 at
  the optimized level (a shared `std::vector<sslm_schema_s>::_M_default_append`), and R-9 at the
  unoptimized level (a shared `sslm_kv_pool_s::sslm_kv_pool_s()`).
- U linked beside P must report duplicate definitions.

At the other levels the runner reports the controls but does not require them to fail. At `-O2`/`/O2`,
`sslm_kv_pool_s`'s constructor is inlined, so no shared symbol carries it. That is why an object-derived
list read at `-O2` missed it.

## GCC (Linux)

```
python3 ci/host/coexist/grown/run_grown.py --toolchain gcc --out <dir>
```

Run from the plugin root. Results from 2026-11-02, GCC 13.3, `-fsanitize=address`, against SuperSLM-Unreal's
vendored engine at v1.13.1 (its 1.2.0) and again at v1.18.1 (its 1.4.0, via `--plain-tree`). The two runs gave
the same results:

| Case | Level | R-1 / body, G vs P | Link, both orders | Run, both orders | Control C: R-1 | Control C: run | Control U: link |
|---|---|---|---|---|---|---|---|
| R-8 | -O0 | pass / pass | 0 duplicates | both copies 0 failures, ASan clean | fails (41 shared, e.g. `sslm_schema_s::sslm_schema_s()`) | passes (informational) | collides |
| R-8 | -O2 | pass / pass | 0 duplicates | both copies 0 failures, ASan clean | fails (`std::vector<sslm_schema_s>::_M_default_append`) | **fails**: plugin first, heap-buffer-overflow in the renamed copy's `sslm_model_map` | collides |
| R-9 | -O0 | pass / pass | 0 duplicates | both copies 0 failures, ASan clean | fails (4 shared: `sslm_kv_pool_s` constructor and destructor) | **fails**: embedder first, heap-buffer-overflow in `sslm_kv_pool_s::sslm_kv_pool_s()` called from the plain copy's `sslm_kv_pool_create` | collides |
| R-9 | -O2 | pass / pass | 0 duplicates | both copies 0 failures, ASan clean | passes (inlined; informational) | passes (informational) | collides |

## MSVC (Windows x64, 2026-11-02)

MSVC 14.44.35207 (`cl` 19.44.35229), `/fsanitize=address`, against SuperSLM-Unreal 1.4.0's engine (v1.18.1, via
`--plain-tree`), with `--host-tu` and `--host-cwd` (below):

| Case | Level | R-1 / body, G vs P | Link, both orders | Run, both orders | Control C: R-1 | Control C: run | Control U: link |
|---|---|---|---|---|---|---|---|
| R-8 | /Od | pass (1,693 shared, 0 strong, 0 listed, 0 differ) | 0 duplicates | both copies 0 failures, ASan clean | fails (`sslm_schema_s::sslm_schema_s()`) | passes (informational) | collides (316) |
| R-8 | /O2 | pass (2,269 shared, 0 / 0 / 0) | 0 duplicates | both copies 0 failures, ASan clean | fails (`sslm_schema_s::sslm_schema_s()`) | **fails**: plugin first, the renamed copy's 2 checks fail | collides (316) |
| R-9 | /Od | pass (1,693 shared, 0 / 0 / 0) | 0 duplicates | both copies 0 failures, ASan clean | fails (2: `sslm_kv_pool_s` constructor and destructor) | **fails**: plugin first ASan new-delete-type-mismatch, embedder first heap-buffer-overflow | collides (316) |
| R-9 | /O2 | pass (2,269 shared, 0 / 0 / 0) | 0 duplicates | both copies 0 failures, ASan clean | fails (informational; MSVC keeps the constructor out of line under ASan, where GCC inlines it) | fails (informational; as at /Od) | collides (316) |

**`--host-tu` is required on MSVC.** Without it the grown tree's list also renames the UE-header names the plugin's
own conflict set keeps out (98 of them, among them `D`, `H` and `T`), and MSVC's decorated names spell builtin types
with those letters, so R-1 G vs P reports plain standard-library symbols such as `_Compressed_pair<allocator<char>,...>`
as listed (470 at /Od, 495 at /O2). Pass the plugin's own host TU, as for its rename header:

```
python Plugins\SuperEmbedderUnreal\ci\host\coexist\grown\run_grown.py --toolchain msvc --out <dir> ^
    --plain-tree <SuperSLM-Unreal's Plugins\SuperSLMUnreal\Source\ThirdParty\SuperSLM> ^
    --host-tu <SuperEmbedderCore's UBT PerModuleInline.gen.cpp.obj.rsp> --host-cwd <engine>\Engine\Source
```

## Running on Windows with MSVC

The machine needs Python 3 and the Visual Studio 2022 C++ tools (MSVC 14.44). Run the command from any
prompt. If `cl.exe` is not on `PATH`, the runner finds `vcvars64.bat` through `vswhere` and loads it itself;
`--vcvars <path>` names it explicitly. The Unreal Engine source tree is not used. These are host builds with
`cl` and `link`, not UBT.

From the repository root:

```
python Plugins\SuperEmbedderUnreal\ci\host\coexist\grown\run_grown.py --toolchain msvc --out <dir>
```

This runs both cases at `/Od` and `/O2` under `/fsanitize=address` (`/MD /Z7 /EHsc /std:c++20 /fp:precise`),
in both link orders. The grown tree's header is derived with MSVC's own preprocessor (`cl /P`). The sibling
`Plugins\SuperSLMUnreal` engine is the plain copy (put SuperSLM-Unreal's plugin folder there, or pass `--plain-tree`). The run ends with a summary table and
`grown-struct runs: PASS`, or the list of failed legs. Each leg's log is under
`<dir>\<case>\msvc_Od-asan\` (and `msvc_O2-asan\`).

To compare against another copy of SuperSLM-Unreal's engine, pass its vendored tree:

```
python Plugins\SuperEmbedderUnreal\ci\host\coexist\grown\run_grown.py --toolchain msvc --out <dir> --plain-tree <SuperSLM-Unreal checkout>\Plugins\SuperSLMUnreal\Source\ThirdParty\SuperSLM
```

Options: `--cases r8` or `--cases r9` runs one case, and `--opts=/Od` runs one level (use the `=` form). Each
`--extra-arg=/D<name>` adds a definition to every compile and to the header derivation. `--no-asan` builds
without the sanitizer.

Things to watch the first time on MSVC:
- The ASan runtime DLL (`clang_rt.asan_dynamic-x86_64.dll`) comes from the MSVC `bin\Hostx64\x64` directory
  that vcvars puts on `PATH`. The runner runs the images with that environment.
- `coff_coexist.py` does both R-1 and the body check, so a body difference in shared standard-library code
  fails the R-1 leg (`r1.txt` lists it). The rule for standard-library bodies that differ only by inlining is
  the one `coff_coexist.py` documents.
- MSVC's COMDAT selection and inlining differ from GCC's. The required control failures are: R-8 at
  `/O2`, R-9 at `/Od`. If a control does not fail on MSVC, the run reports it as a failed leg. That result
  is the finding: do not relax the requirement to make the run pass.
