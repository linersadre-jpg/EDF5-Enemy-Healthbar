# AGENTS.md — 给下一次接手这个项目的 AI 的说明书

> 这份文件是**用返工换来的**。请先读完再动手；它记录的坑每一条都真实发生过，
> 而且多数是"看起来很小、代价很大"的类型（曾让作者在传送塔问题上白绕五、六轮实机验证）。

---

## 0. 这个项目是什么

《地球防卫军 5》(EDF5) 的**敌人头顶血条** MOD：原生 x64 C++17 插件，由 EDFModLoader 加载，
`Mods\Plugins\EDF5EnemyHealthBar.dll`。实现方式：**只读**游戏内存 + 自绘一个分层覆盖层窗口。
当前版本 v1.2.0。

## 1. 硬约束（违反会出事故，不要试探）

| 约束 | 原因 |
|---|---|
| **绝不 patch 游戏代码、绝不写入游戏对象** | 用户要打**联机**；只读是这条路线的前提 |
| 遮挡射线必须**先构造游戏私有的 per-thread Havok 现场**（`EDF5.exe+0x96B50` 的四步配方） | 直接调 `0x74F110` 会 `0xC0000005`（在 `+0x74F13F` 崩） |
| 不要走"C1：复用游戏原生血条"路线 | 已证伪：`HudPlayer_FollowerDurability` 实例数在三次跨关卡扫描中都是 **0** |
| 独占全屏下无解 | 分层窗口(`WS_EX_LAYERED`)在独占全屏不可见；只支持窗口化/无边框窗口 |
| 游戏运行时不要覆盖 DLL | 文件被占用；先确认 `EDF5.exe` 没在跑 |

## 2. 构建 / 部署循环（照抄，别自创）

```bat
cd projects\edf5-enemy-healthbar
cmd /c scripts\build3.cmd          :: 必须经 cmd；脚本是 CRLF + 纯 ASCII
:: 输出 build\EDF5EnemyHealthBar.dll
copy build\EDF5EnemyHealthBar.dll  "E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\Mods\Plugins\"
del  "...\Mods\Plugins\EDF5EnemyHealthBar.log"      :: 想要干净日志就先删
```

- 构建脚本**只能** `cmd /c` 调用。历史上把它改成 LF 换行 + 中文注释后，cmd 会把中文 `rem` 当命令执行并报错。
- 编译前必须 `vcvars64.bat`；直驱 `cl.exe`/`link.exe`，**不要**引入 CMake。
- 用户负责实机测试；**AI 必须自己读日志**（`Mods\Plugins\EDF5EnemyHealthBar.log`，UTF-8，用
  `encoding="utf-8", errors="replace"` 读），**绝不要让用户复制粘贴日志内容**。

## 3. 诊断协议（最重要的一节）

### 3.1 三步判别法：某类目标"没有血条"时

| 顺序 | 看什么 | 结论 |
|---|---|---|
| 1 | 有没有 `[hp] … class=Xxx` 行 | 有 → **枚举和血量读取都没问题**，问题在过滤/绘制；没有 → 才怀疑枚举 |
| 2 | 有没有 `[bar] 画 … class=Xxx` 行 | 有 → 问题在绘制层（位置/尺寸/颜色）；没有 → 被某个过滤器挡了 |
| 3 | 逐个排除过滤器 | 见下表 |

**先用 ini 翻转做对照，绝不先写代码**：

| ini 改动 | 排除什么 |
|---|---|
| `OcclusionCheck=0` | 遮挡判定 ← **传送塔就栽在这里** |
| `MaxViewAngle=120` | 视野半角 |
| `MaxDistance=0` `MinScreenSize=0` | 距离/尺寸过滤 |
| `MaxBars=256` | 条数上限 |
| 打它几下 | "满血不画"（**这是设计，不是 bug**） |

### 3.2 探针必须"自我证明"

一个探针只打印"命中 0"是**没有信息量**的。必须能区分：

- 探针**没启动**（前置条件不满足）；
- 启动了但**没读到内存**（遍历/权限问题）；
- 读到了但**匹配失败**（白名单/偏移错）；
- 匹配了但**字段校验失败**（校验太严）。

### 3.3 不要让用户重复跑同一个场景

一份日志一次要拿全所需信息。曾出现"跑了三轮才发现：夹取把探针模式改成 0 / 扫描在关卡加载前就结束 /
白名单漏类"——**那几轮全是 AI 自己的 bug，不是新信息**。

## 4. 已知陷阱清单（现象 → 根因 → 修法）

| 现象 | 根因 | 修法 |
|---|---|---|
| **大型建筑型目标（传送塔）没血条** | 遮挡**自命中**：塔的原点在身体内部，射线必然打到它自己（日志 `命中比例=0.9211`） | `OcclusionSelfFrac=0.90`：命中比例 ≥ 该值视为打到目标自身 |
| 血条延迟约 0.3 秒 | `DrawFps=0` 被 `if (g_cfg.drawFps < 5) g_cfg.drawFps = 5;` 夹成 5 | 改为 `if (g_cfg.drawFps != 0 && … < 5)` |
| 覆盖层线程 100% 空转 | "内容未变 → `continue`" 没有等待 | 用 auto-reset 事件 `WaitForSingleObject` 驱动 |
| 启动 `0xC0000409`（栈校验失败） | `char desc[64]` 装不下 `sizeof(DXGI_SWAP_CHAIN_DESC)==72` | 用真实结构体；Collector 0x80→0x200、RayQuery→0x80 |
| 关卡切换后 `0xC0000005`（`+0xAE71E6`） | `g_world` 已失效 | 周期性重新获取 world + `g_worldRestale` + 3 次容错 |
| 所有对象 `class=?` | RTTI 解析把 TypeDescriptor[0:8]（其实是 `type_info` 虚表指针）当成 0/1 签名校验 | 去掉该校验；`td+0x10` 才是类名 |
| 探针"设了不生效" | 新增了模式值 4，却忘了放宽 `if (nativeProbe > 3) = 0` 的夹取 | 加模式时**同步改夹取**并检查启动日志里的 `config/config2` 行 |
| 内存扫描命中 0 | 扫描在**关卡加载前**就跑完（对象还不存在） | 条件改成"队伍表有对象才扫"，并**每 15 秒重扫**（新生成的敌人） |
| 92 类"带血量块"白名单扫描 | 按 RTTI 继承关系生成的清单**漏了 `UfoCarrier508`/`GiantAnt`** 等确定存在的类 | 可信度不足，`ExtraObjectScan` **默认关**，仅作备用 |
| 后期日志里没有 `[hp]`/`[bar]` | 采样行数预算（`kHpLineMax`）被前面的目标吃光 | 分区预算（对象 512 / 行 600） |
| 内存扫描每帧卡顿 | 一次全量扫描约 1.8 秒 | 分帧增量（每帧字节预算）+ 区域剩余部分记录进度 |

### PowerShell / 工具链的坑（做自动化时高频踩）

- `Invoke-WebRequest`（PS 5.1）**必须加 `-UseBasicParsing`**，否则在无 IE 配置的机器上抛 NullReference；
- PowerShell **双引号字符串里不要出现 ASCII 双引号**（写中文注释时极易踩），一律用单引号字面量；
- 用 `.NET String.Replace`/正则改源码后，**必须回读确认替换命中**：换行符（CRLF vs LF）会让 `Replace` **静默失败**，
  这次就因此漏过 3 处替换，导致"计数器全是 0"的假日志；
- 删/移文件前先解析并打印**绝对路径**再动手。

## 5. 关键地址与偏移（详见 `docs/`）

| 项 | 值 | 文档 |
|---|---|---|
| 队伍表 TeamManager | `0x125AB80`（`+0x38` rows、`+0x48` count、stride `0x38`） | `EDF5-P0-静态定位结果.md` |
| 对象血量 | `+0x1F4` min / `+0x1F8` max / `+0x1FC` cur（**累积总生命值**口径） | 同上 + `EDF5-敌人血条-MOD-需求修订.md` §11 |
| 遮挡射线 | `EDF5.exe+0x74F110`，`RayFilter=0x16`，需私有 Havok 现场 | `EDF5-遮挡射线定位.md` |
| 传送塔的真实类名 | **`.?AVGeneratorPoll@@`**（名字完全不提示"塔"；在敌方队伍里，`max=2637.6`） | `EDF5-排查手册-某类目标没显示血条.md` |
| exe 指纹 | 19,896,320 字节，`ImageBase 0x140000000`，SHA256 `3512D2A2…` | `EDF5-敌人血条-逆向线索与P0计划.md` |

## 6. 发布约定

- 仓库结构：`src/` `docs/` `scripts/` `tools/` `dist/`，根目录含 `README.md` `LICENSE`(MIT)
  `CHANGELOG.md` `EDF5EnemyHealthBar.ini.example`。
- GitHub token 存放在 `F:\deepseek-harness\default-workspace\secrets\github-token.txt`
  （工作区根目录**不是** git 仓库，不会被提交）。SSH 只能推送、**不能建库**。
- 发布必须勾 **pre-release**，且发布说明**必须保留**"未经过合理强度的单人/多人测试"的免责声明。

## 7. 已经走过、不要再走的弯路

1. **"目标没血条 → 它没被枚举"** 这个默认假设。传送塔当时**已经在日志里**（`[hp] GeneratorPoll`），
   真正的原因在**同一份日志的 `[occl]` 行**。先查日志，再动手。
2. **用新工程去验证旧假设**：先做一个"只打日志"的探针无可厚非，但必须先想清楚
   "哪种输出能区分哪几种可能"，否则只是把用户的实机时间当调试器。
3. **C1 路线**（复用游戏原生 NPC 血条）：被证伪，不要重启。
4. **靠类名白名单枚举对象**：漏类且不可自证，除非能给出完整性证明，否则别用。
