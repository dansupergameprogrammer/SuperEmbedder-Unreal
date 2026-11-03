#!/usr/bin/env python3
"""R-1, the symbol-clash check (the proposal's section 3.1 and cell R-1), GCC objects.

  check_symbols.py <renamed objdir> <plain objdir> <names file> [<conflicts file>]

Fails on any STRONG symbol both object sets define, and on any WEAK (COMDAT) symbol both define whose
demangled name contains an identifier from the rename list as a whole identifier, standard-library
instantiations included. Weak symbols that name no listed identifier (plain standard-library code) are
counted and allowed: the body check (bodycmp.py) judges those by body. When a conflicts file is given,
every shared symbol that names a conflict-set identifier is listed, since it is allowed only while the
body check passes for it.
"""
import re
import subprocess
import sys
from pathlib import Path


def defined(objdir: str):
    objs = sorted(str(p) for p in Path(objdir).glob("*.o"))
    out = subprocess.run(["nm", "-g", "--defined-only"] + objs, capture_output=True, text=True).stdout
    strong, weak = set(), set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        kind, name = parts[1], parts[2]
        if kind in "TDBR":
            strong.add(name)
        elif kind in "WVu":
            weak.add(name)
    return strong, weak


def demangle(names):
    if not names:
        return {}
    out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, out))


def main() -> int:
    renamed, plain, names_file = sys.argv[1:4]
    listed = set(Path(names_file).read_text().split())
    conflicts = set()
    if len(sys.argv) > 4 and Path(sys.argv[4]).is_file():
        conflicts = {l.split("\t")[0] for l in Path(sys.argv[4]).read_text().splitlines() if l.strip()}
    rs, rw = defined(renamed)
    ps, pw = defined(plain)
    strong_both = sorted(rs & ps)
    weak_both = sorted(rw & pw)
    dm = demangle(strong_both + weak_both)
    ident = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    naming = [s for s in weak_both if set(ident.findall(dm[s])) & listed]
    conflict_hits = [s for s in strong_both + weak_both if set(ident.findall(dm[s])) & conflicts]
    print(f"R-1: strong in both: {len(strong_both)}")
    for s in strong_both[:20]:
        print(f"  STRONG {dm[s]}")
    print(f"R-1: weak in both naming a listed engine identifier: {len(naming)}")
    for s in naming[:20]:
        print(f"  WEAK {dm[s]}")
    print(f"R-1: weak in both naming no listed identifier (allowed; judged by body): {len(weak_both) - len(naming)}")
    print(f"R-1: shared symbols naming a conflict-set identifier: {len(conflict_hits)}")
    for s in conflict_hits[:20]:
        print(f"  CONFLICT {dm[s]}")
    return 1 if (strong_both or naming) else 0


if __name__ == "__main__":
    sys.exit(main())
