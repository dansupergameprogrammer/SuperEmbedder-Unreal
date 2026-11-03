"""R-1 and R-1b on MSVC: the two engine copies' COFF objects, compared symbol by symbol.

The MSVC counterpart of ci/host/coexist's nm/objdump checks. It reads the .obj files directly (plain COFF and
/bigobj), so nothing depends on dumpbin's text layout.

  python coff_coexist.py --a <dir of SuperEmbedderCore objs> --b <dir of the other plugin's engine objs>
                         --names <ci/rename/msvc.names> [--json out.json] [--expect-collision]

Each side's defined external symbols are collected. For a symbol defined on both sides:
  R-1   a strong definition on both sides (a non-COMDAT section, or a COMDAT whose selection forbids
        duplicates) is a failure; a COMDAT symbol on both sides whose decorated name contains a listed
        identifier is a failure (the rename should have moved it).
  R-1b  every other shared COMDAT symbol (the linker keeps one copy of it in a monolithic image) has its body
        compared: the section's bytes with every relocated field masked, plus each relocation's type and target.
        An external or COMDAT target compares by name; a static target (a literal, a jump table, a section
        symbol) compares by value, its section's normalised bytes. Weak externals compare their alias target.
        The sides agree when they hold the same set of distinct bodies. A shared COMDAT that cannot be
        compared is a failure, never a pass.
Exit 0 when R-1 and R-1b are clean; 1 otherwise. With --expect-collision (the control: the same engine sources
without the rename) the exit is 0 only if R-1 finds collisions, so the instrument is shown to refuse.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

BIGOBJ_CLASSID = bytes.fromhex('c7a1bad1eebaa94baf20faf66aa4dcb8')
SCN_LNK_COMDAT = 0x1000
CLASS_EXTERNAL, CLASS_STATIC, CLASS_LABEL, CLASS_WEAK_EXTERNAL = 2, 3, 6, 105
COMDAT_NODUPLICATES, COMDAT_ASSOCIATIVE = 1, 5
# AMD64 relocation field widths (IMAGE_REL_AMD64_*).
RELOC_WIDTH = {0: 0, 1: 8, 2: 4, 3: 4, 4: 4, 5: 4, 6: 4, 7: 4, 8: 4, 9: 4, 0xA: 2, 0xB: 4, 0xC: 1, 0xD: 4, 0xE: 4,
               0xF: 4, 0x10: 4}


class Obj:
    def __init__(self, path: Path):
        self.path = path
        d = path.read_bytes()
        self.data = d
        sig1, sig2 = struct.unpack_from('<HH', d, 0)
        if sig1 == 0 and sig2 == 0xFFFF and d[12:28] == BIGOBJ_CLASSID:
            self.bigobj = True
            self.nsect, self.symptr, self.nsym = struct.unpack_from('<III', d, 44)
            hdr, self.symsz = 56, 20
        else:
            self.bigobj = False
            _, self.nsect, _, self.symptr, self.nsym, opt, _ = struct.unpack_from('<HHIIIHH', d, 0)
            hdr, self.symsz = 20 + opt, 18
        self.strtab_off = self.symptr + self.nsym * self.symsz
        self.sections = []
        for i in range(self.nsect):
            o = hdr + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode('latin-1')
            vsize, vaddr, rsize, rptr, relptr, _, nrel, _, ch = struct.unpack_from('<IIIIIIHHI', d, o + 8)
            if name.startswith('/'):
                name = self.string(int(name[1:]))
            self.sections.append({'name': name, 'size': rsize, 'ptr': rptr, 'relptr': relptr, 'nrel': nrel,
                                  'ch': ch, 'sel': None, 'assoc': None})
        self.symbols = []  # index -> dict or None for aux slots
        i = 0
        while i < self.nsym:
            o = self.symptr + i * self.symsz
            raw = d[o:o + 8]
            if raw[:4] == b'\0\0\0\0':
                name = self.string(struct.unpack_from('<I', raw, 4)[0])
            else:
                name = raw.rstrip(b'\0').decode('latin-1')
            if self.bigobj:
                value, secnum, typ, cls, naux = struct.unpack_from('<IiHBB', d, o + 8)
            else:
                value, secnum, typ, cls, naux = struct.unpack_from('<IhHBB', d, o + 8)
            sym = {'name': name, 'value': value, 'sec': secnum, 'cls': cls, 'index': i, 'weak_target': None}
            if cls == CLASS_STATIC and naux and 0 < secnum <= self.nsect and value == 0:
                ao = o + self.symsz
                _, _, _, _, number, sel = struct.unpack_from('<IHHIHB', d, ao)
                sec = self.sections[secnum - 1]
                if sec['ch'] & SCN_LNK_COMDAT and sec['sel'] is None:
                    sec['sel'] = sel
                    if sel == COMDAT_ASSOCIATIVE:
                        sec['assoc'] = number
            if cls == CLASS_WEAK_EXTERNAL and naux:
                sym['weak_target'] = struct.unpack_from('<I', d, o + self.symsz)[0]
            self.symbols.append(sym)
            for _ in range(naux):
                self.symbols.append(None)
            i += 1 + naux

    def string(self, off: int) -> str:
        end = self.data.index(b'\0', self.strtab_off + off)
        return self.data[self.strtab_off + off:end].decode('latin-1')

    def section_bytes(self, n: int) -> bytes:
        s = self.sections[n - 1]
        return self.data[s['ptr']:s['ptr'] + s['size']] if s['ptr'] else b'\0' * s['size']

    def relocs(self, n: int):
        s = self.sections[n - 1]
        for k in range(s['nrel']):
            off, symidx, typ = struct.unpack_from('<IIH', self.data, s['relptr'] + 10 * k)
            yield off, symidx, typ


def body(obj: Obj, secnum: int, depth: int = 0) -> str:
    """Normalised content of a section: bytes with relocated fields masked, plus each relocation's target."""
    raw = bytearray(obj.section_bytes(secnum))
    parts = []
    for off, symidx, typ in obj.relocs(secnum):
        w = RELOC_WIDTH.get(typ, 4)
        addend = bytes(raw[off:off + w])
        raw[off:off + w] = b'\0' * w
        tgt = obj.symbols[symidx] if symidx < len(obj.symbols) else None
        if tgt is None:
            t = '?'
        elif tgt['cls'] in (CLASS_EXTERNAL, CLASS_WEAK_EXTERNAL) or (tgt['cls'] == CLASS_STATIC and tgt['sec'] > 0
                                                                       and obj.sections[tgt['sec'] - 1]['ch'] & SCN_LNK_COMDAT
                                                                       and tgt['value'] == 0 and not tgt['name'].startswith('.')):
            t = 'N:' + tgt['name']
        elif tgt['sec'] > 0 and depth < 2:
            # A static target compares by value: its section's normalised bytes and the offset into it.
            t = f'V:{hashlib.sha256(body(obj, tgt["sec"], depth + 1).encode()).hexdigest()[:24]}+{tgt["value"]}'
        else:
            t = f'L:{tgt["name"]}'
        parts.append(f'{off}:{typ}:{t}:{addend.hex()}')
    return hashlib.sha256(bytes(raw)).hexdigest() + '|' + ';'.join(parts)


# UBT's per-module boilerplate (operator new/delete, FMemory shims, debugger-visualizer globals,
# ThisIsAnUnrealEngineModule): every module of a modular build has one, and a monolithic build emits one per image.
# It is Unreal's, not either engine copy's.
BOILERPLATE = ('PerModuleInline.gen.cpp.obj',)


# R-1b exemption (ruled 2026-10-23): a shared symbol whose demangled name references only std:: names
# and builtin types is exempt from byte equality. MSVC instantiates the same standard-library template differently
# in different translation units (inlining), and those bodies are ODR-equivalent. An engine type anywhere in the
# signature would carry its renamed name and could not be shared, so the exemption never reaches engine code. Every
# other shared symbol keeps byte equality.
BUILTIN_WORDS = {
    'public', 'private', 'protected', 'static', 'virtual', 'const', 'volatile', 'class', 'struct', 'union', 'enum',
    'unsigned', 'signed', 'void', 'bool', 'char', 'short', 'int', 'long', 'float', 'double', 'wchar_t', 'char8_t',
    'char16_t', 'char32_t', '__int8', '__int16', '__int32', '__int64', '__int128', '__cdecl', '__stdcall',
    '__fastcall', '__thiscall', '__vectorcall', '__ptr64', '__restrict', 'operator', 'new', 'delete', 'std', 'nullptr_t',
}


def demangle(names, undname):
    """Demangled text per name via MSVC's undname.exe (batches, so the command line stays short)."""
    import subprocess
    out = {}
    names = list(names)
    for i in range(0, len(names), 40):
        batch = names[i:i + 40]
        p = subprocess.run([str(undname), *batch], capture_output=True, text=True, errors='replace')
        lines = [l for l in p.stdout.splitlines() if l.startswith('is :- ')]
        for n, l in zip(batch, lines):
            d = l[len('is :- '):].strip().strip('"')
            if d != n:  # undname echoes a name it cannot decode; that name is never exempt (fail closed)
                out[n] = d
    return out


def std_only(demangled: str) -> bool:
    """True when every head identifier (one not reached through '::') is std or a builtin word."""
    text = re.sub(r'`[^\']*\'', ' ', demangled)  # compiler-generated labels such as `vftable'
    text = text.replace('::~', '::')  # a destructor is a member of its class, like any other member
    for m in re.finditer(r'(?<![:\w])([A-Za-z_~][A-Za-z0-9_]*)', text):
        start = m.start()
        if text[max(0, start - 2):start] == '::':
            continue  # a member: its scope is a head checked on its own
        if m.group(1) not in BUILTIN_WORDS:
            return False
    return 'std::' in text


# R-1b exemption, second form (ruled 2026-11-02): a standard-library template whose name carries a
# lambda closure type (which undname may not decode) or a CRT type such as _Mbstatet. Exempt only when the symbol is
# in namespace std and its whole name, decorated and demangled, names no identifier of the rename list and no
# superslm, semb or sslm token, so it still never reaches engine code. Listed apart from the first form.
ENGINE_TOKEN = re.compile(r'superslm|semb|sslm', re.I)


def std_no_engine(name: str, demangled: str | None, listed: set, ident) -> bool:
    if '@std@@' not in name:
        return False
    text = name + ' ' + (demangled or '')
    if ENGINE_TOKEN.search(text):
        return False
    return not any(t in listed for t in ident.findall(text))


def collect(dirpath: Path):
    """name -> list of (kind, body or None, obj name). kind: strong, comdat, weak."""
    out = defaultdict(list)
    objs = sorted(p for p in dirpath.glob('*.obj') if p.name not in BOILERPLATE)
    for p in objs:
        o = Obj(p)
        for s in o.symbols:
            if s is None:
                continue
            if s['cls'] == CLASS_WEAK_EXTERNAL:
                tgt = o.symbols[s['weak_target']] if s['weak_target'] is not None else None
                out[s['name']].append(('weak', 'alias:' + (tgt['name'] if tgt else '?'), p.name))
                continue
            if s['cls'] != CLASS_EXTERNAL or s['sec'] <= 0:
                continue
            sec = o.sections[s['sec'] - 1]
            if sec['ch'] & SCN_LNK_COMDAT and sec['sel'] != COMDAT_NODUPLICATES:
                out[s['name']].append(('comdat', body(o, s['sec']) + f'@{s["value"]}', p.name))
            else:
                out[s['name']].append(('strong', None, p.name))
    return out, len(objs)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--a', required=True, type=Path)
    ap.add_argument('--b', required=True, type=Path)
    ap.add_argument('--names', required=True, type=Path)
    ap.add_argument('--json', type=Path)
    ap.add_argument('--expect-collision', action='store_true')
    ap.add_argument('--undname', type=Path, default=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\undname.exe'))
    a = ap.parse_args(argv)
    listed = [n for n in a.names.read_text(encoding='utf-8').split() if n]
    # A decorated MSVC name embeds identifiers between '?', '@' and '$'; match whole identifiers only.
    listed_set = set(listed)
    ident = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
    A, na = collect(a.a)
    B, nb = collect(a.b)
    shared = sorted(set(A) & set(B))
    strong, listed_hits, compared, differ, uncompared = [], [], 0, [], []
    for name in shared:
        ka = {k for k, _, _ in A[name]}; kb = {k for k, _, _ in B[name]}
        if 'strong' in ka and 'strong' in kb:
            strong.append(name); continue
        if 'strong' in ka or 'strong' in kb:
            # One side strong, the other COMDAT: the linker takes the strong one for the whole image.
            strong.append(name); continue
        # A string literal's COMDAT name (??_C@...) embeds the literal's own text, so it is content, not an
        # identifier: it compares by body below.
        if not name.startswith('??_C@') and any(t in listed_set for t in ident.findall(name)):
            listed_hits.append(name); continue
        ba = {b for k, b, _ in A[name] if b}; bb = {b for k, b, _ in B[name] if b}
        if not ba or not bb:
            uncompared.append(name); continue
        compared += 1
        if ba != bb:
            differ.append(name)
    dm = demangle(differ, a.undname) if differ else {}
    exempt = [n for n in differ if n in dm and std_only(dm[n])]
    exempt_lambda = [n for n in differ if n not in exempt and std_no_engine(n, dm.get(n), listed_set, ident)]
    differ = [n for n in differ if n not in exempt and n not in exempt_lambda]
    result = {'objs_a': na, 'objs_b': nb, 'defined_a': len(A), 'defined_b': len(B), 'shared': len(shared),
              'strong_both': len(strong), 'listed_shared': len(listed_hits), 'comdat_compared': compared,
              'bodies_differ': len(differ), 'exempt_std_only': len(exempt),
              'exempt_std_no_engine': len(exempt_lambda), 'uncompared': len(uncompared),
              'exempt': [dm[n] for n in exempt], 'exempt_no_engine': [dm.get(n, n) for n in exempt_lambda],
              'examples': {'strong': strong[:20], 'listed': listed_hits[:20], 'differ': differ[:20],
                           'uncompared': uncompared[:20]}}
    print(json.dumps({k: v for k, v in result.items() if k not in ('examples', 'exempt', 'exempt_no_engine')}))
    for d in result['exempt']:
        print(f'  exempt (std:: and builtin types only): {d}')
    for d in result['exempt_no_engine']:
        print(f'  exempt (std::, no renamed or engine name; lambda or CRT types): {d}')
    for k, v in result['examples'].items():
        for n in v[:8]:
            print(f'  {k}: {n}')
    if a.json:
        a.json.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    clean = not strong and not listed_hits and not differ and not uncompared
    if a.expect_collision:
        return 0 if (strong or listed_hits) else 1
    return 0 if clean else 1


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
