// EDF5 敌人血条 MOD —— 共享定义（偏移 / 内存助手 / 配置 / 快照）
#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// EDF5.exe 偏移（静态逆向 + 探针实测确认）
//   血量  : 0x2D9A10 构造函数写 [+0x1F8]=最大 / [+0x1FC]=当前 / [+0x1F4]=下限
//   位置  : 对象内嵌 3x4 矩阵 @ [+0x80..0xAF]，平移行（世界坐标，w=1.0）在 [+0xA0]
//   队伍  : [+0x218] 作为队伍关系数组下标
//   射线  : EDF5.exe+0x74F110(world, query, collector)，filterInfo 0x16 = 只打地图
// ---------------------------------------------------------------------------
namespace off {
	// 全局指针
	constexpr std::uintptr_t kTeamManagerPtr = 0x125AB80;  // TeamManager*
	constexpr std::uintptr_t kPlayerChain    = 0x125AB68;  // -> ... -> 玩家对象
	constexpr std::uintptr_t kCameraChain    = 0x125B080;  // -> ... -> 相机 {view@+0x80, proj@+0xE0}
	constexpr std::uintptr_t kWindowW        = 0x1256C00;  // int，渲染宽
	constexpr std::uintptr_t kWindowH        = 0x1256C04;  // int，渲染高
	constexpr std::uintptr_t kSwapChainPtr   = 0x1256C98;  // IDXGISwapChain*
	constexpr std::uintptr_t kWorldGlobal    = 0x125AB68;  // 物理世界候选：*(global) + 0x18

	// TeamManager
	constexpr std::size_t kRows  = 0x38;   // 队伍行数组
	constexpr std::size_t kCount = 0x48;   // 队伍数

	// 队伍行
	constexpr std::size_t kRowStride = 0x38;
	constexpr std::size_t kRelations = 0x18;  // int[teamCount]，1=友军 2=敌军
	constexpr std::size_t kRowHead   = 0x00;  // std::map 头节点

	// 对象
	constexpr std::size_t kMinHp   = 0x1F4;  // float 下限
	constexpr std::size_t kMaxHp   = 0x1F8;  // float 最大总血量
	constexpr std::size_t kCurHp   = 0x1FC;  // float 当前总血量
	constexpr std::size_t kTeam    = 0x218;  // int  队伍
	constexpr std::size_t kPos     = 0xA0;   // float[3] 世界坐标（3x4 矩阵平移行）
	constexpr std::size_t kLockOn  = 0x268;  // -> +0x08 -> {+0x10,+0x14,+0x18} 锁定/中心点
	constexpr std::size_t kWorldOf = 0x1D8;  // 部分对象自带的物理世界指针

	// MSVC std::map (_Tree) 节点
	constexpr std::size_t kNodeLeft   = 0x00;
	constexpr std::size_t kNodeParent = 0x08;
	constexpr std::size_t kNodeRight  = 0x10;
	constexpr std::size_t kNodeIsNil  = 0x19;
	constexpr std::size_t kNodeKey    = 0x20;

	// 物理射线
	constexpr std::uintptr_t kCastRay      = 0x74F110;  // void(world, query*, collector*)
	// 游戏射线函数开头 TlsGetValue 用的 TLS 索引槽（由 0x13EE0 里的 TlsAlloc 写入）。
	// 该槽只在游戏自己的工作线程上被填过，渲染线程上是 NULL → 0x74F13F 崩。
	constexpr std::uintptr_t kTlsTraceIndex = 0x11DA464;
	constexpr std::uintptr_t kCollectorVtbl = 0xEBCB70;  // ?AUFrontClosestRayHitCollector@hit_query@@
	constexpr std::uintptr_t kHkpWorldVtbl = 0xCDD8B0;  // ?AVhkpWorld@@
	constexpr std::uintptr_t kXgsSystemVtbl = 0xEE0A00; // ?AVSystem@havok@xgs@@
	constexpr std::uint32_t  kFilterMapOnly = 0x16;     // layer 22：只与地图/建筑/地形碰撞

	constexpr int kRelationEnemy = 2;
	constexpr int kRelationAlly  = 1;
}

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
extern std::uintptr_t g_base;
extern std::uintptr_t g_baseEnd;
extern std::string    g_dir;
extern HMODULE        g_self;
extern volatile LONG  g_stop;

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
struct Config {
	int      enabled        = 1;       // 总开关
	int      toggleKey      = 0;       // 0 = 无热键；否则 VK 码（0x77 = F8）
	uint32_t color          = 0x00FF00;// 血条颜色 RRGGBB
	int      barWidth       = 50;
	int      barHeight      = 5;
	int      barOutline     = 1;       // 细描边（深色 1px），提升可读性
	int      barOutlinePx   = 2;       // 描边厚度（像素）
	int      bgAlpha        = 0;       // 背景不透明度 0..255，0 = 不画背景
	float    maxViewAngle   = 60.0f;   // 视野半角（度）
	int      occlusion      = 1;       // 遮挡检测
	float    heightOffset   = 2.0f;    // 锚点上方额外抬升（米）
	int      scaleHeight    = 1;       // 按最大血量放大抬升（巨型敌人）
	int      useLockOn      = 1;       // 有锁定点就优先用锁定点当锚点
	float    maxDistance    = 0.0f;    // 0 = 不限
	float    minScreenSize  = 0.0f;    // 0 = 不限；否则要求投影后的身高像素 >= 该值
	int      maxBars        = 64;
	int      drawFps        = 60;
	float    eyeHeight      = 1.5f;    // 遮挡射线起点（玩家眼睛高度）
	int      rayFilter      = 0x16;    // 射线 filterInfo：0x16=只打地图/建筑/地形
	float    occlBackoff    = 0.0f;    // 射线终点回收距离（米）；0=保持原行为（实测遮挡正常）
	int      occlIgnoreChars= 1;       // 1 = 角色单位（含友军）不算遮挡，只有地图/建筑才挡血条
	int      occlReuse      = 2;
	float    occlSelfFrac   = 0.90f;   // 命中比例 >= 该值视为打到目标自身，不算遮挡（大型建筑型敌人必须放宽）       // 遮挡结果分帧复用：同一对象每 N 帧才重投一次射线（1=每帧都投）
	int      extraObjectScan= 0;       // 1 = 分帧扫描"带血量块"的表外目标（如传送塔）；2 = 并绘制出来
	uint32_t colorMid       = 0xFFFF00;// 中等血量颜色（黄）
	uint32_t colorLow       = 0xFF0000;// 低血量颜色（红）
	float    lowHpPct       = 60.0f;   // 低于该百分比用 colorMid
	float    critHpPct      = 20.0f;   // 低于该百分比用 colorLow
	int      widthByHp      = 0;       // 1 = 血条长度随最大血量放大（sqrt，夹到 1~3 倍）
	int      scaleByDist    = 0;       // 1 = 血条随距离缩放（越远越小，夹到 0.5~1 倍）
	float    distRef        = 30.0f;   // 距离缩放的参考距离（米）
	int      overlayMode    = 0;       // 0=跟随游戏窗口 1=覆盖整个桌面
	int      debug          = 0;
	int      nativeProbe    = 0;       // C1 只读探针：1=dump 字段/容器 2=再加 SoldierBase 测试 3=再加虚表扫描
	int      drawBars       = 1;       // 0 = 不画我们自绘的覆盖层血条（采集/探针继续跑），用于原生条对照测试
};

extern Config g_cfg;

// ---------------------------------------------------------------------------
// 快照（游戏线程写，覆盖层线程读）
// ---------------------------------------------------------------------------
struct BarRect {
	float    x, y;      // 左上角（渲染分辨率像素）
	float    w, h;
	float    frac;      // 0..1 血量百分比
	uint32_t color;     // 0xRRGGBB
	int      outline;
};

constexpr int kMaxBars = 256;

struct Snapshot {
	volatile LONG seq;
	int      count;
	int      screenW, screenH;
	BarRect  bars[kMaxBars];
};

extern Snapshot          g_snap;
extern CRITICAL_SECTION  g_snapLock;
extern HANDLE            g_snapEvent;   // 游戏线程每发布一帧快照就 SetEvent，覆盖层等它唤醒
extern volatile LONG     g_pubCount;    // 快照发布次数（诊断刷新率）
extern volatile LONG     g_drawCount;   // 覆盖层实际绘制次数
extern volatile DWORD    g_pubTick;     // 最近一次发布的时刻（诊断延迟）

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
void LogInit();
void LogClose();
void Log(const char* fmt, ...);
void LogFlush();

// ---------------------------------------------------------------------------
// 内存助手
// ---------------------------------------------------------------------------
inline bool Readable(const void* p, std::size_t n) {
	if (!p || n == 0) return false;
	const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
	// 区间缓存：采集一帧要读几十个对象的十几个字段，原来每次都调 VirtualQuery（内核调用），
	// 一帧上千次就是毫秒级开销（而且发生在游戏渲染线程上）。同一批对象几乎都落在少数几个
	// region 里，缓存"上次成功查询到的区间"可以把调用次数降到个位数。
	// 只在游戏渲染线程上调用（采集全部在 Present 钩子里），故不加锁；缓存只记成功结果。
	static std::uintptr_t cLo = 0, cHi = 0;
	if (cLo != 0 && a >= cLo && n <= cHi - a) return true;
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
	if (mbi.State != MEM_COMMIT) return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
	const std::uintptr_t rstart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
	const std::uintptr_t rend   = rstart + mbi.RegionSize;
	if (a + n > rend) return false;
	cLo = rstart;
	cHi = rend;
	return true;
}

template <class T>
inline bool Rd(std::uintptr_t a, T& out) {
	if (a == 0 || !Readable(reinterpret_cast<const void*>(a), sizeof(T))) return false;
	std::memcpy(&out, reinterpret_cast<const void*>(a), sizeof(T));
	return true;
}

inline bool RdPtr(std::uintptr_t a, std::uintptr_t& out) {
	void* p = nullptr;
	if (!Rd<void*>(a, p)) return false;
	out = reinterpret_cast<std::uintptr_t>(p);
	return true;
}

inline bool RdF(std::uintptr_t a, float& out) { return Rd<float>(a, out); }
inline bool RdI(std::uintptr_t a, std::int32_t& out) { return Rd<std::int32_t>(a, out); }

inline std::uintptr_t Chain(std::uintptr_t base, std::initializer_list<std::size_t> offs) {
	std::uintptr_t p = base;
	for (std::size_t o : offs) {
		std::uintptr_t q = 0;
		if (!RdPtr(p + o, q)) return 0;
		p = q;
	}
	return p;
}

// MSVC RTTI：对象 -> 类名（形如 .?AVxxx@@），失败返回空串
inline std::string ClassName(std::uintptr_t obj) {
	std::uintptr_t vt = 0;
	if (!RdPtr(obj, vt) || vt < g_base || vt > g_baseEnd) return {};
	std::uintptr_t col = 0;
	if (!RdPtr(vt - 8, col) || col < g_base || col > g_baseEnd) return {};
	std::int32_t sig = 0;
	if (!RdI(col, sig) || (sig != 0 && sig != 1)) return {};
	std::uint32_t tdRva = 0;
	if (!Rd<std::uint32_t>(col + 0x0C, tdRva)) return {};
	const std::uintptr_t td = g_base + tdRva;
	// 注意：TypeDescriptor 的头 8 字节是 type_info 的 vtable 指针（实测 0x0000000140D35ED0），
	// 不是 0/1 签名。曾把它当 32 位 sig 校验，导致所有对象都取不到类名（日志里全是 class=?）。
	char name[256] = {};
	if (!Readable(reinterpret_cast<const void*>(td + 0x10), sizeof name)) return {};
	std::memcpy(name, reinterpret_cast<const void*>(td + 0x10), sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	if (name[0] != '.' || name[1] != '?' || name[2] != 'A') return {};
	return std::string(name);
}

// 从"虚表地址"直接反查类名（类普查用；跳过"取对象首字段"那一步）
inline std::string ClassNameOfVtable(std::uintptr_t vt) {
	if (vt < g_base || vt > g_baseEnd) return {};
	std::uintptr_t col = 0;
	if (!RdPtr(vt - 8, col) || col < g_base || col > g_baseEnd) return {};
	std::int32_t sig = 0;
	if (!RdI(col, sig) || (sig != 0 && sig != 1)) return {};
	std::uint32_t tdRva = 0;
	if (!Rd<std::uint32_t>(col + 0x0C, tdRva)) return {};
	const std::uintptr_t td = g_base + tdRva;
	char name[256] = {};
	if (!Readable(reinterpret_cast<const void*>(td + 0x10), sizeof name)) return {};
	std::memcpy(name, reinterpret_cast<const void*>(td + 0x10), sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	if (name[0] != '.' || name[1] != '?' || name[2] != 'A') return {};
	return std::string(name);
}

// 简名：.?AVHumanoidPartsBreak@@ -> HumanoidPartsBreak（失败返回 "?"）
inline std::string ShortClassName(std::uintptr_t obj) {
	const std::string m = ClassName(obj);
	if (m.size() < 5) return "?";
	std::size_t i = 3;                                  // 跳过 ".?A"
	if (m[i] == 'V' || m[i] == 'U') ++i;                // class / struct
	const std::size_t at = m.find('@', i);
	const std::string s = m.substr(i, (at == std::string::npos ? m.size() : at) - i);
	return s.empty() ? "?" : s;
}

// 简单的键值 ini 读取（key=value，支持 # / ; 注释）
std::string IniGet(const std::string& path, const std::string& key, const std::string& def);
void IniWriteDefault(const std::string& path);
void LoadConfig(const std::string& path);

// 覆盖层（EHB_overlay.cpp）
bool OverlayStart(HWND gameWnd);
void OverlayStop();
void OverlaySetGameWindow(HWND hwnd);
HWND OverlayGameWindow();
