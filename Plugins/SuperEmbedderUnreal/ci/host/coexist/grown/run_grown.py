#!/usr/bin/env python3
"""The grown-struct coexistence runs (the proposal's cells R-8 and R-9), with GCC or MSVC, in one command.

  run_grown.py [--toolchain gcc|msvc] [--cases r8,r9] [--opts "..."] [--out DIR] [--plain-tree DIR]
               [--model FILE] [--no-asan] [--jobs N] [--vcvars FILE] [--extra-arg ARG ...]

  r8  a re-vendor in which sslm_schema_s gains one field
  r9  a re-vendor in which sslm_kv_pool_s gains one field

For each case it writes the grown tree and its rename header (make_grown.py, with the toolchain's own
preprocessor), then at each optimization level (default -O0 -O2 for GCC, /Od /O2 for MSVC), under
AddressSanitizer unless --no-asan:

  P  the SuperSLM plugin's engine, plain (its CMakeLists' SUPERSLM_CORE_SOURCES, from --plain-tree)
  G  the grown tree's engine wrappers and identity verb under the grown tree's rename header
  C  the same under the CONTROL header: the grown header with the grown struct's name left off
  U  the same with no rename header at all

and checks:
  R-1  G vs P: no strong symbol, and no weak/COMDAT symbol naming a listed engine identifier, on both sides
       (GCC: check_symbols.py; MSVC: ci/msvc/coff_coexist.py, which also does the body check)
  R-1b G vs P (GCC: bodycmp.py; MSVC: inside coff_coexist.py)
  link G and P into one image in both object orders (plugin first, embedder first): no duplicate definition
  run  both images on two_schema_model.sslm: each copy maps the model, makes a K/V pool and a sequence, looks
       up both schemas and binds each, with no failure and no sanitizer report
and the controls, which must fail, or the run proves nothing:
  ctl-R-1   C vs P must FAIL R-1, naming a shared symbol that carries the grown struct, where the strike's
            symbol exists: r8 at every level (std::vector<sslm_schema_s> members); r9 at the unoptimized level
            only (sslm_kv_pool_s's constructor and destructor, inlined away when optimizing, which is how the
            third strike's -O2-derived list missed them). Elsewhere it is reported.
  ctl-run   C linked beside P, both orders, run: must fail in at least one order where the strike failed:
            r8 at the optimized level (the shared std::vector<sslm_schema_s>::_M_default_append), r9 at the
            unoptimized level (the shared constructor). Elsewhere it is reported.
  ctl-link  U beside P must collide (duplicate definitions)

Exit 0 when every required leg is as expected. Output under --out (default: a fresh temporary directory).
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
COEXIST = HERE.parent
PLUGIN = HERE.parents[3]
CASES = {"r8": "sslm_schema_s", "r9": "sslm_kv_pool_s"}
# Where each control must be refused: "all", "opt" (the optimized levels) or "unopt" (the unoptimized level).
MUST_FAIL = {"r8": {"r1": "all", "run": "opt"}, "r9": {"r1": "unopt", "run": "unopt"}}
PY = [sys.executable, "-B"]


def sh(cmd, cwd=None, env=None, log: Path | None = None) -> tuple[int, str]:
    p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
    if log:
        log.write_text(p.stdout + p.stderr, encoding="utf-8")
    return p.returncode, p.stdout + p.stderr


class Gcc:
    name, obj, exe = "gcc", ".o", ""
    default_opts = ["-O0", "-O2"]
    unoptimized = "-O0"

    def __init__(self, a):
        self.cxx = a.cxx or os.environ.get("CXX", "g++")
        self.san = [] if a.no_asan else ["-fsanitize=address"]
        self.extra = a.extra_arg

    def compile(self, src: Path, out: Path, opt, inc, defs, fi=None):
        cmd = [self.cxx, "-std=c++20", opt, "-g", "-fPIC", "-ffp-contract=off", *self.san, *self.extra,
               *(["-include", str(fi)] if fi else []), *[f"-I{i}" for i in inc], *[f"-D{d}" for d in defs],
               "-c", str(src), "-o", str(out)]
        return sh(cmd)

    def link(self, objs, out: Path, log: Path):
        rc, text = sh([self.cxx, *self.san, "-o", str(out), *map(str, objs), "-lpthread"], log=log)
        return rc, text.count("multiple definition")

    def run_env(self):
        e = dict(os.environ)
        e["ASAN_OPTIONS"] = "detect_leaks=0"
        return e

    def r1(self, a: Path, b: Path, names: Path, conflicts: Path, log: Path) -> int:
        rc, _ = sh([*PY, str(COEXIST / "check_symbols.py"), str(a), str(b), str(names), str(conflicts)], log=log)
        return rc

    def r1b(self, a: Path, b: Path, log: Path) -> int:
        rc, _ = sh([*PY, str(COEXIST / "bodycmp.py"), str(a), str(b)], log=log)
        return rc


class Msvc:
    name, obj, exe = "msvc", ".obj", ".exe"
    default_opts = ["/Od", "/O2"]
    unoptimized = "/Od"

    def __init__(self, a):
        self.env = msvc_env(a.vcvars)
        self.cxx = a.cxx or "cl.exe"
        self.san = [] if a.no_asan else ["/fsanitize=address"]
        self.extra = a.extra_arg

    def compile(self, src: Path, out: Path, opt, inc, defs, fi=None):
        cmd = [self.cxx, "/nologo", "/c", "/std:c++20", "/EHsc", "/fp:precise", "/utf-8", "/bigobj", "/MD", "/Z7",
               opt, *self.san, *self.extra, *([f"/FI{fi}"] if fi else []), *[f"/I{i}" for i in inc],
               *[f"/D{d}" for d in defs], str(src), f"/Fo{out}"]
        p = subprocess.run(cmd, env=self.env, capture_output=True, text=True, errors="replace")
        return p.returncode, p.stdout + p.stderr

    def link(self, objs, out: Path, log: Path):
        rsp = out.with_suffix(".rsp")
        rsp.write_text("\n".join(f'"{o}"' for o in objs) + "\n", encoding="utf-8")
        cmd = [self.cxx, "/nologo", *self.san, "/MD", f"@{rsp}", f"/Fe{out}", "/link", "/DEBUG", "/INCREMENTAL:NO"]
        p = subprocess.run(cmd, env=self.env, capture_output=True, text=True, errors="replace")
        log.write_text(p.stdout + p.stderr, encoding="utf-8")
        return p.returncode, len(re.findall(r"LNK2005", p.stdout + p.stderr))

    def run_env(self):
        return dict(self.env)

    def coff(self, a: Path, b: Path, names: Path, log: Path, expect_collision=False) -> int:
        cmd = [*PY, str(PLUGIN / "ci" / "msvc" / "coff_coexist.py"), "--a", str(a), "--b", str(b), "--names", str(names)]
        und = shutil.which("undname", path=self.env.get("PATH"))
        if und:
            cmd += ["--undname", und]
        if expect_collision:
            cmd += ["--expect-collision"]
        rc, _ = sh(cmd, log=log)
        return rc

    def r1(self, a, b, names, conflicts, log):
        return self.coff(a, b, names, log)

    def r1b(self, a, b, log):
        log.write_text("R-1b runs inside coff_coexist.py (the R-1 leg)\n", encoding="utf-8")
        return 0


def msvc_env(vcvars: Path | None) -> dict:
    env = dict(os.environ)
    if shutil.which("cl.exe") and not vcvars:
        return env
    if not vcvars:
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
            "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if not vswhere.is_file():
            raise SystemExit("SEMB_GROWN_NO_MSVC: cl.exe is not on PATH and vswhere.exe was not found; "
                             "run from an x64 Native Tools prompt or pass --vcvars <vcvars64.bat>")
        inst = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                               "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                               "installationPath"], capture_output=True, text=True).stdout.strip().splitlines()
        if not inst:
            raise SystemExit("SEMB_GROWN_NO_MSVC: vswhere found no installation with the x64 C++ tools")
        vcvars = Path(inst[0]) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    out = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    if not shutil.which("cl.exe", path=env.get("PATH")):
        raise SystemExit(f"SEMB_GROWN_NO_MSVC: cl.exe not found after {vcvars}")
    print(f"MSVC environment from {vcvars}")
    return env


def required(case: str, what: str, opt: str, unoptimized: str) -> bool:
    w = MUST_FAIL[case][what]
    return w == "all" or (w == "unopt") == (opt == unoptimized)


def core_sources(tree: Path) -> list[Path]:
    text = (tree / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(SUPERSLM_CORE_SOURCES(.*?)\)", text, re.S)
    if not m:
        raise SystemExit(f"SEMB_GROWN_NO_SOURCE_LIST: no SUPERSLM_CORE_SOURCES in {tree / 'CMakeLists.txt'}")
    return [tree / s for s in re.findall(r"src/\S+\.cpp", m.group(1))]


def compile_set(tc, jobs, outdir: Path, srcs, opt, inc, defs, fi=None) -> list[str]:
    outdir.mkdir(parents=True, exist_ok=True)
    errs = []

    def one(s: Path):
        o = outdir / (s.stem + tc.obj)
        rc, text = tc.compile(s, o, opt, inc, defs, fi)
        if rc != 0:
            (outdir / (s.stem + ".err")).write_text(text, encoding="utf-8")
            return f"{s.name}: {text.strip().splitlines()[-1] if text.strip() else 'failed'}"
        return None

    with cf.ThreadPoolExecutor(jobs) as ex:
        for r in ex.map(one, srcs):
            if r:
                errs.append(r)
    return errs


def run_bin(tc, exe: Path, model: Path, log: Path) -> tuple[int, str]:
    p = subprocess.run([str(exe), str(model)], env=tc.run_env(), capture_output=True, text=True, errors="replace")
    text = p.stdout + p.stderr
    log.write_text(text, encoding="utf-8")
    asan = re.search(r"ERROR: AddressSanitizer: ([\w-]+)", text)
    summary = f"exit {p.returncode}" + (f", ASan {asan.group(1)}" if asan else "")
    tail = [l for l in text.splitlines() if l.startswith("grown:") or "free():" in l or "double free" in l]
    if tail:
        summary += f" ({tail[-1].strip()})"
    return p.returncode, summary


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--toolchain", choices=("gcc", "msvc"), default="msvc" if os.name == "nt" else "gcc")
    ap.add_argument("--cases", default="r8,r9")
    ap.add_argument("--opts", default=None)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--plain-tree", type=Path,
                    default=PLUGIN.parent / "SuperSLMUnreal" / "Source" / "ThirdParty" / "SuperSLM")
    ap.add_argument("--model", type=Path, default=HERE / "two_schema_model.sslm")
    ap.add_argument("--no-asan", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--cxx", default=None)
    ap.add_argument("--vcvars", type=Path, default=None)
    ap.add_argument("--extra-arg", action="append", default=[], help="added to every compile")
    ap.add_argument("--host-tu", action="append", default=[], type=Path,
                    help="MSVC: SuperEmbedderCore's UBT host-TU response file, passed to the grown tree's header "
                         "derivation as for the plugin's own (with --host-cwd)")
    ap.add_argument("--host-cwd", type=Path, default=None, help="UBT's compile directory, <engine>/Engine/Source")
    a = ap.parse_args()
    if a.toolchain == "msvc" and not a.host_tu:
        print("note: no --host-tu: the grown MSVC list will also rename the UE-header names the plugin's own "
              "conflict set keeps out, and R-1 G vs P reports standard-library symbols those names reach")

    tc = Gcc(a) if a.toolchain == "gcc" else Msvc(a)
    opts = a.opts.split() if a.opts else tc.default_opts
    out = (a.out or Path(tempfile.mkdtemp(prefix="semb-grown-"))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    plain = a.plain_tree.resolve()
    want = (HERE / "two_schema_model.sslm.sha256").read_text().split()[0]
    got = hashlib.sha256(a.model.read_bytes()).hexdigest()
    if a.model.resolve() == (HERE / "two_schema_model.sslm").resolve() and got != want:
        raise SystemExit(f"SEMB_GROWN_MODEL_CHANGED: {a.model} sha256 {got}, recorded {want}")
    ver = (plain / "VENDORED_VERSION.txt")
    ver = re.search(r"^Tag:\s*(\S+)", ver.read_text(encoding="utf-8", errors="replace"), re.M).group(1) \
        if ver.is_file() else "unknown"
    print(f"toolchain {tc.name}; levels {' '.join(opts)}; ASan {'off' if a.no_asan else 'on'}; plain engine "
          f"{plain} ({ver}); model sha256 {got[:16]}...; output {out}")
    psrcs = core_sources(plain)
    results = []  # (case, opt, leg, ok, detail)

    def leg(case, opt, name, ok, detail):
        results.append((case, opt, name, ok, detail))
        print(f"  [{case} {opt}] {name}: {'ok' if ok else 'FAILED'} - {detail}")

    for case in [c.strip() for c in a.cases.split(",") if c.strip()]:
        struct = CASES[case]
        tree = out / case / "tree"
        print(f"\n== {case}: grow {struct}, derive the rename header ({tc.name} preprocessor)")
        mk = [*PY, str(HERE / "make_grown.py"), "--struct", struct, "--out", str(tree),
              "--preprocessor", tc.name] + (["--cxx", a.cxx] if a.cxx else [])
        for h in a.host_tu:
            mk += ["--host-tu", str(h)]
        if a.host_cwd:
            mk += ["--host-cwd", str(a.host_cwd)]
        p = subprocess.run(mk, capture_output=True, text=True, errors="replace",
                           env=tc.env if tc.name == "msvc" else None)
        print("  " + (p.stdout + p.stderr).strip().replace("\n", "\n  "))
        if p.returncode != 0:
            leg(case, "-", "make_grown", False, "see above")
            continue
        gen = tree / "Source" / "SuperEmbedderCore" / "Private" / "Generated"
        hdr = gen / f"SuperEmbedderCoreRename.{tc.name}.h"
        ctl = tree / "control" / f"ControlRename.{tc.name}.h"
        names, conflicts = tree / "rename" / f"{tc.name}.names", tree / "rename" / f"{tc.name}.conflicts"
        vend = tree / "Source" / "SuperEmbedderCore" / "Private" / "Vendored"
        gsrcs = sorted(vend.glob("SuperEmbedderVendoredEngine_*.cpp")) + [vend / "SuperEmbedderVendored_build_identity.cpp"]
        tp = tree / "Source" / "ThirdParty"
        ginc = [tp / "SuperSLM" / "include", tp / "SuperEmbedder" / "include", gen]
        gdef = ['SEMB_BUILD_CONFIGURATION="ubt-host"', 'SEMB_BUILD_SYSTEM="ubt"']
        for opt in opts:
            O = out / case / (tc.name + opt.replace("/", "_").replace("-", "_") + ("" if a.no_asan else "-asan"))
            if O.exists():
                shutil.rmtree(O)
            O.mkdir(parents=True)
            print(f"\n== [{case} {opt}] compile P (plain plugin engine), G (grown, renamed), C (control), U (unrenamed)")
            errs = compile_set(tc, a.jobs, O / "P", psrcs, opt, [plain / "include"], [])
            errs += compile_set(tc, a.jobs, O / "G", gsrcs, opt, ginc, gdef, hdr)
            errs += compile_set(tc, a.jobs, O / "C", gsrcs, opt, ginc, gdef, ctl)
            errs += compile_set(tc, a.jobs, O / "U", gsrcs, opt, ginc, gdef)
            drv, main_src = HERE / "grown_drv.cpp", HERE / "grown_main.cpp"
            for n, inc, defs, fi in (("drv_p", [plain / "include"], ["DRV_NAME=drv_plugin"], None),
                                     ("drv_g", ginc, ["DRV_NAME=drv_embedder"], hdr),
                                     ("drv_c", ginc, ["DRV_NAME=drv_embedder"], ctl),
                                     ("drv_u", ginc, ["DRV_NAME=drv_embedder"], None)):
                rc, text = tc.compile(drv, O / (n + tc.obj), opt, inc, defs, fi)
                if rc:
                    errs.append(f"{n}: {text.strip()[-400:]}")
            rc, text = tc.compile(main_src, O / ("main" + tc.obj), opt, [], [])
            if rc:
                errs.append(f"main: {text.strip()[-400:]}")
            leg(case, opt, "compile", not errs, f"{len(errs)} failures" + (": " + "; ".join(errs[:3]) if errs else ""))
            if errs:
                continue
            objs = {k: sorted((O / k).glob("*" + tc.obj)) for k in "PGCU"}
            m, dp = O / ("main" + tc.obj), O / ("drv_p" + tc.obj)

            leg(case, opt, "R-1 G vs P", tc.r1(O / "G", O / "P", names, conflicts, O / "r1.txt") == 0, "r1.txt")
            leg(case, opt, "R-1b G vs P", tc.r1b(O / "G", O / "P", O / "r1b.txt") == 0, "r1b.txt")
            for k, label in (("G", "grown"), ("C", "control")):
                dk = O / (f"drv_{k.lower()}" + tc.obj)
                runs = []
                for order, lst in (("pr", [m, dp, *objs["P"], dk, *objs[k]]), ("rp", [m, dk, *objs[k], dp, *objs["P"]])):
                    exe = O / f"{label}_{order}{tc.exe}"
                    rc, dups = tc.link(lst, exe, O / f"link_{label}_{order}.txt")
                    if label == "grown":
                        leg(case, opt, f"link {order}", rc == 0 and dups == 0, f"exit {rc}, duplicate definitions {dups}")
                    if rc != 0:
                        runs.append((order, 1, f"link failed (exit {rc}, duplicates {dups})"))
                        continue
                    r, summary = run_bin(tc, exe, a.model, O / f"run_{label}_{order}.txt")
                    runs.append((order, r, summary))
                    if label == "grown":
                        leg(case, opt, f"run {order}", r == 0 and "AddressSanitizer" not in summary, summary)
                if label == "control":
                    failed = [o for o, r, _ in runs if r != 0]
                    must = required(case, "run", opt, tc.unoptimized)
                    detail = "; ".join(f"{o}: {s}" for o, _, s in runs)
                    if must:
                        leg(case, opt, "ctl-run (must fail)", bool(failed), detail)
                    else:
                        leg(case, opt, "ctl-run (informational)", True, detail)
            if tc.name == "msvc":
                rc = tc.coff(O / "C", O / "P", names, O / "ctl_r1.txt", expect_collision=True)
                if required(case, "r1", opt, tc.unoptimized):
                    leg(case, opt, "ctl-R-1 C vs P (must fail)", rc == 0, "ctl_r1.txt")
                else:
                    leg(case, opt, "ctl-R-1 C vs P (informational)", True, ("fails" if rc == 0 else "passes") + ", ctl_r1.txt")
            else:
                rc = tc.r1(O / "C", O / "P", names, conflicts, O / "ctl_r1.txt")
                text = (O / "ctl_r1.txt").read_text()
                hit = [l.strip()[5:] for l in text.splitlines() if l.startswith("  WEAK") and struct in l]
                n = re.search(r"weak in both naming a listed engine identifier: (\d+)", text)
                caught = rc != 0 and bool(hit)
                must = required(case, "r1", opt, tc.unoptimized)
                leg(case, opt, f"ctl-R-1 C vs P ({'must fail' if must else 'informational'})", caught or not must,
                    ("fails: " if caught else "passes: ") + f"{n.group(1) if n else '?'} shared weak symbol(s) "
                    f"naming a listed identifier" + (f", e.g. {hit[0]}" if hit else ""))
                rc = tc.r1b(O / "C", O / "P", O / "ctl_r1b.txt")
                leg(case, opt, "ctl-R-1b C vs P (informational)", True, "fails" if rc else "passes")
            rc, dups = tc.link([m, O / ("drv_u" + tc.obj), *objs["U"], dp, *objs["P"]],
                               O / f"unrenamed{tc.exe}", O / "link_unrenamed.txt")
            leg(case, opt, "ctl-link U+P (must collide)", dups > 0, f"exit {rc}, duplicate definitions {dups}")

    bad = [r for r in results if not r[3]]
    print("\nsummary")
    for case, opt, name, ok, detail in results:
        print(f"  {case:3} {opt:4} {'ok    ' if ok else 'FAILED'} {name}")
    print(f"\ngrown-struct runs: {'PASS' if not bad else f'{len(bad)} FAILED'} (output in {out})")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
