"""Parse EDF5.exe's import directory and print IAT slot addresses (RVA) per imported name.

用法: py -3 tools\\edf5imports.py [name-filter]
"""
import struct
import sys
from pathlib import Path

EXE = Path(r"E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\EDF5.exe")


def main() -> int:
    filt = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    data = EXE.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    assert magic == 0x20B, hex(magic)
    base = struct.unpack_from("<Q", data, opt + 24)[0]
    # data directory #1 = import table
    dd = opt + 112
    imp_rva, imp_size = struct.unpack_from("<II", data, dd + 8)

    def rva2off(rva: int) -> int:
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        sec = pe + 24 + optsz
        for i in range(nsec):
            o = sec + i * 40
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, o + 8)
            if va <= rva < va + max(vsize, rawsize):
                return raw + (rva - va)
        raise ValueError(hex(rva))

    def cstr(off: int) -> str:
        end = data.index(b"\0", off)
        return data[off:end].decode("latin1")

    out = []
    off = rva2off(imp_rva)
    while True:
        oft, timestamp, forwarder, name_rva, first_thunk = struct.unpack_from("<IIIII", data, off)
        if oft == 0 and name_rva == 0 and first_thunk == 0:
            break
        dll = cstr(rva2off(name_rva))
        thunk = oft or first_thunk
        toff = rva2off(thunk)
        idx = 0
        while True:
            val = struct.unpack_from("<Q", data, toff + idx * 8)[0]
            if val == 0:
                break
            if not (val & (1 << 63)):
                nm = cstr(rva2off(val & 0xFFFFFFFF) + 2)
                if not filt or filt in nm.lower():
                    out.append((nm, dll, first_thunk + idx * 8))
            idx += 1
        off += 20
    for nm, dll, rva in out:
        print(f"{rva:#010x}  {nm:32} {dll}")
    print(f"-- {len(out)} entr(ies) --")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
