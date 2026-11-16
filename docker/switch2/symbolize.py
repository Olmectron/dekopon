#!/usr/bin/env python3
# Resolve a dekopon crash.txt "pc (+0xNNNNNN)" / "lr (+0xNNNNNN)" offset to the
# containing function, using the matching unstripped citra_switch.elf (the
# dekopon-switch2-debug CI artifact from the SAME run as the flashed NRO).
#
#   pip install pyelftools cxxfilt
#   python symbolize.py <citra_switch.elf> <offset-hex> [<offset-hex> ...]
#
# The crash handler reports value - (&_start). The offset therefore equals
# elf_vaddr(target) - elf_vaddr(_start); _start is at vaddr 0x0 here, so the
# offset is the target vaddr directly. We find the STT_FUNC symbol whose
# [st_value, st_value+st_size) covers it.
import sys
from elftools.elf.elffile import ELFFile

def load(elf_path):
    e = ELFFile(open(elf_path, "rb"))
    sym = e.get_section_by_name(".symtab") or e.get_section_by_name(".dynsym")
    start = 0
    funcs = []
    for s in sym.iter_symbols():
        info = s["st_info"]
        if s.name == "_start" and s["st_value"] and info["type"] == "STT_FUNC":
            start = s["st_value"]
        if info["type"] == "STT_FUNC" and s["st_size"] > 0:
            funcs.append((s["st_value"], s["st_value"] + s["st_size"], s.name))
    funcs.sort()
    return start, funcs

def find(funcs, vaddr):
    lo, hi = 0, len(funcs)
    while lo < hi:
        mid = (lo + hi) // 2
        if funcs[mid][0] <= vaddr:
            lo = mid + 1
        else:
            hi = mid
    i = lo - 1
    if i >= 0 and funcs[i][0] <= vaddr < funcs[i][1]:
        s, _, name = funcs[i]
        return name, vaddr - s
    if i >= 0:
        s, _, name = funcs[i]
        return name + " (?nearest)", vaddr - s
    return "<unknown>", 0

def demangle(name):
    try:
        import cxxfilt
        return cxxfilt.demangle(name)
    except Exception:
        return name

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    elf = sys.argv[1]
    start, funcs = load(elf)
    print(f"_start vaddr = {start:#x}; {len(funcs)} function symbols")
    for off_s in sys.argv[2:]:
        off = int(off_s, 16)
        vaddr = start + off
        name, delta = find(funcs, vaddr)
        print(f"\noffset +{off:#x}  -> vaddr {vaddr:#x}")
        print(f"  {demangle(name)}  (+{delta:#x})")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
