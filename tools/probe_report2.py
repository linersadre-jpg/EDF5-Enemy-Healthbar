#!/usr/bin/env python3
"""第二轮探针日志分析：位置偏移投票 / 位置候选扫描 / 类名 / 相机原始数据。"""
from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

LOG = Path(sys.argv[1] if len(sys.argv) > 1 else
           r"E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\Mods\Plugins\EDF5_Probe.log")
raw = LOG.read_bytes()
for enc in ("utf-8", "gbk", "cp936"):
    try:
        text = raw.decode(enc)
    except UnicodeDecodeError:
        text = raw.decode(enc, errors="replace")
    if "位置偏移投票" in text:
        print(f"编码 = {enc}")
        break
else:
    text = raw.decode("utf-8", errors="replace")
    print("警告：两种编码都没找到标记")
lines = text.split("\n")
print(f"总行数 {len(lines)}")

# ---------- 1. 位置偏移投票 ----------
print("\n" + "=" * 70)
print("位置偏移投票")
print("=" * 70)
vote_hdr = re.compile(r"位置偏移投票（敌人 (\d+) 个，屏幕 (\d+)x(\d+)）")
vote_row = re.compile(r"^\s+\+0x([0-9A-Fa-f]{3}):\s*数值合理\s*(\d+)\s*在相机前方\s*(\d+)\s*屏内\s*(\d+)")
i = 0
while i < len(lines):
    m = vote_hdr.search(lines[i])
    if not m:
        i += 1
        continue
    n, w, h = int(m.group(1)), int(m.group(2)), int(m.group(3))
    rows = []
    j = i + 1
    while j < len(lines) and (r := vote_row.match(lines[j])):
        rows.append((r.group(1), int(r.group(2)), int(r.group(3)), int(r.group(4))))
        j += 1
    print(f"\n--- 投票表（敌人 {n} 个，屏幕 {w}x{h}）---")
    for off, sane, front, inside in rows:
        bar = "#" * (inside // 2)
        print(f"  +0x{off}  合理{sane:4d}  前方{front:4d}  屏内{inside:4d}  {bar}")
    i = j

# ---------- 2. 位置候选扫描（取第一块） ----------
print("\n" + "=" * 70)
print("位置候选扫描（第一块，逐偏移的屏幕坐标）")
print("=" * 70)
scan_hdr = re.compile(r"位置候选扫描")
scan_row = re.compile(r"^\s+\+0x([0-9A-Fa-f]{3}) = \(\s*([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)\s*scr=\(\s*([-\d.]+),\s*([-\d.]+)\) w=\s*([-\d.]+)(.*)$")
printed = 0
for k, ln in enumerate(lines):
    if scan_hdr.search(ln) and printed < 3:
        print("\n" + lines[k])
        for j in range(k + 1, min(k + 20, len(lines))):
            m = scan_row.match(lines[j])
            if not m: break
            tag = m.group(8).strip()
            print(f"    +0x{m.group(1)} = ({float(m.group(2)):10.2f} {float(m.group(3)):10.2f} {float(m.group(4)):10.2f})"
                  f"  scr=({float(m.group(5)):8.0f},{float(m.group(6)):8.0f}) w={float(m.group(7)):9.2f} {tag}")
        printed += 1

# ---------- 3. 类名 ----------
print("\n" + "=" * 70)
print("类名")
print("=" * 70)
cls = defaultdict(int)
for ln in lines:
    m = re.search(r"obj=\S+ class=(\S+) team\(", ln)
    if m: cls[m.group(1)] += 1
    m = re.search(r"--- obj \S+ class=(\S+) 邻域 ---", ln)
    if m: cls[m.group(1)] += 1
if cls:
    for c, n in sorted(cls.items(), key=lambda kv: -kv[1])[:30]:
        print(f"  {n:6d}  {c}")
else:
    print("  仍然全空")

# ---------- 4. 敌人血量 ----------
print("\n" + "=" * 70)
print("敌人血量")
print("=" * 70)
hp = re.compile(r"obj=(\S+) class=(\S*) team\(new0x218\)=(-?\d+) team\(old0x180\)=(-?\d+) hp cur/max/min=([\d.eE+-]+)/([\d.eE+-]+)/([\d.eE+-]+)")
team_ctx = re.compile(r"^\s+team (\d+): relation=(-?\d+)")
cur_team = None
byp = defaultdict(list)
cls_of = {}
for ln in lines:
    m = team_ctx.match(ln)
    if m:
        cur_team = (int(m.group(1)), int(m.group(2)))
        continue
    m = hp.search(ln)
    if m:
        byp[m.group(1)].append((float(m.group(5)), float(m.group(6)), cur_team))
        cls_of[m.group(1)] = m.group(2)
print(f"唯一对象 {len(byp)}")
buckets = defaultdict(lambda: {"n": 0, "wounded": 0, "maxs": set(), "mins": []})
for p, lst in byp.items():
    rel = lst[-1][2][1] if lst[-1][2] else None
    b = buckets[rel]
    b["n"] += 1
    b["maxs"].add(round(lst[-1][1]))
    b["mins"].append(min(c for c, _, _ in lst))
    if any(c < mx - 1e-3 for c, mx, _ in lst):
        b["wounded"] += 1
for rel in sorted(buckets, key=lambda r: (r is None, r)):
    b = buckets[rel]
    label = {1: "友军", 2: "敌人", 0: "中立", None: "未知"}.get(rel, str(rel))
    print(f"\nrelation={rel} ({label}): 对象 {b['n']}，其中曾受伤 {b['wounded']}")
    print(f"    最大血量取值: {sorted(b['maxs'])[:20]}")
    print(f"    见过的最低当前血量: {sorted(b['mins'])[:20]}")

# ---------- 5. 相机原始数据（第一块） ----------
print("\n" + "=" * 70)
print("相机对象原始数据（第一块前 60 行）")
print("=" * 70)
for k, ln in enumerate(lines):
    if "相机对象原始数据" in ln:
        for j in range(k, min(k + 60, len(lines))):
            print("  " + lines[j])
        break
