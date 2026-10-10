#!/usr/bin/env python3
"""按“结构体字段偏移”搜索 EDF5.exe 的访问点。

对 .text 做一次线性反汇编（按 .pdata 的函数边界归组），找出所有以指定位移访问内存的
指令，例如 `movss xmm0,[rdi+0x1FC]`。用来回答这类问题：

  * 谁在读敌人血量 +0x1FC / 最大血量 +0x1F8？→ 原版血条的绘制函数就在里面
  * 谁在读队伍 +0x218？
  * 谁在读位置 +0x90 / +0xA4？

  py -3 tools/edf5_fields.py                 # 用默认字段集
  py -3 tools/edf5_fields.py --field 0x1F8 --field 0x1FC --min 2
"""
from __future__ import annotations

import argparse
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from edf5pe import PeFile, load_pe, DEFAULT_EXE  # noqa: E402

import capstone  # noqa: E402

DEFAULT_FIELDS = [0x1F8, 0x1FC, 0x1F4, 0x218, 0x90, 0xA4, 0x268]


def parse_pdata(pe: PeFile) -> list[tuple[int, int]]:
    """(函数起点 RVA, 函数终点 RVA)，来自 x64 的 .pdata 异常表。"""
    out: list[tuple[int, int]] = []
    for s in pe.sections:
        if s.name != ".pdata":
            continue
        blob = pe.data[s.raw_offset : s.raw_offset + s.raw_size]
        for i in range(0, len(blob) - 11, 12):
            begin, end, _unwind = struct.unpack_from("<III", blob, i)
            if 0 < begin < end < 0x14000000:
                out.append((begin, end))
    out.sort()
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="按结构体字段偏移搜索访问点")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--field", action="append", default=None, help="形如 0x1FC，可重复")
    ap.add_argument("--min", type=int, default=2, help="一个函数至少命中几个字段才报告")
    ap.add_argument("--out", default=None, help="把完整结果写到文件")
    args = ap.parse_args()

    fields = [int(x, 0) for x in args.field] if args.field else DEFAULT_FIELDS
    wanted = set(fields)

    pe = load_pe(args.exe)
    text = next(s for s in pe.sections if s.name == ".text")
    code = pe.data[text.raw_offset : text.raw_offset + text.raw_size]
    text_va = text.virtual_address
    text_end = text_va + text.raw_size
    pdata = [r for r in parse_pdata(pe) if text_va <= r[0] < text_end]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    hits: dict[int, list[str]] = collections.defaultdict(list)
    fieldcount: dict[int, set[int]] = collections.defaultdict(set)
    total = 0
    skipped = 0

    for fstart, fend in pdata:
        fend = min(fend, text_end)
        chunk = code[fstart - text_va : fend - text_va]
        for insn in md.disasm(chunk, 0x140000000 + fstart):
            total += 1
            for op in insn.operands:
                if op.type != capstone.x86.X86_OP_MEM:
                    continue
                d = op.mem.disp
                if d not in wanted:
                    continue
                if op.mem.base == 0:
                    continue
                rva = insn.address - 0x140000000
                hits[fstart].append(
                    f"    0x{rva:08X}  {insn.mnemonic:<8}{insn.op_str}"
                )
                fieldcount[fstart].add(d)
                break
        # 连续的反汇编遇到坏字节就停，逐函数独立，不影响别的函数
        if not chunk:
            skipped += 1

    rows = [(f, sorted(fieldcount[f]), hits[f]) for f in hits if len(fieldcount[f]) >= args.min]
    rows.sort(key=lambda r: (-len(r[1]), r[0]))

    lines = [
        f"字段集 {[hex(x) for x in fields]}  扫描 {len(pdata)} 个函数 / {total:,} 条指令",
        f"命中 {len(hits)} 个函数，其中 >= {args.min} 个字段的有 {len(rows)} 个",
        "",
    ]
    for f, fset, ins in rows:
        lines.append(f"函数 0x{f:08X}  命中字段 {[hex(x) for x in fset]}  ({len(ins)} 条)")
        lines.extend(ins)
        lines.append("")

    report = "\n".join(lines)
    print("\n".join(lines[:200]))
    if args.out:
        Path(args.out).write_text(report, encoding="utf-8")
        print(f"[完整结果写入 {args.out}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
