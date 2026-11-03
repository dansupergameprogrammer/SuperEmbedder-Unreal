#!/usr/bin/env python3
"""One wrapper per compiled vendored source, and no other (SuperSLMUnreal's check, applied to this module).

The sources SuperEmbedderCore must compile, read from the vendored build files:
  - the engine's SUPERSLM_CORE_SOURCES (ThirdParty/SuperSLM/CMakeLists.txt): one
    Private/Vendored/SuperEmbedderVendoredEngine_<name>.cpp each. No GPU source (plugin 1.0 has no GPU path);
  - SuperEmbedder's SEMB_LIBRARY_SOURCES (ThirdParty/SuperEmbedder/CMakeLists.txt), its GPU-off adapter
    (src/superembedder/engine_adapter_gpu_off.cpp, the SEMB_BUILD_GPU=OFF branch), and the engine identity
    verb (src/superembedder/engine_identity/build_identity.cpp): one
    Private/Vendored/SuperEmbedderVendored_<name>.cpp each.
Each wrapper must #include exactly one vendored .cpp. Fails naming every source without a wrapper and every
wrapper whose source is not in the set.

Usage: check_vendored_wrappers_complete.py [--plugin-root DIR]
"""
import argparse
import re
import sys
from pathlib import Path


def block(text: str, name: str) -> set[str]:
    m = re.search(r"set\(" + name + r"\s*(.*?)\)", text, re.S)
    if not m:
        raise SystemExit(f"SEMB_UE_SOURCE_LIST_MISSING: no set({name} ...)")
    return set(re.findall(r"^\s*(src/\S+\.cpp)\s*$", m.group(1), re.M))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-root", type=Path, default=Path(__file__).resolve().parent.parent)
    root = ap.parse_args().plugin_root
    tp = root / "Source" / "ThirdParty"
    want = {f"SuperSLM/{s}" for s in block((tp / "SuperSLM/CMakeLists.txt").read_text(), "SUPERSLM_CORE_SOURCES")}
    se = block((tp / "SuperEmbedder/CMakeLists.txt").read_text(), "SEMB_LIBRARY_SOURCES")
    se |= {"src/superembedder/engine_adapter_gpu_off.cpp", "src/superembedder/engine_identity/build_identity.cpp"}
    want |= {f"SuperEmbedder/{s}" for s in se}
    have = {}
    errors = []
    for w in sorted((root / "Source/SuperEmbedderCore/Private/Vendored").glob("*.cpp")):
        inc = re.findall(r'#include\s+"([^"]+\.cpp)"', w.read_text())
        inc = [i for i in inc if "ThirdParty/" in i]
        if len(inc) != 1:
            errors.append(f"{w.name} includes {len(inc)} vendored sources (must be exactly one)")
            continue
        rel = inc[0].split("ThirdParty/", 1)[1]
        prefix = "SuperEmbedderVendoredEngine_" if rel.startswith("SuperSLM/") else "SuperEmbedderVendored_"
        if w.name != prefix + Path(rel).stem + ".cpp":
            errors.append(f"{w.name} wraps {rel}; its name must be {prefix}{Path(rel).stem}.cpp")
        have[rel] = w.name
    for s in sorted(want - set(have)):
        errors.append(f"no wrapper for {s}")
    for s in sorted(set(have) - want):
        errors.append(f"{have[s]} wraps {s}, which the module must not compile")
    for e in errors:
        print("SEMB_UE_WRAPPERS_INCOMPLETE: " + e)
    if not errors:
        print(f"wrappers complete: {len(have)} vendored sources, one wrapper each")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
