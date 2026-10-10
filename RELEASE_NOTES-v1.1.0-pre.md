# EDF5 Enemy Healthbar — v1.1.0-pre (pre-release)

Enemy health bars above enemy heads for **EARTH DEFENSE FORCE 5**.
Native x64 C++ plugin for EDFModLoader. No game file is modified, no game code is patched.

## ⚠️ TESTING STATUS — READ BEFORE USING

> **This mod has NOT been tested at any reasonable level of intensity, in either
> single-player or multiplayer (online co-op).**
>
> Only light, short, offline manual testing was performed by the author, on a handful of
> missions, in windowed / borderless-windowed mode.
>
> | Area | Status |
> |---|---|
> | Single-player, short sessions | ⚠️ Smoke test only — **not** a soak test |
> | Long sessions / many missions in a row | ❌ Not tested |
> | **Online co-op (any mode)** | ❌ **Not tested at all** |
> | Inside vehicles / scoped weapons | ❌ Not tested |
> | Highest difficulties / giant enemies / bosses | ❌ Not tested |
> | Coexistence with other mods | ❌ Not tested |
> | Exclusive fullscreen | ❌ Known limitation: overlay invisible |
>
> **Use at your own risk.** This is an experimental pre-release.
> The mod only *reads* game memory and draws its own overlay window, and it deliberately does
> not patch game code or write to game objects — but that is a design intent, not a tested
> guarantee. **Do not use it online unless you accept the risk.**

## 中文：测试声明（必读）

**本 MOD 没有经过任何合理强度的单人 / 多人测试。**
仅由作者在**窗口化 / 无边框窗口**、**离线**下对少量关卡做过短暂冒烟测试。
**联机（任何模式）完全未测试**，载具内 / 瞄准镜 / 最高难度 / 连续多关卡 / 与其它 MOD 共存均未测试。
**一切风险自负**。本 MOD 只读取游戏内存并自绘覆盖层，设计上不 patch 游戏代码、不修改游戏对象，
但这是设计意图而非经验证的保证——**不愿承担风险请不要在联机时使用**。

## Highlights

- Health bar above each enemy's head, showing the **accumulated total HP** the game's own
  damage function writes (object `+0x1F8` max / `+0x1FC` current) — not a body-part's durability.
- Only shown for **living, damaged, in-view** enemies; optional occlusion by buildings/terrain.
- Occlusion uses the **game's own raycast** (`EDF5.exe+0x74F110`), reachable only after
  replicating the game's private per-thread Havok setup on the render thread — no code patching.
- **Friendly units never hide bars**: a hit is ignored when it resolves to a character unit.
- Fully configurable via `EDF5EnemyHealthBar.ini` next to the DLL.

## Install

1. Put `EDF5EnemyHealthBar.dll` into
   `...\EARTH DEFENSE FORCE 5\Mods\Plugins\` (EDFModLoader required).
2. Launch the game. A config file `EDF5EnemyHealthBar.ini` is created next to the DLL.
3. **Use windowed or borderless-windowed mode** — with exclusive fullscreen a layered
   overlay window cannot be shown.

## Performance (measured)

- Snapshot published once per game frame (~58/s at 60 fps cap); overlay snapshot age ≤ 1 frame.
- Overlay only erases the previous bars' small rects and uploads only the dirty region.
- Occlusion results are reused across frames (`OcclusionReuse`, default 2).
- Recommended for daily play: `Debug=0` (turns off the diagnostic log).

## Known limitations

1. **Invisible in exclusive fullscreen** (inherent to the layered-window approach).
2. Online co-op, vehicles, scoped weapons: **unverified**.
3. The "total effective HP vs body-part durability" question is settled by disassembly
   evidence, but **in-game confirmation on multi-part enemies (Colonist / BigGreyBoss)
   is still pending**.

## Full documentation

See [README.md](../README.md) and the `docs/` folder in the repository.
