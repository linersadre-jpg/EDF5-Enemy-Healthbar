# EDF5 敌人血条 MOD —— 延迟与卡顿：代码级分析与优化方案

## 落地情况（2026-10-10 更新）

**v1.1.0 已实现第 1、2、3 条，用户实机确认"血条几乎和 NPC 原生血条一样随着敌人移动"，卡顿明显减轻。**
第 4 条（遮挡分帧复用）也在 v1.1.0 落地，配置项 `OcclusionReuse`（默认 2）。

实测诊断（`[draw] … | 发布=N 绘制=M 快照年龄=Xms`）：`发布≈175/3s`（≈58 fps，游戏 60 fps 上限）、
`快照年龄 0~16ms`（≤1 帧），即采集与呈现都跟上了。

### 两个真实踩过的坑

1. **`DrawFps=0` 被 clamp 成 5** → 采集与覆盖层双双掉到 5 Hz → 血条滞后约 **0.3 秒**
   （症状：敌人已经出屏，血条还留在屏幕里）。`0` 必须不参与夹取。
2. **事件驱动的循环不能在"内容未变"分支裸 `continue`** → 覆盖层线程空转吃满一个核，
   反过来拖慢游戏、血条更滞后。必须等待事件或 sleep。

（以下为原始分析与方案，保留供参考。）

版本基线：**v1.0.8**（已冻结，见 `snapshots\v1.0.8-20261010\`）。
用户反馈：转动视角时血条略微跟不上敌人模型；疑似有卡顿，影响体验。

下面每一条都指向具体代码位置，并给出改法与预期收益。

---

## 因因 1：覆盖层是"定时重绘"，不是"跟帧重绘" → 转视角时落后

**位置**：`src\EHB_overlay.cpp` 绘制线程

```cpp
if (now - lastDraw < interval) { Sleep(1); continue; }   // interval 由 DrawFps 决定
```

**问题**：

- `DrawFps` 默认 60。游戏若跑 120/144 fps，血条位置**每 16.7 ms 才更新一次**，
  而模型每 6.9 ms 就动了 → 转视角时肉眼可见地"拖在后面"。
- `Sleep(1)` 轮询还会引入最多 ~1 ms 的额外抖动，刷新相位与游戏帧不对齐。
- 游戏线程每帧都会 `InterlockedIncrement(&g_snap.seq)`，但覆盖层完全没用到这个序号。

**改法**：

1. 覆盖层改为**跟随快照序号**：记住上次画的 `seq`，只在 `seq` 变化时绘制
   （可用 `WaitForSingleObject` 到某个事件，或把 `Sleep(1)` 保留但去掉 interval 门限）。
2. `DrawFps` 语义改为**上限**，`0 = 不限**（跟随游戏帧率）。
3. 预期：转视角时血条与模型同步，延迟从"最多 1 个 16.7 ms 周期"降到"1 帧"。

---

## 原因 2：每帧清整屏 + 提交整屏 → 与游戏抢带宽，造成卡顿

**位置**：`src\EHB_overlay.cpp`

```cpp
std::memset(bmpBits, 0, (size_t)cw * ch * 4);   // 清整块 DIB
...
UpdateLayeredWindow(g_overlay, screenDC, &ptDst, &size, memDC, &ptSrc, 0, &bf, ULW_ALPHA);
```

**算一下**：客户区 2560×1410，32bpp → **14.4 MB**。

- 每帧 `memset` 14.4 MB，60 fps → **约 865 MB/s** 的纯内存写。
- 每帧把 14.4 MB 的表面交给 DWM 合成，再加上 DWM 自身的拷贝。
- 这些开销和游戏共享 CPU/内存带宽与 GPU 上行带宽 → 掉帧/卡顿，且游戏本身画得越重越明显。

**改法**（按收益排序）：

1. **持久 DIB + 只擦上一帧的血条矩形**：保留一个常驻 DIB，只把上一帧画过的那些
   小矩形（50×5 像素级别）清零，不再整屏 `memset`。收益：14.4 MB/帧 → 几十 KB/帧。
2. **改用 `UpdateLayeredWindowIndirect` + `prcDirty`**：只上传发生变化的那一小块区域，
   其余部分沿用上一次的表面内容。收益：上传量同量级下降。
3. 可选：把覆盖层窗口本身缩到"所有血条的包围盒"（加 8 px 余量），用 `SetWindowPos` 移动。
   代价是每帧一次 `SetWindowPos`，通常不如 1+2 划算，但窗口小到几十像素时很有用。
4. 预期：卡顿基本消失；这一步对"延迟"也有帮助（提交 14.4 MB 本身就要花时间，
   等于给血条又加了 1~2 帧的延迟）。

---

## 原因 3：采集时每个字段一次 `VirtualQuery` → 渲染线程上的隐形开销

**位置**：`src\EHB_common.h`

```cpp
inline bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;   // ← 每次读都调
    ...
}
template <class T> inline bool Rd(uintptr_t a, T& out) {
    if (a == 0 || !Readable((const void*)a, sizeof(T))) return false;    // ← 每次字段读都过这里
    ...
}
```

**问题**：`Rd` / `RdPtr` / `RdF` / `RdI` **每一次**都调一次 `VirtualQuery`。

- 采集一帧：遍历队伍表（每个节点 `Readable` 一次 + 若干字段读）+ 每个敌人十几个字段读。
- 几十个敌人 × 十几字段 + 树遍历 ≈ **每帧上千次 `VirtualQuery`**。
- `VirtualQuery` 是内核调用，量级在微秒到十几微秒；上千次就是**毫秒级**，
  而且发生在**游戏渲染线程**（Present 钩子里）→ 直接表现为卡顿。

**改法**：给 `Readable` 加"上次查询区间"缓存：

```cpp
static uintptr_t g_qa = 0, g_qb = 0;      // 上次 VirtualQuery 的结果区间
static bool      g_qok = false;
inline bool Readable(const void* p, size_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (g_qok && a >= g_qa && a + n <= g_qb) return true;   // 落在上次区间内，直接用
    ...VirtualQuery...
    g_qa = rstart; g_qb = rstart + RegionSize; g_qok = true;
}
```

同一批对象的内存几乎都落在少数几个 region 里，命中率会非常高。
预期：`VirtualQuery` 次数下降 1~2 个数量级。

> 注意：该缓存只在游戏线程使用（采集全在 Present 钩子里），不需要加锁；
> 覆盖层线程不读游戏内存。

---

## 原因 4：每帧最多 64 次遮挡射线（可选优化）

**位置**：`src\EDF5EnemyHealthBar.cpp` 的遮挡块

射线调用的是游戏自己的 `EDF5.exe+0x74F110`，跑在渲染线程上。实测 13 条/帧没问题，
但敌人多时会线性增长（上限 `MaxBars=64`）。

**改法**（按需，先测再说）：

1. 每个对象的遮挡结果**缓存 N 帧**：第 i 帧只对 `i % N == idx` 的对象重新投射（N=2~3）。
   转视角时遮挡关系变化有 1~3 帧延迟，肉眼基本无感。
2. 每帧射线数上限 `OcclusionRaysPerFrame`（默认 16），超出的对象沿用上次结果。

---

## 建议的实施顺序

| 步骤 | 改动 | 目的 | 风险 |
|---|---|---|---|
| 1 | `Readable` 区间缓存 | 去掉渲染线程上的毫秒级开销 | 低（纯本地缓存） |
| 2 | 持久 DIB + 只擦旧条 + `prdDirty` | 去掉 14.4 MB/帧的清屏与上传 | 中（要正确处理"上一帧的条"） |
| 3 | 覆盖层跟 `seq` 重绘，`DrawFps=0` 不限 | 修掉转视角落后 | 低 |
| 4 | 遮挡结果分帧缓存（可选） | 敌人很多时更稳 | 低 |

步骤 1+3 改完就能明显改善；2 是卡顿的大头。

## 与新方向的关系

如果最终改用**游戏自己的 HUD 绘制时机**来画（见 `EDF5-复用原生NPC血条-调研与方案.md`），
上面第 2、3 条会**整体作废**（不再有独立覆盖层窗口，血条与游戏同帧绘制），
但第 1 条（`Readable` 缓存）与第 4 条（射线分帧）仍然有效，因为采集本身还在渲染线程上。
所以：**先做 1 和 4，把采集成本压下来；2 和 3 等原生方案调研结论出来再决定要不要投入。**
