#!/usr/bin/env python3
"""解析 EDF5_Probe.log v2：按队伍关系分组，报表敌人数量/血量/位置候选。"""
from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

LOG = Path(sys.argv[1] if len(sys.argv) > 1 else
           r"E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\Mods\Plugins\EDF5_Probe.log")
W, H = 2560, 1440

snap_hdr = re.compile(r"^\[snap (\d+)\] .*rows=(\S+) teamCount=(-?\d+)")
team_ln = re.compile(r"^\s+team (\d+): relation=(-?\d+) head=(\S+) objects=(\d+)")
obj_ln = re.compile(r"^\s+obj=(\S+) class=(\S*).*team\(new0x218\)=(-?\d+) team\(old0x180\)=(-?\d+) hp cur/max/min=([\d.eE+-]+)/([\d.eE+-]+)/([\d.eE+-]+)")
pos_ln = re.compile(r"posA\(0x90\)=\(([^)]*)\) scr=\(([^)]*)\) w=([\d.eE+-]+) \| posB\(0xA4\)=\(([^)]*)\) scr=\(([^)]*)\) w=([\d.eE+-]+)")
lock_ln = re.compile(r"lockon ptr=(\S+) pos=\(([^)]*)\) scr=\(([^)]*)\) w=([\d.eE+-]+)")

recs = []
cur_team = (None, None)
snap_no = 0
for ln in open(LOG, encoding="utf-8", errors="replace"):
    m = snap_hdr.match(ln)
    if m:
        snap_no = int(m.group(1)); cur_team = (None, None); continue
    m = team_ln.match(ln)
    if m:
        cur_team = (int(m.group(1)), int(m.group(2))); continue
    m = obj_ln.match(ln)
    if m:
        r = {"snap": snap_no, "team": cur_team[0], "rel": cur_team[1], "ptr": m.group(1),
             "cls": m.group(2), "t218": int(m.group(3)), "t180": int(m.group(4)),
             "cur": float(m.group(5)), "max": float(m.group(6)), "min": float(m.group(7))}
        recs.append(r); continue
    if not recs or recs[-1]["snap"] != snap_no: continue
    m = pos_ln.search(ln)
    if m:
        recs[-1].update(posA=m.group(1), scrA=m.group(2), wA=float(m.group(3)),
                        posB=m.group(4), scrB=m.group(5), wB=float(m.group(6))); continue
    m = lock_ln.search(ln)
    if m:
        recs[-1].update(lockptr=m.group(1), lockpos=m.group(2), lockscr=m.group(3), lockw=float(m.group(4)))

# 每个快照的队伍表
per_snap = defaultdict(dict)
for r in recs:
    per_snap[r["snap"]][r["team"]] = r["rel"]
last_snap = max(per_snap) if per_snap else 0
print(f"快照 {len(per_snap)} 个；对象记录 {len(recs)} 条；最后快照 {last_snap}")

# 各队伍的对象数与 relation 随时间
mid = sorted(per_snap)[len(per_snap) // 2]
rel_by_team = {}
for s, d in per_snap.items():
    for t, rel in d.items():
        rel_by_team[t] = rel
print("\n队伍 relation 汇总:", {f"team{t}": f"rel={r}" for t, r in sorted(rel_by_team.items())})

counts = defaultdict(lambda: defaultdict(int))
for r in recs:
    counts[r["snap"]][r["team"]] += 1
sample = max(counts, key=lambda s: sum(counts[s].values()))
print(f"对象最多的快照 {sample}: " + " ".join(f"team{t}={counts[sample][t]}" for t in sorted(counts[sample])))

# 类名情况
cls = defaultdict(int)
for r in recs:
    if r["cls"]: cls[r["cls"]] += 1
print(f"\n非空类名 {len(cls)} 种" + (": " + ", ".join(f"{c}({n})" for c, n in sorted(cls.items(), key=lambda kv: -kv[1])[:12]) if cls else "  ← 类名解析全空，需要修探针"))
print(f"team(0x218) vs team(0x180) 取值: new={sorted({r['t218'] for r in recs})[:8]} old={sorted({r['t180'] for r in recs})[:8]}")

# 敌人（relation==2）分析
enemies = [r for r in recs if r["rel"] == 2]
print(f"\n===== 敌人对象 (relation==2) {len(enemies)} 条 =====")
if enemies:
    ptrs = defaultdict(list)
    for r in enemies: ptrs[r["ptr"]].append(r)
    print(f"唯一敌人 ptr: {len(ptrs)}")
    wounded = [(p, l) for p, l in ptrs.items() if any(x["cur"] < x["max"] - 1e-3 for x in l)]
    print(f"曾出现过 受伤(cur<max) 的敌人: {len(wounded)}")
    hp = sorted({(round(l[0]['cur']), round(l[0]['max'])) for l in ptrs.values()})
    print("血量( cur / max )分布 top20:")
    for c, m in hp[:20]: print(f"    {c} / {m}")
    print("\n前 6 个敌人的最新状态:")
    for p, l in list(ptrs.items())[:6]:
        l.sort(key=lambda x: x["snap"]); o = l[-1]
        print(f"  {p} rel={o['rel']} t218={o['t218']} hp={o['cur']:.0f}/{o['max']:.0f} n={len(l)}")
        for x in l[:2] + l[-1:]:
            print(f"      snap{x['snap']} hp={x['cur']:.0f}/{x['max']:.0f} posA={x.get('posA')} scrA={x.get('scrA')} wA={x.get('wA')}")
            print(f"                 posB={x.get('posB')} scrB={x.get('scrB')} wB={x.get('wB')} lock={x.get('lockpos')}")

def onscreen(scr):
    try:
        x, y = [float(v) for v in scr.split(",")]
    except Exception:
        return False
    return 0 <= x <= W and 0 <= y <= H

print("\n===== 位置候选：屏幕坐标落在窗口内的比例（仅 relation==2）=====")
for key in ("scrA", "scrB", "lockscr"):
    got = [r for r in enemies if key in r]
    ok = sum(1 for r in got if onscreen(r[key]))
    print(f"  {key}: 样本 {len(got)}，在屏内 {ok} ({100*ok/max(len(got),1):.0f}%)")

print("\n===== 视角内(w>0.1)比例 =====")
for key in ("wA", "wB", "lockw"):
    got = [r for r in enemies if key in r]
    ok = sum(1 for r in got if r[key] > 0.1)
    print(f"  {key}: 样本 {len(got)}，w>0.1 {ok} ({100*ok/max(len(got),1):.0f}%)")
