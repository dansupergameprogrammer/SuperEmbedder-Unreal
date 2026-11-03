# From the coexist probe (plans/superembedder-unreal/coexist-probe/restrike-repair/bodycmp.py, the
# proposal's R-1b prototype). Usage: bodycmp.py <renamed objdir> <plain objdir>
# One change: a relocation to a compiler-local literal label (.LC<n>, which GCC -O2 uses for string and
# constant pools) is compared by the bytes at the label, not by the label's number, which differs between
# translation units. The prototype compared literals by value only for section-relative relocations
# (revision 9's rule: a constant is compared by its value, not its offset); labels are the same rule, and a
# printable NUL-terminated string is compared up to its NUL (ASan's frame descriptions sit in plain .rodata).
# Two more, from revision 9: aliases are resolved by address (the prototype resolved them by name and left
# some standard-library constructors uncompared), and a shared weak symbol left uncompared fails the check.
# R-1b: every weak symbol both copies define must have the same body. Relocation targets are compared by name,
# with the renamed copy's semb_ prefix removed, so a renamed callee is not a difference.
import sys,subprocess,re,glob,collections,functools
@functools.lru_cache(None)
def dm(x):
    base,_,off=x.partition('-') if '-0x' in x else (x,'','')
    d=subprocess.run(['c++filt',base],capture_output=True,text=True).stdout.strip()
    return re.sub(r'\bsemb_','',d)+('-'+off if off else '')
import os,tempfile
@functools.lru_cache(None)
def secbytes(o,sec):
    f=tempfile.mktemp(); subprocess.run(['objcopy','-O','binary','--only-section='+sec,o,f],capture_output=True)
    b=open(f,'rb').read() if os.path.exists(f) else b''
    if os.path.exists(f): os.remove(f)
    return b
def lit(o,sec,off):
    b=secbytes(o,sec)
    end=b.find(b'\0',off)
    if '.str' in sec:  # a mergeable string section: the NUL-terminated string at the offset
        return b[off:(end if end>=0 else off+256)].hex()
    # A NUL-terminated printable string in plain .rodata (ASan's frame descriptions are one): the string,
    # since the bytes after its NUL belong to whatever the compiler placed next.
    if 0 < end-off <= 256 and all(32 <= c < 127 for c in b[off:end]):
        return b[off:end].hex()
    return b[off:off+16].hex()
@functools.lru_cache(None)
def labels(o):
    out={}
    for l in subprocess.run(['objdump','-t','-w',o],capture_output=True,text=True).stdout.splitlines():
        m=re.match(r'^([0-9a-f]+) l\s+\S*\s+(\S+)\s+[0-9a-f]+\s+(\.L[A-Za-z]*\d+)$',l)
        if m: out[m.group(3)]=(m.group(2),int(m.group(1),16))
    return out
def label_lit(o,kind,name,addend):
    sec,val=labels(o).get(name,(None,None))
    if sec is None: return 'LABEL['+name+']'
    return 'LIT['+lit(o,sec,val+addend+(4 if 'PC32' in kind or 'PLT32' in kind else 0))+']'
def datasyms(d):
    out={}
    for o in sorted(glob.glob(d+'/*.o')):
        t=subprocess.run(['objdump','-t','-w',o],capture_output=True,text=True).stdout
        for l in t.splitlines():
            m=re.match(r'^([0-9a-f]+) (.{7}) (\S+)\s+([0-9a-f]+)\s+(?:\.hidden\s+)?(\S+)$',l)
            if m and 'O' in m.group(2) and ('w' in m.group(2) or 'u' in m.group(2)):
                sec,size,name=m.group(3),int(m.group(4),16),m.group(5); val=int(m.group(1),16)
                if name in out: continue
                rel=subprocess.run(['objdump','-r','-j',sec,'-w',o],capture_output=True,text=True).stdout
                rels=[re.sub(r'^[0-9a-f]+\s+','',x) for x in rel.splitlines() if re.match(r'^[0-9a-f]+\s+R_',x)]
                raw=b'' if sec.startswith('.bss') or sec=='*COM*' else secbytes(o,sec)[val:val+size]
                out[name]=(raw.hex(),size,tuple(dm(r.split()[-1]) for r in rels))
    return out
def bodies(d):
    out={}
    for o in sorted(glob.glob(d+'/*.o')):
        txt=subprocess.run(['objdump','-d','-r','--no-show-raw-insn','-w',o],capture_output=True,text=True).stdout
        cur=None
        for l in txt.splitlines():
            m=re.match(r'^[0-9a-f]+ <(.+)>:$',l)
            if m: cur=m.group(1); buf=out.setdefault(cur,[]) if cur not in out else None; continue
            if l.startswith('Disassembly of section'): cur=None; continue
            if cur is None or buf is None: continue
            if not l.strip(): continue
            l=re.sub(r'^\s*[0-9a-f]+:\s*','',l)            # address
            l=re.sub(r'#?\s*<[^>]*>','',l)                  # objdump's nearest-symbol guesses
            l=re.sub(r'\b[0-9a-f]+:\s*(R_\S+)\s+(\S+)',lambda m:m.group(1)+' '+dm(m.group(2)),l)  # relocation: name by demangled target
            l=re.sub(r'(R_\S+)\s+(\.rodata[.\w]*)([+-]0x[0-9a-f]+)',lambda m: 'LIT['+lit(o,m.group(2),int(m.group(3),16)+(4 if 'PC32' in m.group(1) or 'PLT32' in m.group(1) else 0))+']',l)  # constant by value
            l=re.sub(r'R_\S+\s+(\.rodata[.\w]*)(?=\s|$)',lambda m: 'LIT['+lit(o,m.group(1),0)+']',l)
            l=re.sub(r'(R_\S+)\s+(\.L[A-Za-z]*\d+)([+-]0x[0-9a-f]+)?',lambda m: label_lit(o,m.group(1),m.group(2),int(m.group(3) or '0',16)),l)  # local label by value
            buf.append(l)
    return out
weak=lambda d: set(subprocess.run(f"nm -g --defined-only {d}/*.o | awk '$2 ~ /^[WVu]$/ {{print $3}}'",shell=True,capture_output=True,text=True).stdout.split())
def aliases(d,have):
    """Code aliases (a complete and a base constructor or destructor sharing one body): a symbol objdump
    printed no body for is compared through the symbol at the same section and address that has one."""
    out={}
    for o in sorted(glob.glob(d+'/*.o')):
        at={}
        for l in subprocess.run(['objdump','-t','-w',o],capture_output=True,text=True).stdout.splitlines():
            m=re.match(r'^([0-9a-f]+) (.{7}) (\S+)\s+[0-9a-f]+\s+(?:\.hidden\s+)?(\S+)$',l)
            if m and 'F' in m.group(2): at.setdefault((m.group(3),m.group(1)),[]).append(m.group(4))
        for names in at.values():
            body=[n for n in names if n in have]
            for n in names:
                if n not in have and body: out.setdefault(n,body[0])
    return out
a,b=sys.argv[1],sys.argv[2]
shared=weak(a)&weak(b); A=bodies(a); B=bodies(b)
for X,d in ((A,a),(B,b)):
    for n,t in aliases(d,X).items(): X[n]=X[t]
diff=[s for s in sorted(shared) if s in A and s in B and A[s]!=B[s]]
DA=datasyms(a); DB=datasyms(b)
ddiff=[s for s in sorted(shared) if s in DA and s in DB and DA[s]!=DB[s]]
diff+=ddiff
nodata=[s for s in shared if not ((s in A and s in B) or (s in DA and s in DB))]
print(f"R-1b: weak symbols in both: {len(shared)}; bodies compared: {len(shared)-len(nodata)} (code and data; not compared: {len(nodata)}); bodies differ: {len(diff)}")
for s in diff[:8]: print("  DIFFERS:", subprocess.run(['c++filt',s],capture_output=True,text=True).stdout.strip())
# Revision 9: a shared weak symbol the check cannot compare fails it.
for s in sorted(nodata)[:8]: print("  NOT COMPARED:", subprocess.run(['c++filt',s],capture_output=True,text=True).stdout.strip())
sys.exit(1 if (diff or nodata) else 0)
