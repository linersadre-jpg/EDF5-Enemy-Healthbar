#!/usr/bin/env python3
"""枚举"带血量块"的所有类（含全部派生类），生成插件用的虚表白名单。

原理：
1. 血量块（+0x1F4/+0x1F8/+0x1FC）的构造函数是 `0x2D9A10`；
   它的**直接调用者**就是那 37 个"血量块拥有类"（实体基类/根类，如 HumanBase / InsectBase /
   MonsterBase / VehicleBase / TransporterBase / Deiroi / Nephila ...）。派生类不会直接调基类构造函数，
   所以这 37 个只是"根"。
2. MSVC RTTI 的每个类都有 ClassHierarchyDescriptor → BaseClassArray，
   里面列出该类**全部基类**（第 0 项是它自己）。
   于是：某类"带血量块" ⇔ 它自己或任一基类属于那 37 个。
3. 输出 `src/EHB_damageable.h`：排序 + 去重的虚表 RVA 数组。
   运行时只要在内存里发现虚表命中该表的值，就说明那是一个"可受伤对象"——
   不依赖它是否被注册进冲突对象表。

用法：
  py -3 tools\\edf5_damageable_classes.py            # 生成头文件 + 打印统计
  py -3 tools\\edf5_damageable_classes.py --list     # 只打印类名清单
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from edf5pe import DEFAULT_EXE, load_pe  # noqa: E402
from edf5re import direct_callers, func_of, make_md, parse_pdata  # noqa: E402
from edf5rtti import Rtti  # noqa: E402
from edf5_ctor_classes import CTOR, WINDOW, build_vt_map, lea_targets  # noqa: E402


def u32(pe, rva: int):
    o = pe.rva_to_off(rva)
    if o is None or o + 4 > len(pe.data):
        return None
    return struct.unpack_from("<I", pe.data, o)[0]


def td_name(pe, td_rva: int) -> str:
    o = pe.rva_to_off(td_rva + 0x10)
    if o is None:
        return ""
    raw = pe.data[o:o + 0x100]
    return raw.split(b"\x00")[0].decode("latin1", "replace")


def base_classes(pe, col_rva: int) -> list[str]:
    """COL -> ClassHierarchyDescriptor -> BaseClassArray -> 基类名（第 0 项是自己）"""
    if not col_rva or col_rva < 0:
        return []
    chd = u32(pe, col_rva + 0x10)
    if not chd:
        return []
    n = u32(pe, chd + 8)
    arr = u32(pe, chd + 12)
    if not n or not arr or n > 64:
        return []
    out = []
    for i in range(n):
        bcd = u32(pe, arr + i * 4)
        if not bcd:
            continue
        td = u32(pe, bcd)
        if not td:
            continue
        nm = td_name(pe, td)
        if nm:
            out.append(nm)
    return out


def root_classes(pe) -> dict[str, int]:
    """血量块构造函数 0x2D9A10 的直接调用者 -> 类名（那 37 个根类）"""
    pdata = parse_pdata(pe)
    md = make_md()
    vt2name = build_vt_map(Rtti(pe))
    roots: dict[str, int] = {}
    for site, _kind in direct_callers(pe, CTOR):
        fn = func_of(pdata, site)
        if fn is None:
            continue
        fs, fe, _ = fn
        lo = max(fs, site - WINDOW)
        hi = min(fe, site + WINDOW)
        for _ins, target in lea_targets(md, pe, lo, hi):
            nm = vt2name.get(target)
            if nm:
                short = nm[4:nm.find("@", 4)] if "@" in nm[4:] else nm
                roots[short] = target
                break
    return roots


def main() -> int:
    ap = argparse.ArgumentParser(description="枚举带血量块的类（含派生类）")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out", default=None, help="输出头文件路径")
    ap.add_argument("--list", action="store_true", help="只打印类名")
    args = ap.parse_args()

    pe = load_pe(args.exe)
    rtti = Rtti(pe)

    roots = root_classes(pe)
    print("# 血量块根类: %d 个" % len(roots))
    for nm in sorted(roots):
        print("   %s" % nm)

    damageable: dict[str, list[int]] = {}
    for name, entries in rtti.by_name.items():
        for _td, vtables, col in entries:
            if not vtables or not col or col < 0:
                continue
            bases = base_classes(pe, col)
            if not bases:
                continue
            short_bases = set()
            for b in bases:
                s = b[4:b.find("@", 4)] if "@" in b[4:] else b
                short_bases.add(s)
            if not (short_bases & set(roots)):
                continue
            short = name[4:name.find("@", 4)] if "@" in name[4:] else name
            damageable.setdefault(short, []).extend(vtables)
            break

    total_vt = sum(len(v) for v in damageable.values())
    print("\n# 带血量块的类: %d 个，虚表条目: %d" % (len(damageable), total_vt))
    tr = sorted(k for k in damageable if "Transporter" in k)
    print("# 其中 Transporter 相关: %s" % (", ".join(tr) if tr else "（无）"))

    if args.list:
        for nm in sorted(damageable):
            print("  %-46s vt=%s" % (nm, ", ".join("0x%X" % v for v in sorted(damageable[nm]))))
        return 0

    vts = sorted({v for vs in damageable.values() for v in vs})
    out = Path(args.out) if args.out else Path(__file__).resolve().parent.parent / "src" / "EHB_damageable.h"
    lines = [
        "// 自动生成，请勿手改：由 tools/edf5_damageable_classes.py 产出",
        "// 内容：所有\"带血量块\"(+0x1F4/+0x1F8/+0x1FC) 的类的虚表 RVA（升序、去重）。",
        "// 用途：运行时在私有内存里发现虚表命中下表的值 → 说明是可受伤对象，",
        "//       不依赖它是否被注册进冲突对象表（传送塔这类目标就在表外）。",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace ehb {",
        "constexpr std::uint32_t kDamageableVtables[] = {",
    ]
    for i in range(0, len(vts), 6):
        chunk = ", ".join("0x%X" % v for v in vts[i:i + 6])
        lines.append("    " + chunk + ",")
    lines += [
        "};",
        "constexpr std::uint32_t kDamageableVtableCount = %d;" % len(vts),
        "} // namespace ehb",
        "",
    ]
    out.write_text("\n".join(lines), encoding="utf-8")
    print("# 已写出 %s（%d 个虚表）" % (out, len(vts)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
