"""Find every RIP-relative reference to a target VA in EDF5.exe's .text, marking store vs load.

用法: py -3 tools\\edf5find.py 0x11D37F0 [--stores]
"""
import struct
import sys
from pathlib import Path

import capstone

EXE = Path(r"E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\EDF5.exe")


def sections(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    off = pe + 24 + optsz
    out = []
    for i in range(nsec):
        o = off + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, o + 8)
        out.append((name, base + va, vsize, raw, rawsize))
    return base, out


def main() -> int:
    target = int(sys.argv[1], 0)
    stores_only = "--stores" in sys.argv
    data = EXE.read_bytes()
    base, secs = sections(data)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    hits = 0
    for name, va, vsize, raw, rawsize in secs:
        if name != ".text":
            continue
        blob = data[raw:raw + rawsize]
        for insn in md.disasm(blob, va):
            ops = insn.operands
            if not ops:
                continue
            for k, op in enumerate(ops):
                if op.type != capstone.x86.X86_OP_MEM:
                    continue
                if op.mem.base != capstone.x86.X86_REG_RIP:
                    continue
                tgt = insn.address + insn.size + op.mem.disp
                if tgt != target:
                    continue
                is_store = (k == 0 and insn.mnemonic.split()[0] in ("mov", "and", "or", "add", "sub", "inc", "dec", "xchg"))
                if stores_only and not is_store:
                    continue
                hits += 1
                tag = "STORE" if is_store else "load "
                print(f"{tag} {insn.address - base:#010x}  {insn.mnemonic:8} {insn.op_str}")
    print(f"-- {hits} hit(s) --")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
