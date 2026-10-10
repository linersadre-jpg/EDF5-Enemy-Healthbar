# EDF5 敌人血条 MOD — 逆向线索与 P0 探针计划

参考项目（均已 clone 到 `ref\`，git 可达）：
- `ref\EDF5-Damage-Number-Display` — **EDF5 直接参考**，MIT。给出伤害 Hook、相机矩阵、世界转屏幕、IMGUI 叠加实现。
- `ref\EDF6PlayerAim` — 你的 EDF6 自瞄插件，含 `docs\player-aim-re.md`、`docs\raycast-re.md`。
- `ref\edf6-all-forces` — 你的 EDF6 整合包，**逆向方法论主要来源**（`docs\相机/射线/HUD 逆向笔记`、`tools\` 工具链）。
- `ref\EDFModLoader` — 加载器与 Patcher 源码（tag v1.0.10 = 本机版本），`Patcher\EDF5\Patches.md` 有 EDF5 的 AOB 补丁实例。

---

## 1. 已验证的 EDF5 事实（本机静态确认）

- 目标文件：`E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\EDF5.exe`
  - `SHA256 = 3512D2A2E61D532C5D12DC0C5FD1AC61F6743E13349AE21301BB8449DD4BAE5F`
  - 大小 19,896,320 字节，`Machine = 0x8664`，`ImageBase = 0x140000000`
  - PE `TimeDateStamp = 1561452115 (0x5D11DE53)` = 2019-06-25（**注意：无版本资源，靠哈希+签名锚定**）
- 参考 MOD 的伤害 Hook 特征码 `F3 0F 58 87 FC 01 00 00`（`addss xmm0,[rdi+0x1FC]`）在 **`EDF5.exe+0x2DB641` 唯一命中** → 该 MOD 的偏移对本机这版有效。
- 参考 MOD 使用的关键偏移（EDF5）：
  | 用途 | 地址/偏移 |
  |---|---|
  | **敌人血量（float）** | `target + 0x1FC` |
  | 敌人类型字节 | `target + 0x218` |
  | 锁定点/中心点指针 | `GetPointerAddress(target, {0x268, 0x8})` → `+0x10/+0x14/+0x18` 为世界坐标 |
  | 对象位置兜底 | `target + 0xA4 / +0xA8 / +0xAC`（`pos = target+0x94` 再 `+0x10`） |
  | 玩家对象链 | `EDF5.exe+0x125AB68 → +0x238 → +0x290 → +0x10` |
  | 玩家是否在载具内 | `player + 0x1168 != 0` |
  | 相机 view / projection 矩阵 | `EDF5.exe+0x125B080 → +0x8 → +0x8 → +0x8`，view 在 `+0x80`、projection 在 `+0xE0`（各 float[16]） |
  | 窗口宽/高（int） | `EDF5.exe+0x1256C00` / `+0x1256C04` |
  | IDXGISwapChain VMT | `EDF5.exe+0x1256C98 → +0x0`（Present 用 VMT 索引） |
- 以上全局地址均落在 `.data` 的运行时初始化区（文件中无初值），符合"运行时全局"的预期。

---

## 2. 待定位的锚点（P0 目标）

EDF6 工程已经把"每个锚点长什么样"摸清了，EDF5 只需按同样思路找对应物：

| # | 需要的锚点 | EDF6/EDF4.1 的对应物（方法论出处） | EDF5 现状 |
|---|---|---|---|
| 1 | **敌人对象表** | EDF6：原版雷达 `HUiHudRader` 第 2 槽 `0x82B4D0` → `EnumConflictObject(player+0x314)`（`docs\camera-re.md` §雷达）；EDF4.1：per-team 对象 map（`work\edf41-playeraim\src\plugin.cpp:414` 起） | **未知，P0 首要目标** |
| 2 | **最大血量字段** | EDF6：`obj+0x2F4`（最大）/`+0x2F8`（当前），即原版跟随者血条读的那一对（`docs\hud-re.md` §1 表） | 已知当前血量 `+0x1FC`，**最大值未知**（可能邻近，如 `+0x1F8`，需实测） |
| 3 | **游戏自身的 NPC 血条绘制** | EDF6：`HudPlayer_FollowerDurability 0x8040E0`，绘制 `0x804300`，插件在 `0x8042AD` 处 `RedirectCall` 改成自己的 `GaugeHook`（`docs\hud-re.md` §0、`subcarrier-re.md` §4） | **未知**（EDF5 有 NPC 队友血条，必然存在对应函数） |
| 4 | **血条绘制原语（四边形）** | EDF6：`0xC2FB0`，画布 `*(img+0x2139A78)`；原版条半宽 `31·uiW/1920`、半高 `3·uiH/1080`，位置在对象 `+0x90` 上方 2 m | **未知** |
| 5 | **射线检测（遮挡用）** | EDF6：`docs\raycast-re.md`；EDF4.1：插件已实现视线判定（被地形/建筑挡住则丢目标） | **未知** |

补充：EDF6 判定"大型敌人"的既有口径为「最大血量 ≥ 全部敌人中位数的 6 倍，或 ≥ 30000」（`docs\camera-re.md`），EDF5 可复用同样的分档思路。

---

## 3. P0 只读探针要 dump 的东西

不改游戏任何字节，挂在游戏每帧函数上（或参考 MOD 的 Present Hook 线程），每 N 帧输出：

1. 从候选来源（雷达 / 冲突对象管理器 / 全局对象数组）枚举的对象：指针、`+0x1FC`、`+0x218`、`+0x2F4`/`+0x1F8` 等邻近浮点、坐标候选。
2. 受伤前后对照：确认哪个字段随伤害单调下降（当前血量），哪个不随伤害变化且与初始血量一致（最大血量）。
3. **父对象聚合字段 vs 各部位**：对埃尔基努斯级敌人，分别记录父对象字段与各部位字段，判断"总血量"的真实口径（用户已选父字段，此处是裁决点）。
4. 相机：view/projection 矩阵在载具内、瞄准镜下、第三人称下的取值，确认始终跟随当前渲染相机。
5. 对象包围盒/渲染尺寸来源（供头顶定位，替代固定偏移）。
6. 敌人对象表的**数量**是否与原版雷达一致（EDF6 就是拿这个当验收标准）。

---

## 4. 工具

- 已写：`tools\edf5pe.py`（PE 解析、SHA256、AOB 扫描、RVA/文件偏移互转）。
  ```
  py -3 tools\edf5pe.py info
  py -3 tools\edf5pe.py sig "F3 0F 58 87 FC 01 00 00"
  py -3 tools\edf5pe.py bytes 0x2DB641 16
  ```
- 可借鉴：`ref\EDF6PlayerAim\tools\edfre2.py`（EDF6 的逆向后处理）、`ref\edf6-all-forces\tools\`（`edfre.py`、`fnrefs.py`、`dump.py`、`calls.py`）、`work\edf41-playeraim\tools\edf41re.py`。

---

## 5. 对已定方案的修正建议（供决策）

EDF6 工程的经验显示：**"hook 游戏自带的跟随者血条绘制"这条路的成本比原先估计的低**——EDF6 已经验证了配方（找到原版血条绘制函数 → 在它的调用点改指向自己的绘制 → 复用同一次的 view-projection、绘制上下文和视口 → 用游戏的四边形原语画自己的条），并且能**独占全屏可见**、不需要 ImGui、不需要叠加窗口。

对本项目的影响：
- 已选方案 B（分层透明窗口 + GDI）仍然可行且最快，但只在窗口/无边框下有效；
- 若 EDF5 能找到等价的"原版 NPC 血条绘制函数"（锚点 #3、#4），那么"用游戏自己的绘制画敌方血条"会同时拿到**与 NPC 队友血条像素级一致的观感**和**独占全屏兼容**——即原需求文档 §2.3"样式模仿 NPC 队友血条"的最优解。
- 建议：P0 探针阶段顺便确认 #3/#4 是否可定位；若可，方案 B 与"原生绘制"可以并存（B 作为快速验证与降级路径）。
