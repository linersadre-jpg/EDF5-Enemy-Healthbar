# EDF5 敌人血条 MOD — 需求修订（v2）

> 本文件取代原需求文档的第三章"技术方案"与 §2.1 的遮挡实现细节。
> 修订依据：本机 EDF5 / EDFModLoader / 参考 MOD 实测 + 本机既有 EDF4.1、EDF6 插件工程。

---

## 0. 原文档必须作废的假设

| 原文档写法 | 实测事实 |
|---|---|
| EDF5 是 Unity 游戏，引用 `UnityEngine.dll` / `Assembly-CSharp.dll` | **错误**。EDF5 目录内无 `Managed\`、无 `Assembly-CSharp.dll`、无 `UnityPlayer.dll`；`EDF5.exe` 为 19,896,320 字节原生 x64 PE（`Machine=0x8664`），逻辑在 exe 内，引擎为 Sandlot 自研 + `umbra_sandlot.dll` |
| 用 dnSpy/ILSpy 反编译找 `EnemyHealth` / `ApplyDamage` | **不可行**，没有托管程序集 |
| C# 类库项目 + 引用游戏程序集 | **不可行**，无 CLR 运行时 |
| 用 `ImGui.NET` 绘制，`ImGui.NET.dll` 放入 `Mods\Plugins\` | **不可行**，`ImGui.NET` 是托管封装；且本项目最终不采用 ImGui |
| `Physics.RaycastNonAlloc` / `Camera.WorldToViewportPoint` / `Vector3.Angle` / `Renderer.bounds` / layer mask | **全部不存在**，均需自行实现或逆向 |
| 必须 Hook 血量更新函数 | 降级为可选；已确认采用每帧轮询 |
| 配置放游戏根目录 | 改为插件 DLL 旁边 |

**EDFModLoader 的真实契约**（`ModLoader.log` 显示 v1.0.10，winmm.dll 代理注入，`Running under EDF5`）：
- winmm.dll 仅导入 `KERNEL32 / MSVCP140 / VCRUNTIME140 / VCRUNTIME140_1 / SHLWAPI / winmm`，字符串含 `MinHook`、`LoadLibrary`、`GetProcAddress`、`LoadPlugin` 及 `EML4_Load|EML5_Load|EML6_Load` → **原生 x64 C++ 加载器，无托管宿主**。
- 插件入口（EDF5 对应第 5 号）：

```cpp
extern "C" __declspec(dllexport) bool EDFMLAPI EML5_Load(PluginInfo* info);
```

参考实现见 `E:\GAME\edf6-all-forces-nightly\src\plugin.cpp:319` 与 `third_party\EDFModLoader\PluginAPI.h`。

---

## 1. 已确认的决策

| 项目 | 决定 |
|---|---|
| 技术栈 | 原生 C++17 x64 DLL，CMake + MSVC（`C:\BuildTools`），插件导出 `EML5_Load` |
| 绘制方式 | **分层透明窗口 + GDI/D2D 自绘**（复用 EDF6 `src/overlay.cpp` 模式）；预留可替换绘制后端，日后可升级 ImGui + D3D11 Present |
| 血量来源 | **每帧轮询对象字段**（EDF4.1 已验证 `maxHealth=+0x168`、`health=+0x16C`，float，且来自游戏雷达同源对象表）；EDF5 偏移需重新定位 |
| 总血量口径 | 读**父对象聚合字段**；P0 探针同时 dump 各部位血量做交叉核对，若父字段不成立立即修正 |
| 配置文件 | `Mods\Plugins\<插件名>.ini`（DLL 旁边），由插件自读自写，热重载 |
| 功能范围 | 仅敌方（不含友军 NPC/玩家）；包含巨大敌人与 Boss；联机也要正确；载具内/瞄准镜下也显示；支持开关热键；含最大显示距离/最小屏幕尺寸 |
| 实机验证 | 用户实机跑并回传日志 + 截图 |

---

## 2. 插件骨架

- 名称（暂定）：`EDF5EnemyHealthBar.dll` + `EDF5EnemyHealthBar.ini`
- 构建：照抄 `edf6-all-forces-nightly\build-ws.cmd` 的直连 `cl/link` 方式（沙箱下 CMake 的 piped stdio 探测会挂）
- 入口流程：`EML5_Load` → 初始化 ini 与日志 → 识别 EDF5.exe 镜像 → 签名校验 → 全部通过后才安装 hook；校验失败直接 `return false`（保持游戏原样）
- 兼容性锚点：`EDF5.exe` **无版本资源**，PE `TimeDateStamp = 918027`（无效值），禁止用版本号判断；使用
  - 文件哈希：`SHA256 = 3512D2A2E61D532C5D12DC0C5FD1AC61F6743E13349AE21301BB8449DD4BAE5F`（本机当前版本）
  - AOB 签名扫描定位各锚点（同 `Mods\Patches\*.txt` / Patcher 的做法），签名不匹配则拒绝加载并记录日志

---

## 3. 数据获取（P0 只读探针先行）

**P0 目标：不修改游戏任何字节，先把数据源钉死。**

挂在游戏自带的每帧函数上（同 EDF6 的 per-frame hook 模式），每 N 帧向日志输出：

- 敌我对象表来源与数量（游戏雷达同源表），`类名 / 指针 / 坐标 / 队伍关系`；
- 每个对象的 `hp / maxHp` 候选偏移扫描结果（对候选偏移做"受伤前后变化"验证）；
- 巨大敌人的父对象字段 vs 各部位血量对照（裁决"总血量口径"）；
- 相机：当前渲染相机的 view / projection 矩阵位置候选（载具内、瞄准镜下各取一份）；
- 对象包围盒/骨骼半尺寸来源（供头顶定位）。

**线程约束**：对象表与血量必须在**游戏线程**上读（在 per-frame hook 里取值并写入快照），绘制线程只读快照；不可在 overlay 线程直接遍历游戏对象。

---

## 4. 绘制方案（分层透明窗口 + GDI/D2D）

- 窗口：`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`，`WS_POPUP`；
- 透明：血条为硬边矩形，用 `LWA_COLORKEY`（黑底键控）；若要抗锯齿边缘则用 `UpdateLayeredWindow` + 32 位预乘 alpha；
- 跟随游戏窗口：每帧或 `WM_SIZE/WM_MOVE` 时用 `GetClientRect` + `ClientToScreen` 对齐客户区，并处理 DPI 缩放；
- 限帧（如 60fps）重绘，避免空转吃 CPU；
- 已知限制：**独占全屏下会被游戏盖住**（用户使用无边框窗口，已装 `Mods\ExtraPatches\BorderlessWindow.txt`）；
- 与已装参考 MOD（`EDF5_damage_number_display.dll`，hook D3D11 Present + ImGui，占用根目录 `imgui.ini`）**无渲染冲突**，因本方案不碰 D3D。

---

## 5. 显示过滤链（每帧，按开销从低到高短路）

1. 对象有效、存活（`hp > 0`）；
2. **未满血**：`curHp < maxHp`；满血直接跳过，不做后续任何计算与遮挡检测；
3. 头顶世界坐标有效 → 投影到屏幕，`z > 0`（相机前方）；
4. 与相机前方向夹角 ≤ `MaxViewAngle`（默认 60°，半角）；
5. 最大显示距离 `MaxDistance` 与最小屏幕尺寸 `MinScreenSize`（规避"不需要距离判断"带来的满屏小血条与 draw call 爆炸）；
6. 每帧绘制上限 `MaxBars`；
7. 遮挡检测（可关，默认开）：**仅对已受伤且通过前面过滤的敌人**执行；遮挡利用游戏自身射线函数（EDF6 已有 `docs\raycast-re.md` 可参照），命中环境/建筑/地形则隐藏；需按命中对象类型/队伍过滤自身与其他敌人（原生引擎无 layer mask）。

---

## 6. 头顶定位

- 优先用对象包围盒/骨骼半尺寸取顶部（EDF6 `tools/mrab.py` 已能给骨骼 index/parent/半尺寸/中心）；
- 兜底：对象原点 + `HeightOffset`；
- 必须支持巨大敌人与 Boss（固定偏移会让埃尔基努斯级血条插进身体）。

---

## 7. 配置项（`Mods\Plugins\EDF5EnemyHealthBar.ini`，热重载 + 范围校验并记录）

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | 1 | 总开关 |
| `ToggleKey` | 0 | 开关热键（虚拟键码，0 = 关） |
| `BarColor` | 绿 | 前景色（可扩展为按血量百分比渐变） |
| `BarWidth` / `BarHeight` | 50 / 5 | 像素；随分辨率缩放 |
| `MaxViewAngle` | 60 | 半角（度） |
| `OcclusionCheck` | 1 | 遮挡检测开关 |
| `HeightOffset` | 0.9 | 兜底头顶偏移（米） |
| `MaxDistance` | 待定（如 300） | 最大显示距离 |
| `MinScreenSize` | 待定 | 小于此屏幕尺寸不画 |
| `MaxBars` | 待定 | 每帧上限 |
| `DrawFps` | 60 | 重绘限帧 |
| `LogLevel` / `Debug` | 0 | 诊断输出 |

原文档的"颜色/高度/宽度/视野角/遮挡开关"五项全部保留，其余为新增。

---

## 8. 交付物

1. `EDF5EnemyHealthBar.dll`（静态链接 CRT 或依赖系统 VC 运行库，**无额外第三方 DLL**）；
2. `EDF5EnemyHealthBar.ini`（默认配置，自动生成）；
3. 使用说明（安装路径、配置项含义、已知限制：独占全屏不可用、需无边框窗口）；
4. P0 探针 DLL 与探针日志（作为数据源证据留存）。

---

## 9. 已知风险与待验证项

| 风险 | 处置 |
|---|---|
| EDF5 对象偏移与 EDF4.1 不同 | P0 探针实测，不假设 |
| 父对象聚合字段可能不存在/不是总量 | P0 交叉核对（用户已选父字段口径，若证伪则回退为部位求和） |
| 游戏自身射线函数难定位 | 遮挡做成可关 + P2 交付；第一步先不做遮挡上线 |
| 游戏更新导致地址失效 | AOB 签名 + 哈希校验，不匹配则拒绝加载 |
| Windows Defender 误报/拦截 | 本机 Defender 当前阻止读取 `EDF5_damage_number_display.dll`（报"contains a virus"），实时防护开启；需给游戏目录加排除项，否则新插件同样可能被拦或删 |
| 联机非主机端血量同步 | P2 实机联机验证 |
| 载具内/瞄准镜相机与主相机不同 | P0 分别 dump 相机候选，运行时始终跟随当前渲染相机 |
| 首次实机崩溃 | 所有 hook 包 `__try/__except`；`EML5_Load` 任一步失败即返回 false；先只装必要 hook |

---

## 10. 实施阶段

- **P0 探针（只读）**：dump 对象表 / 血量 / 部位 / 相机 / 包围盒 → 用户实机跑一局回传日志 → 钉死数据源。
- **P1 绘制管道**：投影正确 + 单个测试矩形稳定跟随某个敌人（无过滤、无遮挡）。
- **P2 完整过滤 + 配置 + 遮挡 + 巨大敌人高度 + 上限/距离**。
- **P3 打磨交付**：联机与载具内实测、性能、README、默认 ini、探针证据归档。

---

## 11. 追加要求（v3）：血量口径必须是「累积总有效生命值」

**用户要求原文要点**：血条必须反映"敌人多久会死"，即总有效生命值，而不是任何部位耐久。
青蛙人（Colonist/Deiroi）的头/躯干/四肢部位耐久是从总血量按比例算出来的，攻击部位同时在消耗总血量；
小灰人（Cosmonaut/BigGreyBoss）护甲未破时本体免疫，但全身上下累积到一定总伤害就会死亡，该总伤害池独立存在、不随部位再生恢复。
反例（必须避免）：打部位血条不动、打护甲血条乱动、部位再生后血条回涨。

### 11.1 数据源结论：`[obj+0x1FC]`（当前）/ `[obj+0x1F8]`（最大）就是该累积池

游戏自己的伤害应用函数 `EDF5.exe+0x2DB370`（唯一调用者 `0x2DACCF`，位于伤害类型 switch 的第一支）：

```
0x2DB496  movss  xmm0, [rdi+0x1FC]   ; 当前值
0x2DB4B1  movss  xmm1, [rdi+0x1F8]   ; 上限
0x2DB4B9  subss  xmm1, xmm0
0x2DB641  addss  xmm0, [rdi+0x1FC]   ; ← 参考 MOD 的 AOB 特征码落点
0x2DB649  minss  xmm0, [rdi+0x1F8]   ; 钳到最大总血量
0x2DB651  maxss  xmm0, [rdi+0x1F4]   ; 钳到下限
0x2DB659  movss  [rdi+0x1FC], xmm0   ; 写回 = 累积结果
```

即"伤害 = clamp(cur + delta, 下限, 最大)"，写回 `+0x1FC`。

旁证：参考 MOD `refs\EDF5-Damage-Number-Display\EDF5_damage_number_display.cpp:184`
`d->life = *(float*)(target + 0x1fc) + dmg;`（第 461 行用于显示）——它读的**就是同一个字段**，
并在自己的 hook 里 `+dmg` 回推"打之前"的值；**它并不维护独立的伤害累积字典**，因为该字段本身就是累积结果。

因此本 MOD **不 hook 伤害函数**：每帧轮询该字段的结果，与 hook 它等价，且不向游戏代码写入任何补丁
（联机/反作弊暴露面更小，也不需要 Detours/MinHook）。

### 11.2 已发现的结构性风险：部位对象确实自带血量字段

用 `tools\edf5_ctor_classes.py` 把血量块构造函数 `EDF5.exe+0x2D9A10` 的 37 个直接调用点映射到 RTTI 类名，
其中 4 个是**部位类**（即它们各自拥有 `[+0x1F4/+0x1F8/+0x1FC]`）：

| 部位类 | 构造调用点 |
|---|---|
| `.?AVDeiroiFootParts_Base@@` | EDF5.exe+0x1E5833 |
| `.?AVHumanoid_BigGreyBoss_PartsBase@@` | EDF5.exe+0x25D3BB |
| `.?AVMovingFortress_PartsBase@@` | EDF5.exe+0x28AB75 |
| `.?AVUfoMother511PartsBase@@` | EDF5.exe+0x2C01CA |

其余 33 个是实体根/基类（`HumanBase` `InsectBase` `MonsterBase` `VehicleBase` `TransporterBase`
`Deiroi` `AlienTrailer` `Nephila` `MovingFortress` `UfoMother511` `UfoSmall507/515` `UfoCarrier` `GiantAntEgg` …）。

**结论**：风险不在"字段读错"，而在"**部位对象是否也被登记进我们遍历的队伍表**"。
若被登记，血条就会画部位耐久 → 必须过滤。

### 11.3 判定手段：实机取证（已实现，Debug=1 时生效）

| 日志行 | 含义 |
|---|---|
| `[hp] 新对象 <ptr> class=<RTTI类名> max=… cur=…` | 队伍表里每个对象的类名与总血量（首次出现，上限 300 行） |
| `[hp] <ptr> <类名> a -> b (max …)` | 当前总血量发生变化（打腿/打护甲/再生都能看出来） |
| `[bar] 画 <ptr> class=<类名> …` | 真正画出条的对象属于哪个类（上限 120 行）← **定性关键** |
| `[frame N] 敌人条=… 队伍表对象=… 遮挡射线=…` | 新增"队伍表对象"总数，用于判断队伍表里有没有混入部位 |

判定规则：
- 若 `[bar]` 全是 `Deiroi` / `Humanoid_*Boss` / `Nephila` 等根对象 → 队伍表只有根实体，`+0x1FC` = 累积池，口径正确，收工。
- 若出现 `...Parts...` 的 `[bar]` 行 → 部位混入队伍表 → 加"按类名过滤部位"配置项（默认开），只保留根对象。

### 11.4 验收测试（用户实机）

选一关有青蛙人（Deiroi）/小灰人（BigGreyBoss）的：

1. **只打腿** 2~3 发 → 日志里同一 `<ptr>` 的 `cur` 应下降，血条应缩短；
2. **打小灰人护甲**（本体免疫阶段）→ 根对象的 `cur` 应下降（该总伤害池独立存在）；
3. **等部位再生** → 该 `<ptr>` 的 `cur` **不得回涨**（若回涨说明读成了部位耐久）；
4. **打死** → `cur`→0，血条消失；
5. 对照 `[bar]` 行确认每一条血条背后都是根对象而不是 `...Parts...`。

