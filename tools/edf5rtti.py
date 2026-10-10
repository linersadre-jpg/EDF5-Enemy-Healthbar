#!/usr/bin/env python3
"""EDF5 RTTI 类名/虚表提取器。

EDF5.exe 保留了 MSVC RTTI（.data 里有 .?AV...@@ 类型名），可以直接把类名、
RTTICompleteObjectLocator 和虚表地址对上，用类名定位游戏内部函数——
EDF6 工程就是用 HUiHudRader 这类名字找到雷达和 HUD 的。

用法：
  py -3 edf5rtti.py list                 # 列出所有类名
  py -3 edf5rtti.py grep Hud             # 按关键词过滤类名
  py -3 edf5rtti.py class HUiHudRader    # 看某个类的虚表与虚函数地址
  py -3 edf5rtti.py dump > classes.txt
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from edf5pe import DEFAULT_EXE, PeFile, load_pe  # noqa: E402

NAME_RE = re.compile(rb"\.\?A[VU][A-Za-z0-9_@?$<>:\-]*?@@")


class Rtti:
    def __init__(self, pe: PeFile):
        self.pe = pe
        self.by_name: dict[str, list[tuple[int, list[int], int]]] = {}
        self._scan()

    def _scan(self) -> None:
        pe = self.pe
        data = pe.data
        names: list[tuple[int, str]] = []  # (type_descriptor_rva, name)
        for m in NAME_RE.finditer(data):
            str_rva = pe.off_to_rva(m.start())
            if str_rva is None:
                continue
            td_rva = str_rva - 0x10
            name = m.group().decode("ascii", "replace")
            names.append((td_rva, name))

        # COL: 24 字节 {1, offset, cdOffset, pTypeDescriptor, pClassDescriptor, pSelf}
        cols: dict[int, int] = {}  # td_rva -> col_rva
        sec = next((s for s in pe.sections if s.name == ".rdata"), None)
        targets = {td: name for td, name in names}
        if sec is not None:
            blob = pe.data[sec.raw_offset : sec.raw_offset + sec.raw_size]
            for off in range(0, len(blob) - 23, 4):
                if struct.unpack_from("<I", blob, off)[0] != 1:
                    continue
                td = struct.unpack_from("<I", blob, off + 12)[0]
                col = struct.unpack_from("<I", blob, off + 20)[0]
                if td in targets and col == sec.virtual_address + off:
                    cols.setdefault(td, sec.virtual_address + off)

        # 虚表: .rdata 中指向 COL 的指针，其后紧跟虚函数表
        vtables: dict[int, list[int]] = {}  # col_rva -> [vtable_rva, ...]
        if sec is not None:
            blob = pe.data[sec.raw_offset : sec.raw_offset + sec.raw_size]
            base = sec.virtual_address
            col_set = {v: k for k, v in cols.items()}
            for off in range(0, len(blob) - 15, 8):
                val = struct.unpack_from("<Q", blob, off)[0]
                col_rva = val - pe.image_base
                if col_rva in col_set:
                    vtables.setdefault(col_rva, []).append(base + off + 8)

        for td_rva, name in names:
            col_rva = cols.get(td_rva, -1)
            vt = vtables.get(col_rva, [])
            self.by_name.setdefault(name, []).append((td_rva, vt, col_rva))

    def vtable_funcs(self, vtable_rva: int, count: int = 12) -> list[tuple[int, int]]:
        """返回虚表里的 (槽号, 函数 RVA)，跳过空槽。"""
        off = self.pe.rva_to_off(vtable_rva)
        if off is None:
            return []
        out = []
        for i in range(count):
            val = struct.unpack_from("<Q", self.pe.data, off + i * 8)[0]
            if val == 0:
                out.append((i, 0))
                continue
            out.append((i, val - self.pe.image_base))
        return out


def main() -> int:
    ap = argparse.ArgumentParser(description="EDF5 RTTI 提取")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("dump")
    g = sub.add_parser("grep")
    g.add_argument("keyword")
    g.add_argument("--limit", type=int, default=200)
    c = sub.add_parser("class")
    c.add_argument("name")
    c.add_argument("--slots", type=int, default=16)

    args = ap.parse_args()
    pe = load_pe(args.exe)
    r = Rtti(pe)

    if args.cmd in ("list", "dump"):
        print(f"共 {len(r.by_name):,} 个类名")
        for name in sorted(r.by_name):
            tds = r.by_name[name]
            vt = tds[0][1]
            vts = " ".join(f"0x{v:X}" for v in vt) if vt else "-"
            print(f"{name:<60} td=0x{tds[0][0]:08X} col=0x{tds[0][2]:08X} vtable={vts}")
        return 0

    if args.cmd == "grep":
        kw = args.keyword.lower()
        hits = sorted(n for n in r.by_name if kw in n.lower())
        print(f"含 '{args.keyword}' 的类名: {len(hits)} 个")
        for name in hits[: args.limit]:
            tds = r.by_name[name]
            vts = tds[0][1]
            vts_s = " ".join(f"0x{v:X}" for v in vts) if vts else "-"
            print(f"  {name:<58} vtable={vts_s}")
        return 0

    if args.cmd == "class":
        tds = r.by_name.get(args.name)
        if not tds:
            print(f"未找到类 {args.name}")
            return 1
        for td, vts, col in tds:
            print(f"类 {args.name}: td=0x{td:08X} col=0x{col:08X}")
            for v in vts:
                print(f"  vtable 0x{v:X}:")
                for slot, fn in r.vtable_funcs(v, args.slots):
                    print(f"    [{slot:>2}] {'空' if fn == 0 else f'EDF5.exe+0x{fn:08X}'}")
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
