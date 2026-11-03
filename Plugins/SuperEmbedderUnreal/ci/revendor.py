#!/usr/bin/env python3
"""Re-vendor SuperEmbedderCore's two libraries at a new pin: the vendored trees, their manifests and the wrappers.

The vendored subsets (VENDORED_VERSION.txt in each tree says why these and no more):
  ThirdParty/SuperSLM       include/, src/, CMakeLists.txt, .gitattributes, CHANGELOG.md, LICENSE, NOTICE, README.md
  ThirdParty/SuperEmbedder  include/, src/, cmake/, CMakeLists.txt, superslm.lock, docs/certified-builds.json,
                            docs/certification-identity.json, tests/fixtures/wq/{wq-queries.jsonl,
                            wq-queries.manifest.json, wq-truncation-leg.jsonl}, .gitattributes, CHANGELOG.md,
                            LICENSE, NOTICE, README.md
Every file is written as `git cat-file blob` of the revision's own blob (raw, no checkout filters), files the new
revision no longer has are removed, and VENDORED_MANIFEST.sha256 is rewritten. Private/Vendored/ then gets one
wrapper per source check_vendored_wrappers_complete.py requires and no other.

The SuperEmbedder revision must pin the SuperSLM revision (its superslm.lock names that commit). Afterwards:
VENDORED_VERSION.txt (by hand: it is prose), generate_rename_header.py, generate_semb_config.py, and the checks.

Usage: revendor.py --superslm-repo DIR --superslm-rev REV --superembedder-repo DIR --superembedder-rev REV
                   [--plugin-root DIR] [--dry-run]
"""
from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

SUBSETS = {
    'SuperSLM': (['include/', 'src/'], ['CMakeLists.txt', '.gitattributes', 'CHANGELOG.md', 'LICENSE', 'NOTICE', 'README.md']),
    'SuperEmbedder': (['include/', 'src/', 'cmake/'],
                      ['CMakeLists.txt', 'superslm.lock', 'docs/certified-builds.json', 'docs/certification-identity.json',
                       'tests/fixtures/wq/wq-queries.jsonl', 'tests/fixtures/wq/wq-queries.manifest.json',
                       'tests/fixtures/wq/wq-truncation-leg.jsonl', '.gitattributes', 'CHANGELOG.md', 'LICENSE',
                       'NOTICE', 'README.md']),
}
KEEP = {'VENDORED_MANIFEST.sha256', 'VENDORED_VERSION.txt'}
HEADER = '# SHA-256 of every vendored file except this manifest and VENDORED_VERSION.txt. See VENDORED_VERSION.txt.\n'
ENGINE_WRAPPER = ('// Vendored-source wrapper: one translation unit per engine source, as in the engine\'s own build (several\n'
                  '// engine sources define same-named helpers in anonymous namespaces, so two in one TU collide). This module\n'
                  '// compiles the engine under the force-included rename header (SuperEmbedderCore.Build.cs), so every engine\n'
                  '// link name here is this plugin\'s own and cannot meet the SuperSLM plugin\'s copy. The vendored file is not\n'
                  '// edited. SuperSLM {tag}, {src}.\n'
                  '#include "../../../ThirdParty/SuperSLM/{src}"\n')
SEMB_WRAPPER = ('// Vendored-source wrapper: one translation unit per SuperEmbedder library source, as in its own CMake\n'
                '// build. Compiled under the force-included rename header, so the engine names this source uses resolve to\n'
                '// this module\'s own renamed engine copy. The vendored file is not edited. SuperEmbedder {src}.\n'
                '#include "../../../ThirdParty/SuperEmbedder/{src}"\n')


def same(path: Path, data: bytes) -> bool:
    # A host repository's eol=crlf checks the SuperSLM tree and the wrappers out with CR LF on Windows; the repository holds
    # the LF bytes, which is what the manifest records and what is compared here.
    # A file whose committed bytes carry CR LF of their own (SuperEmbedder's `* -text` tree) compares raw.
    if not path.is_file():
        return False
    have = path.read_bytes()
    return have == data or have.replace(b'\r\n', b'\n') == data


def git(repo: Path, *args: str) -> bytes:
    return subprocess.run(['git', '-C', str(repo), *args], check=True, stdout=subprocess.PIPE).stdout


def wanted(repo: Path, rev: str, name: str) -> list[str]:
    dirs, files = SUBSETS[name]
    listed = git(repo, 'ls-tree', '-r', '--name-only', '-z', rev).decode('utf-8').split('\0')
    paths = sorted(p for p in listed if p and (p in files or any(p.startswith(d) for d in dirs)))
    missing = [f for f in files if f not in paths]
    if missing:
        raise SystemExit(f'SEMB_UE_REVENDOR: {name} at {rev} has no {missing}')
    return paths


def vendor(repo: Path, rev: str, dest: Path, name: str, dry: bool) -> dict:
    paths = wanted(repo, rev, name)
    old = {p.relative_to(dest).as_posix() for p in dest.rglob('*') if p.is_file() and p.name not in KEEP}
    lines, changed = [], 0
    for p in paths:
        data = git(repo, 'cat-file', 'blob', f'{rev}:{p}')
        out = dest / p
        if not same(out, data):
            changed += 1
            if not dry:
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(data)
        lines.append(f'{hashlib.sha256(data).hexdigest()}  {p}\n')
    removed = sorted(old - set(paths))
    if not dry:
        for p in removed:
            (dest / p).unlink()
        for d in sorted({q for q in dest.rglob('*') if q.is_dir()}, key=lambda q: len(q.parts), reverse=True):
            if not any(d.iterdir()):
                d.rmdir()
        (dest / 'VENDORED_MANIFEST.sha256').write_bytes((HEADER + ''.join(lines)).encode('utf-8'))
    return {'files': len(paths), 'changed': changed, 'added': sorted(set(paths) - old), 'removed': removed}


def block(text: str, var: str) -> list[str]:
    m = re.search(r'set\(' + var + r'\s*(.*?)\)', text, re.S)
    if not m:
        raise SystemExit(f'SEMB_UE_SOURCE_LIST_MISSING: no set({var} ...)')
    return re.findall(r'^\s*(src/\S+\.cpp)\s*$', m.group(1), re.M)


def wrappers(root: Path, tag: str, dry: bool) -> dict:
    tp = root / 'Source' / 'ThirdParty'
    vd = root / 'Source' / 'SuperEmbedderCore' / 'Private' / 'Vendored'
    want = {}
    for src in block((tp / 'SuperSLM/CMakeLists.txt').read_text(encoding='utf-8'), 'SUPERSLM_CORE_SOURCES'):
        want[f'SuperEmbedderVendoredEngine_{Path(src).stem}.cpp'] = ENGINE_WRAPPER.format(tag=tag, src=src)
    se = block((tp / 'SuperEmbedder/CMakeLists.txt').read_text(encoding='utf-8'), 'SEMB_LIBRARY_SOURCES')
    se += ['src/superembedder/engine_adapter_gpu_off.cpp', 'src/superembedder/engine_identity/build_identity.cpp']
    for src in se:
        want[f'SuperEmbedderVendored_{Path(src).stem}.cpp'] = SEMB_WRAPPER.format(src=src)
    have = {p.name: p for p in vd.glob('*.cpp')}
    report = {'added': sorted(set(want) - set(have)), 'removed': sorted(set(have) - set(want)), 'rewritten': []}
    for name, text in want.items():
        p = vd / name
        if not same(p, text.encode('utf-8')):
            if name in have:
                report['rewritten'].append(name)
            if not dry:
                p.write_bytes(text.encode('utf-8'))
    if not dry:
        for name in report['removed']:
            have[name].unlink()
    return report


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--superslm-repo', type=Path, required=True)
    ap.add_argument('--superslm-rev', required=True)
    ap.add_argument('--superembedder-repo', type=Path, required=True)
    ap.add_argument('--superembedder-rev', required=True)
    ap.add_argument('--plugin-root', type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    slm = git(a.superslm_repo, 'rev-parse', f'{a.superslm_rev}^{{commit}}').decode().strip()
    lock = git(a.superembedder_repo, 'cat-file', 'blob', f'{a.superembedder_rev}:superslm.lock').decode()
    pinned = re.search(r'^commit=([0-9a-f]{40})$', lock, re.M)
    if not pinned or pinned[1] != slm:
        raise SystemExit(f'SEMB_UE_REVENDOR: SuperEmbedder {a.superembedder_rev} pins {pinned[1] if pinned else "?"}, '
                         f'not SuperSLM {a.superslm_rev} ({slm})')
    tag = re.search(r'^tag=(\S+)$', lock, re.M)[1]
    tp = a.plugin_root / 'Source' / 'ThirdParty'
    for name, repo, rev in (('SuperSLM', a.superslm_repo, slm), ('SuperEmbedder', a.superembedder_repo, a.superembedder_rev)):
        r = vendor(repo, rev, tp / name, name, a.dry_run)
        print(f'{name} at {rev}: {r["files"]} files, {r["changed"]} written; added {r["added"] or "none"}; '
              f'removed {r["removed"] or "none"}')
    w = wrappers(a.plugin_root, tag, a.dry_run)
    print(f'wrappers: added {w["added"] or "none"}; removed {w["removed"] or "none"}; rewritten {len(w["rewritten"])}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
