#!/usr/bin/env python3
"""源码字符串字面量闸门（编译前跑，秒级）。

抓两类"看起来没问题、编译却报诡异错误"的情况：

1. **字符串里混进了 ASCII 双引号**。典型写法是把中文注释整体塞进 C++ 字面量：
   `"; 排查"这个 MOD 吃了多少性能"时用\r\n"` —— 这个字面量在第 2 个 `"` 就结束了，
   后面的中文被当成 token，MSVC 报 `C3688 文本后缀"…"无效` 等一串看不懂的错误。
   判据：以 `"` 开头的整行字面量，`"` 的个数应当恰好为 2；> 2 就是可疑（本项目实测踩过）。

2. **源码编码**。MSVC 在无 `/utf-8` 且无 BOM 时会按系统 ANSI 读源码，中文注释/字面量会坏。
   判据：文件开头必须有 UTF-8 BOM（\xEF\xBB\xBF），或构建脚本里显式带 `/utf-8`。

用法：
  py -3 tools\\check_literals.py            # 检查 src\\*.cpp src\\*.h
  py -3 tools\\check_literals.py --verbose  # 额外列出每行的引号计数
退出码：0 = 通过；1 = 发现问题（构建脚本可据此提前中止）。
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"


def check_file(p: Path, verbose: bool) -> list[str]:
    raw = p.read_bytes()
    problems: list[str] = []
    has_bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig", errors="replace")
    build = (ROOT / "scripts" / "build3.cmd")
    utf8_flag = False
    if build.exists():
        try:
            utf8_flag = "/utf-8" in build.read_text(encoding="utf-8", errors="replace")
        except OSError:
            utf8_flag = False
    if not has_bom and not utf8_flag:
        problems.append(f"{p.name}: 既无 UTF-8 BOM，构建脚本里也没有 /utf-8 —— MSVC 会按 GBK 读中文")

    for i, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s.startswith('"'):
            continue
        # 只数未转义的双引号
        quotes = len(re.findall(r'(?<!\\)"', line))
        if quotes <= 2:
            continue
        # 逗号分隔的多个短字面量（如 `"Gauge", "Bar", ...`）是合法写法
        if re.search(r'",\s*"', line):
            continue
        problems.append(
            f"{p.name}:{i}: 字符串字面量里出现 {quotes} 个双引号（应为 2）—— 极可能把 ASCII 引号写进了字面量\n"
            f"      {s[:120]}"
        )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description="源码字符串字面量闸门")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    files = sorted(list(SRC.glob("*.cpp")) + list(SRC.glob("*.h")))
    if not files:
        print(f"没找到源文件（{SRC}）")
        return 1

    all_problems: list[str] = []
    for p in files:
        all_problems += check_file(p, args.verbose)

    if all_problems:
        print("字符串字面量闸门：发现问题")
        for x in all_problems:
            print("  " + x)
        return 1
    print(f"字符串字面量闸门：通过（{len(files)} 个文件）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
