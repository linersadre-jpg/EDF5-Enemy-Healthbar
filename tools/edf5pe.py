#!/usr/bin/env python3
"""EDF5 PE 逆向小工具：解析 PE、SHA256 指纹、特征码(AOB)扫描、偏移/RVA 互转。

用法：
  py -3 edf5pe.py info
  py -3 edf5pe.py sig "F3 0F 58 87 FC 01 00 00"
  py -3 edf5pe.py sig "F3 0F 58 87 ?? ?? ?? ??" --section .text
  py -3 edf5pe.py rva 0x1FC          # 报告某 RVA 落在哪个节
  py -3 edf5pe.py bytes <rva> <n>    # 打印该 RVA 处的原始字节

默认目标为 EDF5.exe；用 --exe 指定别的模块。
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

DEFAULT_EXE = r"E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\EDF5.exe"


@dataclass
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int

    @property
    def virt_end(self) -> int:
        return self.virtual_address + self.virtual_size

    @property
    def raw_end(self) -> int:
        return self.raw_offset + self.raw_size


@dataclass
class PeFile:
    path: Path
    data: bytes
    machine: int
    timestamp: int
    image_base: int
    sections: list[Section]
    sha256: str

    @property
    def is_64bit(self) -> bool:
        return self.machine == 0x8664

    def rva_to_off(self, rva: int) -> int | None:
        for s in self.sections:
            if s.virtual_address <= rva < s.virt_end:
                delta = rva - s.virtual_address
                if delta < s.raw_size:
                    return s.raw_offset + delta
                return None
        return None

    def off_to_rva(self, off: int) -> int | None:
        for s in self.sections:
            if s.raw_offset <= off < s.raw_end:
                return s.virtual_address + (off - s.raw_offset)
        return None

    def section_of_rva(self, rva: int) -> Section | None:
        for s in self.sections:
            if s.virtual_address <= rva < s.virt_end:
                return s
        return None


def load_pe(path: str | Path) -> PeFile:
    p = Path(path)
    data = p.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{p} 不是 PE 文件")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off : pe_off + 4] != b"PE\0\0":
        raise ValueError(f"{p} 的 PE 签名无效")
    machine, nsec = struct.unpack_from("<HH", data, pe_off + 4)
    timestamp = struct.unpack_from("<I", data, pe_off + 8)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic == 0x20B:  # PE32+
        image_base = struct.unpack_from("<Q", data, opt_off + 24)[0]
    elif magic == 0x10B:
        image_base = struct.unpack_from("<I", data, opt_off + 28)[0]
    else:
        raise ValueError(f"未知的可选头 magic 0x{magic:X}")
    sec_off = opt_off + opt_size
    sections = []
    for i in range(nsec):
        base = sec_off + i * 40
        name = data[base : base + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, roff = struct.unpack_from("<IIII", data, base + 8)
        sections.append(Section(name, vaddr, vsize, roff, rsize))
    return PeFile(
        path=p,
        data=data,
        machine=machine,
        timestamp=timestamp,
        image_base=image_base,
        sections=sections,
        sha256=hashlib.sha256(data).hexdigest().upper(),
    )


def parse_pattern(text: str) -> tuple[bytes, bytes]:
    """把 'F3 0F 58 ?? FF' 解析成 (值, 掩码)。"""
    toks = text.replace(",", " ").split()
    values = bytearray()
    mask = bytearray()
    for t in toks:
        if t in ("?", "??"):
            values.append(0)
            mask.append(0)
        else:
            if len(t) != 2:
                raise ValueError(f"无法解析的字节 '{t}'")
            values.append(int(t, 16))
            mask.append(0xFF)
    return bytes(values), bytes(mask)


def scan(pe: PeFile, pattern: str, sections: list[str] | None = None, limit: int = 50) -> list[int]:
    values, mask = parse_pattern(pattern)
    n = len(values)
    hits: list[int] = []
    for s in pe.sections:
        if sections and s.name not in sections:
            continue
        blob = pe.data[s.raw_offset : s.raw_offset + s.raw_size]
        # 逐字节扫描：先找首字节，再整体比对掩码
        first = values[0]
        pos = blob.find(bytes([first]))
        while pos != -1 and pos + n <= len(blob):
            ok = True
            for i in range(1, n):
                if mask[i] and blob[pos + i] != values[i]:
                    ok = False
                    break
            if ok:
                hits.append(s.virtual_address + pos)
                if len(hits) >= limit:
                    return hits
            pos = blob.find(bytes([first]), pos + 1)
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description="EDF5 PE 逆向小工具")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info", help="打印 PE 头部信息与 SHA256")

    sp = sub.add_parser("sig", help="特征码扫描")
    sp.add_argument("pattern")
    sp.add_argument("--section", action="append", default=None)
    sp.add_argument("--limit", type=int, default=50)

    rp = sub.add_parser("rva", help="报告 RVA 所在节")
    rp.add_argument("rva")

    bp = sub.add_parser("bytes", help="打印 RVA 处原始字节")
    bp.add_argument("rva")
    bp.add_argument("n", type=int, default=16)

    args = ap.parse_args()
    pe = load_pe(args.exe)

    if args.cmd == "info":
        print(f"文件      : {pe.path}")
        print(f"大小      : {len(pe.data):,} 字节")
        print(f"SHA256    : {pe.sha256}")
        print(f"Machine   : 0x{pe.machine:04X} ({'x64' if pe.is_64bit else 'x86'})")
        print(f"TimeStamp : {pe.timestamp} (0x{pe.timestamp:X})")
        print(f"ImageBase : 0x{pe.image_base:X}")
        print("节表:")
        for s in pe.sections:
            print(
                f"  {s.name:<8} RVA 0x{s.virtual_address:08X} virt 0x{s.virtual_size:08X} "
                f"raw 0x{s.raw_offset:08X} rawsize 0x{s.raw_size:08X}"
            )
        return 0

    if args.cmd == "sig":
        hits = scan(pe, args.pattern, args.section, args.limit)
        print(f"模式 {args.pattern} -> {len(hits)} 处命中")
        for rva in hits:
            sec = pe.section_of_rva(rva)
            off = pe.rva_to_off(rva)
            print(f"  EDF5.exe+0x{rva:08X}  (文件偏移 0x{off:08X}, 节 {sec.name if sec else '?'})")
        return 0 if hits else 1

    if args.cmd == "rva":
        rva = int(args.rva, 0)
        sec = pe.section_of_rva(rva)
        off = pe.rva_to_off(rva)
        if sec is None:
            print(f"0x{rva:08X} 不在任何节内")
            return 1
        print(f"0x{rva:08X} -> 节 {sec.name}, 文件偏移 {'0x%08X' % off if off is not None else '无(未初始化数据)'}")
        return 0

    if args.cmd == "bytes":
        rva = int(args.rva, 0)
        off = pe.rva_to_off(rva)
        if off is None:
            print(f"0x{rva:08X} 无文件数据")
            return 1
        raw = pe.data[off : off + args.n]
        print(f"EDF5.exe+0x{rva:08X}: " + " ".join(f"{b:02X}" for b in raw))
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
