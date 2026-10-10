// EDF5 敌人血条 MOD —— 插件主体
//
// 架构：
//   游戏渲染线程（Present 钩子里）：枚举敌人 -> 过滤 -> 投影 -> 遮挡射线 -> 写快照
//   覆盖层线程（本 DLL 自己的窗口）：读快照 -> 分层窗口自绘
//
// 一切游戏内存访问都发生在游戏线程，覆盖层线程只碰自己的快照。
//
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>      // 必须用真正的 DXGI_SWAP_CHAIN_DESC：sizeof 是 72 不是 64

#include "EHB_common.h"
#include "EHB_damageable.h"
#include "PluginAPI.h"

#include <cmath>

// ---------------------------------------------------------------------------
// 全局
// ---------------------------------------------------------------------------
HMODULE       g_self  = nullptr;
std::uintptr_t g_base  = 0;
std::uintptr_t g_baseEnd = 0;
std::string   g_dir;
volatile LONG g_stop = 0;

Config g_cfg;

Snapshot         g_snap = {};
CRITICAL_SECTION g_snapLock;
HANDLE           g_snapEvent = nullptr;   // 游戏线程发布快照后 SetEvent，唤醒覆盖层
volatile LONG    g_pubCount  = 0;
volatile LONG    g_drawCount = 0;
volatile DWORD   g_pubTick   = 0;

static FILE* g_log = nullptr;
static unsigned long long g_logBytes = 0;
static CRITICAL_SECTION   g_logLock;
static bool               g_logLockReady = false;
constexpr unsigned long long kLogMaxBytes = 8ull * 1024 * 1024;   // 日志上限 8MB

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
void LogInit() {
	if (!g_logLockReady) {
		InitializeCriticalSection(&g_logLock);
		g_logLockReady = true;
	}
	if (g_log) return;
	const std::string path = g_dir + "\\EDF5EnemyHealthBar.log";
	fopen_s(&g_log, path.c_str(), "wb");
	// 无缓冲：进程如果被 /GS / 崩溃直接带走，缓冲区里的日志不会丢
	if (g_log) setvbuf(g_log, nullptr, _IONBF, 0);
	g_logBytes = 0;
}

void LogClose() {
	if (!g_logLockReady) return;
	EnterCriticalSection(&g_logLock);
	if (g_log) { fclose(g_log); g_log = nullptr; }
	LeaveCriticalSection(&g_logLock);
}

void LogFlush() {
	if (!g_logLockReady) return;
	EnterCriticalSection(&g_logLock);
	if (g_log) fflush(g_log);
	LeaveCriticalSection(&g_logLock);
}

void Log(const char* fmt, ...) {
	if (!g_log || !g_logLockReady) return;
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
	va_end(ap);

	// 连续打很多关也不会把日志写成几百 MB：超过上限就截断重开
	EnterCriticalSection(&g_logLock);
	if (g_log && g_logBytes > kLogMaxBytes) {
		const std::string path = g_dir + "\\EDF5EnemyHealthBar.log";
		fclose(g_log);
		g_log = nullptr;
		if (fopen_s(&g_log, path.c_str(), "wb") == 0 && g_log) {
			setvbuf(g_log, nullptr, _IONBF, 0);
			g_logBytes = 0;
			fputs("[log] 超过 8MB，已截断重开\n", g_log);
		}
	}
	if (g_log) {
		fputs(buf, g_log);
		fputc('\n', g_log);
		g_logBytes += std::strlen(buf) + 1;
	}
	LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
static std::string Trim(const std::string& in) {
	std::size_t b = 0, e = in.size();
	while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) ++b;
	while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\r' || in[e - 1] == '\n')) --e;
	return in.substr(b, e - b);
}

std::string IniGet(const std::string& path, const std::string& key, const std::string& def) {
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return def;
	char line[1024];
	std::string result = def;
	while (fgets(line, sizeof line, f)) {
		std::string s = Trim(line);
		if (s.empty() || s[0] == ';' || s[0] == '#') continue;
		const std::size_t eq = s.find('=');
		if (eq == std::string::npos) continue;
		if (Trim(s.substr(0, eq)) == key) {
			std::string v = Trim(s.substr(eq + 1));
			const std::size_t sc = v.find(';');
			if (sc != std::string::npos) v = Trim(v.substr(0, sc));
			if (!v.empty()) result = v;
			break;
		}
	}
	fclose(f);
	return result;
}

static long ParseInt(const std::string& s, long def) {
	if (s.empty()) return def;
	char* end = nullptr;
	const long v = std::strtol(s.c_str(), &end, 0);  // 支持 0x 前缀
	return (end == s.c_str()) ? def : v;
}

static float ParseFloat(const std::string& s, float def) {
	if (s.empty()) return def;
	char* end = nullptr;
	const float v = std::strtof(s.c_str(), &end);
	return (end == s.c_str()) ? def : v;
}

static std::uint32_t ParseColor(const std::string& s, std::uint32_t def) {
	std::string t = s;
	if (!t.empty() && t[0] == '#') t.erase(0, 1);
	if (t.size() != 6) return def;
	std::uint32_t v = 0;
	for (char c : t) {
		v <<= 4;
		if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
		else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
		else return def;
	}
	return v;
}

void IniWriteDefault(const std::string& path) {
	if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return;
	fputs(
		"; EDF5 敌人血条 MOD 配置\r\n"
		"; 修改后重新进入关卡或重启游戏生效（热键开关立即生效）\r\n"
		"\r\n"
		"; ---- 总开关 ----\r\n"
		"Enabled=1\r\n"
		"; 开关热键，VK 码；0 = 不设热键。例如 F8 = 0x77，F9 = 0x78\r\n"
		"ToggleKey=0\r\n"
		"\r\n"
		"; ---- 外观 ----\r\n"
		"; 血条颜色 RRGGBB\r\n"
		"BarColor=00FF00\r\n"
		"BarWidth=42\r\n"
		"BarHeight=7\r\n"
		"; 1 = 给血条加深色描边（厚度见 BarOutlinePx）\r\n"
		"BarOutline=1\r\n"
		"; 描边厚度（像素）\r\n"
		"BarOutlinePx=2\r\n"
		"; 背景不透明度 0..255；0 = 不画背景。建议 140~180：未填充部分是深色，对比度好很多\r\n"
		"BackgroundAlpha=150\r\n"
		"\r\n"
		"; ---- 显示条件 ----\r\n"
		"; 视野半角（度）\r\n"
		"MaxViewAngle=60\r\n"
		"; 遮挡检测：被建筑/地形挡住就不画（1 开 0 关）\r\n"
		"OcclusionCheck=1\r\n"
		"; 最大显示距离（米）；0 = 不限\r\n"
		"MaxDistance=0\r\n"
		"; 最小的屏幕身高（像素）；低于这个值就不画。0 = 不限\r\n"
		"MinScreenSize=0\r\n"
		"; 同屏最多画多少条\r\n"
		"MaxBars=64\r\n"
		"\r\n"
		"; ---- 头顶定位 ----\r\n"
		"; 锚点上方额外抬升（米）\r\n"
		"HeightOffset=2.0\r\n"
		"; 1 = 血量越厚的敌人抬得越高（巨型/Boss 血条不会埋在身体里）\r\n"
		"ScaleHeightByMaxHp=1\r\n"
		"; 1 = 优先用游戏自己的锁定点当锚点（更贴合多部位敌人）\r\n"
		"UseLockOnPoint=1\r\n"
		"\r\n"
		"; ---- 其它 ----\r\n"
		"; 覆盖层重绘上限（fps）。0 = 不限，直接跟随游戏帧（转视角不拖影，推荐）\r\n"
		"DrawFps=0\r\n"
		"; 遮挡射线起点：玩家眼睛离地高度（米）\r\n"
		"EyeHeight=1.5\r\n"
		"; 遮挡射线的 filterInfo。0x16 是游戏自己多处使用的值（另见 0x17），\r\n"
		"; 但是否忽略角色单位未经验证；0 = 打一切。\r\n"
		"RayFilter=0x16\r\n"
		"; 射线终点往回收的距离（米）。0 = 保持原行为（实测遮挡判定正常，默认 0）\r\n"
		"; 只有日志里出现大量 `[occl] 挡住 … 命中比例=0.99x`（打到敌人自己）时才调大。\r\n"
		"OcclusionBackoff=0.0\r\n"
		"; 1 = 友军等角色单位不算遮挡（只有地图/建筑/地形才挡血条）；0 = 关掉这个判断。\r\n"
		"; 判断依据：命中对象可读 + 有合法队伍号 + 能取到类名 + 不是 hk*/xgs* 等引擎类型。\r\n"
		"; 若发现血条穿墙显示，就把这里改成 0，并把日志里的 `[occl] 挡住` 行发出来。\r\n"
		"OcclusionIgnoreCharacters=1\r\n"
		"; 遮挡结果分帧复用：同一对象每 N 帧才重新投一次射线，中间沿用上次结果。\r\n"
		"; 1 = 每帧都投（最准）；2~3 可省 1/2~1/3 的射线开销，肉眼几乎无差别。\r\n"
		"OcclusionReuse=2\r\n"
		"; 命中比例 >= 该值视为打到目标自身，不算遮挡（传送塔这类大型建筑型敌人必须放宽，否则会被自己挡住）\r\n"
		"OcclusionSelfFrac=0.90\r\n"
		"; ---- 颜色随血量 ----\r\n"
		"; 血量 <= LowHpPercent 用 BarColorMid；<= CriticalHpPercent 用 BarColorLow\r\n"
		"BarColorMid=FFFF00\r\n"
		"BarColorLow=FF0000\r\n"
		"LowHpPercent=60\r\n"
		"CriticalHpPercent=20\r\n"
		"; 1 = 血条长度随最大血量放大（以普通兵 81 血为基准，sqrt 缩放并夹到 1~3 倍）\r\n"
		"BarWidthByMaxHp=0\r\n"
		"; 1 = 血条随距离缩小（以 DistanceRef 米为基准，夹到 0.5~1 倍）\r\n"
		"BarScaleByDistance=0\r\n"
		"DistanceRef=30.0\r\n"
		"; 覆盖层定位方式：0 = 跟随游戏窗口；1 = 覆盖整个桌面（游戏窗口找不准时用这个）\r\n"
		"OverlayMode=0\r\n"
		"; 1 = 往 DLL 同目录写调试日志（首次测试建议保持 1）\r\n"
		"Debug=1\r\n"
		"; ---- C1 原生血条调研（只读，不创建任何游戏对象）----\r\n"
		"; 0 = 关；1 = dump 字段/容器；2 = 再加 SoldierBase 转换测试（仍只读）\r\n"
		"; 3 = 再加：按虚表 0xEC92F8 扫描进程私有内存，找出游戏自己的原生条实例\r\n"
		"NativeProbe=0\r\n"
		"; 0 = 不画我们自绘的覆盖层血条（采集与探针照常工作）。做原生条对照测试时设 0\r\n"
		"DrawBars=1\r\n",
		f);
	fclose(f);
}

void LoadConfig(const std::string& path) {
	IniWriteDefault(path);
	g_cfg.enabled      = ParseInt(IniGet(path, "Enabled", "1"), 1) != 0;
	g_cfg.toggleKey    = static_cast<int>(ParseInt(IniGet(path, "ToggleKey", "0"), 0));
	g_cfg.color        = ParseColor(IniGet(path, "BarColor", "00FF00"), 0x00FF00);
	g_cfg.barWidth     = static_cast<int>(ParseInt(IniGet(path, "BarWidth", "50"), 50));
	g_cfg.barHeight    = static_cast<int>(ParseInt(IniGet(path, "BarHeight", "5"), 5));
	g_cfg.barOutline   = ParseInt(IniGet(path, "BarOutline", "1"), 1) != 0;
	g_cfg.barOutlinePx = static_cast<int>(ParseInt(IniGet(path, "BarOutlinePx", "2"), 2));
	if (g_cfg.barOutlinePx < 0) g_cfg.barOutlinePx = 0;
	if (g_cfg.barOutlinePx > 4) g_cfg.barOutlinePx = 4;
	g_cfg.bgAlpha      = static_cast<int>(ParseInt(IniGet(path, "BackgroundAlpha", "0"), 0));
	g_cfg.maxViewAngle = ParseFloat(IniGet(path, "MaxViewAngle", "60"), 60.0f);
	g_cfg.occlusion    = ParseInt(IniGet(path, "OcclusionCheck", "1"), 1) != 0;
	g_cfg.maxDistance  = ParseFloat(IniGet(path, "MaxDistance", "0"), 0.0f);
	g_cfg.minScreenSize= ParseFloat(IniGet(path, "MinScreenSize", "0"), 0.0f);
	g_cfg.maxBars      = static_cast<int>(ParseInt(IniGet(path, "MaxBars", "64"), 64));
	g_cfg.heightOffset = ParseFloat(IniGet(path, "HeightOffset", "2.0"), 2.0f);
	g_cfg.scaleHeight  = ParseInt(IniGet(path, "ScaleHeightByMaxHp", "1"), 1) != 0;
	g_cfg.useLockOn    = ParseInt(IniGet(path, "UseLockOnPoint", "1"), 1) != 0;
	g_cfg.drawFps      = static_cast<int>(ParseInt(IniGet(path, "DrawFps", "60"), 60));
	g_cfg.eyeHeight    = ParseFloat(IniGet(path, "EyeHeight", "1.5"), 1.5f);
	g_cfg.rayFilter    = static_cast<int>(ParseInt(IniGet(path, "RayFilter", "0x16"), 0x16));
	g_cfg.occlBackoff  = ParseFloat(IniGet(path, "OcclusionBackoff", "0.0"), 0.0f);
	if (g_cfg.occlBackoff < 0.0f) g_cfg.occlBackoff = 0.0f;
	if (g_cfg.occlBackoff > 5.0f) g_cfg.occlBackoff = 5.0f;
	g_cfg.occlIgnoreChars = ParseInt(IniGet(path, "OcclusionIgnoreCharacters", "1"), 1) != 0;
	g_cfg.occlReuse    = static_cast<int>(ParseInt(IniGet(path, "OcclusionReuse", "2"), 2));
	if (g_cfg.occlReuse < 1)  g_cfg.occlReuse = 1;
	if (g_cfg.occlReuse > 10) g_cfg.occlReuse = 10;
	g_cfg.occlSelfFrac = ParseFloat(IniGet(path, "OcclusionSelfFrac", "0.90"), 0.90f);
	if (g_cfg.occlSelfFrac < 0.5f) g_cfg.occlSelfFrac = 0.5f;
	if (g_cfg.occlSelfFrac > 0.999f) g_cfg.occlSelfFrac = 0.999f;
	g_cfg.colorMid    = ParseColor(IniGet(path, "BarColorMid", "FFFF00"), 0xFFFF00);
	g_cfg.colorLow    = ParseColor(IniGet(path, "BarColorLow", "FF0000"), 0xFF0000);
	g_cfg.lowHpPct    = ParseFloat(IniGet(path, "LowHpPercent", "60"), 60.0f);
	g_cfg.critHpPct   = ParseFloat(IniGet(path, "CriticalHpPercent", "20"), 20.0f);
	if (g_cfg.lowHpPct  < 1.0f)  g_cfg.lowHpPct = 1.0f;
	if (g_cfg.lowHpPct  > 100.0f) g_cfg.lowHpPct = 100.0f;
	if (g_cfg.critHpPct < 1.0f)  g_cfg.critHpPct = 1.0f;
	if (g_cfg.critHpPct > g_cfg.lowHpPct) g_cfg.critHpPct = g_cfg.lowHpPct;
	g_cfg.widthByHp   = ParseInt(IniGet(path, "BarWidthByMaxHp", "0"), 0) != 0;
	g_cfg.scaleByDist = ParseInt(IniGet(path, "BarScaleByDistance", "0"), 0) != 0;
	g_cfg.distRef     = ParseFloat(IniGet(path, "DistanceRef", "30.0"), 30.0f);
	if (g_cfg.distRef < 1.0f) g_cfg.distRef = 1.0f;
	g_cfg.overlayMode  = static_cast<int>(ParseInt(IniGet(path, "OverlayMode", "0"), 0));
	if (g_cfg.overlayMode != 0 && g_cfg.overlayMode != 1) g_cfg.overlayMode = 0;
	g_cfg.debug        = ParseInt(IniGet(path, "Debug", "1"), 1) != 0;
	g_cfg.nativeProbe  = static_cast<int>(ParseInt(IniGet(path, "NativeProbe", "0"), 0));
	if (g_cfg.nativeProbe < 0 || g_cfg.nativeProbe > 4) g_cfg.nativeProbe = 0;
	g_cfg.drawBars     = ParseInt(IniGet(path, "DrawBars", "1"), 1) != 0;
	g_cfg.extraObjectScan = static_cast<int>(ParseInt(IniGet(path, "ExtraObjectScan", "0"), 0));
	if (g_cfg.extraObjectScan < 0 || g_cfg.extraObjectScan > 2) g_cfg.extraObjectScan = 0;

	if (g_cfg.barWidth  < 4)   g_cfg.barWidth = 4;
	if (g_cfg.barWidth  > 400) g_cfg.barWidth = 400;
	if (g_cfg.barHeight < 1)   g_cfg.barHeight = 1;
	if (g_cfg.barHeight > 100) g_cfg.barHeight = 100;
	if (g_cfg.maxBars   < 1)   g_cfg.maxBars = 1;
	if (g_cfg.maxBars   > kMaxBars) g_cfg.maxBars = kMaxBars;
	// 0 = 不限（跟随游戏帧），不能被夹成 5——那会让采集与覆盖层都掉到 5Hz（实测 0.3 秒延迟）
	if (g_cfg.drawFps != 0 && g_cfg.drawFps < 5) g_cfg.drawFps = 5;
	if (g_cfg.drawFps   > 240) g_cfg.drawFps = 240;
	if (g_cfg.bgAlpha   < 0)   g_cfg.bgAlpha = 0;
	if (g_cfg.bgAlpha   > 255) g_cfg.bgAlpha = 255;
}

// ---------------------------------------------------------------------------
// 相机：view/projection -> 世界转屏幕
//   与 EDF5-Damage-Number-Display 完全一致的数学（该 MOD 在本机这版游戏上工作正常）
// ---------------------------------------------------------------------------
static float g_vp[16]    = {};
static float g_tanHalfX  = 1.0f;
static float g_tanHalfY  = 1.0f;
static bool  g_vpValid   = false;

static void BuildViewProjection(const float* v, const float* p) {
	g_vp[0]  = v[0] * p[0];   g_vp[1]  = v[1] * p[0];   g_vp[2]  = v[2] * p[0];   g_vp[3]  = v[3] * p[0];
	g_vp[4]  = v[4] * p[5];   g_vp[5]  = v[5] * p[5];   g_vp[6]  = v[6] * p[5];   g_vp[7]  = v[7] * p[5];
	g_vp[8]  = v[8] * p[10];  g_vp[9]  = v[9] * p[10];  g_vp[10] = v[10] * p[10]; g_vp[11] = v[11] * p[10] + p[11];
	g_vp[12] = v[8] * p[14];  g_vp[13] = v[9] * p[14];  g_vp[14] = v[10] * p[14]; g_vp[15] = v[11] * p[14];

	g_tanHalfX = (std::fabs(p[0]) > 1e-6f) ? 1.0f / std::fabs(p[0]) : 1.0f;
	g_tanHalfY = (std::fabs(p[5]) > 1e-6f) ? 1.0f / std::fabs(p[5]) : 1.0f;
}

struct Projected {
	float sx, sy;      // 屏幕像素，左上原点
	float depth;       // 相机前方深度（= clip.w）
	float ndcX, ndcY;
};

static bool Project(const float pos[3], int w, int h, Projected& out) {
	const float cx = pos[0] * g_vp[0] + pos[1] * g_vp[1] + pos[2] * g_vp[2] + g_vp[3];
	const float cy = pos[0] * g_vp[4] + pos[1] * g_vp[5] + pos[2] * g_vp[6] + g_vp[7];
	const float cw = pos[0] * g_vp[12] + pos[1] * g_vp[13] + pos[2] * g_vp[14] + g_vp[15];
	out.depth = cw;
	if (!(cw > 0.1f)) return false;
	out.ndcX = cx / cw;
	out.ndcY = cy / cw;
	out.sx = -(static_cast<float>(w) * 0.5f * out.ndcX) + static_cast<float>(w) * 0.5f;
	out.sy = -(static_cast<float>(h) * 0.5f * out.ndcY) + static_cast<float>(h) * 0.5f;
	return true;
}

// ---------------------------------------------------------------------------
// 物理射线（EDF5.exe+0x74F110）
// ---------------------------------------------------------------------------
typedef void (*CastRayFn)(void* world, void* query, void* collector);

struct alignas(16) RayQuery {
	float    from[4];
	float    to[4];
	std::uint32_t unk20;
	std::uint32_t filter;
	std::uint8_t  pad[0x80 - 0x28];   // 留足余量：游戏可能读比我们已知更多的字段
};

// 游戏会往这个 collector 里写命中结果。EDF4.1 插件记录它的字段一直到 +0xC0，
// 所以绝不能只开 0x80 —— 写爆栈数组就是 /GS 崩游戏的经典姿势。
struct alignas(16) Collector {
	unsigned char d[0x200];
};

static bool CastRayHit(void* world, const float a[3], const float b[3], float* outFrac,
                       std::uintptr_t* outObj) {
	if (!world || !Readable(world, 0x40)) return false;
	Collector col;
	std::memset(&col, 0, sizeof col);
	*reinterpret_cast<void**>(col.d + 0x00)          = reinterpret_cast<void*>(g_base + off::kCollectorVtbl);
	*reinterpret_cast<float*>(col.d + 0x08)          = 1.0f;
	*reinterpret_cast<float*>(col.d + 0x20)          = 1.0f;
	*reinterpret_cast<std::int32_t*>(col.d + 0x24)   = -1;
	*reinterpret_cast<std::int32_t*>(col.d + 0x30)   = -1;
	*reinterpret_cast<std::uint64_t*>(col.d + 0x60)  = 0;

	RayQuery q;
	std::memset(&q, 0, sizeof q);
	q.from[0] = a[0]; q.from[1] = a[1]; q.from[2] = a[2]; q.from[3] = 1.0f;
	q.to[0]   = b[0]; q.to[1]   = b[1]; q.to[2]   = b[2]; q.to[3]   = 1.0f;
	q.unk20   = 0;
	q.filter  = static_cast<std::uint32_t>(g_cfg.rayFilter);

	reinterpret_cast<CastRayFn>(g_base + off::kCastRay)(world, &q, &col);

	// col.d+0x60 不是布尔标志，而是"命中对象指针"：游戏自己的调用者（EDF5.exe+0x118510）
	// 调用后就是 `cmp qword ptr [rbp-0x20], 0 / je`（[rbp-0x20] 即 collector+0x60）。
	const std::uint64_t hit = *reinterpret_cast<std::uint64_t*>(col.d + 0x60);
	if (outFrac) *outFrac = *reinterpret_cast<float*>(col.d + 0x20);
	if (outObj)  *outObj = static_cast<std::uintptr_t>(hit);
	return hit != 0;
}

// ---------------------------------------------------------------------------
// 异常过滤器：把崩溃点写清楚，便于定位
// ---------------------------------------------------------------------------
static int EhFilter(LPEXCEPTION_POINTERS ep) {
	static volatile LONG n = 0;
	const LONG c = InterlockedIncrement(&n);
	if (c <= 20) {
		const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
		Log("[!!] 采集异常 #%ld code=0x%08lX addr=%p (EDF5.exe+0x%llX)", c,
		    static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
		    reinterpret_cast<void*>(addr),
		    static_cast<unsigned long long>(addr - g_base));
		LogFlush();
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

// 游戏的射线函数依赖两套「只有游戏工作线程才有」的 per-thread Havok 现场，
// 从渲染线程直接调会连崩两次：
//   0x74F110 开头 TlsGetValue(0x11DA464) → 空 → 崩在 0x74F13F (mov r9,[rax+8])
//   0xB3DF20 里   TlsGetValue(0x11D37F0) → 空 → 崩在 0xB3DF85 (cmp [rax+0x10])
// 前者是 scope 计时器、后者是 hkThreadMemory（thread memory 分配器）。
//
// 好消息：游戏自己给了标准配方。EDF5.exe+0x96B50 是它的 AI 工作线程入口，开头就是：
//   0x96B8E  call 0x637190            ; hkThreadMemory::hkThreadMemory(this)，现场构造（约 0x80 字节）
//   0x96B94  call 0x646D50            ; rax = *(0x11D92D0) —— hkMemorySystem 单例
//   0x96BB4  rdx = &mem / r8 = "havokAI" / r9d = 3
//   0x96BBC  call [r10+0x18]          ; host->vt[3](host, &mem, "havokAI", 3) 登记并接上内部分配器
//   0x96BCD  call 0x646ED0            ; enter(&out, &mem)：装 TLS 0x11D37F0 + 建 scope 计时器到 0x11DA464
// 我们照抄这一遍，就在渲染线程上得到一套**完全私有**的 Havok 线程内存：
// 不跨线程共享、不产生数据竞争、不需要给游戏代码打任何补丁。
static volatile LONG g_occlBroken = 0;
static volatile LONG g_occlOk     = 0;
static volatile LONG g_occlFaults = 0;   // 累计射线异常次数
static volatile LONG g_worldRestale = 0; // 射线异常后请求丢弃缓存的世界指针
constexpr LONG kOcclFaultLimit = 3;      // 连续异常到这个数才永久关闭遮挡

namespace {
constexpr std::uintptr_t kHavokMemSystemGlobal = 0x11D92D0; // hkMemorySystem 单例指针
constexpr std::uintptr_t kThreadMemCtor        = 0x637190;  // hkThreadMemory 构造函数
constexpr std::uintptr_t kHavokThreadEnter     = 0x646ED0;  // Havok 线程进入
constexpr std::uintptr_t kHavokAiNameString    = 0xE933E8;  // "havokAI\0"
constexpr std::size_t    kThreadMemSize        = 0x100;     // 构造函数最多写到 +0x78，留足余量

alignas(64) std::uint8_t  g_threadMem[kThreadMemSize];
alignas(8)  std::uint64_t g_havokEnterOut[8];
volatile LONG g_havokState = 0; // 0=未尝试 1=成功 2=失败

bool InstallHavokThread() {
	const LONG st = InterlockedCompareExchange(&g_havokState, 0, 0);
	if (st == 1) return true;
	if (st == 2) return false;
	if (g_base == 0) return false;

	void* host = nullptr;
	bool ok = false;
	const bool exeMatches =
	    g_base != 0 &&
	    std::memcmp(reinterpret_cast<const void*>(g_base + kHavokAiNameString), "havokAI", 7) == 0;
	__try {
		if (!exeMatches) {
			// 版本自检失败：宁可不做遮挡，也不要拿着错的偏移去调游戏函数
			host = nullptr;
		} else {
			auto ctor = reinterpret_cast<void*(*)(void*)>(g_base + kThreadMemCtor);
			ctor(g_threadMem);

			host = *reinterpret_cast<void**>(g_base + kHavokMemSystemGlobal);
			if (host != nullptr) {
				void** vt = *reinterpret_cast<void***>(host);
				const std::uintptr_t vtAddr = reinterpret_cast<std::uintptr_t>(vt);
				const std::uintptr_t regAddr =
				    (vt != nullptr) ? reinterpret_cast<std::uintptr_t>(vt[3]) : 0;
				// 虚表必须落在游戏镜像里，vt[3] 必须落在 .text 里
				if (vtAddr >= g_base && vtAddr < g_baseEnd &&
				    regAddr >= g_base + 0x1000 && regAddr < g_base + 0xC97000) {
					auto reg = reinterpret_cast<void(*)(void*, void*, const char*, int)>(regAddr);
					reg(host, g_threadMem, reinterpret_cast<const char*>(g_base + kHavokAiNameString), 3);

					auto enter = reinterpret_cast<void(*)(void*, void*)>(g_base + kHavokThreadEnter);
					enter(g_havokEnterOut, g_threadMem);
					ok = true;
				}
			}
		}
	} __except (EhFilter(GetExceptionInformation())) {
		ok = false;
	}

	InterlockedExchange(&g_havokState, ok ? 1 : 2);
	if (ok) {
		Log("[occl] 已为渲染线程建立私有 Havok 线程内存（host=%p），遮挡检测启用", host);
	} else {
		Log("[occl] 无法为渲染线程建立 Havok 线程内存（host=%p），本次运行关闭遮挡检测", host);
	}
	LogFlush();
	return ok;
}
} // namespace

static bool SafeCastRay(void* world, const float a[3], const float b[3], float* outFrac,
                        std::uintptr_t* outObj = nullptr) {
	if (g_occlBroken || !world) return false;
	if (!InstallHavokThread()) return false;

	bool hit = false;
	__try {
		hit = CastRayHit(world, a, b, outFrac, outObj);
	} __except (EhFilter(GetExceptionInformation())) {
		// 单次异常不足以判死刑：换关卡/换世界时可能只是一次瞬时失效。
		// 先丢缓存世界 + 重建私有 Havok 线程内存再试，连续 kOcclFaultLimit 次才永久关闭。
		const LONG n = InterlockedIncrement(&g_occlFaults);
		if (n < kOcclFaultLimit) {
			Log("[occl] 射线异常第 %ld 次：丢弃缓存世界并重建 Havok 线程内存后继续", n);
			InterlockedExchange(&g_worldRestale, 1);
			InterlockedExchange(&g_havokState, 0);
		} else if (InterlockedExchange(&g_occlBroken, 1) == 0) {
			Log("[occl] 射线连续异常 %ld 次，本次运行永久关闭遮挡检测", n);
		}
		LogFlush();
		hit = false;
	}

	if (!g_occlBroken && InterlockedExchange(&g_occlOk, 1) == 0) {
		Log("[occl] 遮挡射线可用（私有 Havok 线程内存）");
		LogFlush();
	}
	return hit;
}

// 物理世界缓存
static void* g_world          = nullptr;
static int   g_worldAttempts  = 0;

static bool WorldLooksGood(std::uintptr_t w) {
	std::uintptr_t vt = 0;
	if (!RdPtr(w, vt)) return false;
	return vt == g_base + off::kHkpWorldVtbl;
}

// 找一个可用的物理世界指针。用「从脚下向下打 200 米必然打到地面」自检；
// vtable 直接就是 hkpWorld 的话直接采用。没找到就下次再来（游戏加载过程中一直没有很正常）。
static void* PickWorld(std::uintptr_t player, const std::uintptr_t* enemies, int nEnemies, bool verbose) {
	std::uintptr_t cands[32] = {};
	const char*    srcs[32]  = {};
	int nc = 0;
	auto add = [&](std::uintptr_t v, const char* s) {
		if (v && nc < 32) { cands[nc] = v; srcs[nc] = s; ++nc; }
	};

	std::uintptr_t root = 0;
	if (RdPtr(g_base + off::kWorldGlobal, root) && root) {
		std::uintptr_t w = 0;
		if (RdPtr(root + 0x18, w)) add(w, "*(0x125AB68)+0x18");
		if (RdPtr(root + 0x10, w)) {
			std::uintptr_t w2 = 0;
			if (RdPtr(w + 0x10, w2)) add(w2, "*(0x125AB68)+0x10,+0x10");
		}
	}
	if (player) {
		std::uintptr_t w = 0;
		if (RdPtr(player + off::kWorldOf, w)) add(w, "player+0x1D8");
	}
	for (int i = 0; i < nEnemies && i < 8; ++i) {
		std::uintptr_t w = 0;
		if (RdPtr(enemies[i] + off::kWorldOf, w)) add(w, "enemy+0x1D8");
	}

	float eye[3] = {};
	bool haveEye = false;
	if (player) {
		const bool a = RdF(player + off::kPos, eye[0]);
		const bool b = RdF(player + off::kPos + 4, eye[1]);
		const bool c = RdF(player + off::kPos + 8, eye[2]);
		haveEye = a && b && c && (eye[0] != 0.0f || eye[1] != 0.0f || eye[2] != 0.0f);
	}

	for (int i = 0; i < nc; ++i) {
		void* w = reinterpret_cast<void*>(cands[i]);
		if (!Readable(w, 0x80)) {
			if (verbose) Log("[world] %s -> %p 不可读，跳过", srcs[i], w);
			continue;
		}
		if (WorldLooksGood(cands[i])) {
			Log("[world] %s -> %p vtable = hkpWorld，采用", srcs[i], w);
			return w;
		}
		// 只有 vtable 落在游戏模块里才敢拿去调游戏的射线函数
		std::uintptr_t vt = 0;
		RdPtr(cands[i], vt);
		const bool vtInModule = (vt >= g_base && vt < g_baseEnd);
		if (!vtInModule || !haveEye) {
			if (verbose) Log("[world] %s -> %p vtable=%p 不在游戏模块内或玩家坐标未知，跳过", srcs[i], w, reinterpret_cast<void*>(vt));
			continue;
		}
		float from[3] = { eye[0], eye[1] + 5.0f, eye[2] };
		float to[3]   = { eye[0], eye[1] - 200.0f, eye[2] };
		float frac = -1.0f;
		const bool hit = SafeCastRay(w, from, to, &frac);
		if (verbose) Log("[world] %s -> %p vtable=%p 向下自检命中=%d frac=%.4f", srcs[i], w,
		                 reinterpret_cast<void*>(vt), hit ? 1 : 0, frac);
		if (hit) return w;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// 敌人枚举（TeamManager -> 队伍行 -> MSVC std::map）
// ---------------------------------------------------------------------------
struct MapTraversal {
	std::uintptr_t stack[128];
	int sp = 0;
};

static int CollectTeamObjects(std::uintptr_t head, std::uintptr_t* out, int cap) {
	if (!head) return 0;
	int n = 0;
	std::uintptr_t stack[128] = {};
	int sp = 0;
	std::uintptr_t root = 0;
	if (RdPtr(head + off::kNodeParent, root) && root && root != head) stack[sp++] = root;
	int seen = 0;
	while (sp > 0 && seen < 8192) {
		const std::uintptr_t node = stack[--sp];
		++seen;
		if (!node || node == head || !Readable(reinterpret_cast<const void*>(node), 0x28)) continue;
		std::uint8_t isNil = 0;
		Rd<std::uint8_t>(node + off::kNodeIsNil, isNil);
		if (isNil) continue;
		std::uintptr_t obj = 0;
		RdPtr(node + off::kNodeKey, obj);
		std::uintptr_t left = 0, right = 0;
		RdPtr(node + off::kNodeLeft, left);
		RdPtr(node + off::kNodeRight, right);
		if (left && left != head && sp < 127) stack[sp++] = left;
		if (right && right != head && sp < 127) stack[sp++] = right;
		if (obj && n < cap) out[n++] = obj;
	}
	return n;
}

static int CollectEnemies(std::uintptr_t player, std::uintptr_t* out, int cap) {
	if (!player) return 0;   // 还没进关卡 / 玩家对象链不可用，什么都不画
	std::uintptr_t mgr = 0;
	if (!RdPtr(g_base + off::kTeamManagerPtr, mgr) || !mgr) return 0;
	std::uintptr_t rows = 0;
	if (!RdPtr(mgr + off::kRows, rows) || !rows) return 0;
	std::int32_t teamCount = 0;
	if (!RdI(mgr + off::kCount, teamCount) || teamCount <= 0 || teamCount > 32) return 0;

	std::int32_t myTeam = -1;
	if (player) RdI(player + off::kTeam, myTeam);

	// 玩家队伍行里的关系数组：myRel[t] = 队伍 t 对玩家的关系
	std::int32_t myRel[32];
	for (int i = 0; i < 32; ++i) myRel[i] = -1;
	if (myTeam >= 0 && myTeam < teamCount) {
		std::uintptr_t relPtr = 0;
		if (RdPtr(rows + static_cast<std::size_t>(myTeam) * off::kRowStride + off::kRelations, relPtr) && relPtr) {
			for (int t = 0; t < teamCount; ++t) RdI(relPtr + static_cast<std::size_t>(t) * 4, myRel[t]);
		}
	}

	int n = 0;
	for (int t = 0; t < teamCount && n < cap; ++t) {
		const int rel = myRel[t];
		// 关系数组取不到时（还没进关卡）退化成“除玩家队伍以外都试一遍”
		const bool enemyTeam = (rel == off::kRelationEnemy) || (rel < 0 && t != myTeam);
		if (!enemyTeam) continue;
		if (myTeam >= 0 && t == myTeam) continue;

		const std::uintptr_t row = rows + static_cast<std::size_t>(t) * off::kRowStride;
		std::uintptr_t head = 0;
		RdPtr(row + off::kRowHead, head);
		n += CollectTeamObjects(head, out + n, cap - n);
	}
	return n;
}

// ---------------------------------------------------------------------------
// 每帧采集（只在游戏渲染线程调用）
// ---------------------------------------------------------------------------
static void ToggleHotkey() {
	if (g_cfg.toggleKey <= 0) return;
	static bool down = false;
	const bool now = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
	if (now && !down) {
		g_cfg.enabled = !g_cfg.enabled;
		Log("[hotkey] 血条 %s", g_cfg.enabled ? "开启" : "关闭");
	}
	down = now;
}

static int  g_frame      = 0;   // Present 帧计数（只在游戏渲染线程改）
static int  g_occlUsed   = 0;
static int  g_occlDropped = 0;  // 本帧因遮挡被丢掉的条数
static int  g_occlDropLog = 0;  // 遮挡案例的日志预算
static int  g_registry   = 0;   // 本帧队伍表里的对象总数（未过滤）

// ---------------------------------------------------------------------------
// 血量来源取证（Debug=1 时开启，有总量上限，不刷屏）
//
// 目的：证明我们读的 [+0x1F8]/[+0x1FC] 是“累积总有效生命值”，而不是某个部位的耐久。
//   - 每个对象第一次出现时打一行，带 RTTI 类名（若是 ...Parts... 类说明队伍表里混进了部位）
//   - 之后只要当前血量变化就打一行，可以据此判断：打腿/打护甲是否掉总池、部位再生是否回涨
// ---------------------------------------------------------------------------
constexpr int kHpObjMax  = 512;    // 最多跟踪多少个对象
constexpr int kHpLineMax = 2000;   // 最多打多少行（新对象 + 变化合计）
static std::uintptr_t g_hpObj[kHpObjMax] = {};
static float          g_hpCur[kHpObjMax] = {};
static int            g_hpCount = 0;
static int            g_hpLines = 0;

// 对象所属类名；取不到时退化成 vtable 偏移，便于离线用 tools\edf5rtti.py 反查
static std::string ClassTag(std::uintptr_t o) {
	const std::string s = ShortClassName(o);
	if (s != "?") return s;
	std::uintptr_t vt = 0;
	if (!RdPtr(o, vt) || vt < g_base) return "?";
	return "?vt=0x" + [](std::uintptr_t v) {
		std::string h;
		const char* d = "0123456789ABCDEF";
		for (int i = 0; i < 8; ++i) h.insert(h.begin(), d[(v >> (i * 4)) & 0xF]);
		return h;
	}(vt - g_base);
}

static void NoteHealth(std::uintptr_t o, float maxHp, float curHp) {
	if (!g_cfg.debug || g_hpLines >= kHpLineMax) return;
	for (int i = 0; i < g_hpCount; ++i) {
		if (g_hpObj[i] != o) continue;
		if (g_hpCur[i] != curHp) {
			Log("[hp] %p %s %.1f -> %.1f (max %.1f 比例 %.0f%%)", reinterpret_cast<void*>(o),
			    ClassTag(o).c_str(), g_hpCur[i], curHp, maxHp,
			    static_cast<double>(maxHp > 0.0f ? curHp / maxHp * 100.0f : 0.0f));
			g_hpCur[i] = curHp;
			++g_hpLines;
		}
		return;
	}
	if (g_hpCount < kHpObjMax) {
		g_hpObj[g_hpCount] = o;
		g_hpCur[g_hpCount] = curHp;
		++g_hpCount;
		Log("[hp] 新对象 %p class=%s max=%.1f cur=%.1f", reinterpret_cast<void*>(o),
		    ClassTag(o).c_str(), maxHp, curHp);
		++g_hpLines;
	}
}

constexpr int kBarLogMax = 200;
static std::uintptr_t g_barObj[kBarLogMax] = {};
static int            g_barCount = 0;

// 记录“真的画出血条”的对象属于哪个类：可以据此确认某条血条背后是实体根对象还是部位对象
static void NoteBar(std::uintptr_t o, float maxHp, float curHp) {
	if (!g_cfg.debug || g_barCount >= kBarLogMax) return;
	for (int i = 0; i < g_barCount; ++i) {
		if (g_barObj[i] == o) return;
	}
	g_barObj[g_barCount++] = o;
	Log("[bar] 画 %p class=%s max=%.1f cur=%.1f (%.0f%%)", reinterpret_cast<void*>(o),
	    ClassTag(o).c_str(), maxHp, curHp,
	    static_cast<double>(maxHp > 0.0f ? curHp / maxHp * 100.0f : 0.0f));
}

// 判断"挡住血条的那个东西"是不是角色单位（友军/敌人/中立），而不是地图/建筑/地形。
// 依据：角色单位在 +0x218 上带合法队伍号，且能取到 RTTI 类名；静态几何/Havok 体没有队伍号。
// 用户要求：友军不得遮蔽敌人血条。默认只记录不生效（OcclusionIgnoreCharacters=1 才真正忽略），
// 先用日志确认命中对象到底是什么，再决定是否打开。
static bool HitLooksLikeCharacter(std::uintptr_t o) {
	if (!o || !Readable(reinterpret_cast<const void*>(o), 0x300)) return false;
	std::int32_t team = 0;
	if (!RdI(o + off::kTeam, team)) return false;
	if (team < 0 || team > 31) return false;
	const std::string cls = ShortClassName(o);
	if (cls == "?") return false;
	// Havok / 引擎内部类型不是游戏角色，直接排除，避免把地图碰撞体误判成角色
	static const char* kEnginePrefix[] = { "hk", "xgs", "xgl", "anm", "hid", "std" };
	for (const char* p : kEnginePrefix) {
		const std::size_t n = std::strlen(p);
		if (cls.size() >= n && cls.compare(0, n, p) == 0) return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// C1 原生血条：只读探针（NativeProbe=1/2）
//
// 目的：在真正调用 0x4B5210 创建原生条之前，先把三个前置条件问清楚——
//   ① 敌人对象 +0x118/+0x11C（int32）是不是"当前/最大耐久"（原生条的比例来源）
//   ② 原生条是被插进 [watched + 0x188] 容器的，敌人身上那个容器是否像合法容器
//   ③ 现存原生条对象长什么样：它的 +0x70/+0x78/+0x80（owner/上下文）能不能直接借用
// 全程只读，不创建、不修改任何游戏对象。
// ---------------------------------------------------------------------------
namespace {
constexpr std::uintptr_t kFollowerBarVtbl = 0xEC92F8; // ?AVHudPlayer_FollowerDurability@@
constexpr std::uintptr_t kRtDynamicCast   = 0x9C8228; // jmp [__RTDynamicCast]，VCRUNTIME140
constexpr std::uintptr_t kTdSceneObject   = 0x11A49C0; // .?AVSceneObject@game@xgs@@
constexpr std::uintptr_t kTdSoldierBase   = 0x11A4970; // .?AVSoldierBase@@
constexpr std::size_t    kBarContainerOff = 0x188;
typedef void* (*RtDynamicCastFn)(void*, long, void*, void*, int);
} // namespace

static int TryCastToSoldier(std::uintptr_t o) {
	__try {
		RtDynamicCastFn dc = reinterpret_cast<RtDynamicCastFn>(g_base + kRtDynamicCast);
		void* r = dc(reinterpret_cast<void*>(o), 0,
		             reinterpret_cast<void*>(g_base + kTdSceneObject),
		             reinterpret_cast<void*>(g_base + kTdSoldierBase), 0);
		return r ? 1 : 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

static bool IsFollowerBarObject(std::uintptr_t p) {
	std::uintptr_t vt = 0;
	return p != 0 && RdPtr(p, vt) && vt == g_base + kFollowerBarVtbl;
}

// 收集"友军"队伍（relation == 1）里的对象
static int CollectAllies(std::uintptr_t player, std::uintptr_t* out, int cap) {
	if (!player) return 0;
	std::uintptr_t mgr = 0, rows = 0;
	if (!RdPtr(g_base + off::kTeamManagerPtr, mgr) || !mgr) return 0;
	if (!RdPtr(mgr + off::kRows, rows) || !rows) return 0;
	std::int32_t teamCount = 0;
	if (!RdI(mgr + off::kCount, teamCount) || teamCount <= 0 || teamCount > 32) return 0;

	std::int32_t myTeam = -1;
	RdI(player + off::kTeam, myTeam);
	std::int32_t myRel[32];
	for (int i = 0; i < 32; ++i) myRel[i] = -1;
	if (myTeam >= 0 && myTeam < teamCount) {
		std::uintptr_t relPtr = 0;
		if (RdPtr(rows + static_cast<std::size_t>(myTeam) * off::kRowStride + off::kRelations, relPtr) && relPtr) {
			for (int t = 0; t < teamCount; ++t) RdI(relPtr + static_cast<std::size_t>(t) * 4, myRel[t]);
		}
	}

	int n = 0;
	for (int t = 0; t < teamCount && n < cap; ++t) {
		if (myRel[t] != off::kRelationAlly) continue;
		const std::uintptr_t row = rows + static_cast<std::size_t>(t) * off::kRowStride;
		std::uintptr_t head = 0;
		RdPtr(row + off::kRowHead, head);
		n += CollectTeamObjects(head, out + n, cap - n);
	}
	return n;
}

static int g_nativeProbeLines = 0;

// 只读 dump 一个对象上"原生条会用到"的字段：
//   +0x118/+0x11C = 原生条比例来源（int32）
//   +0x24/+0x28/+0x2C = 原生条几何浮点（0x5BE600 的入参）
//   +0x1A0 = 4x4 世界矩阵（0x624730 只从被监视对象上读这个）
//   +0xA0 = 我们已知的世界坐标，用来对照 +0x1A0 是不是同一个变换
static void LogObjectFields(const char* tag, std::uintptr_t o) {
	if (!o || g_nativeProbeLines >= 600) return;
	if (!Readable(reinterpret_cast<const void*>(o), 0x300)) return;
	std::int32_t i118 = 0, i11c = 0;
	float f24 = 0, f28 = 0, f2c = 0, fmax = 0, fcur = 0;
	Rd<std::int32_t>(o + 0x118, i118);
	Rd<std::int32_t>(o + 0x11C, i11c);
	RdF(o + 0x24, f24); RdF(o + 0x28, f28); RdF(o + 0x2C, f2c);
	RdF(o + off::kMaxHp, fmax); RdF(o + off::kCurHp, fcur);
	std::uintptr_t b = 0, e = 0, c = 0;
	RdPtr(o + kBarContainerOff, b);
	RdPtr(o + kBarContainerOff + 8, e);
	RdPtr(o + kBarContainerOff + 16, c);
	float m0 = 0, m1 = 0, m2 = 0, m3 = 0, m12 = 0, m13 = 0, m14 = 0;
	RdF(o + 0x1A0, m0); RdF(o + 0x1A4, m1); RdF(o + 0x1A8, m2); RdF(o + 0x1AC, m3);
	RdF(o + 0x1D0, m12); RdF(o + 0x1D4, m13); RdF(o + 0x1D8, m14);
	float px = 0, py = 0, pz = 0;
	RdF(o + off::kPos, px); RdF(o + off::kPos + 4, py); RdF(o + off::kPos + 8, pz);
	std::uint8_t dcb = 0;
	Rd<std::uint8_t>(o + 0xDC6, dcb);
	const int soldier = (g_cfg.nativeProbe >= 2) ? TryCastToSoldier(o) : -2;
	Log("[nat] %s %p class=%s Soldier=%d | +118=%d +11C=%d | +24=%.3f +28=%.3f +2C=%.3f | max=%.1f cur=%.1f | pos=(%.1f,%.1f,%.1f) | +1A0=[%.2f %.2f %.2f %.2f] +1D0=[%.2f %.2f %.2f] | +188=%p/%p/%p DC6=%d",
	    tag, reinterpret_cast<void*>(o), ClassTag(o).c_str(), soldier, i118, i11c,
	    static_cast<double>(f24), static_cast<double>(f28), static_cast<double>(f2c),
	    static_cast<double>(fmax), static_cast<double>(fcur),
	    static_cast<double>(px), static_cast<double>(py), static_cast<double>(pz),
	    static_cast<double>(m0), static_cast<double>(m1), static_cast<double>(m2), static_cast<double>(m3),
	    static_cast<double>(m12), static_cast<double>(m13), static_cast<double>(m14),
	    reinterpret_cast<void*>(b), reinterpret_cast<void*>(e), reinterpret_cast<void*>(c),
	    static_cast<int>(dcb));
	++g_nativeProbeLines;
}

// 按虚表扫描进程私有内存，找出游戏**自己的** HudPlayer_FollowerDurability 实例。
// 之前只在"友军对象 +0x188"里找，没找到——这一步直接以虚表 0xEC92F8 为准，
// 用来判定：游戏到底有没有在运行时创建这个类的实例（只读，一次）。
// 

// 纯裸内存扫描（函数内不得出现任何需要析构的 C++ 对象，否则 __try 不可用）
static int ScanRegionsRaw(std::uintptr_t vt, std::uintptr_t* hits, int cap) {
	int regions = 0, found = 0;
	__try {
		SYSTEM_INFO si = {};
		GetSystemInfo(&si);
		std::uintptr_t a = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
		const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
		MEMORY_BASIC_INFORMATION mbi = {};
		while (a < end && found < cap && regions < 40000) {
			if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi) != sizeof mbi) break;
			const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const std::uintptr_t sz = mbi.RegionSize;
			if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && sz >= 8 && sz <= 0x4000000 &&
			    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
			    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY))) {
				const std::uintptr_t* p = reinterpret_cast<const std::uintptr_t*>(base);
				const std::size_t n = static_cast<std::size_t>(sz) / sizeof(std::uintptr_t);
				for (std::size_t i = 0; i + 48 < n && found < cap; ++i) {
					if (p[i] != vt) continue;
					hits[found++] = base + i * sizeof(std::uintptr_t);
				}
				++regions;
			}
			a = base + sz;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -found;   // 负值 = 中途异常，绝对值是已找到数量
	}
	return found;
}

// 类普查：扫描进程私有内存，收集所有"像虚表指针"的值（裸函数，不能含 C++ 对象）
static int CollectVtablesRaw(std::uintptr_t lo, std::uintptr_t hi, std::uintptr_t* out, int cap) {
	int found = 0;
	__try {
		SYSTEM_INFO si = {};
		GetSystemInfo(&si);
		std::uintptr_t a = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
		const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
		MEMORY_BASIC_INFORMATION mbi = {};
		while (a < end && found < cap) {
			if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi) != sizeof mbi) break;
			const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const std::uintptr_t sz = mbi.RegionSize;
			if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && sz >= 8 && sz <= 0x4000000 &&
			    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
			    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY))) {
				const std::uintptr_t* p = reinterpret_cast<const std::uintptr_t*>(base);
				const std::size_t n = static_cast<std::size_t>(sz) / sizeof(std::uintptr_t);
				for (std::size_t i = 0; i < n && found < cap; ++i) {
					const std::uintptr_t v = p[i];
					if (v < lo || v >= hi) continue;
					int dup = 0;
					for (int k = 0; k < found; ++k) {
						if (out[k] == v) { dup = 1; break; }
					}
					if (!dup) out[found++] = v;
				}
			}
			a = base + sz;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -found;
	}
	return found;
}

static int g_censusDone = 0;
static std::uintptr_t g_censusVts[2000] = {};

static void DoClassCensus() {
	if (g_censusDone) return;
	g_censusDone = 1;
	const int n = CollectVtablesRaw(g_base + 0xC97000, g_baseEnd, g_censusVts, 2000);
	const int cnt = (n < 0) ? -n : n;
	Log("[nat] 类普查：私有内存里收集到虚表候选 %d 个", cnt);
	++g_nativeProbeLines;

	// 关键词：任何可能是"头顶条/HUD 标记"的类名
	static const char* kKeys[] = { "Hud", "Marker", "Billboard", "Durability", "Follower",
	                               "Gauge", "Bar", "Life", "Target", "Icon", "Sprite", "Ui" };
	int hitLines = 0, listed = 0;
	for (int i = 0; i < cnt && g_nativeProbeLines < 600; ++i) {
		const std::string cls = ClassNameOfVtable(g_censusVts[i]);
		if (cls.size() < 4) continue;
		if (listed < 25) {
			const std::size_t at = cls.find('@');
			Log("[nat]   vt=%p %s", reinterpret_cast<void*>(g_censusVts[i]),
			    (at == std::string::npos ? cls : cls.substr(0, at)).c_str());
			++g_nativeProbeLines;
			++listed;
		}
		int hit = 0;
		for (const char* k : kKeys) {
			if (cls.find(k) != std::string::npos) { hit = 1; break; }
		}
		if (!hit) continue;
		const std::size_t at = cls.find('@');
		Log("[nat] ★可疑 vt=%p %s", reinterpret_cast<void*>(g_censusVts[i]),
		    (at == std::string::npos ? cls : cls.substr(0, at)).c_str());
		++g_nativeProbeLines;
		++hitLines;
	}
	Log("[nat] 类普查结束：共 %d 个候选，可疑类 %d 个", cnt, hitLines);
	++g_nativeProbeLines;
}

static int g_scanAttempts = 0;
static int g_scanReal = 0;   // "像真实例"的命中数
static std::uintptr_t g_scanHits[16] = {};
static int g_scanHitCount = 0;

// 一个"像真的"条对象：+0x18（被监视对象）或 +0x80（渲染上下文）要指向可读内存。
// 用来滤掉"内存里恰好存着虚表数值"的假阳性。
static bool LooksLikeBarInstance(std::uintptr_t o) {
	std::uintptr_t a = 0, b = 0;
	const bool okA = RdPtr(o + 0x18, a) && a > 0x10000 && Readable(reinterpret_cast<const void*>(a), 0x100);
	const bool okB = RdPtr(o + 0x80, b) && b > 0x10000 && Readable(reinterpret_cast<const void*>(b), 0x40);
	return okA || okB;
}

static void DoVtableScan(std::uintptr_t vt) {
	++g_scanAttempts;
	const unsigned long long t0 = GetTickCount64();
	const int r = ScanRegionsRaw(vt, g_scanHits, 16);
	const unsigned long long ms = GetTickCount64() - t0;
	g_scanHitCount = (r < 0) ? -r : r;
	int real = 0;
	for (int i = 0; i < g_scanHitCount; ++i) {
		if (LooksLikeBarInstance(g_scanHits[i])) ++real;
	}
	g_scanReal = real;
	Log("[nat] 虚表扫描（第 %d 次，耗时 %llu ms）0x%llX：原始命中 %d 个，像真实例 %d 个",
	    g_scanAttempts, ms, static_cast<unsigned long long>(vt - g_base), g_scanHitCount, real);
	++g_nativeProbeLines;
	for (int i = 0; i < g_scanHitCount && g_nativeProbeLines < 600; ++i) {
		const std::uintptr_t o = g_scanHits[i];
		if (!LooksLikeBarInstance(o)) continue;
		std::uintptr_t watched = 0, o70 = 0, o78 = 0, o80 = 0;
		RdPtr(o + 0x18, watched);
		RdPtr(o + 0x70, o70);
		RdPtr(o + 0x78, o78);
		RdPtr(o + 0x80, o80);
		Log("[nat] ★实例 %p watched=%p class=%s | +0x70=%p +0x78=%p +0x80=%p class=%s",
		    reinterpret_cast<void*>(o), reinterpret_cast<void*>(watched), ClassTag(watched).c_str(),
		    reinterpret_cast<void*>(o70), reinterpret_cast<void*>(o78), reinterpret_cast<void*>(o80),
		    ClassTag(o80).c_str());
		++g_nativeProbeLines;
		LogObjectFields("  条的被监视对象", watched);
	}
}

static void NativeBarProbe(std::uintptr_t player, const std::uintptr_t* enemies, int nEnemies) {
	if (!g_cfg.nativeProbe || !g_cfg.debug) return;
	if (g_nativeProbeLines >= 600) return;
	if ((g_frame % 120) != 0) return;

	std::uintptr_t allies[128];
	const int na = CollectAllies(player, allies, 128);

	// 关键：原生条是"玩家靠近友军才出现"的 → 只有在有友军贴近时才去扫描/普查，
	// 否则扫到的永远是"还没创建"的状态（这就是前三次扫描 0 命中的原因）。
	float px = 0, py = 0, pz = 0;
	const bool pOk = RdF(player + off::kPos, px) && RdF(player + off::kPos + 4, py) &&
	                 RdF(player + off::kPos + 8, pz);
	float nearDist = 1e9f;
	std::uintptr_t nearAlly = 0;
	if (pOk) {
		for (int i = 0; i < na; ++i) {
			float ax = 0, ay = 0, az = 0;
			if (!RdF(allies[i] + off::kPos, ax) || !RdF(allies[i] + off::kPos + 4, ay) ||
			    !RdF(allies[i] + off::kPos + 8, az)) continue;
			const float dx = ax - px, dy = ay - py, dz = az - pz;
			const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (d < nearDist) { nearDist = d; nearAlly = allies[i]; }
		}
	}
	const bool close = pOk && nearDist < 25.0f;

	Log("[nat] 探针：友军对象=%d 敌人对象=%d 最近友军=%.1fm 贴近=%d 已用日志=%d",
	    na, nEnemies, static_cast<double>(nearDist), close ? 1 : 0, g_nativeProbeLines);
	++g_nativeProbeLines;

	if (g_cfg.nativeProbe >= 3 && close && g_scanReal == 0 && g_scanAttempts < 8 && (g_frame % 1200) == 0)
		DoVtableScan(g_base + kFollowerBarVtbl);
	if (g_cfg.nativeProbe >= 3 && close && !g_censusDone && g_scanAttempts >= 1 && (g_frame % 1200) == 960)
		DoClassCensus();
	if (close && (g_frame % 600) == 240) {
		Log("[nat] 近友军 距离=%.1fm ptr=%p（原生条应在此时可见）",
		    static_cast<double>(nearDist), reinterpret_cast<void*>(nearAlly));
		++g_nativeProbeLines;
		LogObjectFields("近友军", nearAlly);
	}

	// ① 从友军对象上找现成的原生条对象
	int found = 0;
	for (int i = 0; i < na && g_nativeProbeLines < 600; ++i) {
		const std::uintptr_t o = allies[i];
		if (!Readable(reinterpret_cast<const void*>(o), 0x300)) continue;
		std::uintptr_t b = 0, e = 0, c = 0;
		RdPtr(o + kBarContainerOff, b);
		RdPtr(o + kBarContainerOff + 8, e);
		RdPtr(o + kBarContainerOff + 16, c);
		if (!b || !e || e < b) continue;
		const std::uintptr_t count = (e - b) / 8;
		if (count > 256) continue;
		Log("[nat] 友军 %p class=%s 容器+0x188 begin=%p end=%p cap=%p 条目=%llu", reinterpret_cast<void*>(o),
		    ClassTag(o).c_str(), reinterpret_cast<void*>(b), reinterpret_cast<void*>(e),
		    reinterpret_cast<void*>(c), static_cast<unsigned long long>(count));
		++g_nativeProbeLines;
		for (std::uintptr_t k = 0; k < count && g_nativeProbeLines < 600; ++k) {
			std::uintptr_t entry = 0;
			if (!RdPtr(b + k * 8, entry) || !IsFollowerBarObject(entry)) continue;
			std::uintptr_t watched = 0, own70 = 0, own78 = 0, ctx80 = 0, ctxVt = 0;
			RdPtr(entry + 0x18, watched);
			RdPtr(entry + 0x70, own70);
			RdPtr(entry + 0x78, own78);
			RdPtr(entry + 0x80, ctx80);
			RdPtr(ctx80, ctxVt);
			Log("[nat] ★原生条 %p watched=%p class=%s | +0x70=%p +0x78=%p +0x80=%p(vt=%p class=%s)",
			    reinterpret_cast<void*>(entry), reinterpret_cast<void*>(watched), ClassTag(watched).c_str(),
			    reinterpret_cast<void*>(own70), reinterpret_cast<void*>(own78), reinterpret_cast<void*>(ctx80),
			    reinterpret_cast<void*>(ctxVt), ClassTag(ctx80).c_str());
			++g_nativeProbeLines;
			++found;
		}
	}
	if (found == 0) {
		Log("[nat] 未在友军 +0x188 容器中找到原生条对象（vtable 应为 %p）",
		    reinterpret_cast<void*>(g_base + kFollowerBarVtbl));
		++g_nativeProbeLines;
	}

	// ② 字段 dump：玩家 / 友军（原生条真正服务的对象，作模板）/ 敌人（对照）
	LogObjectFields("玩家", player);

	int allyShown = 0;
	for (int i = 0; i < na && allyShown < 6 && g_nativeProbeLines < 600; ++i) {
		LogObjectFields("友军", allies[i]);
		++allyShown;
	}

	int shown = 0;
	for (int i = 0; i < nEnemies && shown < 6 && g_nativeProbeLines < 600; ++i) {
		LogObjectFields("敌  ", enemies[i]);
		++shown;
	}
}

// 遮挡结果分帧缓存：射线是渲染线程上最大的单项开销（实测 12~13 条/帧），
// 而遮挡关系随视角变化很慢，所以同一对象每 occlReuse 帧才重投一次，中间沿用上次结果。
constexpr int kOcclSlots = 512;
static std::uintptr_t g_occlSlotObj[kOcclSlots]     = {};
static std::uint8_t   g_occlSlotBlocked[kOcclSlots] = {};
static int            g_occlSlotFrame[kOcclSlots]   = {};
static int            g_occlSlotCount = 0;
static int            g_prevObjCount  = 0;   // 上一帧队伍表对象数，用于识别"关卡切换/回基地"

static int OcclFind(std::uintptr_t o) {
	for (int i = 0; i < g_occlSlotCount; ++i) {
		if (g_occlSlotObj[i] == o) return i;
	}
	return -1;
}

static int OcclAdd(std::uintptr_t o) {
	if (g_occlSlotCount >= kOcclSlots) return -1;
	const int i = g_occlSlotCount++;
	g_occlSlotObj[i]     = o;
	g_occlSlotBlocked[i] = 0;
	g_occlSlotFrame[i]   = -10000;
	return i;
}

// 队伍表全量 dump：NativeProbe=4 时，把每个队伍里的对象连同类名/血量/坐标打出来，
// 用来排查"某类敌人（例如传送塔）为什么不显示血条"。
static void DebugDumpTeams(std::uintptr_t player) {
	if (!g_cfg.debug || g_cfg.nativeProbe < 4) return;
	if (g_nativeProbeLines >= 600) return;
	if ((g_frame % 600) != 0) return;
	if (!player) return;

	std::uintptr_t mgr = 0, rows = 0;
	if (!RdPtr(g_base + off::kTeamManagerPtr, mgr) || !mgr) return;
	if (!RdPtr(mgr + off::kRows, rows) || !rows) return;
	std::int32_t teamCount = 0;
	if (!RdI(mgr + off::kCount, teamCount) || teamCount <= 0 || teamCount > 32) return;
	std::int32_t myTeam = -1;
	RdI(player + off::kTeam, myTeam);

	std::int32_t myRel[32];
	for (int i = 0; i < 32; ++i) myRel[i] = -1;
	if (myTeam >= 0 && myTeam < teamCount) {
		std::uintptr_t relPtr = 0;
		if (RdPtr(rows + static_cast<std::size_t>(myTeam) * off::kRowStride + off::kRelations, relPtr) && relPtr) {
			for (int t = 0; t < teamCount; ++t) RdI(relPtr + static_cast<std::size_t>(t) * 4, myRel[t]);
		}
	}

	Log("[team] 队伍数=%d 玩家队伍=%d（关系 2=敌 1=友 0=中立）", teamCount, myTeam);
	++g_nativeProbeLines;
	for (int t = 0; t < teamCount && g_nativeProbeLines < 600; ++t) {
		const std::uintptr_t row = rows + static_cast<std::size_t>(t) * off::kRowStride;
		std::uintptr_t head = 0;
		RdPtr(row + off::kRowHead, head);
		std::uintptr_t objs[64];
		const int n = CollectTeamObjects(head, objs, 64);

		// 先打"类名分布"（完整覆盖该队伍的所有对象，不占多少行）——
		// 这是判断"某类敌人是否根本不在队伍表里"的关键
		std::string hName[16];
		int hCnt[16] = {};
		int hn = 0;
		for (int i = 0; i < n; ++i) {
			const std::string c = ShortClassName(objs[i]);
			int k = -1;
			for (int j = 0; j < hn; ++j) {
				if (hName[j] == c) { k = j; break; }
			}
			if (k < 0) {
				if (hn >= 16) continue;
				k = hn++;
				hName[k] = c;
			}
			hCnt[k]++;
		}
		std::string hist;
		for (int j = 0; j < hn; ++j) {
			if (j) hist += ", ";
			hist += hName[j] + " x" + std::to_string(hCnt[j]);
		}
		Log("[team] 队伍 %d（对玩家关系=%d）对象=%d 类名分布: %s", t, myRel[t], n, hist.c_str());
		++g_nativeProbeLines;
		for (int i = 0; i < n && i < 8 && g_nativeProbeLines < 600; ++i) {
			LogObjectFields("  队伍对象", objs[i]);
		}
	}
}

// ---------------------------------------------------------------------------
// "表外目标"扫描：传送塔这类对象**带血量块但不在冲突对象表里**，队伍表遍历看不到。
// 做法：分帧扫描进程私有内存，找虚表命中 EHB_damageable.h 那张表（= 带血量块的类）的对象，
//       再做字段校验。每帧有字节预算，不会造成单帧卡顿。
//       ExtraObjectScan=1 只记录；=2 另说（绘制留到验证通过后再接）。
// ---------------------------------------------------------------------------
static int            g_exFound = 0;
static std::uintptr_t g_exPtr[64] = {};
static std::uintptr_t g_exAddr = 0;
static bool           g_exInit = false;
static bool           g_exDone = false;
static int            g_exNextScan = 0;  // 下次重扫的帧号（传送塔可能后生成）
static double         g_exBytes = 0;      // 已扫描字节
static int            g_exRegions = 0;    // 已扫描区域数
static int            g_exVtRange = 0;    // 落在虚表地址范围内的值
static int            g_exVtHit = 0;      // 其中命中白名单的值

static bool IsDamageableVtable(std::uintptr_t vtRva) {
	int lo = 0, hi = static_cast<int>(ehb::kDamageableVtableCount) - 1;
	while (lo <= hi) {
		const int m = (lo + hi) / 2;
		const std::uint32_t v = ehb::kDamageableVtables[m];
		if (v == vtRva) return true;
		if (v < vtRva) lo = m + 1;
		else hi = m - 1;
	}
	return false;
}

// 字段校验：血量与坐标都要合理，避免把"碰巧存着虚表数值的随机内存"当成对象
static bool LooksLikeDamageableRaw(std::uintptr_t o) {
	if (!Readable(reinterpret_cast<const void*>(o), 0x300)) return false;
	float mx = 0, cur = 0, x = 0, y = 0, z = 0;
	if (!RdF(o + off::kMaxHp, mx) || !RdF(o + off::kCurHp, cur)) return false;
	if (!(mx > 0.0f && mx < 1.0e8f)) return false;
	if (!(cur >= 0.0f && cur <= mx + 0.5f)) return false;
	if (!RdF(o + off::kPos, x) || !RdF(o + off::kPos + 4, y) || !RdF(o + off::kPos + 8, z)) return false;
	if (std::fabs(x) > 1.0e5f || std::fabs(y) > 1.0e5f || std::fabs(z) > 1.0e5f) return false;
	return true;
}

// 裸函数：内部不得出现需要析构的 C++ 对象，否则 __try 不可用
static int ScanChunkRaw(const std::uintptr_t* p, std::size_t n, std::uintptr_t base,
                        std::uintptr_t* out, int cap, int have) {
	int found = have;
	__try {
		for (std::size_t i = 0; i + 48 < n && found < cap; ++i) {
			const std::uintptr_t v = p[i];
			if (v < g_base + 0xC97000 || v >= g_baseEnd) continue;
			if (!IsDamageableVtable(v - g_base)) continue;
			const std::uintptr_t o = base + i * sizeof(std::uintptr_t);
			if (!LooksLikeDamageableRaw(o)) continue;
			int dup = 0;
			for (int k = 0; k < found; ++k) { if (out[k] == o) { dup = 1; break; } }
			if (!dup) out[found++] = o;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return found;
	}
	return found;
}

static void ExtraScanStep(bool inMission) {
	// 必须"已经在关卡里"才扫：否则会在读图阶段就把内存扫完，那时对象还不存在（实测命中 0）
	if (!inMission) { g_exInit = false; g_exDone = false; g_exFound = 0; return; }
	if (g_cfg.extraObjectScan <= 0) return;
	if (g_exDone) { if (g_frame < g_exNextScan) return; g_exDone = false; g_exInit = false; }
	SYSTEM_INFO si = {};
	GetSystemInfo(&si);
	const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
	const std::uintptr_t hi = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
	if (!g_exInit) {
		g_exAddr = lo;
		g_exInit = true;
		g_exBytes = 0; g_exRegions = 0; g_exVtRange = 0; g_exVtHit = 0;
		Log("[extra] 开始扫描（帧 %d）", g_frame);
	}
	std::size_t budget = 16u * 1024u * 1024u;   // 每帧最多扫 16MB
	while (budget > 0 && g_exFound < 64 && g_exAddr < hi) {
		MEMORY_BASIC_INFORMATION mbi = {};
		if (VirtualQuery(reinterpret_cast<LPCVOID>(g_exAddr), &mbi, sizeof mbi) != sizeof mbi) break;
		const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
		std::size_t sz = static_cast<std::size_t>(mbi.RegionSize);
		g_exAddr = base + sz;
		const bool ok = (mbi.State == MEM_COMMIT) && (mbi.Type == MEM_PRIVATE) && sz >= 8 &&
		                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
		                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY));
		if (ok) {
			if (sz > 32u * 1024u * 1024u) sz = 32u * 1024u * 1024u;   // 超大区域只扫前 32MB
			const std::size_t take = (sz < budget) ? sz : budget;
			g_exFound = ScanChunkRaw(reinterpret_cast<const std::uintptr_t*>(base),
			                         take / sizeof(std::uintptr_t), base, g_exPtr, 64, g_exFound);
			budget -= take;
		}
	}
	if (!g_exDone && (g_frame % 120) == 0 && g_exRegions > 0) {
		Log("[extra] 扫描中：已扫 %.1f MB / 区域 %d / 虚表域候选 %d / 表内命中 %d / 通过校验 %d",
		    g_exBytes / 1048576.0, g_exRegions, g_exVtRange, g_exVtHit, g_exFound);
	}
	if (g_exAddr >= hi) {
		g_exDone = true;
		g_exNextScan = g_frame + 900;   // 15 秒后重扫一次（新生成的目标也能被找到）
		Log("[extra] 扫描完成：已扫 %.1f MB / 区域 %d / 虚表域候选 %d / 表内命中 %d / 通过校验 %d",
		    g_exBytes / 1048576.0, g_exRegions, g_exVtRange, g_exVtHit, g_exFound);
		for (int i = 0; i < g_exFound; ++i) {
			const std::uintptr_t o = g_exPtr[i];
			float mx = 0, cur = 0, x = 0, y = 0, z = 0;
			RdF(o + off::kMaxHp, mx);
			RdF(o + off::kCurHp, cur);
			RdF(o + off::kPos, x);
			RdF(o + off::kPos + 4, y);
			RdF(o + off::kPos + 8, z);
			Log("[extra] %p class=%s max=%.1f cur=%.1f pos=(%.0f,%.0f,%.0f)",
			    reinterpret_cast<void*>(o), ClassTag(o).c_str(), static_cast<double>(mx),
			    static_cast<double>(cur), static_cast<double>(x), static_cast<double>(y),
			    static_cast<double>(z));
		}
	}
}

static void CollectFrame() {
	ToggleHotkey();

	if (!g_cfg.enabled) {
		EnterCriticalSection(&g_snapLock);
		g_snap.count = 0;
		LeaveCriticalSection(&g_snapLock);
		return;
	}

	// 渲染分辨率与相机
	std::int32_t winW = 0, winH = 0;
	RdI(g_base + off::kWindowW, winW);
	RdI(g_base + off::kWindowH, winH);
	if (winW <= 0 || winH <= 0) { winW = 1920; winH = 1080; }

	g_vpValid = false;
	const std::uintptr_t cam = Chain(g_base, { off::kCameraChain, 0x8, 0x8, 0x8 });
	if (cam && Readable(reinterpret_cast<const void*>(cam + 0x80), 64) &&
	    Readable(reinterpret_cast<const void*>(cam + 0xE0), 64)) {
		float v[16] = {}, p[16] = {};
		std::memcpy(v, reinterpret_cast<const void*>(cam + 0x80), 64);
		std::memcpy(p, reinterpret_cast<const void*>(cam + 0xE0), 64);
		BuildViewProjection(v, p);
		g_vpValid = true;
	}
	if (!g_vpValid) {
		EnterCriticalSection(&g_snapLock);
		g_snap.count = 0;
		LeaveCriticalSection(&g_snapLock);
		return;
	}

	const std::uintptr_t player = Chain(g_base, { off::kPlayerChain, 0x238, 0x290, 0x10 });
	float eye[3] = {};
	bool  eyeOk = false;
	if (player) {
		const bool a = RdF(player + off::kPos, eye[0]);
		const bool b = RdF(player + off::kPos + 4, eye[1]);
		const bool c = RdF(player + off::kPos + 8, eye[2]);
		eyeOk = a && b && c;
		if (eyeOk) eye[1] += g_cfg.eyeHeight;
	}

	std::uintptr_t enemies[256];
	const int n = CollectEnemies(player, enemies, 256);
	g_registry = n;
	NativeBarProbe(player, enemies, n);

	// 关卡切换/回基地时队伍表会清空（实测 队伍表对象=0）。此时把"按对象指针索引"的缓存清掉：
	// 旧指针会被新关卡的对象复用，不清就可能让新敌人继承"被挡住"的旧结论；
	// 同时避免 512 个缓存槽被历史对象占满后失效。
	if (n == 0 && g_prevObjCount > 0) {
		g_occlSlotCount = 0;
		g_hpCount = 0;
		g_hpLines = 0;
		g_barCount = 0;
		g_occlDropLog = 0;
		Log("[life] 关卡切换（队伍表清空）：已清空遮挡缓存与取证表");
	}
	g_prevObjCount = n;
	DebugDumpTeams(player);
	ExtraScanStep(n > 0 && player != 0);

	// 射线刚出过异常：那个世界指针很可能已经失效，丢掉重找
	if (InterlockedExchange(&g_worldRestale, 0) != 0 && g_world) {
		Log("[world] 射线异常后丢弃缓存世界 %p，本次重新寻找", g_world);
		g_world = nullptr;
	}
	// 物理世界：跨关卡会换。曾经只取一次 → 第二局开始时拿着已释放的旧 hkpWorld 去调游戏射线，
	// 崩在 EDF5.exe+0xAE71E6，随后遮挡被永久关闭。现在定期从全局重新取当前世界。
	if (g_cfg.occlusion && (g_frame % 30) == 0) {
		std::uintptr_t root = 0, w = 0;
		if (RdPtr(g_base + off::kWorldGlobal, root) && root && RdPtr(root + 0x18, w) && w &&
		    w != reinterpret_cast<std::uintptr_t>(g_world) &&
		    Readable(reinterpret_cast<const void*>(w), 0x80) && WorldLooksGood(w)) {
			Log("[world] 世界更新 %p -> %p（帧 %d）", g_world, reinterpret_cast<void*>(w), g_frame);
			g_world = reinterpret_cast<void*>(w);
		}
	}
	// 物理世界：找不到就一直重试（关卡加载期间一直找不到很正常）
	if (!g_world && g_cfg.occlusion && player && (g_frame % 10) == 0) {
		++g_worldAttempts;
		const bool verbose = (g_worldAttempts <= 3) || (g_worldAttempts % 100) == 0;
		g_world = PickWorld(player, enemies, n, verbose);
		if (g_world) Log("[world] 采用 %p（第 %d 次尝试，帧 %d）", g_world, g_worldAttempts, g_frame);
		else if (verbose) Log("[world] 第 %d 次尝试仍没找到可用物理世界", g_worldAttempts);
	}

	BarRect tmp[kMaxBars];
	int count = 0;
	int occl = 0;
	g_occlDropped = 0;

	const float maxAngle = g_cfg.maxViewAngle;
	const float cosLimit = std::cos(maxAngle * 3.14159265358979f / 180.0f);

	for (int i = 0; i < n && count < g_cfg.maxBars; ++i) {
		const std::uintptr_t o = enemies[i];
		if (!Readable(reinterpret_cast<const void*>(o), 0x300)) continue;

		float maxHp = 0, curHp = 0;
		if (!RdF(o + off::kMaxHp, maxHp) || !RdF(o + off::kCurHp, curHp)) continue;
		if (!(maxHp > 0.0f)) continue;
		NoteHealth(o, maxHp, curHp);
		if (!(curHp > 0.0f)) continue;          // 已死亡
		if (!(curHp < maxHp)) continue;         // 满血不画

		float pos[3] = {};
		if (!RdF(o + off::kPos, pos[0]) || !RdF(o + off::kPos + 4, pos[1]) || !RdF(o + off::kPos + 8, pos[2])) continue;
		if (std::fabs(pos[0]) > 1e6f || std::fabs(pos[1]) > 1e6f || std::fabs(pos[2]) > 1e6f) continue;

		// 锚点：优先用游戏自己的锁定/中心点（多部位敌人更准），否则用基座位置
		float anchor[3] = { pos[0], pos[1], pos[2] };
		if (g_cfg.useLockOn) {
			std::uintptr_t lock = 0, lockPos = 0;
			if (RdPtr(o + off::kLockOn, lock) && lock && RdPtr(lock + 0x08, lockPos) && lockPos) {
				float lp[3] = {};
				if (RdF(lockPos + 0x10, lp[0]) && RdF(lockPos + 0x14, lp[1]) && RdF(lockPos + 0x18, lp[2]) &&
				    std::fabs(lp[0]) < 1e6f && std::fabs(lp[1]) < 1e6f && std::fabs(lp[2]) < 1e6f) {
					anchor[0] = lp[0]; anchor[1] = lp[1]; anchor[2] = lp[2];
				}
			}
		}

		// 巨型敌人：抬升量随最大血量放大（以普通兵 81 血为基准）
		float scale = 1.0f;
		if (g_cfg.scaleHeight) {
			scale = std::sqrt(maxHp / 81.0f);
			if (scale < 1.0f) scale = 1.0f;
			if (scale > 10.0f) scale = 10.0f;
		}
		anchor[1] += g_cfg.heightOffset * scale;

		Projected pa;
		if (!Project(anchor, winW, winH, pa)) continue;

		// 视野半角
		const float tx = pa.ndcX * g_tanHalfX;
		const float ty = pa.ndcY * g_tanHalfY;
		const float tanAngle = std::sqrt(tx * tx + ty * ty);
		const float cosAngle = 1.0f / std::sqrt(1.0f + tanAngle * tanAngle);
		if (cosAngle < cosLimit) continue;

		if (g_cfg.maxDistance > 0.0f && pa.depth > g_cfg.maxDistance) continue;

		// 最小屏幕身高
		if (g_cfg.minScreenSize > 0.0f) {
			Projected pb;
			if (!Project(pos, winW, winH, pb)) continue;
			if (std::fabs(pa.sy - pb.sy) < g_cfg.minScreenSize) continue;
		}

		// 遮挡：只在已经通过前面所有过滤的敌人身上做射线。
		// 射线终点必须往回收一点：直接打到敌人头顶会命中敌人自己的碰撞体，
		// 结果就是"永远判定为被遮挡、血条全都不画"（实测 13 条射线 0 条血条）。
		if (g_cfg.occlusion && g_world && eyeOk && !g_occlBroken) {
			int slot = OcclFind(o);
			bool blocked = false;
			if (slot >= 0 && (g_frame - g_occlSlotFrame[slot]) < g_cfg.occlReuse) {
				blocked = (g_occlSlotBlocked[slot] != 0);   // 沿用上次结果，本帧不投射线
			} else {
				++occl;
				const float dx = anchor[0] - eye[0];
				const float dy = anchor[1] - eye[1];
				const float dz = anchor[2] - eye[2];
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				float to[3] = { anchor[0], anchor[1], anchor[2] };
				if (dist > g_cfg.occlBackoff + 0.1f) {
					const float k = (dist - g_cfg.occlBackoff) / dist;
					to[0] = eye[0] + dx * k;
					to[1] = eye[1] + dy * k;
					to[2] = eye[2] + dz * k;
				}
				float frac = -1.0f;
				std::uintptr_t hitObj = 0;
				if (SafeCastRay(g_world, eye, to, &frac, &hitObj) && frac < g_cfg.occlSelfFrac) {
					const bool isChar = HitLooksLikeCharacter(hitObj);
					if (g_cfg.debug && g_occlDropLog < 40) {
						++g_occlDropLog;
						Log("[occl] 挡住 %p class=%s 距离=%.1fm 命中比例=%.4f 命中对象=%p class=%s 判为角色=%d",
						    reinterpret_cast<void*>(o), ClassTag(o).c_str(),
						    static_cast<double>(dist), static_cast<double>(frac),
						    reinterpret_cast<void*>(hitObj), ClassTag(hitObj).c_str(), isChar ? 1 : 0);
					}
					// 友军/任何角色单位都不算遮挡（用户明确要求）；只有地图/建筑/地形才丢条
					if (!(g_cfg.occlIgnoreChars && isChar)) blocked = true;
				}
				if (slot < 0) slot = OcclAdd(o);
				if (slot >= 0) {
					g_occlSlotBlocked[slot] = blocked ? 1 : 0;
					g_occlSlotFrame[slot]   = g_frame;
				}
			}
			if (blocked) {
				++g_occlDropped;
				continue;
			}
		}

		BarRect& b = tmp[count++];
		NoteBar(o, maxHp, curHp);
		float bw = static_cast<float>(g_cfg.barWidth);
		float bh = static_cast<float>(g_cfg.barHeight);
		if (g_cfg.widthByHp) {                       // 血条长度随最大血量（sqrt，夹 1~3 倍）
			float s = std::sqrt(maxHp / 81.0f);
			if (s < 1.0f) s = 1.0f;
			if (s > 3.0f) s = 3.0f;
			bw *= s;
		}
		if (g_cfg.scaleByDist && g_cfg.distRef > 1.0f) {   // 越远越小（夹 0.5~1 倍）
			float s = g_cfg.distRef / (pa.depth > 1.0f ? pa.depth : 1.0f);
			if (s < 0.5f) s = 0.5f;
			if (s > 1.0f) s = 1.0f;
			bw *= s;
			bh *= s;
		}
		b.w = bw;
		b.h = bh;
		b.x = pa.sx - b.w * 0.5f;
		b.y = pa.sy - b.h - 2.0f;
		b.frac = curHp / maxHp;
		if (b.frac < 0.0f) b.frac = 0.0f;
		if (b.frac > 1.0f) b.frac = 1.0f;
		const float pct = b.frac * 100.0f;
		b.color = (pct <= g_cfg.critHpPct) ? g_cfg.colorLow
		        : ((pct <= g_cfg.lowHpPct) ? g_cfg.colorMid : g_cfg.color);
		b.outline = g_cfg.barOutline ? 1 : 0;
	}

	g_occlUsed = occl;

	EnterCriticalSection(&g_snapLock);
	g_snap.screenW = winW;
	g_snap.screenH = winH;
	if (!g_cfg.drawBars) count = 0;   // 只关掉自绘，采集/探针不受影响
	if (count > kMaxBars) count = kMaxBars;
	g_snap.count = count;
	std::memcpy(g_snap.bars, tmp, sizeof(BarRect) * static_cast<std::size_t>(count));
	InterlockedIncrement(&g_snap.seq);
	LeaveCriticalSection(&g_snapLock);
	if (!g_snapEvent) g_snapEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // 自动重置
	if (g_snapEvent) SetEvent(g_snapEvent);
	InterlockedIncrement(&g_pubCount);
	g_pubTick = GetTickCount();
}

// ---------------------------------------------------------------------------
// Present 钩子
// ---------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(void* swapChain, UINT syncInterval, UINT flags);
typedef HRESULT (STDMETHODCALLTYPE *GetDescFn)(void* swapChain, void* desc);

static PresentFn g_origPresent = nullptr;
static void**    g_scVtbl      = nullptr;
static DWORD     g_lastFrame   = 0;

// 从交换链描述里取真实游戏窗口（IDXGISwapChain::GetDesc，vtable 槽 12）
static void TryGetSwapChainWindow(void* sc) {
	if (!sc) return;
	void** vt = *reinterpret_cast<void***>(sc);
	if (!vt || !Readable(vt, 13 * sizeof(void*))) return;
	GetDescFn fn = reinterpret_cast<GetDescFn>(vt[12]);
	if (!fn) return;
	DXGI_SWAP_CHAIN_DESC desc;
	std::memset(&desc, 0, sizeof desc);
	__try {
		const HRESULT hr = fn(sc, &desc);
		const HWND h = desc.OutputWindow;
		if (SUCCEEDED(hr) && h) {
			if (OverlayGameWindow() != h) {
				Log("[hook] 交换链输出窗口 hwnd=%p（GetDesc hr=0x%08lX）", reinterpret_cast<void*>(h), static_cast<unsigned long>(hr));
				OverlaySetGameWindow(h);
			}
		} else {
			static int n = 0;
			if (n++ < 3) Log("[hook] GetDesc hr=0x%08lX hwnd=%p", static_cast<unsigned long>(hr), reinterpret_cast<void*>(h));
		}
	} __except (EhFilter(GetExceptionInformation())) {
		static int n = 0;
		if (n++ < 3) Log("[hook] GetDesc 异常");
	}
}

static HRESULT STDMETHODCALLTYPE HookPresent(void* swapChain, UINT syncInterval, UINT flags) {
	const DWORD now = GetTickCount();
	const DWORD interval = (g_cfg.drawFps > 0) ? (1000u / static_cast<DWORD>(g_cfg.drawFps)) : 0;
	// 游戏失去焦点时不再采集（覆盖层那边也已停画并隐藏），省掉渲染线程上的射线与遍历开销
	HWND fgWnd = GetForegroundWindow();
	DWORD fgPid = 0;
	if (fgWnd) GetWindowThreadProcessId(fgWnd, &fgPid);
	const bool gameFocused = (fgWnd == nullptr) || (fgPid == GetCurrentProcessId());
	if (gameFocused && now - g_lastFrame >= interval) {
		g_lastFrame = now;
		++g_frame;
		if ((g_frame % 120) == 1) TryGetSwapChainWindow(swapChain);
		__try {
			CollectFrame();
		} __except (EhFilter(GetExceptionInformation())) {
		}
		if (g_cfg.debug && (g_frame % 600) == 0) {
			EnterCriticalSection(&g_snapLock);
			Log("[frame %d] 敌人条=%d 队伍表对象=%d 遮挡射线=%d 遮挡命中=%d world=%p", g_frame, g_snap.count, g_registry, g_occlUsed, g_occlDropped, g_world);
			LeaveCriticalSection(&g_snapLock);
		}
		if (g_cfg.debug && (g_frame % 600) == 1) LogFlush();
	}
	return g_origPresent(swapChain, syncInterval, flags);
}

static bool InstallPresentHook() {
	std::uintptr_t sc = 0;
	if (!RdPtr(g_base + off::kSwapChainPtr, sc) || !sc) return false;
	void** vt = *reinterpret_cast<void***>(sc);
	if (!vt || !Readable(vt, 9 * sizeof(void*))) return false;
	DWORD old = 0;
	if (!VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
	g_origPresent = reinterpret_cast<PresentFn>(vt[8]);
	vt[8] = reinterpret_cast<void*>(&HookPresent);
	DWORD dummy = 0;
	VirtualProtect(&vt[8], sizeof(void*), old, &dummy);
	g_scVtbl = vt;
	Log("[hook] IDXGISwapChain=%p vtable=%p Present=%p", reinterpret_cast<void*>(sc), vt, reinterpret_cast<void*>(g_origPresent));
	return true;
}

// ---------------------------------------------------------------------------
// 初始化线程
// ---------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID) {
	LogInit();
	Log("================ EDF5 敌人血条 MOD ================");
	Log("base=%p end=%p dir=%s", reinterpret_cast<void*>(g_base), reinterpret_cast<void*>(g_baseEnd), g_dir.c_str());
	Log("构建标记: EHB-1.2.0 (性能版：Readable 区间缓存；覆盖层只擦旧条/只传脏区/内容未变则跳过；DrawFps=0 跟随游戏帧)");
	Log("尺寸自检: DXGI_SWAP_CHAIN_DESC=%zu Collector=%zu RayQuery=%zu BarRect=%zu",
	    sizeof(DXGI_SWAP_CHAIN_DESC), sizeof(Collector), sizeof(RayQuery), sizeof(BarRect));

	LoadConfig(g_dir + "\\EDF5EnemyHealthBar.ini");
	Log("config: Enabled=%d Color=%06X %dx%d MaxViewAngle=%.1f Occlusion=%d HeightOffset=%.2f MaxBars=%d DrawFps=%d Debug=%d",
	    g_cfg.enabled, g_cfg.color, g_cfg.barWidth, g_cfg.barHeight, g_cfg.maxViewAngle,
	    g_cfg.occlusion, g_cfg.heightOffset, g_cfg.maxBars, g_cfg.drawFps, g_cfg.debug);
	Log("config2: MaxDistance=%.1f MinScreenSize=%.1f OcclBackoff=%.2f OcclIgnoreChars=%d RayFilter=0x%X EyeHeight=%.2f UseLockOn=%d ScaleHeight=%d NativeProbe=%d",
	    g_cfg.maxDistance, g_cfg.minScreenSize, g_cfg.occlBackoff, g_cfg.occlIgnoreChars, g_cfg.rayFilter,
	    g_cfg.eyeHeight, g_cfg.useLockOn, g_cfg.scaleHeight, g_cfg.nativeProbe);

	// 覆盖层窗口（独立线程）
	OverlayStart(nullptr);

	// 等游戏把自己的 swapchain 建起来
	for (int i = 0; i < 600 && !g_stop; ++i) {
		if (InstallPresentHook()) break;
		Sleep(100);
	}
	if (!g_origPresent) {
		Log("[!!] 没能挂上 Present 钩子（swapchain 全局 %p 一直为空）", reinterpret_cast<void*>(g_base + off::kSwapChainPtr));
		return 1;
	}
	Log("[ok] 初始化完成");

	while (!g_stop) Sleep(500);
	OverlayStop();
	Log("[bye]");
	LogClose();
	return 0;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = hinst;
		DisableThreadLibraryCalls(hinst);
	}
	return TRUE;
}

extern "C" BOOL __declspec(dllexport) EDFMLAPI EML5_Load(PluginInfo* pluginInfo) {
	pluginInfo->infoVersion = PluginInfo::MaxInfoVer;
	pluginInfo->name        = "EDF5 Enemy Healthbar";
	pluginInfo->version     = PLUG_VER(1, 2, 0, 0);

	char path[MAX_PATH] = {};
	GetModuleFileNameA(g_self, path, MAX_PATH);
	g_dir = path;
	const std::size_t slash = g_dir.find_last_of('\\');
	g_dir = (slash == std::string::npos) ? std::string(".") : g_dir.substr(0, slash);

	g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	g_baseEnd = g_base + 0x2000000;
	if (g_base) {
		const auto* dos = reinterpret_cast<const std::uint8_t*>(g_base);
		if (dos[0] == 'M' && dos[1] == 'Z') {
			std::int32_t peOff = 0;
			std::memcpy(&peOff, dos + 0x3C, 4);
			const std::uint8_t* pe = dos + peOff;
			if (pe[0] == 'P' && pe[1] == 'E') {
				std::uint32_t sizeOfImage = 0;
				std::memcpy(&sizeOfImage, pe + 0x18 + 0x38, 4);
				if (sizeOfImage > 0x100000 && sizeOfImage < 0x40000000) g_baseEnd = g_base + sizeOfImage;
			}
		}
	}

	InitializeCriticalSection(&g_snapLock);
	CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
	return TRUE;
}
