"""把「谁调用了伤害目标对象的构造函数 0x2D9A10」映射到 RTTI 类名。

思路：MSVC 派生类构造函数会先 call 基类构造，随后 lea rax,[自己的虚表]; mov [this],rax。
因此对每个调用点所在函数，扫描 call 前后一段指令里指向已知虚表的 lea，即可得到该实体的类名。
用途：确认哪些类拥有 [+0x1F8]/[+0x1FC]（最大/当前总血量），以及是否只有实体根对象才有。
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))

from edf5pe import DEFAULT_EXE, load_pe  # noqa: E402
from edf5re import direct_callers, func_of, make_md, parse_pdata  # noqa: E402
from edf5rtti import Rtti  # noqa: E402

CTOR = 0x2D9A10
WINDOW = 0x300


def build_vt_map(rtti: Rtti) -> dict[int, str]:
    vt2name: dict[int, str] = {}
    for name, entries in rtti.by_name.items():
        for _td, vtables, _col in entries:
            for vt in vtables:
                vt2name.setdefault(vt, name)
    return vt2name


def lea_targets(md, pe, start_rva: int, end_rva: int):
    """返回 [(指令 RVA, 目标 RVA)]，只看 rip 相对的 lea。"""
    off = pe.rva_to_off(start_rva)
    if off is None:
        return []
    end_off = pe.rva_to_off(end_rva) or (off + WINDOW)
    code = pe.data[off:end_off]
    out = []
    for insn in md.disasm(code, pe.image_base + start_rva):
        if insn.mnemonic.startswith("lea") and len(insn.operands) == 2:
            mem = insn.operands[1]
            if mem.type == capstone.x86.X86_OP_MEM and mem.mem.base == capstone.x86.X86_REG_RIP:
                target = insn.address + insn.size + mem.mem.disp - pe.image_base
                out.append((insn.address - pe.image_base, target))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="构造函数调用点 -> RTTI 类名")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--ctor", default=hex(CTOR))
    ap.add_argument("--filter", default="", help="只显示类名包含该子串的项")
    args = ap.parse_args()

    ctor = int(args.ctor, 0)
    pe = load_pe(args.exe)
    md = make_md()
    pdata = parse_pdata(pe)
    vt2name = build_vt_map(Rtti(pe))
    print(f"# 虚表条目 {len(vt2name)} 个；构造函数 EDF5.exe+0x{ctor:X}")

    callers = direct_callers(pe, ctor)
    seen: dict[str, int] = {}
    rows: list[tuple[int, str, int]] = []
    unresolved = 0
    for site, _kind in callers:
        fn = func_of(pdata, site)
        if fn is None:
            unresolved += 1
            continue
        fs, fe, _u = fn
        lo = max(fs, site - WINDOW)
        hi = min(fe, site + WINDOW)
        hit = ""
        for ins_rva, target in lea_targets(md, pe, lo, hi):
            name = vt2name.get(target)
            if name:
                hit = name
                break
        if not hit:
            unresolved += 1
            continue
        rows.append((site, hit, fs))
        seen[hit] = seen.get(hit, 0) + 1

    print(f"# 调用点 {len(callers)} 个，识别出类名 {len(rows)} 个，未识别 {unresolved} 个\n")
    for site, name, fs in sorted(rows, key=lambda r: r[1]):
        if args.filter and args.filter.lower() not in name.lower():
            continue
        print(f"  调用点 EDF5.exe+0x{site:07X}  (函数 0x{fs:07X})  ->  {name}")

    print("\n# 汇总（类名 -> 调用点数量）")
    for name in sorted(seen):
        if args.filter and args.filter.lower() not in name.lower():
            continue
        print(f"  {seen[name]:3d}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
