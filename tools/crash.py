"""Minimal x64 minidump reader for the EDF5 crash: module list, exception record,
context RIP/RSP, and a stack scan mapping return addresses back to our DLL symbols
via the linker .map file."""
import struct, sys, re, os

DUMP = os.path.expandvars(r"%LOCALAPPDATA%\CrashDumps\EDF5.exe.22868.dmp")
DLL_NEW = r"F:\deepseek-harness\default-workspace\build\EDF5EnemyHealthBar.dll"
DLL_DEP = r"E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\Mods\Plugins\EDF5EnemyHealthBar.dll"
MAP = r"F:\deepseek-harness\default-workspace\build\EDF5EnemyHealthBar.map"


def pe_sections(path):
    d = open(path, "rb").read()
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, e_lfanew + 6)[0]
    opt = e_lfanew + 24
    size_opt = struct.unpack_from("<H", d, e_lfanew + 20)[0]
    base = struct.unpack_from("<Q", d, opt + 24)[0]
    size_img = struct.unpack_from("<I", d, opt + 56)[0]
    secs = []
    off = opt + size_opt
    for i in range(nsec):
        name = d[off:off + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, off + 8)
        secs.append((name, vaddr, vsize, rawptr, rawsize))
        off += 40
    return base, size_img, secs


def load_map(path, text_rva):
    syms = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)", line)
        if m:
            syms.append((int(m.group(2), 16) + text_rva, m.group(3)))
    syms.sort()
    return syms


def sym_at(syms, rva):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= rva:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    print("== our DLL (deployed) ==")
    db, ds, dsec = pe_sections(DLL_DEP)
    nb, ns, nsec = pe_sections(DLL_NEW)
    for tag, s in (("deployed", dsec), ("new", nsec)):
        print(" ", tag, s)
    text_rva_dep = [s for s in dsec if s[0] == ".text"][0][1]
    text_rva_new = [s for s in nsec if s[0] == ".text"][0][1]
    text_size_dep = [s for s in dsec if s[0] == ".text"][0][2]
    text_size_new = [s for s in nsec if s[0] == ".text"][0][2]
    print("  .text rva dep=%#x size=%#x | new=%#x size=%#x  identical_layout=%s"
          % (text_rva_dep, text_size_dep, text_rva_new, text_size_new,
             (text_rva_dep, text_size_dep) == (text_rva_new, text_size_new)))
    syms = load_map(MAP, text_rva_new)

    data = open(DUMP, "rb").read()
    sig, ver, nstreams, dirrva = struct.unpack_from("<IIII", data, 0)
    assert sig == 0x504D444D, hex(sig)
    streams = {}
    for i in range(nstreams):
        st, sz, rva = struct.unpack_from("<III", data, dirrva + i * 12)
        streams.setdefault(st, (sz, rva))
    print("\n== streams ==", sorted(streams))

    # modules
    mods = []
    if 4 in streams:
        _, rva = streams[4]
        n = struct.unpack_from("<I", data, rva)[0]
        for i in range(n):
            m = rva + 4 + i * 108
            base = struct.unpack_from("<Q", data, m)[0]
            size = struct.unpack_from("<I", data, m + 8)[0]
            namerva = struct.unpack_from("<I", data, m + 20)[0]
            ln = struct.unpack_from("<I", data, namerva)[0]
            name = data[namerva + 4:namerva + 4 + ln].decode("utf-16-le", "replace")
            mods.append((base, size, name))
    print("\n== modules (%d) ==" % len(mods))
    for b, s, n in mods:
        if "EDF5" in n or "dxgi" in n or "d3d" in n:
            print("  %016X +%08X  %s" % (b, s, n))

    def mod_for(addr):
        for b, s, n in mods:
            if b <= addr < b + s:
                return b, s, n
        return None

    # exception
    if 6 in streams:
        _, rva = streams[6]
        tid, _al = struct.unpack_from("<II", data, rva)
        ecode, eflags, erec, eaddr, nparam = struct.unpack_from("<IIQQI", data, rva + 8)
        params = struct.unpack_from("<15Q", data, rva + 36)
        csz, crva = struct.unpack_from("<II", data, rva + 8 + 152)
        rsp = struct.unpack_from("<Q", data, crva + 152)[0]
        rbp = struct.unpack_from("<Q", data, crva + 160)[0]
        rip = struct.unpack_from("<Q", data, crva + 248)[0]
        rax = struct.unpack_from("<Q", data, crva + 120)[0]
        rcx = struct.unpack_from("<Q", data, crva + 128)[0]
        rdx = struct.unpack_from("<Q", data, crva + 136)[0]
        print("\n== exception ==")
        print("  thread id = %#x" % tid)
        print("  code          = %#010x" % ecode)
        print("  flags         = %#x" % eflags)
        print("  exception addr= %#018x" % eaddr)
        print("  nparams=%d params=%s" % (nparam, [hex(p) for p in params[:nparam]]))
        for tag, v in (("rip", rip), ("rsp", rsp), ("rbp", rbp),
                       ("rax", rax), ("rcx", rcx), ("rdx", rdx)):
            mm = mod_for(v)
            extra = ""
            if mm:
                extra = "  <- %s+%#x" % (os.path.basename(mm[2]), v - mm[0])
                if "EDF5EnemyHealthBar" in mm[2]:
                    s = sym_at(syms, v - mm[0])
                    if s:
                        extra += "  [%s+%#x]" % (s[1], v - mm[0] - s[0])
            print("  %s = %#018x%s" % (tag, v, extra))
    else:
        print("no exception stream")
        rsp = None

    # memory ranges
    mem = []
    if 9 in streams:
        _, rva = streams[9]
        nranges, baserva = struct.unpack_from("<QQ", data, rva)
        off = rva + 16
        cur = baserva
        for i in range(nranges):
            start, size = struct.unpack_from("<QQ", data, off)
            mem.append((start, size, cur))
            cur += size
            off += 16
    if 5 in streams:
        _, rva = streams[5]
        n = struct.unpack_from("<I", data, rva)[0]
        for i in range(n):
            m = rva + 4 + i * 16
            start, size, mrva = struct.unpack_from("<QII", data, m)
            mem.append((start, size, mrva))
    print("\n== memory ranges: %d ==" % len(mem))

    def mem_read(addr):
        for start, size, rva in mem:
            if start <= addr < start + size:
                return data[rva + (addr - start):rva + size]
        return None

    if rsp:
        print("\n== stack scan from rsp ==")
        chunk = mem_read(rsp)
        if chunk is None:
            print("  stack memory not present in dump")
        else:
            chunk = chunk[:0x4000]
            seen = 0
            for i in range(0, len(chunk) - 8, 8):
                v = struct.unpack_from("<Q", chunk, i)[0]
                mm = mod_for(v)
                if not mm:
                    continue
                name = os.path.basename(mm[2])
                if "EDF5EnemyHealthBar" not in name and "EDF5.exe" not in name:
                    continue
                rva_off = v - mm[0]
                line = "  [rsp+%#06x] %016X  %s+%#x" % (i, v, name, rva_off)
                if "EDF5EnemyHealthBar" in name:
                    s = sym_at(syms, rva_off)
                    if s:
                        line += "  %s +%#x" % (s[1], rva_off - s[0])
                print(line)
                seen += 1
                if seen > 60:
                    print("  ... (truncated)")
                    break
    # disasm around fault offset
    print("\n== bytes around deployed fault RVA 0x8771 ==")
    for name, vaddr, vsize, rawptr, rawsize in dsec:
        if vaddr <= 0x8771 < vaddr + vsize:
            o = rawptr + (0x8771 - vaddr)
            print("  section %s raw off %#x: %s" % (name, o,
                  open(DLL_DEP, "rb").read()[o - 32:o + 32].hex(" ")))


main()
