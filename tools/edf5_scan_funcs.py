#!/usr/bin/env python3
"""在一批"引用了某个全局/虚表"的函数里，找出真正在遍历 MSVC std::map 的那些。

判定特征（EDF4.1 插件已证实的布局）：
  +0x38 队伍行数组 / +0x18 关系数组 / +0x20 map 节点 key / +0x19 节点 nil 标志

用法：
  py -3 edf5_scan_funcs.py xref 0x125AB80          # 扫描引用该地址的函数
  py -3 edf5_scan_funcs.py func 0x111560           # 只分析一个函数
  py -3 edf5_scan_funcs.py xref 0x125AB80 --dis 0x111560
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from edf5pe import DEFAULT_EXE, PeFile, load_pe  # noqa: E402

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
MD.detail = True


def parse_pdata(pe: PeFile) -> list[tuple[int, int, int]]:
    """返回 [(begin, end, unwind)]，均为 RVA。"""
    sec = next((s for s in pe.sections if s.name == ".pdata"), None)
    if sec is None:
        return []
    out = []
    for off in range(sec.raw_offset, sec.raw_offset + sec.raw_size, 12):
        b, e, u = struct.unpack_from("<III", pe.data, off)
        if b == 0:
            continue
        out.append((b, e, u))
    return out


def find_function(pdata, rva: int) -> tuple[int, int] | None:
    for b, e, _ in pdata:
        if b <= rva < e:
            return b, e
    return None


def rip_targets(pe: PeFile, start: int, end: int) -> tuple[set[int], int]:
    """反汇编一段代码，返回 (RIP 相对引用的目标 RVA 集合, 函数里的一条代表指令)。"""
    off = pe.rva_to_off(start)
    off_end = pe.rva_to_off(end)
    if off is None or off_end is None:
        return set(), start
    code = pe.data[off:off_end]
    targets: set[int] = set()
    for ins in MD.disasm(code, start):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                targets.add(ins.address + ins.size + op.mem.disp)
    return targets, start


def profile(pe: PeFile, start: int, end: int) -> dict[int, list[str]]:
    """函数内对 [reg+disp] 的访问与分支调用，按 disp 归类。"""
    off = pe.rva_to_off(start)
    off_end = pe.rva_to_off(end)
    hits: dict[int, list[str]] = {}
    if off is None or off_end is None:
        return hits
    code = pe.data[off:off_end]
    for ins in MD.disasm(code, start):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base not in (
                capstone.x86.X86_REG_RIP,
                capstone.x86.X86_REG_INVALID,
            ):
                key = op.mem.disp
                hits.setdefault(key, []).append(f"0x{ins.address:08X} {ins.mnemonic} {ins.op_str}")
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("mode", choices=["xref", "func"])
    ap.add_argument("addr")
    ap.add_argument("--dis", default=None, help="把这些偏移的访问指令打印出来(十六进制,逗号分隔)")
    ap.add_argument("--min-keys", type=int, default=3, help="至少命中几个特征偏移才报告")
    args = ap.parse_args()

    pe = load_pe(args.exe)
    pdata = parse_pdata(pe)
    addr = int(args.addr, 0)

    if args.mode == "func":
        span = find_function(pdata, addr)
        if not span:
            print(f"0x{addr:08X} 不在 .pdata 任何函数内")
            return 1
        funcs = [span]
    else:
        seen = set()
        funcs = []
        for b, e, u in pdata:
            tset, _ = rip_targets(pe, b, e)
            if addr in tset and (b, e) not in seen:
                seen.add((b, e))
                funcs.append((b, e))

    feats = [0x38, 0x18, 0x20, 0x19, 0x00]
    print(f"共 {len(funcs)} 个候选函数")
    print(f"{'函数区间':<32} {'大小':>7}  命中特征")
    ranked = []
    for b, e in funcs:
        h = profile(pe, b, e)
        got = [f for f in feats if f in h]
        ranked.append((len(got), b, e, h, got))
    ranked.sort(reverse=True)
    for score, b, e, h, got in ranked:
        if score < args.min_keys:
            continue
        mark = " ".join(f"+0x{f:02X}" for f in got)
        print(f"0x{b:08X}..0x{e:08X} {e - b:>7}  [{score}] {mark}")
    print()
    if args.dis:
        want = {int(x, 0) for x in args.dis.split(",")}
        for score, b, e, h, got in ranked:
            if score < args.min_keys:
                continue
            hit_lines = [ln for d in want if d in h for ln in h[d]]
            if not hit_lines:
                continue
            print(f"--- 0x{b:08X}..0x{e:08X} ---")
            for ln in sorted(hit_lines, key=lambda s: s.split()[0]):
                print("   ", ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
