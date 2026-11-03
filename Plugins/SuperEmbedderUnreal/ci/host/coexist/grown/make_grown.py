#!/usr/bin/env python3
"""Writes a grown-struct re-vendor fixture: a copy of SuperEmbedderCore's vendored engine in which one engine
struct gains one field, with its own rename header derived by the current rule.

  make_grown.py --struct sslm_schema_s  --out DIR [--preprocessor gcc|msvc] [--cxx CXX] [--extra-arg ARG ...]
                [--host-tu RSP --host-cwd DIR]
  make_grown.py --struct sslm_kv_pool_s --out DIR ...

This is the instrument of the two re-vendor strikes against the rename rule: a later re-vendor changes an
engine struct's layout, so the SuperSLM plugin's plain engine and this plugin's renamed engine disagree on it.
Every link name that carries the struct (its constructors and destructor, std::vector<struct> and other
standard-library instantiations on it) must differ between the two copies, or a monolithic link keeps one body
for both and the copy with the other layout runs it.

What it writes, all under --out (nothing in the plugin tree changes; the vendored sources are read only):
  Source/ThirdParty/SuperSLM/            the vendored engine, with the one-field patch applied to the copy
  Source/ThirdParty/SuperEmbedder/       the vendored SuperEmbedder (the engine-identity verb is engine text)
  Source/SuperEmbedderCore/Private/Vendored/   the wrappers, unchanged (their relative includes reach the copy)
  Source/SuperEmbedderCore/Private/Generated/  the generated configuration headers, and the grown tree's
                                         rename header SuperEmbedderCoreRename.<flavor>.h (derived here)
  rename/<flavor>.names, rename/<flavor>.conflicts   the grown tree's list and conflict set
  control/ControlRename.<flavor>.h       the same header with the grown struct's name left OUT: every other
                                         engine name is renamed, the struct keeps its plain link name. This is
                                         the shape of the rules the strikes broke (the struct escaped the list),
                                         and the run must fail with it, or the run proves nothing.
  GROWN.txt                              what was patched: struct, file, inserted line, file SHA-256 before/after

The patch is deterministic: one line, `unsigned char grown_field[64] = {};`, inserted before the closing `};` of
the struct's one definition. The script fails (exit 2) if the definition is not found exactly once, so an
upstream change that moves or renames the struct cannot silently produce an ungrown fixture.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PLUGIN = HERE.parents[3]
FIELD = "unsigned char grown_field[64] = {};"
STRUCTS = ("sslm_schema_s", "sslm_kv_pool_s")


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def copy_tree(src: Path, dst: Path, skip=()) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns(*skip) if skip else None)


def grow(tree: Path, struct: str) -> tuple[Path, str, str]:
    """Insert FIELD into the one definition of `struct` under tree/src and tree/include."""
    pat = re.compile(r"^struct[ \t]+" + re.escape(struct) + r"[ \t]*\{[ \t]*\r?$", re.M)
    hits = []
    for f in sorted(list((tree / "src").rglob("*")) + list((tree / "include").rglob("*"))):
        if f.is_file() and f.suffix in (".h", ".hpp", ".cpp", ".inc"):
            text = f.read_bytes().decode("utf-8")
            for m in pat.finditer(text):
                hits.append((f, text, m))
    if len(hits) != 1:
        where = ", ".join(f"{h[0].relative_to(tree)}:{h[1].count(chr(10), 0, h[2].start()) + 1}" for h in hits)
        raise SystemExit(f"SEMB_GROWN_STRUCT_NOT_FOUND: `struct {struct} {{` must be defined exactly once in the "
                         f"vendored tree; found {len(hits)}{' (' + where + ')' if where else ''}")
    f, text, m = hits[0]
    close = re.compile(r"^\};[ \t]*\r?$", re.M).search(text, m.end())
    if not close:
        raise SystemExit(f"SEMB_GROWN_STRUCT_NOT_FOUND: no closing `}};` after `struct {struct} {{` in {f}")
    nl = "\r\n" if text[m.start():m.end() + 2].find("\r") >= 0 else "\n"
    line = f"\t{FIELD}{nl}"
    before = sha(f)
    f.write_bytes((text[:close.start()] + line + text[close.start():]).encode("utf-8"))
    return f, before, sha(f)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--struct", required=True, choices=STRUCTS)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--plugin-root", type=Path, default=PLUGIN)
    ap.add_argument("--preprocessor", choices=("gcc", "msvc"), default="gcc")
    ap.add_argument("--cxx", default=None)
    ap.add_argument("--extra-arg", action="append", default=[])
    # MSVC: the module's UBT host TU, as for the plugin's own header (the plugin README). Without it the grown
    # list also renames the UE-header names the plugin's conflict set keeps out, so it is not the rule under test.
    ap.add_argument("--host-tu", action="append", default=[], type=Path)
    ap.add_argument("--host-cwd", type=Path, default=None)
    a = ap.parse_args()
    src = a.plugin_root.resolve()
    out = a.out.resolve()
    if out == src or out == src / "Source" or (src / "Source") in out.parents:
        raise SystemExit("--out must not be the plugin tree itself")
    out.mkdir(parents=True, exist_ok=True)
    s_tp, o_tp = src / "Source" / "ThirdParty", out / "Source" / "ThirdParty"
    s_core, o_core = src / "Source" / "SuperEmbedderCore" / "Private", out / "Source" / "SuperEmbedderCore" / "Private"
    copy_tree(s_tp / "SuperSLM", o_tp / "SuperSLM")
    copy_tree(s_tp / "SuperEmbedder", o_tp / "SuperEmbedder")
    copy_tree(s_core / "Vendored", o_core / "Vendored")
    copy_tree(s_core / "Generated", o_core / "Generated", skip=("SuperEmbedderCoreRename.*",))
    f, before, after = grow(o_tp / "SuperSLM", a.struct)

    gen = [sys.executable, str(src / "ci" / "generate_rename_header.py"), "--plugin-root", str(out),
           "--lists-dir", str(out / "rename"), "--preprocessor", a.preprocessor]
    if a.cxx:
        gen += ["--cxx", a.cxx]
    for e in a.extra_arg:
        gen += [f"--extra-arg={e}"]
    for h in a.host_tu:
        gen += ["--host-tu", str(h)]
    if a.host_cwd:
        gen += ["--host-cwd", str(a.host_cwd)]
    proc = subprocess.run(gen, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit("SEMB_GROWN_RENAME_FAILED: generate_rename_header.py over the grown tree failed")
    print(proc.stdout.splitlines()[0] if proc.stdout else "")

    header = o_core / "Generated" / f"SuperEmbedderCoreRename.{a.preprocessor}.h"
    names = (out / "rename" / f"{a.preprocessor}.names").read_text(encoding="utf-8").split()
    if a.struct not in names or "grown_field" not in names:
        raise SystemExit(f"SEMB_GROWN_RENAME_INCOMPLETE: the derived list lacks {a.struct} or grown_field")
    text = header.read_text(encoding="utf-8")
    drop = {f"#define {a.struct} semb_{a.struct}", f"#undef {a.struct}"}
    ctl = [l for l in text.splitlines() if l not in drop]
    if len(ctl) != len(text.splitlines()) - 2:
        raise SystemExit("SEMB_GROWN_CONTROL_FAILED: the header's define/undef lines for the struct were not found")
    (out / "control").mkdir(exist_ok=True)
    (out / "control" / f"ControlRename.{a.preprocessor}.h").write_text(
        f"// CONTROL: the grown tree's rename header with {a.struct} left off the list. Must fail.\n" +
        "\n".join(ctl) + "\n", encoding="utf-8", newline="\n")
    (out / "GROWN.txt").write_text(
        f"struct {a.struct}\nfile {f.relative_to(out).as_posix()}\ninserted {FIELD}\n"
        f"sha256_before {before}\nsha256_after {after}\npreprocessor {a.preprocessor}\n"
        f"names {len(names)}\n", encoding="utf-8", newline="\n")
    print(f"grown {a.struct} in {f.relative_to(out).as_posix()}; {len(names)} names; control header without "
          f"{a.struct} written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
