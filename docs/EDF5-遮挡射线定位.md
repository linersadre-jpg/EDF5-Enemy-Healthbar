# EDF5 遮挡（视线）检测：逆向定位结果

版本：EDF5.exe SHA256 `3512D2A2E61D532C5D12DC0C5FD1AC61F6743E13349AE21301BB8449DD4BAE5F`（19,896,320 字节，ImageBase 0x140000000）。全部为静态分析结论，**尚未实机验证**。

## 0. 结论（一句话）

EDF5 用的是 **Havok（hkp 世代）+ xgs 封装层**，不是 EDF6 的 hknp。游戏自己的射线查询入口是：

```
void castRay(void* world /*rcx*/, const RayQuery* q /*rdx*/, Collector* col /*r8*/)   // RVA 0x74F110
```

`0x74F110` 有 11 个直接调用者：`0x118621 / 0x1186DE / 0x1195DE / 0x119700 / 0x11987A / 0x119BFE / 0x119D19 / 0x1F9B62 / 0x368FDD / 0x3CEC4F / 0x7E3E85`。

过滤器 `filterInfo = 0x16`（layer 22）就是 EDF6 笔记里说的「打地图专用层」——只与 `{15,16,17,18,19,20,26}` 层碰撞（地图对象/建筑/地形），**天然忽略所有敌人和角色**。这正是血条遮挡想要的语义。

## 1. 入参结构（从 `0x3CEB10`、`0x368EB0`、`0x118510` 三处调用点归纳）

```
struct RayQuery {          // 大小 0x28 或 0x30
    float from[4];         // +0x00 起点（w 一般写 1.0）
    float to[4];           // +0x10 终点
    uint32_t unk20;        // +0x20 常见为 0
    uint32_t filterInfo;   // +0x24 0x16 = 只打地图；0 = 打一切
};
```

- `0x3CEB10`：`mov dword ptr [rsp + 0x74], 0x16`（query 在 `rsp+0x50`，故 `+0x24`）
- `0x368EB0`：`mov dword ptr [rsp + 0x64], ecx(0)`（query 在 `rsp+0x40`）
- `0x118510`：`mov dword ptr [rsp + 0x74], 0x16`（query 在 `rsp+0x50`）

## 2. 收集器 Collector

游戏侧用的是自己的收集器（`hit_query` 名字空间），栈上 0x80 字节，布局：

| 偏移 | 类型 | 含义 | 证据 |
|---|---|---|---|
| +0x00 | ptr | vtable = `0xEBCB70`（`?AUFrontClosestRayHitCollector@hit_query@@`）或 `0xEB6C00` | `0x3CEC3B lea rax,[rip+0xaedf2e]` → 0xEBCB70 |
| +0x08 | float | 1.0f | `[rbp-0x78] = 0x3f800000` |
| +0x20 | float | 命中比例（初值 1.0f） | `[rbp-0x60] = 0x3f800000`，命中后读它做插值 |
| +0x24 | i32 | -1 | `[rbp-0x5c] = 0xffffffff` |
| +0x30 | i32 | -1 | `[rbp-0x50] = 0xffffffff` |
| +0x60 | qword | **命中标记 / 命中结果指针（0 = 没打到）** | `0x3CEC54 cmp qword ptr [rbp-0x20], 0 ; je`；`0x368FE2 cmp qword ptr [rbp-0x30], 0 ; setne al` |

命中点 = `from + (to − from) × [col+0x20]`，见 `0x3CEC5F`–`0x3CEC79`。

RTTI 里可用的收集器类（`tools\edf5rtti.py grep`）：
- `.?AUFrontClosestRayHitCollector@hit_query@@` vtable 0xEBCB70（构造点唯一：`0x3CEC3B`，所在函数 `0x3CEB10..0x3CEDF0`）
- `.?AVTargetCollector@hit_query@@` vtable 0xE93A58，构造点 `0xAB720`（**EDF4.1 自瞄插件用的就是这一类**，共 20 个调用者：`0xA6F71 / 0x11AD61 / 0x11B165 / 0x11B661 / 0x141EC8 / 0x17F7AC / 0x18952B / 0x1A5171 / 0x1A66F1 / 0x1A7DC1 / 0x1AB331 / 0x2151F0 / 0x25C6E8 / 0x2D8502 / 0x305810 / 0x38B3FF / 0x38F6B9 / 0x39E33C / 0x3C1DEF`）
- `.?AUObjectCreatePosCollector@@` vtable 0xE98D20（对应 EDF6 的 NpObjectCreatePosCollector）

## 3. world 指针的来源（三处样本）

| 出处 | world 表达式 |
|---|---|
| `0x11861D`（函数 `0x118510`，签名带 xmm3 标量 + 第 4 参） | `rcx = [[EDF5.exe+0x125AB68] + 0x18]` |
| `0x368FD6`（函数 `0x368EB0`，直升机激光/着地检测） | `rcx = [obj + 0x1D8]` |
| `0x3CEC4F`（函数 `0x3CEB10`） | `rcx = [[ctx + 0x10] + 0x10]` |

注意 `EDF5.exe+0x125AB68` 与 TeamManager 全局 `EDF5.exe+0x125AB80`（= +0x18）相邻；也正好是参考 MOD 玩家链的第一跳全局。**哪个是通用可用的 world，需要用探针实测**（见 §5）。

## 4. 与 EDF4.1 / EDF6 的对照

- EDF4.1 插件（`work\edf41-playeraim\src\plugin.cpp`）用的是同一个 `hit_query::TargetCollector` + 一个 `CastRay` 封装（`EDF41.exe+0xEC700`，特征码 `48 89 5C 24 10 57 48 83 EC 70 48 8B 05 AF 81 B4`）。**该特征码在 EDF5.exe 里不成立**——纯前缀 `48 89 5C 24 10 57 48 83 EC 70` 命中 11 处，加上 `48 8B 05` 后剩 4 处（`0x281420 / 0x3DC250 / 0x3DC390 / 0x3DC880`），逐个反汇编发现全是普通栈 cookie 函数，不是 CastRay。所以走 RTTI + `0x74F110` 调用者这条路才对。
- EDF6 是 **hknp** 世代（`hknpWorld::castRay`，vtable 槽 54 `+0x1B0`，包装 `0x11A7EE0`，`wrapper = *(image+0x20B2958)+0x10`，`filterInfo = 0x16`）。EDF5 的 RTTI 里 **`hknp*` 类为 0 个**，只有 `hkp*` / `hkcd*` / `hkai*`，说明 EDF5 是更老的 hkp。
- 但两代的 **filterInfo 都是 `0x16` / layer 22**，语义一致 → 可以直接照搬「打地图」这个思路。

## 5. 需要探针实测的事项（P0.5）

1. 三个 world 候选哪个在任意时刻都非空、且指向同一个 `hkpWorld`（可比较指针是否相等；`hkpWorld` vtable 应为 `0xCDD8B0`，可校验 `*(world) == image+0xCDD8B0`）。
2. 从玩家头顶向 30 m 前方（必然打到地形/建筑）发一条 `filterInfo = 0x16` 的射线，确认 `col+0x60 != 0`；再改成打敌人方向验证敌人**不会**被命中。
3. 收集器构造：直接照抄 `0x3CEB10` 的写法（vtable 0xEBCB70 + `[+0x08]=1.0f`、`[+0x20]=1.0f`、`[+0x24]=-1`、`[+0x30]=-1`、`[+0x60]=0`），不调用游戏析构。

## 6. 线程规则（重要，影响 MOD 架构）

EDF6 笔记 §4 明确：这些射线函数本身不加锁，因为**游戏自己就在主/渲染线程上这么调**。因此本 MOD 必须：

- 在 **Present 钩子（游戏渲染线程）** 里完成：读敌人对象 → 算可见性 → 发遮挡射线 → 写快照；
- 覆盖层窗口线程只读快照、只负责画。

参考 MOD `EDF5-Damage-Number-Display` 正是这么做的（钩 `IDXGISwapChain::Present` 并直接读敌人内存），说明 EDF5 的 Present 所在的线程可以安全访问这些对象。

## 7. 附带确认的对象布局

`0x368EB0` 里用了 `[obj+0x90]`（方向/基向量）与 `[obj+0xA0]`（位置），与探针第二轮结论一致：**对象内嵌 3x4 矩阵在 `obj+0x80..0xAF`，位置 = `obj+0xA0 / +0xA4 / +0xA8`**（平移行 w=1.0 在 +0xAC）。

## 8. 复现命令

```
py -3 tools\edf5rtti.py grep hkpWorld
py -3 tools\edf5rtti.py grep hit_query
py -3 tools\edf5re.py xref 0xEBCB70
py -3 tools\edf5re.py callers 0x74F110
py -3 tools\edf5re.py func 0x3CEB10
py -3 tools\edf5re.py func 0x118510
py -3 tools\edf5re.py dis 0x368EB0 --n 60
py -3 tools\edf5pe.py bytes 0xEE67E8 4    # float 10000.0 = 100 m 上限（0x3CEB10 用的阈值）
```
