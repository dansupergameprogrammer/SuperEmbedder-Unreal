#!/usr/bin/env python3
"""R-7: the rename macros change no text outside the two vendored trees.

  check_r7.py <plugin root> <rename header> [--preprocessor gcc|msvc] [--cxx g++|cl.exe] [--extra-arg ARG ...]

Preprocesses every vendored translation unit of SuperEmbedderCore plainly and under the force-included
rename header (GCC -E and -include, or MSVC /P and /FI, through the generator's own preprocess) and fails, naming the translation unit and the file, if any line that comes from a
header outside the SuperSLM and SuperEmbedder vendored trees differs. Lines from SuperEmbedder's own sources
do change, correctly, because they name engine identifiers.
"""
import argparse
import difflib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import generate_rename_header as g  # noqa: E402


def outside_lines(text: str, trees: "g.Trees", header: str):
    out, cur, keep = [], "", False
    for line in text.splitlines():
        m = g.MARKER.match(line)
        if m:
            cur = m.group(1)
            keep = trees.kind(cur) in ("system", "builtin") and Path(cur).name != header
            continue
        if keep:
            out.append(f"{cur}: {line}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("header", type=Path)
    ap.add_argument("--preprocessor", choices=("gcc", "msvc"), default="gcc")
    ap.add_argument("--cxx", default=None, help="the compiler driver (default g++ or cl.exe)")
    ap.add_argument("--extra-arg", action="append", default=[])
    a = ap.parse_args()
    root = a.root.resolve()
    cxx = a.cxx or ("g++" if a.preprocessor == "gcc" else "cl.exe")
    force = ["-include", str(a.header.resolve())] if a.preprocessor == "gcc" else [f"/FI{a.header.resolve()}"]
    trees = g.Trees(root)
    wrappers, includes, defines = g.module_inputs(root)
    changed = 0
    for w in wrappers:
        plain = outside_lines(g.preprocess(a.preprocessor, cxx, w, includes, defines, a.extra_arg), trees,
                              a.header.name)
        renamed = outside_lines(g.preprocess(a.preprocessor, cxx, w, includes, defines, a.extra_arg + force),
                                trees, a.header.name)
        if plain != renamed:
            changed += 1
            print(f"R-7: non-engine text changed in {w.name}:")
            for d in list(difflib.unified_diff(plain, renamed, lineterm="", n=0))[2:8]:
                print("   " + d)
    print(f"R-7: translation units whose non-engine text changed: {changed} of {len(wrappers)}")
    return 1 if changed else 0


if __name__ == "__main__":
    sys.exit(main())
