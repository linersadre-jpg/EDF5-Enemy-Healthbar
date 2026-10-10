# EDF5 敌人血条 MOD — P0 静态定位结果

目标 EDF5.exe：`E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\EDF5.exe`
SHA256 `3512D2A2E61D532C5D12DC0C5FD1AC61F6743E13349AE21301BB8449DD4BAE5F`
（19,896,320 字节 / x64 / ImageBase 0x140000000）。本文地址一律是 `EDF5.exe+RVA`（模块基址 + 偏移）。

---

## 1. 敌人血量结构（已确认，指令级证据）

**构造函数 `EDF5.exe+0x2D9A10`** 初始化这些字段：

```
0x002D9CBD  lea   r8, [rdi + 0x90]              ; 位置/矩阵相关的对象成员
0x002D9F25  movss [rdi + 0x1F8], xmm0           ; ← 最大血量
0x002D9F2F  mov   [rdi + 0x1F4], ebp            ; ← 血量下限（钳制地板）
0x002D9F35  movss [rdi + 0x1FC], xmm0           ; ← 当前血量 = 满血
0x002D9F61  mov   qword [rdi + 0x218], -1       ; ← 队伍 = -1（未分配）
```

**伤害函数 `EDF5.exe+0x2DB370`**（`rdi` = 被命中的敌人对象，`rsi` = 伤害信息结构）：

```
0x002DB411  movsxd rcx, dword ptr [rdi + 0x218]  ; 目标队伍
0x002DB496  movss  xmm0, dword ptr [rdi + 0x1FC] ; 当前血量
0x002DB4B1  movss  xmm1, dword ptr [rdi + 0x1F8] ; 最大血量
0x002DB641  addss  xmm0, dword ptr [rdi + 0x1FC] ; ← 参考 MOD 挂钩点
0x002DB649  minss  xmm0, dword ptr [rdi + 0x1F8] ; 钳到最大
0x002DB651  maxss  xmm0, dword ptr [rdi + 0x1F4] ; 钳到下限
0x002DB659  movss  dword ptr [rdi + 0x1FC], xmm0 ; 写回当前血量
```

**结论（对象布局）**

| 偏移 | 含义 | 依据 |
|---|---|---|
| `+0x1F4` | float 血量下限（钳制地板） | 构造函数 + 伤害函数 minss/maxss |
| `+0x1F8` | float **最大血量** | 构造函数赋满血 + 伤害函数上限钳制 |
| `+0x1FC` | float **当前血量** | 参考 MOD 已证实，本文再确认 |
| `+0x218` | int32 **队伍**（索引队伍关系数组） | 构造函数置 -1 + 伤害函数查关系 |
| `+0x268` | 锁定点指针链 | 参考 MOD |
| `+0x90` / `+0xA4` | 位置候选（两者只能有一个对） | 见 §5 |

未满血判定 = `+0x1FC < +0x1F8`，不需要挂钩血量更新函数，每帧轮询即可。

---

## 2. 敌人列表来源（结构已确认，运行期待探针确认）

**TeamManager 全局：`EDF5.exe+0x125AB80`**（存放 `TeamManager*`）。

结构取自伤害函数本身（`EDF5.exe+0x2DB41D` 起）：

```
0x002DB41D  imul  rdx, rax, 0x38                 ; 队伍行步长 0x38
0x002DB421  mov   rax, [rip + 0xF7F758]          ; → EDF5.exe+0x125AB80
0x002DB428  mov   rax, [rax + 0x38]              ; TeamManager+0x38 = 队伍行数组
0x002DB42C  mov   rax, [rdx + rax + 0x18]        ; 行+0x18 = 队伍关系 int 数组
0x002DB431  cmp   dword ptr [rax + rcx * 4], 1   ; relations[目标队伍] == 1（友军）
```

这和本机已有的 EDF4.1 自瞄插件（`work\edf41-playeraim\src\plugin.cpp:31-34`、`:94-97`）
用的是**同一套结构**，只是全局地址不同（EDF4.1 是 `EDF41.exe+0xCC8510`）：

| 字段 | EDF4.1 | EDF5 |
|---|---|---|
| Manager 全局 | `0xCC8510` | **`0x125AB80`** |
| 行数组 | `+0x38` | **`+0x38`** ✓ 同 |
| 队伍数 int | `+0x48` | 待确认 |
| 行步长 | `0x38` | **`0x38`** ✓ 同 |
| 行内关系数组 | `+0x18` | **`+0x18`** ✓ 同 |
| 关系值 | 1=友军 2=敌军 | 同 |

队伍的 std::map 遍历（MSVC `_Tree`，EDF4.1 已验证）：行 `+0x00` 是 map 的哨兵
`_Myhead`；**真正的根在哨兵 `+0x08`（`_Parent`）**；节点 `+0x00` 左 / `+0x08` 父 /
`+0x10` 右 / `+0x19` `_Isnil` / `+0x20` 是对象指针。

顺带记录了游戏自己往队伍 map 里插入对象的函数：`EDF5.exe+0x4CE1A0`、`+0x4CE340`
（特征是 `mov qword ptr [rax+0x20], rdi` + 多次 `cmp byte ptr [rax+0x19], 0`）。

---

## 3. 原版血条绘制（候选已锁定，待确认）

EDF5 的 HUD 代码集中在 `0x4B9xxx–0x4CDFxx`。按“同时读 `+0x1FC` 和 `+0x1F8` 并做除法
（= 血量百分比）”筛出来的函数：

| 函数 | 关键指令 | 判断 |
|---|---|---|
| **`0x4B9F0D`** | `divss xmm2, [rax+0x1F8]`（0x4BA310），比值随后被用于画条 | **NPC 队友血条最可能**：与 `HudPlayer_FollowerDurability` 虚表槽（`0x4B9D30` / `0x4B9CB0`）相邻 |
| `0x4CADB0` | `divss xmm0, [rax+0x1F8]`（0x4CB593） | HUD 血量条（另一处） |
| `0x4CB7F0` | `divss xmm0, [rax+0x1F8]`（0x4CB84E） | 同上 |
| `0x4CBDE0` | `divss xmm0, [rax+0x1F8]`（0x4CBE66） | 同上 |
| `0x4CC210` | `cvttss2si` 血量→像素（0x4CC763/0x4CC790） | 同上（带像素换算） |

EDF6 的对应实现（`ref\edf6-all-forces\docs\hud-re.md`）是挂 `HudPlayer_FollowerDurability`
的绘制 `0x804300`、在调用点 `0x8042AD` 处 `RedirectCall`，原版条半宽 `31·uiW/1920`、
半高 `3·uiH/1080`。EDF5 的 `0x4B9F0D` 是同一位置的候选，下一步反汇编确认。

---

## 4. RTTI（EDF5 保留了完整类名，可直接查类）

| 类 | TypeDescriptor | COL | vtable |
|---|---|---|---|
| `HudPlayer_FollowerDurability` | `0x11C0688` | `0xF8B908` | `0xEC92F8` |
| `HUiHudRader@ui` | `0x11C1E10` | `0xF8DA80` | `0xECB230` |
| `GameObjectBase` | — | — | `0xEACD48` |
| `TeamManager` | — | — | `0xEB2128` |
| `AttackTargetManager` | — | — | `0xE98E50` |

`HudPlayer_FollowerDurability` 虚表槽：`[0]0x24980 [1]0x23390 [2]0x23390 [3]0x4B9D30
[4]0x4B9CB0 [5]0x4B41E0 [6]0x4B42C0`（`0x23390` 是空实现桩）。
全库共 3,872 个类名；敌人按种类各自成类（`AssultSoldier`、各 `Factory` 等），没有统一的
`Enemy` 类 —— 所以“是不是敌人”要靠**队伍关系**判断，不能靠类名。

---

## 5. 待探针确认的项（都已编进 `EDF5_Probe.cpp`）

1. `TeamManager+0x48` 是不是队伍数、行数组能不能遍历出对象
2. 敌对队伍对象数量是否与实际敌人数量相当
3. 敌人 `+0x1FC` 是否随射击下降、`+0x1F8` 是否保持不变
4. 位置到底在 `+0x90` 还是 `+0xA4`（只有一个是合理屏幕坐标）
5. 世界转屏幕数学（参考 MOD 的 view/projection 链 `0x125B080 → +0x8/+0x8/+0x8 → +0x80/+0xE0`）
6. 玩家对象链 `0x125AB68 → +0x238 → +0x290 → +0x10` 是否仍有效

---

## 6. 工具（工作区 `tools\`）

| 工具 | 用途 |
|---|---|
| `tools\edf5pe.py` | PE 头 / SHA256 / AOB 特征扫描（支持 `??`）/ RVA↔文件偏移 |
| `tools\edf5re.py` | `.pdata` 函数边界、capstone 反汇编、直接调用者、全局交叉引用（sweep） |
| `tools\edf5_fields.py` | **按结构体字段偏移全库搜索访问点**（本文 §3 就是它扫出来的） |
| `tools\edf5rtti.py` | 提取 RTTI 类名 / COL / 虚表 |
| `tools\fields-report.txt` | 全量字段命中报告（756 个函数命中 ≥2 个字段） |

---

## 7. 实机日志结论（第一轮，2026-10-10）

日志：`E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\Mods\Plugins\EDF5_Probe.log`
（226 个快照 / 7272 行）。分析脚本 `tools\probe_report.py`。

### 7.1 敌人列表 —— 确认可用 ✅

队伍表实际取值（`relation` 就是 `relations[本队][该队]`）：

| 队伍 | relation | 对象数 | 是什么 |
|---|---|---|---|
| team 0 | 1 | 1 | 玩家自己 |
| **team 1** | **2** | **34** | **敌人** |
| team 2 | 1 | 18 | 友军 NPC |
| team 3 | 0 | 48 | 中立（平民/物件） |
| team 4–6 | — | 0 | 空 |

遍历配方（与 §2 静态结论一致）全部跑通：`TeamManager+0x38` 行数组、行步长 `0x38`、
行 `+0x00` 是 `_Tree` 哨兵、根在哨兵 `+0x08`、节点 `+0x20` 是对象指针、`+0x19` 跳过 nil。
**遍历时对象数为 0 说明还在主菜单**（快照 0–115 全是 0，116 起才有对象）。

### 7.2 血量字段 —— 确认可用 ✅

34 只敌人全部 `+0x1F8`（最大）= **81.0** 恒定，`+0x1FC`（当前）从 81 一路掉到
12 / 18 / 58 / 68……**32/34 只被记录到受伤状态**。跨快照数值单调下降、最大值不动
——`+0x1F8` = 最大总血量、`+0x1FC` = 当前血量，**彻底证实**。

`+0x218` 对每只敌人都等于 `1`（= team 1 的行号），对玩家是 `0`，**确认它是队伍索引**；
`+0x180`（EDF4.1 的 team 偏移）恒为 0，在 EDF5 里**不是**队伍。

### 7.3 位置偏移 —— 上一轮探针读错了 ⚠️

- `+0x90` 读出来是**单位向量**（如 `(0.55, 0.00, -0.84)`，模长恒为 1）→ 那是**朝向
  （forward）**，不是坐标。EDF4.1 的 `kPosition=0x90` 在 EDF5 **不适用**。
- `+0xA4` 读出 `(3.76, 323.65, 1.00)`；而锁定点链给出 `(487.11, 7.46, 323.54)`。
  两个来源的 **z 分量吻合**（323.65 vs 323.54），且 `+0xAC` 恒为 `1.00`（矩阵的 w）。

**推论：变换矩阵的平移行在 `+0xA0 / +0xA4 / +0xA8`**（x / y / z，w 在 `+0xAC`），
矩阵的第 3 行（forward）在 `+0x90`。也就是说 EDF5 的矩阵比 EDF4.1 **整体后移 0x10**。

参考 MOD 的兜底 `pos = target + 0x94` 再读 `+0x10/+0x14/+0x18` = 读 `target+0xA4/A8/AC`
——它读到的是 `(y, z, w)`，**这个兜底分支本身是错位的**（只有在对象没有锁定点时才会走到，
所以一直没被发现）。真正可用的位置是 `+0xA0` 起，或锁定点链 `+0x268 → +0x8 → +0x10`。

### 7.4 已修的两个探针 bug

1. **类名解析全空**：MSVC `RTTICompleteObjectLocator` 的 `signature` 在 x64 上是
   **1**，32 位才是 0。原先按 0 校验 → 全部返回空。已改为接受 0 或 1。
2. **原始转储没触发**：转储条件写的是 `g_snap <= 1`，而快照 0–115 全在主菜单（没有对象），
   于是什么都没转储。已改为「第一个真的有敌人的快照」触发。

### 7.5 第二轮探针新增的输出

- `===== 位置偏移投票 =====`：把 `+0x60..+0xC0` 每 4 字节当 `(x,y,z)` 投影一次，统计
  34 只敌人里有多少「数值合理 / 在相机前方 / 落在屏幕内」——**得票最高的偏移就是位置**。
- 位置候选逐条扫描（前 3 只敌人，`+0x80..+0xC0`，逐条打印屏幕坐标）。
- 首个有敌人的快照转储 `+0x60..+0x120` 原始浮点 + 相机对象 `+0x00..+0x180`
  （后者用来找**相机世界坐标**，以备「最大显示距离」）。

### 7.6 仍需实测的项

- 位置偏移投票的最终结果（第二轮日志）
- **多部位/巨大敌人**：本轮那关全是 81 血的同一种小怪，需要换一关打**大型敌人或 Boss**，
  验证 `+0x1F8` 仍然是"汇总后的总血量"而不是某个部位的血
- 相机世界坐标（用于 MaxDistance）
- 遮挡射线函数（静态仍在找）

