#!/usr/bin/env python3
"""Derives SuperEmbedderCore's engine rename list from preprocessed source and writes the force-included
rename header (the proposal's section 3.1, revision 7 rule).

The rule. Preprocess every vendored translation unit the module compiles (the wrappers under
Source/SuperEmbedderCore/Private/Vendored/) plainly. Every identifier in a line that comes from the engine's
own source text is a candidate. The engine's text is the vendored SuperSLM tree, plus SuperEmbedder's
engine-identity verb (src/superembedder/engine_identity/), which SuperEmbedder's own build compiles INTO the
engine. A candidate is left off the list when it is:
  - a C++ keyword, an alternative token, or a name beginning with "__";
  - used by any line from a header outside the two vendored trees (the standard library, the C runtime,
    the platform headers): renaming it would rename their text too;
  - used by the public header include/superembedder/semb.h, which the plugin's other modules compile
    WITHOUT the rename;
  - a word inside an attribute (__attribute__((...)), __declspec(...), [[...]]): attribute tokens are
    macro-expanded, and a renamed attribute word is a different (ignored or rejected) attribute;
  - named by a preprocessor directive in either vendored tree (#define, #undef, #ifdef, #ifndef,
    defined(...)): a rename macro would turn an #ifdef true, or collide with a #define.
Every name left off for one of the last four reasons is reported as the CONFLICT SET. A conflict-set member
keeps its plain link name in both engine copies, so it is allowed only while the symbol check and the body
check (ci/host/coexist) pass for it.

sslm_build_identity and SslmBuildIdentity are always on the list (the engine identity verb, review S-2).

The preprocessor is a parameter. The Linux host checks use GCC (--preprocessor gcc); the shipped header is
derived on Windows with MSVC (--preprocessor msvc, `cl /P`), because the excluded set depends on which
words that toolchain's standard library and the Windows SDK use.

Usage:
  generate_rename_header.py --preprocessor gcc  [--cxx g++] [--extra-arg ARG ...] [--check]
  generate_rename_header.py --preprocessor msvc [--cxx cl.exe] [--extra-arg ARG ...] [--check]
Writes Source/SuperEmbedderCore/Private/Generated/SuperEmbedderCoreRename.<flavor>.h and, beside this
script, rename/<flavor>.names (the list) and rename/<flavor>.conflicts (the conflict set, with reasons).
--plugin-root and --lists-dir point a derivation at another tree laid out like the plugin's (the grown-struct
fixtures, ci/host/coexist/grown/make_grown.py).
--check writes nothing and exits 1 if the committed header differs from the derivation.
"""
from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

KEYWORDS = set("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class
compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype
default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline
int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public
register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch
template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile
wchar_t while xor xor_eq final override import module
""".split())
ALWAYS = ("sslm_build_identity", "SslmBuildIdentity")
PREFIX = "semb_"

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
LITERAL = re.compile(r'(?:u8|u|U|L)?R"([^()\\ ]{0,16})\((?:.|\n)*?\)\1"'      # raw strings
                     r'|(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*"'                   # strings
                     r"|(?:u8|u|U|L)?'(?:\\.|[^'\\\n])*'")                  # character literals
# GCC: `# 12 "path" 1 3`; MSVC /P: `#line 12 "path"`.
MARKER = re.compile(r'^\s*#\s*(?:line\s+)?\d+\s+"((?:\\.|[^"\\])*)"')
DIRECTIVE = re.compile(r"^\s*#\s*(define|undef|ifdef|ifndef|if|elif|elifdef|elifndef)\b(.*)$")


def norm(p: str) -> str:
    p = p.replace("\\\\", "\\").replace("\\", "/")
    p = os.path.normpath(p).replace("\\", "/")
    return p.lower() if os.name == "nt" else p


class Trees:
    def __init__(self, root: Path):
        tp = root / "Source" / "ThirdParty"
        core = root / "Source" / "SuperEmbedderCore" / "Private"
        self.engine = [norm(str(tp / "SuperSLM")) + "/",
                       norm(str(tp / "SuperEmbedder" / "src" / "superembedder" / "engine_identity")) + "/"]
        self.embedder = [norm(str(tp / "SuperEmbedder")) + "/", norm(str(core / "Vendored")) + "/",
                         norm(str(core / "Generated")) + "/"]

    def kind(self, path: str) -> str:
        p = norm(path)
        if any(p.startswith(e) for e in self.engine):
            return "engine"
        if any(p.startswith(e) for e in self.embedder):
            return "embedder"
        if p.startswith("<"):  # <built-in>, <command-line>
            return "builtin"
        return "system"


def strip_literals(text: str) -> str:
    return LITERAL.sub(" ", text)


def attribute_words(text: str) -> set[str]:
    """Identifiers inside __attribute__((...)), __declspec(...) and [[...]]."""
    out: set[str] = set()
    for m in re.finditer(r"__attribute__\s*\(|__declspec\s*\(|\[\[", text):
        i, depth = m.end(), 1
        closer = "]" if m.group(0) == "[[" else ")"
        opener = "[" if closer == "]" else "("
        start = i
        while i < len(text) and depth:
            if text[i] == opener:
                depth += 1
            elif text[i] == closer:
                depth -= 1
            i += 1
        out.update(IDENT.findall(text[start:i]))
    return out


def preprocess(flavor: str, cxx: str, src: Path, includes: list[Path], defines: list[str],
               extra: list[str]) -> str:
    if flavor == "gcc":
        cmd = shlex.split(cxx) + ["-std=c++20", "-E"] + [f"-D{d}" for d in defines] + \
              [f"-I{i}" for i in includes] + extra + [str(src)]
        proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        if proc.returncode != 0:
            raise SystemExit(f"SEMB_UE_PREPROCESS_FAILED: {' '.join(cmd)}\n{proc.stderr[-3000:]}")
        return proc.stdout
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / (src.stem + ".i")
        cmd = shlex.split(cxx, posix=os.name != "nt") + ["/nologo", "/std:c++20", "/EHsc", "/P", f"/Fi{out}"] + \
              [f"/D{d}" for d in defines] + [f"/I{i}" for i in includes] + extra + [str(src)]
        proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        if proc.returncode != 0 or not out.is_file():
            raise SystemExit(f"SEMB_UE_PREPROCESS_FAILED: {' '.join(cmd)}\n{(proc.stdout + proc.stderr)[-3000:]}")
        return out.read_text(encoding="utf-8", errors="replace")


def split_by_origin(text: str, trees: Trees) -> dict[str, list[str]]:
    by: dict[str, list[str]] = {"engine": [], "embedder": [], "system": [], "builtin": []}
    cur = "builtin"
    for line in text.splitlines():
        m = MARKER.match(line)
        if m:
            cur = trees.kind(m.group(1))
            continue
        by[cur].append(line)
    return by


def module_inputs(root: Path) -> tuple[list[Path], list[Path], list[str]]:
    core = root / "Source" / "SuperEmbedderCore" / "Private"
    tp = root / "Source" / "ThirdParty"
    wrappers = sorted((core / "Vendored").glob("*.cpp"))
    # The include paths and definitions SuperEmbedderCore.Build.cs gives the vendored TUs.
    includes = [tp / "SuperSLM" / "include", tp / "SuperEmbedder" / "include", core / "Generated"]
    defines = ['SEMB_BUILD_CONFIGURATION="ubt"', 'SEMB_BUILD_SYSTEM="ubt"']
    return wrappers, includes, defines


def directive_names(root: Path) -> set[str]:
    names: set[str] = set()
    tp = root / "Source" / "ThirdParty"
    for tree in (tp / "SuperSLM", tp / "SuperEmbedder"):
        for f in list(tree.rglob("*.h")) + list(tree.rglob("*.hpp")) + list(tree.rglob("*.cpp")) + \
                 list(tree.rglob("*.inc")):
            for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
                m = DIRECTIVE.match(line)
                if not m:
                    continue
                kind, rest = m.group(1), strip_literals(m.group(2))
                if kind in ("define", "undef", "ifdef", "ifndef", "elifdef", "elifndef"):
                    w = IDENT.match(rest.strip())
                    if w:
                        names.add(w.group(0))
                else:
                    names.update(re.findall(r"\bdefined\s*\(?\s*([A-Za-z_][A-Za-z0-9_]*)", rest))
    return names


def public_header_words(root: Path) -> set[str]:
    text = (root / "Source" / "ThirdParty" / "SuperEmbedder" / "include" / "superembedder" / "semb.h"
            ).read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return set(IDENT.findall(strip_literals(text)))


def source_words(text: str) -> set[str]:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return set(IDENT.findall(strip_literals(text)))


def host_words(cxx: str, rsp: Path, cwd: Path, trees: Trees) -> tuple[set[str], int]:
    """Every identifier of the UE headers a host TU of this module reads, from UBT's own response file.

    The rename header is force-included into EVERY translation unit of SuperEmbedderCore, including the ones
    UBT generates (PerModuleInline.gen.cpp in a modular build), which read UE's own headers with the rename
    active. A renamed name in that text would rename UE's code (`check` becomes `semb_check`). So the TU is
    preprocessed as UBT compiles it, without the rename force-include, and every identifier in the text of
    every header it includes outside the vendored trees (macros, unexpanded bodies and all) is kept off the list.
    `cwd` is UBT's working directory for the compile (Engine/Source): the response files' relative include
    paths resolve against it.
    """
    rsp = rsp.resolve()
    args = [l.strip() for l in rsp.read_text(encoding="utf-8").splitlines() if l.strip()]
    drop = ("/Fo", "/experimental:log", "/sourceDependencies", "/c")
    keep, src = [], None
    for a in args:
        bare = a.strip('"')
        if a.startswith("/FI") and "SuperEmbedderCoreRename." in a:
            continue
        if any(a.startswith(d) for d in drop):
            continue
        if not a.startswith(("/", "@", "-")) and src is None:
            src = bare
            continue
        keep.append(a)
    if src is None:
        raise SystemExit(f"SEMB_UE_HOST_TU_UNREADABLE: no source file in {rsp}")
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "host.i"
        line = " ".join([f'"{cxx}"' if " " in cxx else cxx, *keep, "/P", f'/Fi"{out}"', "/showIncludes", f'"{src}"'])
        proc = subprocess.run(line, capture_output=True, text=True, errors="replace", shell=True, cwd=cwd)
        if proc.returncode != 0 or not out.is_file():
            raise SystemExit(f"SEMB_UE_PREPROCESS_FAILED: {line}\n{(proc.stdout + proc.stderr)[-3000:]}")
        words = source_words(out.read_text(encoding="utf-8", errors="replace"))
    headers = set()
    for l in (proc.stdout + proc.stderr).splitlines():
        m = re.match(r"^Note: including file:\s*(.+)$", l)
        if m:
            headers.add(m.group(1).strip())
    for a in keep:
        if a.startswith("/FI"):
            headers.add(a[3:].strip('"'))
    for h in sorted(headers):
        p = Path(h) if Path(h).is_absolute() else cwd / h
        if trees.kind(str(p.resolve())) == "system" and p.is_file():
            words |= source_words(p.read_text(encoding="utf-8", errors="replace"))
    return words, len(headers)


def derive(root: Path, flavor: str, cxx: str, extra: list[str], host_tus: list[Path] = (),
           host_cwd: Path | None = None) -> tuple[list[str], dict[str, str], int]:
    trees = Trees(root)
    wrappers, includes, defines = module_inputs(root)
    engine: set[str] = set()
    system: set[str] = set()
    attrs: set[str] = set()
    for w in wrappers:
        by = split_by_origin(preprocess(flavor, cxx, w, includes, defines, extra), trees)
        eng_text = strip_literals("\n".join(l for l in by["engine"] if not l.lstrip().startswith("#")))
        engine.update(IDENT.findall(eng_text))
        sys_text = strip_literals("\n".join(by["system"] + by["builtin"]))
        system.update(IDENT.findall(sys_text))
        attrs |= attribute_words(strip_literals("\n".join(by["engine"] + by["embedder"] + by["system"])))
    public = public_header_words(root)
    directives = directive_names(root)
    host: set[str] = set()
    for rsp in host_tus:
        words, n = host_words(cxx, rsp, host_cwd, trees)
        print(f"host TU {rsp.name}: {n} headers read, {len(words)} identifiers kept off the list")
        host |= words
    names: list[str] = []
    conflicts: dict[str, str] = {}
    for n in sorted(engine):
        if n in KEYWORDS or n.startswith("__"):
            continue
        reasons = []
        if n in system:
            continue  # a standard-library / platform word: never the engine's to rename
        if n in public:
            reasons.append("used by the public header semb.h")
        if n in attrs:
            reasons.append("an attribute word")
        if n in directives:
            reasons.append("named by a preprocessor directive")
        if n in host:
            reasons.append("used by a UE header a host TU of the module reads (--host-tu)")
        if reasons:
            conflicts[n] = "; ".join(reasons)
            continue
        names.append(n)
    for n in ALWAYS:
        if n not in names:
            names.append(n)
    return sorted(set(names)), conflicts, len(wrappers)


LINKER_INCLUDE = re.compile(r'#\s*pragma\s+comment\s*\(\s*linker\s*,\s*"/INCLUDE:([A-Za-z_][A-Za-z0-9_]*)"')


def linker_string_names(root: Path, names: list[str]) -> list[str]:
    """Renamed engine names a vendored source names inside a linker-directive STRING (MSVC
    `#pragma comment(linker, "/INCLUDE:name")`). A string is not macro-expanded, so that directive keeps
    asking for the plain name while the definition carries the renamed one. x64 names only (the module is
    Win64-only on MSVC; the x86 spelling with a leading underscore is never compiled)."""
    listed = set(names)
    found: set[str] = set()
    tree = root / "Source" / "ThirdParty" / "SuperSLM"
    for f in sorted(list(tree.rglob("*.cpp")) + list(tree.rglob("*.h"))):
        for m in LINKER_INCLUDE.finditer(f.read_text(encoding="utf-8", errors="replace")):
            if m.group(1) in listed:
                found.add(m.group(1))
    return sorted(found)


def render(names: list[str], flavor: str, cxx: str, tu_count: int, linker: list[str] = ()) -> str:
    lines = [
        f"// SuperEmbedderCore's engine rename header, {flavor} derivation. Generated by",
        "// ci/generate_rename_header.py from the preprocessed vendored translation units; do not edit.",
        f"// {len(names)} names from {tu_count} translation units, preprocessor `{Path(shlex.split(cxx)[0]).name}`.",
        "//",
        "// Force-included into every SuperEmbedderCore translation unit (SuperEmbedderCore.Build.cs), so every",
        "// engine identifier this module compiles carries the semb_ prefix and the engine copy's link names",
        "// differ from the SuperSLM plugin's. Included a second time with SUPEREMBEDDERCORE_RENAME_UNDO",
        "// defined, it removes the macros again (the module's own UE source does that before any UE header).",
        "#ifndef SUPEREMBEDDERCORE_RENAME_ACTIVE",
        "#define SUPEREMBEDDERCORE_RENAME_ACTIVE 1",
    ]
    lines += [f"#define {n} {PREFIX}{n}" for n in names]
    if linker:
        # The vendored sources name these in a linker-directive string (`/INCLUDE:<name>`), which no macro
        # reaches. /ALTERNATENAME answers the plain request with this copy's renamed definition when nothing
        # else defines the plain name (the SuperSLM plugin's copy, when it has the symbol, answers its own), and
        # /INCLUDE keeps the renamed definition under /OPT:REF either way.
        lines += ["#if defined(_MSC_VER) && defined(_M_X64)"]
        for n in linker:
            lines += [f'#pragma comment(linker, "/ALTERNATENAME:{n}={PREFIX}{n}")',
                      f'#pragma comment(linker, "/INCLUDE:{PREFIX}{n}")']
        lines += ["#endif"]
    lines += ["#elif defined(SUPEREMBEDDERCORE_RENAME_UNDO)"]
    lines += [f"#undef {n}" for n in names]
    lines += ["#undef SUPEREMBEDDERCORE_RENAME_ACTIVE", "#endif", ""]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-root", type=Path, default=HERE.parent)
    ap.add_argument("--preprocessor", choices=("gcc", "msvc"), required=True)
    ap.add_argument("--cxx", default=None, help="the compiler driver (default g++ or cl.exe)")
    ap.add_argument("--extra-arg", action="append", default=[], help="passed to every preprocess")
    ap.add_argument("--host-tu", action="append", default=[], type=Path,
                    help="MSVC: UBT's response file for a module TU that is not vendored (PerModuleInline.gen.cpp)")
    ap.add_argument("--host-cwd", type=Path, default=None,
                    help="UBT's compile working directory, which its relative include paths name (<engine>/Engine/Source)")
    ap.add_argument("--lists-dir", type=Path, default=HERE / "rename",
                    help="where <flavor>.names and <flavor>.conflicts go (default ci/rename); set it with an "
                         "alternate --plugin-root so a derivation over another tree never touches the plugin's lists")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    root = a.plugin_root.resolve()
    cxx = a.cxx or ("g++" if a.preprocessor == "gcc" else "cl.exe")
    if a.host_tu and (a.preprocessor != "msvc" or a.host_cwd is None):
        raise SystemExit("--host-tu reads a UBT MSVC response file: use it with --preprocessor msvc and --host-cwd")
    names, conflicts, tus = derive(root, a.preprocessor, cxx, a.extra_arg, a.host_tu, a.host_cwd)
    header = root / "Source" / "SuperEmbedderCore" / "Private" / "Generated" / \
        f"SuperEmbedderCoreRename.{a.preprocessor}.h"
    linker = linker_string_names(root, names) if a.preprocessor == "msvc" else []
    text = render(names, a.preprocessor, cxx, tus, linker)
    if linker:
        print(f"linker-string names answered by /ALTERNATENAME: {', '.join(linker)}")
    lists = a.lists_dir
    conflict_text = "".join(f"{n}\t{r}\n" for n, r in sorted(conflicts.items()))
    if a.check:
        bad = []
        if not header.is_file() or header.read_bytes() != text.encode("utf-8"):
            bad.append(str(header))
        if (lists / f"{a.preprocessor}.names").read_text(encoding="utf-8") != "\n".join(names) + "\n":
            bad.append(f"rename/{a.preprocessor}.names")
        if bad:
            print("SEMB_UE_RENAME_HEADER_STALE: " + ", ".join(bad) + " differ from the derivation")
            return 1
        print(f"rename header current: {len(names)} names, conflict set {len(conflicts)}")
        return 0
    header.write_bytes(text.encode("utf-8"))
    lists.mkdir(exist_ok=True)
    (lists / f"{a.preprocessor}.names").write_text("\n".join(names) + "\n", encoding="utf-8", newline="\n")
    (lists / f"{a.preprocessor}.conflicts").write_text(conflict_text, encoding="utf-8", newline="\n")
    print(f"wrote {header.name}: {len(names)} names from {tus} TUs; conflict set {len(conflicts)}")
    for n, r in sorted(conflicts.items()):
        print(f"  conflict: {n}: {r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
