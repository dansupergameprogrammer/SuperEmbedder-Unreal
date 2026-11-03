#!/usr/bin/env python3
"""Each vendored tree (Source/ThirdParty/SuperSLM, Source/ThirdParty/SuperEmbedder) matches its
VENDORED_MANIFEST.sha256 exactly: every listed file present with its hash, and no unlisted file
(VENDORED_VERSION.txt and the manifest itself excepted). A hand edit to a vendored file fails here.

Hashes are of the committed bytes. The SuperEmbedder tree carries its own `* -text`, so its bytes are
checkout-invariant; the SuperSLM tree's .cpp/.h files take a host repository's eol=crlf on a Windows checkout, so on
Windows this check reads them with CR LF folded to LF (the manifest records the repository's LF bytes).

Usage: check_vendored_tree_matches_manifest.py [--plugin-root DIR]
"""
import argparse
import hashlib
import sys
from pathlib import Path

SKIP = {"VENDORED_MANIFEST.sha256", "VENDORED_VERSION.txt"}


def check(tree: Path) -> list[str]:
    errors = []
    listed = {}
    for line in (tree / "VENDORED_MANIFEST.sha256").read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        digest, path = line.split("  ", 1)
        listed[path] = digest
    present = {p.relative_to(tree).as_posix() for p in tree.rglob("*") if p.is_file() and p.name not in SKIP}
    for path in sorted(set(listed) - present):
        errors.append(f"{tree.name}: missing {path}")
    for path in sorted(present - set(listed)):
        errors.append(f"{tree.name}: not in the manifest: {path}")
    for path in sorted(set(listed) & present):
        data = (tree / path).read_bytes()
        if hashlib.sha256(data).hexdigest() != listed[path] and \
                hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest() != listed[path]:
            errors.append(f"{tree.name}: content differs from the manifest: {path}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-root", type=Path, default=Path(__file__).resolve().parent.parent)
    root = ap.parse_args().plugin_root
    errors = []
    for name in ("SuperSLM", "SuperEmbedder"):
        errors += check(root / "Source" / "ThirdParty" / name)
    for e in errors:
        print("SEMB_UE_VENDORED_TREE_DRIFT: " + e)
    if not errors:
        print("vendored trees match their manifests")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
