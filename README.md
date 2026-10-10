# EDF5 Enemy Healthbar

Enemy health bars for **EARTH DEFENSE FORCE 5** (地球防卫军 5), drawn above enemy heads.
Native x64 C++ plugin loaded by [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader).
**It never modifies any game file and patches no game code.**

> ## ⚠️ TESTING DISCLAIMER — READ FIRST
>
> **This mod has NOT been tested at any reasonable level of intensity, in either single-player
> or multiplayer (online co-op).**
>
> What has actually been exercised is only light, short manual testing by the author on a few
> missions, in **windowed / borderless-windowed** mode, offline:
>
> | Area | Test status |
> |---|---|
> | Single-player, short sessions, a handful of missions | ⚠️ Light smoke test only — **not** a soak test |
> | Long sessions / many missions in a row | ❌ **Not tested** |
> | Online co-op (any mode) | ❌ **Not tested at all** |
> | Inside vehicles / scoped weapons / other cameras | ❌ **Not tested** |
> | Highest difficulties / giant enemies / boss fights | ❌ **Not tested** |
> | Interaction with other mods | ❌ **Not tested** |
> | Exclusive fullscreen | ❌ Known limitation — the overlay is invisible (see below) |
>
> **Use at your own risk.** Treat this as a pre-release / experimental build.
> Multiplayer is completely unverified: the mod only *reads* game memory and draws its own
> overlay window, and it deliberately does **not** patch game code or write to game objects,
> but that is a design intent, not a tested guarantee. **Do not use it online if you are not
> willing to accept the risk.**
>
> Bug reports with the log file (`Mods\Plugins\EDF5EnemyHealthBar.log`, set `Debug=1`) are welcome.

**Current version: `v1.2.0-pre` — pre-release, see [CHANGELOG.md](CHANGELOG.md).**

---

## 中文说明（Chinese）

给《地球防卫军 5》做的**敌人头顶血条** MOD：原生 x64 C++ 插件，由 EDFModLoader 加载，
放进 `Mods\Plugins\` 即可。**不修改游戏任何文件、不向游戏代码写补丁。**

### ⚠️ 测试免责声明（重要）

**本 MOD 没有经过任何合理强度的单人 / 多人测试。**

实际只做过：作者在**窗口化 / 无边框窗口**模式、**离线**下，对少量关卡做了短暂的冒烟测试。

| 项目 | 测试状态 |
|---|---|
| 单人、短时间、少量关卡 | ⚠️ 仅冒烟测试，**不等于**压力测试 |
| 长时间游戏 / 连续打很多关 | ❌ **未测试** |
| 联机多人（任何模式） | ❌ **完全未测试** |
| 载具内 / 瞄准镜 / 其它相机 | ❌ **未测试** |
| 最高难度 / 巨型敌人 / Boss | ❌ **未测试** |
| 与其它 MOD 共存 | ❌ **未测试** |
| 独占全屏 | ❌ 已知限制：覆盖层不可见（见下） |

**一切风险自负**，请当作预发布 / 实验版看待。联机完全未验证：本 MOD 只**读取**游戏内存并自绘一个
覆盖层窗口，设计上**不 patch 游戏代码、不修改游戏对象**，但这是设计意图，不是经过测试的保证。
**如果你不愿承担风险，请不要在联机时使用。**

### 功能

- 敌人头顶按血量百分比画血条（仅敌方、仅未满血、仅视野内）；
- **遮挡检测**：被建筑/地形挡住就不画；**友军等角色单位不会遮蔽血条**；
- 血量读的是对象的 `+0x1F8`(最大) / `+0x1FC`(当前) —— 游戏伤害函数写回的**累积总生命值**；
- 配置文件 `EDF5EnemyHealthBar.ini` 放在 DLL 同目录，改完重进关卡生效。

### 安装

```
E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\Mods\Plugins\EDF5EnemyHealthBar.dll
```

放进去即可，ModLoader 会自动加载。**显示模式必须用「无边框窗口」或「窗口化」** ——
独占全屏下分层窗口不可见（这是该方案的固有代价）。

### 主要配置项

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | 1 | 总开关（可配热键） |
| `MaxViewAngle` | 60 | 视野半角（度） |
| `OcclusionCheck` | 1 | 遮挡检测总开关 |
| `OcclusionIgnoreCharacters` | 1 | 友军/角色单位不算遮挡 |
| `OcclusionReuse` | 2 | 遮挡结果分帧复用（同一对象每 N 帧才重投射线） |
| `OcclusionSelfFrac` | 0.90 | 命中比例 ≥ 该值视为打到目标自身，不算遮挡（大型建筑型目标必需） |
| `DrawFps` | 0 | 覆盖层重绘上限；`0` = 不限，跟随游戏帧 |
| `MaxBars` | 64 | 同屏最多画几条 |
| `Debug` | 1 | 往同目录写日志（日常玩建议设 0） |

完整配置项与排查方法见 **[docs/EDF5敌人血条MOD-使用说明.md](docs/EDF5敌人血条MOD-使用说明.md)**。

### 构建

```bat
cd projects\edf5-enemy-healthbar
cmd /c scripts\build3.cmd
copy build\EDF5EnemyHealthBar.dll "…\EARTH DEFENSE FORCE 5\Mods\Plugins\"
```

需要 MSVC（`vcvars64.bat`，`/std:c++17`，x64），直驱 `cl.exe`/`link.exe`，不使用 CMake。

### 文档

| 文档 | 内容 |
|---|---|
| [docs/EDF5敌人血条MOD-使用说明.md](docs/EDF5敌人血条MOD-使用说明.md) | 安装、全部配置项、日志怎么读、排查、已知问题 |
| [CHANGELOG.md](CHANGELOG.md) | 版本历史与被修掉的关键 bug |
| [docs/EDF5-血条延迟与卡顿-分析与优化方案.md](docs/EDF5-血条延迟与卡顿-分析与优化方案.md) | 延迟/卡顿的代码级根因与优化（已完成项 + 实测数据） |
| [docs/EDF5-敌人血条-MOD-需求修订.md](docs/EDF5-敌人血条-MOD-需求修订.md) | 需求与决策（含"血量口径必须是累积总有效生命值"的完整论证） |
| [docs/EDF5-遮挡射线定位.md](docs/EDF5-遮挡射线定位.md) | 遮挡射线逆向：入口、query/collector 布局、per-thread Havok 现场 |
| [docs/EDF5-P0-静态定位结果.md](docs/EDF5-P0-静态定位结果.md) | 对象偏移、队伍表、伤害函数、血量字段 |
| [docs/EDF5-复用原生NPC血条-调研与方案.md](docs/EDF5-复用原生NPC血条-调研与方案.md) | "改用游戏原生血条"的调研与被证伪过程 |
| [docs/EDF5-排查手册-某类目标没显示血条.md](docs/EDF5-排查手册-某类目标没显示血条.md) | **排查手册**：某类目标没血条时的三步判别法 + 传送塔案例 |
| [docs/P0探针-使用说明.md](docs/P0探针-使用说明.md) | 只读探针的用法 |

### 已知限制

1. **独占全屏下看不到血条**（分层窗口方案的固有代价）→ 用无边框窗口 / 窗口化。
2. 联机、载具内、瞄准镜相机**均未验证**（见上文免责声明）。
3. ~~传送塔这类"非冲突对象"没有血条~~ —— 已解决：传送塔的类名是 `GeneratorPoll`，它在敌方队伍里，缺条原因是遮挡自命中（见 `OcclusionSelfFrac`）。
4. 血量的"总有效生命值 vs 部位耐久"口径，已用反汇编证据确认读的是前者，
   但在多部位敌人（青蛙人 / 小灰人）上的**实机确认仍未完成**。

## License

[MIT](LICENSE)
