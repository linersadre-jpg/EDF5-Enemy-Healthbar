#!/usr/bin/env python3
"""EDF5 逆向工具：函数边界(.pdata)、反汇编、交叉引用(call/jmp/RIP-相对)。

用法示例：
  py -3 edf5re.py func 0x2DB641            # 该 RVA 所在函数(边界来自 .pdata) + 反汇编
  py -3 edf5re.py dis 0x2DB641 --n 40
  py -3 edf5re.py callers 0x2DB641         # 谁 call/jmp 到它（只扫 E8/E9，很快）
  py -3 edf5re.py xref 0x125AB68           # 谁引用这个全局(RIP-相对 + 直接)
  py -3 edf5re.py str "damage"             # 搜索 ASCII / UTF-16 字符串
  py -3 edf5re.py sweep                    # 建立全 .text 交叉引用缓存(较慢, 之后复用)

缓存：tools/.cache/edf5re_xref.json
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from edf5pe import DEFAULT_EXE, PeFile, load_pe  # noqa: E402

try:
    import capstone
except ImportError:  # pragma: no cover
    print("需要 capstone：py -3 -m pip install capstone", file=sys.stderr)
    raise

CACHE = Path(__file__).resolve().parent / ".cache" / "edf5re_xref.json"


# ---------------------------------------------------------------- .pdata
def parse_pdata(pe: PeFile) -> list[tuple[int, int, int]]:
    sec = next((s for s in pe.sections if s.name == ".pdata"), None)
    if sec is None:
        return []
    blob = pe.data[sec.raw_offset : sec.raw_offset + sec.raw_size]
    out = []
    for off in range(0, len(blob) - 11, 12):
        begin, end, unwind = struct.unpack_from("<III", blob, off)
        if begin == 0 and end == 0:
            continue
        out.append((begin, end, unwind))
    out.sort()
    return out


def func_of(pdata: list[tuple[int, int, int]], rva: int):
    """二分查找包含 rva 的函数；返回 (begin,end,unwind) 或 None。"""
    lo, hi = 0, len(pdata) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        b, e, u = pdata[mid]
        if rva < b:
            hi = mid - 1
        elif rva >= e:
            lo = mid + 1
        else:
            return (b, e, u)
    return None


# ---------------------------------------------------------------- 反汇编
def make_md() -> "capstone.Cs":
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    return md


def disasm_range(pe: PeFile, rva: int, n_insn: int | None = None, end_rva: int | None = None):
    """线性反汇编。返回 [(rva, bytes, mnemonic, op_str, insn)]。"""
    md = make_md()
    off = pe.rva_to_off(rva)
    if off is None:
        return []
    if end_rva is not None:
        eoff = pe.rva_to_off(end_rva - 1)
        blob = pe.data[off : (eoff + 1) if eoff else off + 4096]
    else:
        blob = pe.data[off : off + 4096]
    out = []
    for insn in md.disasm(blob, rva):
        out.append((insn.address, insn.bytes, insn.mnemonic, insn.op_str, insn))
        if n_insn is not None and len(out) >= n_insn:
            break
        if end_rva is not None and insn.address >= end_rva:
            break
    return out


def fmt(pe: PeFile, rows) -> str:
    lines = []
    for rva, _b, mn, ops, _i in rows:
        lines.append(f"  EDF5.exe+0x{rva:08X}  {mn:<8} {ops}")
    return "\n".join(lines)


# ---------------------------------------------------------------- 交叉引用
def insn_is_call_or_jmp(insn) -> bool:
    return insn.mnemonic in ("call", "jmp") and insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM


def sweep_xrefs_fast(pe: PeFile, rebuild: bool = False) -> dict:
    """线性扫描 .text，收集指向代码地址的 call/jmp 与 RIP-相对数据引用。

    逐条解码，遇到非法字节则前进 1 字节重同步。
    返回 {"code": {target_hex: [[site, kind], ...]}, "data": {...}, "meta": {...}}
    """
    if CACHE.exists() and not rebuild:
        cached = json.loads(CACHE.read_text(encoding="utf-8"))
        if cached.get("meta", {}).get("sha256") == pe.sha256:
            return cached
    sec = next((s for s in pe.sections if s.name == ".text"), None)
    code = pe.data[sec.raw_offset : sec.raw_offset + sec.raw_size]
    base = sec.virtual_address
    md = make_md()
    code_refs: dict[str, list] = {}
    data_refs: dict[str, list] = {}
    t0 = time.time()
    insn_count = 0
    pos = 0
    total = len(code)
    while pos < total:
        gen = md.disasm(code[pos:], base + pos)
        got = 0
        for insn in gen:
            got += insn.size
            insn_count += 1
            if insn_is_call_or_jmp(insn):
                tgt = insn.operands[0].imm
                code_refs.setdefault(f"{tgt & 0xFFFFFFFFFFFFFFFF:x}", []).append([insn.address, insn.mnemonic])
            for op in insn.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    tgt = insn.address + insn.size + op.mem.disp
                    data_refs.setdefault(f"{tgt & 0xFFFFFFFFFFFFFFFF:x}", []).append(
                        [insn.address, insn.mnemonic]
                    )
        pos += got if got else 1
    print(f"[sweep] {insn_count:,} 条指令, {time.time() - t0:.1f}s", file=sys.stderr)
    result = {
        "meta": {"sha256": pe.sha256, "text_rva": base, "text_size": len(code), "insn": insn_count},
        "code": code_refs,
        "data": data_refs,
    }
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    CACHE.write_text(json.dumps(result), encoding="utf-8")
    return result


def direct_callers(pe: PeFile, target: int) -> list[tuple[int, str]]:
    """只扫 E8/E9 rel32，极快，用于找直接调用者。"""
    sec = next(s for s in pe.sections if s.name == ".text")
    code = pe.data[sec.raw_offset : sec.raw_offset + sec.raw_size]
    base = sec.virtual_address
    hits = []
    for i in range(len(code) - 4):
        if code[i] in (0xE8, 0xE9):
            rel = struct.unpack_from("<i", code, i + 1)[0]
            if base + i + 5 + rel == target:
                hits.append((base + i, "call" if code[i] == 0xE8 else "jmp"))
    return hits


# ---------------------------------------------------------------- 字符串
def find_strings(pe: PeFile, needle: str, utf16: bool = False, limit: int = 40):
    enc = "utf-16-le" if utf16 else "ascii"
    bs = needle.encode(enc)
    hits = []
    start = 0
    while len(hits) < limit:
        idx = pe.data.find(bs, start)
        if idx == -1:
            break
        rva = pe.off_to_rva(idx)
        sec = pe.section_of_rva(rva) if rva is not None else None
        hits.append((rva, sec.name if sec else "?", idx))
        start = idx + 1
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description="EDF5 逆向工具")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    sub = ap.add_subparsers(dest="cmd", required=True)

    f = sub.add_parser("func", help="函数边界 + 反汇编")
    f.add_argument("rva")
    f.add_argument("--n", type=int, default=0, help="只显示前 n 条")

    d = sub.add_parser("dis", help="反汇编")
    d.add_argument("rva")
    d.add_argument("--n", type=int, default=40)

    c = sub.add_parser("callers", help="直接调用者(E8/E9)")
    c.add_argument("rva")

    x = sub.add_parser("xref", help="交叉引用(需先 sweep)")
    x.add_argument("rva")
    x.add_argument("--rebuild", action="store_true")

    s = sub.add_parser("str", help="搜索字符串")
    s.add_argument("text")
    s.add_argument("--utf16", action="store_true")

    sub.add_parser("sweep", help="建立交叉引用缓存")
    sw = sub.add_parser("swp", help="建立交叉引用缓存")
    sw.add_argument("--rebuild", action="store_true")

    sub.add_parser("sections", help="打印节表")

    args = ap.parse_args()
    pe = load_pe(args.exe)

    if args.cmd == "sections":
        for s in pe.sections:
            print(f"{s.name:<8} RVA 0x{s.virtual_address:08X} virt 0x{s.virtual_size:08X} raw 0x{s.raw_offset:08X}")
        return 0

    if args.cmd in ("sweep", "swp"):
        res = sweep_xrefs_fast(pe, rebuild=getattr(args, "rebuild", False) or args.cmd == "sweep")
        print(f"code xref 目标 {len(res['code']):,} 个, data xref 目标 {len(res['data']):,} 个")
        return 0

    if args.cmd == "func":
        rva = int(args.rva, 0)
        pd = parse_pdata(pe)
        fn = func_of(pd, rva)
        print(f".pdata 共 {len(pd):,} 个函数")
        if fn:
            b, e, u = fn
            print(f"包含 0x{rva:X} 的函数: EDF5.exe+0x{b:08X} .. 0x{e:08X}  (大小 {e - b} 字节, unwind 0x{u:X})")
            rows = disasm_range(pe, b, n_insn=args.n or None, end_rva=e)
        else:
            print(f"0x{rva:X} 不在任何 .pdata 函数内")
            rows = disasm_range(pe, rva, n_insn=args.n or 40)
        print(fmt(pe, rows))
        return 0

    if args.cmd == "dis":
        rows = disasm_range(pe, int(args.rva, 0), n_insn=args.n)
        print(fmt(pe, rows))
        return 0

    if args.cmd == "callers":
        tgt = int(args.rva, 0)
        hits = direct_callers(pe, tgt)
        print(f"0x{tgt:X} 的直接调用者: {len(hits)} 个")
        for site, kind in hits:
            fn = func_of(parse_pdata(pe), site)
            where = f"函数 0x{fn[0]:08X}..0x{fn[1]:08X}" if fn else "?"
            print(f"  {kind} @ EDF5.exe+0x{site:08X}   ({where})")
        return 0

    if args.cmd == "xref":
        tgt = int(args.rva, 0)
        res = sweep_xrefs_fast(pe, rebuild=args.rebuild)
        key = f"{tgt:x}"
        print(f"== 代码引用 0x{tgt:X} ==")
        for site, kind in res["code"].get(key, []):
            fn = func_of(parse_pdata(pe), site)
            where = f"函数 0x{fn[0]:08X}..0x{fn[1]:08X}" if fn else "?"
            print(f"  {kind:<4} @ EDF5.exe+0x{site:08X}   ({where})")
        print(f"== 数据引用(RIP-相对) 0x{tgt:X} ==")
        for site, kind in res["data"].get(key, []):
            fn = func_of(parse_pdata(pe), site)
            where = f"函数 0x{fn[0]:08X}..0x{fn[1]:08X}" if fn else "?"
            print(f"  {kind:<4} @ EDF5.exe+0x{site:08X}   ({where})")
        return 0

    if args.cmd == "str":
        hits = find_strings(pe, args.text, utf16=args.utf16)
        print(f"'{args.text}' 命中 {len(hits)} 处")
        for rva, secname, off in hits:
            print(f"  RVA 0x{rva:08X} 节 {secname} (文件 0x{off:X})")
        return 0 if hits else 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
