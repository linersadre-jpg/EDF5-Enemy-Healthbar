// EDF5 敌人血条 MOD — P0 只读探针
//
// 目的：在真实游戏里验证静态逆向得到的地址与结构，不做任何代码改写（不挂钩、不打补丁、
// 不写游戏内存）。数据全部写进 DLL 同目录的 EDF5_Probe.log，交给离线分析。
//
// 验证清单：
//   1. TeamManager 全局 (EDF5.exe+0x125AB80) 的 +0x38 行数组 / +0x48 队伍数 / 行 +0x18 关系数组
//   2. 队伍行里 std::map 的遍历（+0x00 head / +0x20 key / +0x19 nil），拿到对象指针
//   3. 对象布局：+0x1F4/+0x1F8/+0x1FC 血量的语义、+0x218 队伍、位置（+0x90 与 +0xA4 两个候选）
//   4. 对象真实类名（走 MSVC RTTI），用来区分敌人 / 子弹 / 友军
//   5. 相机 view/projection 矩阵链与世界转屏幕，验证投影数学
//
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "PluginAPI.h"

// ---------------------------------------------------------------------------
// EDF5 偏移（静态逆向结论，见 EDF5-敌人血条-逆向线索与P0计划.md）
// ---------------------------------------------------------------------------
namespace off {
	// 全局
	constexpr std::uintptr_t kTeamManagerPtr = 0x125AB80;  // -> TeamManager*
	constexpr std::uintptr_t kPlayerChain    = 0x125AB68;  // -> ... -> 玩家对象
	constexpr std::uintptr_t kCameraChain    = 0x125B080;  // -> ... -> 相机 {view@+0x80, proj@+0xE0}
	constexpr std::uintptr_t kWindowW        = 0x1256C00;  // int
	constexpr std::uintptr_t kWindowH        = 0x1256C04;  // int
	// TeamManager
	constexpr std::size_t kRows      = 0x38;
	constexpr std::size_t kCount     = 0x48;
	// 队伍行
	constexpr std::size_t kRowStride = 0x38;
	constexpr std::size_t kRelations = 0x18;
	// 对象
	constexpr std::size_t kMaxHp  = 0x1F8;  // 假定：最大血量（current - 4）
	constexpr std::size_t kCurHp  = 0x1FC;  // 已由参考 MOD 证实
	constexpr std::size_t kMinHp  = 0x1F4;  // 假定：当前血量的钳制下限
	constexpr std::size_t kTeam   = 0x218;  // 由伤害函数证实：用来索引队伍关系数组
	constexpr std::size_t kTeamOld= 0x180;  // EDF4.1 的 team 偏移，一并记录下来对照
	constexpr std::size_t kPosA   = 0x90;   // EDF4.1 / EDF6 的矩阵位置行
	constexpr std::size_t kPosB   = 0xA4;   // 参考 MOD 的兜底位置
	constexpr std::size_t kLockOn = 0x268;  // -> ... -> 锁定点 {+0x10,+0x14,+0x18}
	// std::map 节点（MSVC _Tree，EDF4.1 已验证）
	constexpr std::size_t kNodeLeft = 0x00;
	constexpr std::size_t kNodeParent = 0x08;
	constexpr std::size_t kNodeRight = 0x10;
	constexpr std::size_t kNodeIsNil = 0x19;
	constexpr std::size_t kNodeKey = 0x20;
}

static HMODULE g_self = nullptr;
static std::uintptr_t g_base = 0;
static std::uintptr_t g_baseEnd = 0;
static std::string g_dir;
static volatile LONG g_stop = 0;

// 配置
static int g_intervalMs = 500;
static int g_snapshots = 600;
static int g_maxEnemies = 64;
static bool g_rawDump = true;
// 只在“真的有敌人”的快照上做原始内存转储/位置扫描，且总量受限，避免日志爆掉
static int g_rawDumpSnaps = 0;
static int g_posScanSnaps = 0;

// ---------------------------------------------------------------------------
// 安全内存读
// ---------------------------------------------------------------------------
static bool Readable(const void* p, std::size_t n) {
	if (!p || n == 0) return false;
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
	if (mbi.State != MEM_COMMIT) return false;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
	const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(p);
	const std::uintptr_t rstart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
	return start + n <= rstart + mbi.RegionSize;
}

template <class T>
static bool Rd(std::uintptr_t a, T& out) {
	if (a == 0 || !Readable(reinterpret_cast<const void*>(a), sizeof(T))) return false;
	std::memcpy(&out, reinterpret_cast<const void*>(a), sizeof(T));
	return true;
}

static bool RdPtr(std::uintptr_t a, std::uintptr_t& out) {
	void* p = nullptr;
	if (!Rd<void*>(a, p)) return false;
	out = reinterpret_cast<std::uintptr_t>(p);
	return true;
}

static bool RdF(std::uintptr_t a, float& out) { return Rd<float>(a, out); }
static bool RdI(std::uintptr_t a, std::int32_t& out) { return Rd<std::int32_t>(a, out); }

static std::uintptr_t Chain(std::uintptr_t base, std::initializer_list<std::size_t> offs) {
	std::uintptr_t p = base;
	for (std::size_t o : offs) {
		std::uintptr_t q = 0;
		if (!RdPtr(p + o, q)) return 0;
		p = q;
	}
	return p;
}

// ---------------------------------------------------------------------------
// MSVC RTTI：对象 -> 类名（映射 .?AV 开头）
// ---------------------------------------------------------------------------
static std::string ClassName(std::uintptr_t obj) {
	std::uintptr_t vt = 0;
	if (!RdPtr(obj, vt) || vt < g_base || vt > g_baseEnd) return {};
	std::uintptr_t col = 0;
	if (!RdPtr(vt - 8, col) || col < g_base || col > g_baseEnd) return {};
	std::int32_t sig = 0;
	// MSVC RTTICompleteObjectLocator 的 signature：32 位 = 0，64 位 = 1
	if (!RdI(col, sig) || (sig != 0 && sig != 1)) return {};
	std::uint32_t tdRva = 0;
	if (!Rd<std::uint32_t>(col + 0x0C, tdRva)) return {};
	std::uintptr_t td = g_base + tdRva;
	sig = 0;
	if (!RdI(td, sig) || (sig != 0 && sig != 1)) return {};
	char name[256] = {};
	if (!Readable(reinterpret_cast<const void*>(td + 0x10), sizeof name)) return {};
	std::memcpy(name, reinterpret_cast<const void*>(td + 0x10), sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	if (name[0] != '.' || name[1] != '?' || name[2] != 'A') return {};
	return std::string(name);
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
static void Trim(std::string& s) {
	while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
	std::size_t i = 0;
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
	s.erase(0, i);
}

static void LoadConfig() {
	const std::string path = g_dir + "\\EDF5_Probe.ini";
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		std::string s(line);
		Trim(s);
		if (s.empty() || s[0] == ';' || s[0] == '#') continue;
		const std::size_t eq = s.find('=');
		if (eq == std::string::npos) continue;
		std::string k = s.substr(0, eq), v = s.substr(eq + 1);
		Trim(k);
		Trim(v);
		const int n = atoi(v.c_str());
		if (k == "IntervalMs") g_intervalMs = std::max(50, n);
		else if (k == "Snapshots") g_snapshots = std::max(1, n);
		else if (k == "MaxEnemies") g_maxEnemies = std::max(1, n);
		else if (k == "RawDump") g_rawDump = (n != 0);
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// 相机 / 世界转屏幕（复用参考 MOD 的数学）
// ---------------------------------------------------------------------------
static float g_vp[16] = {};

static void BuildViewProjection(const float* v, const float* p) {
	g_vp[0]  = v[0] * p[0];  g_vp[1]  = v[1] * p[0];  g_vp[2]  = v[2] * p[0];  g_vp[3]  = v[3] * p[0];
	g_vp[4]  = v[4] * p[5];  g_vp[5]  = v[5] * p[5];  g_vp[6]  = v[6] * p[5];  g_vp[7]  = v[7] * p[5];
	g_vp[8]  = v[8] * p[10]; g_vp[9]  = v[9] * p[10]; g_vp[10] = v[10] * p[10];g_vp[11] = v[11] * p[10] + p[11];
	g_vp[12] = v[8] * p[14]; g_vp[13] = v[9] * p[14]; g_vp[14] = v[10] * p[14];g_vp[15] = v[11] * p[14];
}

// 世界坐标 -> 屏幕（左上原点，返回 false 表示在相机后方）
static bool WorldToScreen(const float pos[3], int w, int h, float& sx, float& sy, float& depth) {
	const float cx = pos[0] * g_vp[0] + pos[1] * g_vp[1] + pos[2] * g_vp[2] + g_vp[3];
	const float cy = pos[0] * g_vp[4] + pos[1] * g_vp[5] + pos[2] * g_vp[6] + g_vp[7];
	const float cw = pos[0] * g_vp[12] + pos[1] * g_vp[13] + pos[2] * g_vp[14] + g_vp[15];
	depth = cw;
	if (!(cw > 0.1f)) return false;
	sx = -(static_cast<float>(w) * 0.5f * (cx / cw)) + static_cast<float>(w) * 0.5f;
	sy = -(static_cast<float>(h) * 0.5f * (cy / cw)) + static_cast<float>(h) * 0.5f;
	return true;
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...) {
	if (!g_log) return;
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
	va_end(ap);
	fputs(buf, g_log);
	fputc('\n', g_log);
}

static void DumpFloats(const char* tag, std::uintptr_t a, std::size_t from, std::size_t to, std::size_t step) {
	for (std::size_t o = from; o <= to; o += step) {
		float f = 0;
		if (RdF(a + o, f)) Log("      %s +0x%03zX = %12.4f  (0x%08X)", tag, o, f, *reinterpret_cast<std::uint32_t*>(&f));
	}
}

static void DumpHex(std::uintptr_t a, std::size_t from, std::size_t to) {
	for (std::size_t o = from; o < to; o += 16) {
		char line[256] = {};
		int n = _snprintf_s(line, sizeof line, _TRUNCATE, "      +0x%04zX:", o);
		for (std::size_t i = 0; i < 16; ++i) {
			std::uint8_t b = 0;
			if (!Rd<std::uint8_t>(a + o + i, b)) break;
			const int w = _snprintf_s(line + n, sizeof(line) - static_cast<std::size_t>(n), _TRUNCATE, " %02X", b);
			if (w < 0) break;
			n += w;
		}
		Log("%s", line);
	}
}

// ---------------------------------------------------------------------------
// 快照
// ---------------------------------------------------------------------------
static int g_snap = 0;

static void TakeSnapshot() {
	// --- 基础指针 ---
	std::uintptr_t mgr = 0;
	if (!RdPtr(g_base + off::kTeamManagerPtr, mgr) || !mgr) {
		Log("[snap %d] TeamManager 尚未就绪 (global=%p)", g_snap, reinterpret_cast<void*>(mgr));
		return;
	}
	std::uintptr_t rows = 0, player = 0;
	if (!RdPtr(mgr + off::kRows, rows)) rows = 0;
	std::int32_t teamCount = 0;
	if (!RdI(mgr + off::kCount, teamCount)) teamCount = 0;
	player = Chain(g_base, { off::kPlayerChain, 0x238, 0x290, 0x10 });

	std::uintptr_t cam = Chain(g_base, { off::kCameraChain, 0x8, 0x8, 0x8 });
	std::int32_t winW = 0, winH = 0;
	RdI(g_base + off::kWindowW, winW);
	RdI(g_base + off::kWindowH, winH);

	const float* view = nullptr;
	const float* proj = nullptr;
	float viewBuf[16] = {}, projBuf[16] = {};
	if (cam && Readable(reinterpret_cast<const void*>(cam + 0x80), 64) &&
	    Readable(reinterpret_cast<const void*>(cam + 0xE0), 64)) {
		std::memcpy(viewBuf, reinterpret_cast<const void*>(cam + 0x80), 64);
		std::memcpy(projBuf, reinterpret_cast<const void*>(cam + 0xE0), 64);
		view = viewBuf;
		proj = projBuf;
		BuildViewProjection(viewBuf, projBuf);
	}

	// --- 玩家 ---
	std::int32_t myTeam = -1;
	if (player) {
		RdI(player + off::kTeam, myTeam);
		float pp[3] = {};
		float ppb[3] = {};
		RdF(player + off::kPosA, pp[0]); RdF(player + off::kPosA + 4, pp[1]); RdF(player + off::kPosA + 8, pp[2]);
		RdF(player + off::kPosB, ppb[0]); RdF(player + off::kPosB + 4, ppb[1]); RdF(player + off::kPosB + 8, ppb[2]);
		float sx = 0, sy = 0, d = 0;
		bool on = view && WorldToScreen(pp, winW, winH, sx, sy, d);
		Log("[snap %d] player=%p class=%s team=%d posA(0x90)=(%.2f,%.2f,%.2f) posB(0xA4)=(%.2f,%.2f,%.2f) screen=(%.1f,%.1f) depth=%.2f",
		    g_snap, reinterpret_cast<void*>(player), ClassName(player).c_str(), myTeam,
		    pp[0], pp[1], pp[2], ppb[0], ppb[1], ppb[2], sx, sy, d);
		(void)on;
	}

	Log("[snap %d] base=%p mgr=%p rows=%p teamCount=%d cam=%p win=%dx%d view=%p proj=%p",
	    g_snap, reinterpret_cast<void*>(g_base), reinterpret_cast<void*>(mgr), reinterpret_cast<void*>(rows),
	    teamCount, reinterpret_cast<void*>(cam), winW, winH,
	    reinterpret_cast<const void*>(view), reinterpret_cast<const void*>(proj));
	if (view) {
		Log("      view  = [%8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f]",
		    view[0], view[1], view[2], view[3], view[4], view[5], view[6], view[7],
		    view[8], view[9], view[10], view[11], view[12], view[13], view[14], view[15]);
		Log("      proj  = [%8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f]",
		    proj[0], proj[1], proj[2], proj[3], proj[4], proj[5], proj[6], proj[7],
		    proj[8], proj[9], proj[10], proj[11], proj[12], proj[13], proj[14], proj[15]);
	}

	if (!rows || teamCount <= 0 || teamCount > 32) {
		Log("      rows/teamCount 异常，跳过队伍遍历");
		return;
	}

	// --- 关系数组：玩家队伍对各队的关系 ---
	std::vector<std::int32_t> myRel(teamCount, -1);
	if (myTeam >= 0 && myTeam < teamCount) {
		std::uintptr_t relPtr = 0;
		if (RdPtr(rows + static_cast<std::size_t>(myTeam) * off::kRowStride + off::kRelations, relPtr)) {
			for (int t = 0; t < teamCount; ++t) RdI(relPtr + t * 4, myRel[t]);
		}
	}

	int logged = 0;
	std::vector<std::uintptr_t> allEnemies;
	allEnemies.reserve(256);
	for (int t = 0; t < teamCount; ++t) {
		const std::uintptr_t row = rows + static_cast<std::size_t>(t) * off::kRowStride;
		std::uintptr_t head = 0, relPtr = 0;
		RdPtr(row + 0x00, head);
		RdPtr(row + off::kRelations, relPtr);

		// 收集这一队的对象：MSVC _Tree，head 是哨兵，真正的根在 head->_Parent(+0x08)
		std::vector<std::uintptr_t> objects;
		objects.reserve(256);
		if (head) {
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
				if (obj) objects.push_back(obj);
			}
		}

		const int rel = (t < static_cast<int>(myRel.size())) ? myRel[t] : -1;
		Log("    team %d: relation=%d head=%p objects=%zu%s", t, rel, reinterpret_cast<void*>(head),
		    objects.size(), (rel == 2 && t != myTeam) ? "  <== 敌对队伍" : "");

		const bool isEnemy = (rel == 2 && t != myTeam);
		std::size_t limit = isEnemy ? static_cast<std::size_t>(g_maxEnemies) : 2;
		for (std::size_t i = 0; i < objects.size() && i < limit && logged < 200; ++i) {
			const std::uintptr_t o = objects[i];
			++logged;
			if (isEnemy) allEnemies.push_back(o);
			std::int32_t teamNew = -1, teamOld = -1;
			float minHp = 0, maxHp = 0, curHp = 0;
			RdI(o + off::kTeam, teamNew);
			RdI(o + off::kTeamOld, teamOld);
			RdF(o + off::kMinHp, minHp);
			RdF(o + off::kMaxHp, maxHp);
			RdF(o + off::kCurHp, curHp);
			float pa[3] = {}, pb[3] = {};
			RdF(o + off::kPosA, pa[0]); RdF(o + off::kPosA + 4, pa[1]); RdF(o + off::kPosA + 8, pa[2]);
			RdF(o + off::kPosB, pb[0]); RdF(o + off::kPosB + 4, pb[1]); RdF(o + off::kPosB + 8, pb[2]);

			std::uintptr_t lock = 0, lockPos = 0;
			RdPtr(o + off::kLockOn, lock);
			if (lock) RdPtr(lock + 8, lockPos);
			float lp[3] = {};
			if (lockPos) { RdF(lockPos + 0x10, lp[0]); RdF(lockPos + 0x14, lp[1]); RdF(lockPos + 0x18, lp[2]); }

			float sxa = 0, sya = 0, da = 0, sxb = 0, syb = 0, db = 0, sxl = 0, syl = 0, dl = 0;
			const bool onA = view && WorldToScreen(pa, winW, winH, sxa, sya, da);
			const bool onB = view && WorldToScreen(pb, winW, winH, sxb, syb, db);
			const bool onL = (view && lockPos) ? WorldToScreen(lp, winW, winH, sxl, syl, dl) : false;

			Log("      obj=%p class=%s team(new0x218)=%d team(old0x180)=%d hp cur/max/min=%.1f/%.1f/%.1f",
			    reinterpret_cast<void*>(o), ClassName(o).c_str(), teamNew, teamOld, curHp, maxHp, minHp);
			Log("         posA(0x90)=(%.2f,%.2f,%.2f) scr=(%.0f,%.0f) w=%.2f | posB(0xA4)=(%.2f,%.2f,%.2f) scr=(%.0f,%.0f) w=%.2f%s",
			    pa[0], pa[1], pa[2], sxa, sya, da, pb[0], pb[1], pb[2], sxb, syb, db,
			    onA != onB ? " [前后判定不一致]" : "");
			if (lockPos) Log("         lockon ptr=%p pos=(%.2f,%.2f,%.2f) scr=(%.0f,%.0f) w=%.2f%s", reinterpret_cast<void*>(lockPos), lp[0], lp[1], lp[2], sxl, syl, dl, onL ? "" : " (在相机后方)");
			else Log("         lockon ptr=%p (无)", reinterpret_cast<void*>(lock));

			// 位置候选扫描：把 +0x80..+0xC0 每 4 字节当成 (x,y,z) 投一次影。
			// 谁能稳定落在屏幕内、且在相机前方，谁才是真正的位置。
			if (g_rawDump && isEnemy && i < 3 && g_posScanSnaps < 3) {
				Log("        位置候选扫描 (本帧第 %zu 个敌人):", i);
				for (std::size_t cz = 0x80; cz <= 0xC0; cz += 4) {
					float c[3] = {};
					if (!RdF(o + cz, c[0]) || !RdF(o + cz + 4, c[1]) || !RdF(o + cz + 8, c[2])) continue;
					float qx = 0, qy = 0, qd = 0;
					const bool ok = view ? WorldToScreen(c, winW, winH, qx, qy, qd) : false;
					const bool inside = ok && qx >= 0 && qx <= static_cast<float>(winW) && qy >= 0 && qy <= static_cast<float>(winH);
					Log("          +0x%03zX = (%10.2f %10.2f %10.2f)  scr=(%8.0f,%8.0f) w=%9.2f%s",
					    cz, c[0], c[1], c[2], qx, qy, qd, inside ? "  <屏内>" : "");
				}
			}

			if (g_rawDump && isEnemy && i < 2 && g_rawDumpSnaps < 2) {
				Log("      --- obj %p class=%s 邻域 ---", reinterpret_cast<void*>(o), ClassName(o).c_str());
				Log("        血量邻域 (0x1E0..0x210):");
				DumpFloats("f", o, 0x1E0, 0x210, 4);
				Log("        队伍/标志邻域 (0x210..0x220):");
				for (std::size_t k = 0x210; k <= 0x21C; k += 4) {
					std::int32_t iv = 0;
					if (RdI(o + k, iv)) Log("      i +0x%03zX = %d (0x%08X)", k, iv, static_cast<std::uint32_t>(iv));
				}
				Log("        变换/位置邻域 (0x60..0x120):");
				DumpFloats("f", o, 0x60, 0x120, 4);
				Log("        对象头部 hex (0x00..0x80):");
				DumpHex(o, 0x00, 0x80);
			}
		}
	}

	// 首帧：TeamManager 结构本体
	if (g_rawDump && g_snap <= 1) {
		Log("    TeamManager 结构 (mgr+0x00..0x80) hex:");
		DumpHex(mgr, 0x00, 0x80);
		Log("    TeamManager 指针字段 (每 8 字节):");
		for (std::size_t k = 0; k <= 0x60; k += 8) {
			std::uintptr_t p = 0;
			if (RdPtr(mgr + k, p)) Log("      +0x%03zX -> %p", k, reinterpret_cast<void*>(p));
		}
	}

	// --- 相机对象原始数据（为“最大显示距离”找相机世界坐标）---
	if (g_rawDump && g_rawDumpSnaps == 0 && view) {
		const std::uintptr_t cam0 = reinterpret_cast<std::uintptr_t>(view) - 0x80;
		Log("    相机对象原始数据 %p (0x00..0x180):", reinterpret_cast<void*>(cam0));
		DumpFloats("f", cam0, 0x00, 0x180, 4);
	}

	// --- 位置偏移投票：所有敌人一起投票，看哪个偏移最像“世界坐标” ---
	if (view && !allEnemies.empty() && g_posScanSnaps <= 4) {
		Log("    ===== 位置偏移投票（敌人 %zu 个，屏幕 %dx%d）=====", allEnemies.size(), winW, winH);
		for (std::size_t cz = 0x60; cz <= 0xC0; cz += 4) {
			int sane = 0, front = 0, inside = 0;
			for (std::uintptr_t o : allEnemies) {
				float c[3] = {};
				if (!RdF(o + cz, c[0]) || !RdF(o + cz + 4, c[1]) || !RdF(o + cz + 8, c[2])) continue;
				if (c[0] > -1e5f && c[0] < 1e5f && c[1] > -1e5f && c[1] < 1e5f && c[2] > -1e5f && c[2] < 1e5f) ++sane;
				float qx = 0, qy = 0, qd = 0;
				if (!WorldToScreen(c, winW, winH, qx, qy, qd)) continue;
				++front;
				if (qx >= 0 && qx <= static_cast<float>(winW) && qy >= 0 && qy <= static_cast<float>(winH)) ++inside;
			}
			Log("      +0x%03zX: 数值合理 %3d  在相机前方 %3d  屏内 %3d", cz, sane, front, inside);
		}
	}
	if (!allEnemies.empty()) { ++g_posScanSnaps; ++g_rawDumpSnaps; }

	fflush(g_log);
}

// ---------------------------------------------------------------------------
// 线程
// ---------------------------------------------------------------------------
static DWORD WINAPI ProbeThread(LPVOID) {
	LoadConfig();
	g_log = nullptr;
	const std::string path = g_dir + "\\EDF5_Probe.log";
	fopen_s(&g_log, path.c_str(), "wb");
	if (!g_log) return 1;

	Log("EDF5 敌人血条 MOD - P0 只读探针");
	Log("exe=%p base=%p end=%p", reinterpret_cast<void*>(g_base), reinterpret_cast<void*>(g_base),
	    reinterpret_cast<void*>(g_baseEnd));
	Log("config: IntervalMs=%d Snapshots=%d MaxEnemies=%d RawDump=%d", g_intervalMs, g_snapshots, g_maxEnemies, g_rawDump ? 1 : 0);

	while (!g_stop && g_snap < g_snapshots) {
		// 等游戏把 TeamManager 建起来（在主菜单/加载时它还不在）
		std::uintptr_t mgr = 0;
		RdPtr(g_base + off::kTeamManagerPtr, mgr);
		if (!mgr) {
			Sleep(500);
			continue;
		}
		TakeSnapshot();
		++g_snap;
		for (int i = 0; i < g_intervalMs / 50 && !g_stop; ++i) Sleep(50);
	}
	Log("探针结束：共 %d 个快照", g_snap);
	if (g_log) { fflush(g_log); fclose(g_log); g_log = nullptr; }
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
	pluginInfo->name = "EDF5 Enemy Healthbar Probe";
	pluginInfo->version = PLUG_VER(0, 1, 0, 0);

	char path[MAX_PATH] = {};
	GetModuleFileNameA(g_self, path, MAX_PATH);
	g_dir = path;
	const std::size_t slash = g_dir.find_last_of('\\');
	g_dir = (slash == std::string::npos) ? std::string(".") : g_dir.substr(0, slash);

	g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	// 从 PE 头取 SizeOfImage，作为 RTTI 指针的有效范围
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

	CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
	return TRUE;
}
