# EDF5 Enemy Healthbar — v1.2.0-pre (pre-release)

Enemy health bars above enemy heads for **EARTH DEFENSE FORCE 5**.
Native x64 C++ plugin for EDFModLoader. No game file is modified, no game code is patched.

## ⚠️ TESTING STATUS — READ BEFORE USING

> **This mod has NOT been tested at any reasonable level of intensity, in either
> single-player or multiplayer (online co-op).**
>
> Only light, short, offline manual testing by the author on a handful of missions,
> in windowed / borderless-windowed mode.
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
> **Use at your own risk.** This is an experimental pre-release. The mod only *reads* game
> memory and draws its own overlay window, and it deliberately does not patch game code or
> write to game objects — but that is a design intent, not a tested guarantee.
> **Do not use it online unless you accept the risk.**

## What's new in v1.2.0

- **Fixed: large "building-type" enemies showing no bar (teleport towers).**
  The teleport tower is the `GeneratorPoll` class. Its origin lies *inside* its own body, so the
  occlusion ray always hit the tower itself (logged hit fraction `0.9211`) and the bar was
  suppressed as "hidden behind a wall". New `OcclusionSelfFrac` (default `0.90`): a hit at or
  beyond that fraction is treated as the target itself, not as an occluder.
- **Stops drawing when the game loses focus** — the overlay is hidden and collection is paused
  (no raycasts/enumeration while you are alt-tabbed).
- **HP-based colours**: `BarColorMid` / `BarColorLow`, thresholds `LowHpPercent=60` /
  `CriticalHpPercent=20`.
- **Better readability**: defaults changed to a "teammate-bar-like" look —
  `BarWidth 50→42`, `BarHeight 5→7`, `BackgroundAlpha 0→150` (dark backing for the unfilled part),
  new `BarOutlinePx=2`.
- **Optional size adaptation**: `BarWidthByMaxHp` (length scales with max HP, sqrt, clamped 1–3×)
  and `BarScaleByDistance` + `DistanceRef` (shrinks with distance, clamped 0.5–1×).
- **New troubleshooting guide**: `docs/EDF5-排查手册-某类目标没显示血条.md` —
  a three-step method (`[hp]` → `[bar] 画` → which filter) for "enemy type X has no bar",
  with the teleport-tower case study.
- A fully commented example config ships as `EDF5EnemyHealthBar.ini.example`.

## Install

1. Put `EDF5EnemyHealthBar.dll` into `...\EARTH DEFENSE FORCE 5\Mods\Plugins\`
   (EDFModLoader required).
2. Launch the game — a commented `EDF5EnemyHealthBar.ini` is created next to the DLL.
3. **Use windowed or borderless-windowed mode**: with exclusive fullscreen a layered overlay
   window cannot be displayed.

## Performance notes

- Snapshot published once per game frame (~58/s at the 60 fps cap); overlay snapshot age ≤ 1 frame.
- The overlay only erases the previous bars' small rects and uploads just the dirty region.
- Occlusion results are reused across frames (`OcclusionReuse`, default 2).
- Recommended for daily play: `Debug=0`.

## Known limitations

1. **Invisible in exclusive fullscreen** (inherent to the layered-window approach).
2. Online co-op, vehicles and scoped weapons remain **unverified**.
3. The "total effective HP vs body-part durability" question is settled by disassembly evidence,
   but in-game confirmation on multi-part enemies (Colonist / BigGreyBoss) is still pending.

See [README.md](https://github.com/linersadre-jpg/EDF5-Enemy-Healthbar/blob/master/README.md)
and the `docs/` folder for details.
