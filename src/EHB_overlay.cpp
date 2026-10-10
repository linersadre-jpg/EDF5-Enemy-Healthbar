// EDF5 敌人血条 MOD —— 分层覆盖层窗口
//
// 一个 WS_EX_LAYERED | WS_EX_TRANSPARENT 的置顶弹出窗口，用 UpdateLayeredWindow 提交
// 32 位预乘 alpha 位图。窗口本身不接收鼠标/键盘，也不抢焦点。
//
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "EHB_common.h"

#include <cmath>

static const wchar_t* kClassName = L"EDF5EnemyHealthBarOverlay";

static HWND   g_overlay   = nullptr;
static HANDLE g_ovThread  = nullptr;
static volatile LONG g_ovStop = 0;
static volatile LONG_PTR g_gameHwnd = 0;
static int  g_noWinFrames   = 0;
static bool g_desktopLogged = false;

// ---------------------------------------------------------------------------
// 窗口查找：枚举本进程所有可见顶层窗口，优先选客户区正好等于渲染分辨率的那个
// ---------------------------------------------------------------------------
struct WindowList {
	HWND hwnd[64];
	LONG cw[64], ch[64];
	int  n;
};

static bool g_winLogged = false;

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
	WindowList* L = reinterpret_cast<WindowList*>(lp);
	if (L->n >= 64) return TRUE;
	if (hwnd == g_overlay) return TRUE;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != GetCurrentProcessId()) return TRUE;
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
	RECT rc = {};
	if (!GetClientRect(hwnd, &rc)) return TRUE;
	const LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
	if (w < 64 || h < 64) return TRUE;
	L->hwnd[L->n] = hwnd; L->cw[L->n] = w; L->ch[L->n] = h; ++L->n;
	return TRUE;
}

static HWND FindGameWindow(int wantW, int wantH) {
	WindowList L = {};
	EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&L));
	if (L.n == 0) return nullptr;
	int best = -1;
	for (int i = 0; i < L.n; ++i) {
		if (wantW > 0 && wantH > 0 && L.cw[i] == wantW && L.ch[i] == wantH) { best = i; break; }
	}
	if (best < 0) {
		LONG bestArea = -1;
		for (int i = 0; i < L.n; ++i) {
			const LONG a = L.cw[i] * L.ch[i];
			if (a > bestArea) { bestArea = a; best = i; }
		}
	}
	if (!g_winLogged) {
		g_winLogged = true;
		Log("[overlay] 本进程可见窗口 %d 个，渲染分辨率 %dx%d", L.n, wantW, wantH);
		for (int i = 0; i < L.n; ++i)
			Log("[overlay]   hwnd=%p 客户区=%ldx%ld%s", reinterpret_cast<void*>(L.hwnd[i]), L.cw[i], L.ch[i],
			    (i == best) ? "   <== 选中" : "");
	}
	return L.hwnd[best];
}

// ---------------------------------------------------------------------------
// 像素绘制（预乘 alpha）
// ---------------------------------------------------------------------------
static inline void BlendPx(std::uint32_t* buf, int W, int H, int x, int y, std::uint32_t color, int alpha) {
	if (x < 0 || y < 0 || x >= W || y >= H || alpha <= 0) return;
	const std::uint32_t sa = static_cast<std::uint32_t>(alpha);
	std::uint32_t dst = buf[static_cast<std::size_t>(y) * W + x];
	const std::uint32_t da = (dst >> 24) & 0xFF;

	const std::uint32_t inv = 255 - sa;
	const std::uint32_t outA = sa + da * inv / 255;
	if (outA == 0) { buf[static_cast<std::size_t>(y) * W + x] = 0; return; }

	const std::uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
	const std::uint32_t dr = (dst >> 16) & 0xFF,    dg = (dst >> 8) & 0xFF,    db = dst & 0xFF;

	const std::uint32_t r = sr * sa / 255 + dr * inv / 255;
	const std::uint32_t g = sg * sa / 255 + dg * inv / 255;
	const std::uint32_t b = sb * sa / 255 + db * inv / 255;

	buf[static_cast<std::size_t>(y) * W + x] = (outA << 24) | (r << 16) | (g << 8) | b;
}

static void FillRect(std::uint32_t* buf, int W, int H, int x0, int y0, int x1, int y1,
                     std::uint32_t color, int alpha) {
	if (x1 < x0) { const int t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { const int t = y0; y0 = y1; y1 = t; }
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > W - 1) x1 = W - 1;
	if (y1 > H - 1) y1 = H - 1;
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x) BlendPx(buf, W, H, x, y, color, alpha);
}

// ---------------------------------------------------------------------------
// 覆盖层线程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_ERASEBKGND: return 1;
	case WM_CLOSE: return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	default: break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI OverlayThreadProc(LPVOID) {
	WNDCLASSEXW wc = {};
	wc.cbSize        = sizeof wc;
	wc.lpfnWndProc   = OverlayProc;
	wc.hInstance     = g_self;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = kClassName;
	RegisterClassExW(&wc);

	g_overlay = CreateWindowExW(
		WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
		kClassName, L"EDF5 Enemy Healthbar", WS_POPUP,
		0, 0, 1, 1, nullptr, nullptr, g_self, nullptr);
	if (!g_overlay) { Log("[overlay] 创建窗口失败 err=%lu", GetLastError()); return 1; }
	ShowWindow(g_overlay, SW_SHOWNOACTIVATE);

	HDC     memDC   = nullptr;
	HBITMAP bmp     = nullptr;
	void*   bmpBits = nullptr;
	int     bufW = 0, bufH = 0;

	HWND  cachedGame = nullptr;
	DWORD lastDraw = 0;

	// 增量绘制状态：只擦"上一帧画过的条"、只上传变化区域
	static BarRect prevBars[kMaxBars];                 // 上一帧的条（渲染坐标，用于"内容没变就跳过"）
	int  prevCount = 0;
	int  prevX0[kMaxBars], prevY0[kMaxBars], prevX1[kMaxBars], prevY1[kMaxBars]; // 上一帧的条（客户区像素，用于擦除）
	int  newX0[kMaxBars],  newY0[kMaxBars],  newX1[kMaxBars],  newY1[kMaxBars];
	LONG placedX = LONG_MIN, placedY = LONG_MIN;
	int  placedW = 0, placedH = 0;
	bool placed = false;
	bool firstUpload = true;

	while (!g_ovStop) {
		const DWORD now = GetTickCount();

		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		// 等"游戏又出了一帧新快照"（Present 钩子里 SetEvent），而不是空转轮询：
		// 空转会占满一个核、反过来拖慢游戏线程（实测会让血条更滞后）。
		// DrawFps>0 时用它当上限，否则最多等 4ms（保证窗口跟随/清理动作也及时）。
		{
			const DWORD waitMs = (g_cfg.drawFps > 0) ? (1000u / static_cast<DWORD>(g_cfg.drawFps)) : 4u;
			if (g_snapEvent) WaitForSingleObject(g_snapEvent, waitMs);
			else Sleep(waitMs);
		}

		lastDraw = now;

		int wantW = 0, wantH = 0;
		EnterCriticalSection(&g_snapLock);
		wantW = g_snap.screenW;
		wantH = g_snap.screenH;
		LeaveCriticalSection(&g_snapLock);

		int cw = 0, ch = 0;
		POINT origin = { 0, 0 };
		bool  haveBox = false;

		if (g_cfg.overlayMode != 1) {
			// 首选交换链描述里的真实窗口（唯一可靠来源）
			HWND g = OverlayGameWindow();
			if (g && IsWindow(g) && IsWindowVisible(g) && !IsIconic(g)) cachedGame = g;
			else if (!cachedGame || !IsWindow(cachedGame) || !IsWindowVisible(cachedGame))
				cachedGame = FindGameWindow(wantW, wantH);

			if (cachedGame && IsWindow(cachedGame) && IsWindowVisible(cachedGame) && !IsIconic(cachedGame)) {
				RECT client = {};
				if (GetClientRect(cachedGame, &client)) {
					cw = client.right - client.left;
					ch = client.bottom - client.top;
					if (cw > 0 && ch > 0) {
						ClientToScreen(cachedGame, &origin);
						haveBox = true;
					}
				}
			}
		}

		if (!haveBox) {
			// 找不到游戏窗口时先等一会儿；一直找不到就铺满整屏兜底（独占全屏下看不见，会被日志点明）
			if (++g_noWinFrames > 120 || g_cfg.overlayMode == 1) {
				cw = GetSystemMetrics(SM_CXSCREEN);
				ch = GetSystemMetrics(SM_CYSCREEN);
				origin.x = 0; origin.y = 0;
				haveBox = (cw > 0 && ch > 0);
				if (haveBox && !g_desktopLogged) {
					g_desktopLogged = true;
					Log("[overlay] 找不到游戏窗口，改用整屏兜底 %dx%d（注意：独占全屏下这样依然看不见）", cw, ch);
				}
			} else {
				static DWORD lastHideLog = 0;
				const DWORD t = GetTickCount();
				if (t - lastHideLog > 5000) { lastHideLog = t;
					Log("[overlay] 还没找到游戏窗口（交换链窗口=%p 缓存=%p）", reinterpret_cast<void*>(OverlayGameWindow()), reinterpret_cast<void*>(cachedGame)); }
				ShowWindow(g_overlay, SW_HIDE);
				placed = false;      // 重新出现时必须重新摆一次窗口
				Sleep(20);
				continue;
			}
			if (!haveBox) { Sleep(50); continue; }
		}

		// 游戏失去焦点（切到桌面/别的程序）就完全停画并隐藏覆盖层，不再空跑。
		// 判定方式：前台窗口的进程 PID 是否就是本进程（本 DLL 运行在游戏进程里）。
		// GetForegroundWindow 返回 NULL 时按"仍有焦点"处理，避免误隐藏。
		{
			const HWND fg = GetForegroundWindow();
			DWORD fgPid = 0;
			if (fg) GetWindowThreadProcessId(fg, &fgPid);
			const bool focused = (fg == nullptr) || (fgPid == GetCurrentProcessId());
			if (!focused) {
				if (placed) {
					ShowWindow(g_overlay, SW_HIDE);
					placed = false;
					firstUpload = true;   // 重新出现时整张重传，避免残留旧条
				}
				Sleep(50);
				continue;
			}
		}

		// 位图按客户区大小重建（只在尺寸变化时重建；持久 DIB，不再每帧新建）
		bool fresh = false;
		if (cw != bufW || ch != bufH || !memDC) {
			if (bmp) { DeleteObject(bmp); bmp = nullptr; }
			if (memDC) { DeleteDC(memDC); memDC = nullptr; }
			memDC = CreateCompatibleDC(nullptr);
			BITMAPINFO bi = {};
			bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth       = cw;
			bi.bmiHeader.biHeight      = -ch;   // 自上而下
			bi.bmiHeader.biPlanes      = 1;
			bi.bmiHeader.biBitCount    = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			bmp = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bmpBits, nullptr, 0);
			if (!bmp || !bmpBits) { Sleep(200); continue; }
			SelectObject(memDC, bmp);
			bufW = cw; bufH = ch;
			fresh = true;
			prevCount = 0;
			firstUpload = true;
		}

		// 取快照
		int count = 0, dispW = 0, dispH = 0;
		EnterCriticalSection(&g_snapLock);
		count = g_snap.count;
		if (count < 0) count = 0;
		if (count > kMaxBars) count = kMaxBars;    // 绝不让快照里的数字决定拷贝长度
		dispW = g_snap.screenW;
		dispH = g_snap.screenH;
		static BarRect bars[kMaxBars];
		std::memcpy(bars, g_snap.bars, sizeof(BarRect) * static_cast<std::size_t>(count));
		LeaveCriticalSection(&g_snapLock);
		if (dispW <= 0) dispW = cw;      // 渲染分辨率没读到就用客户区兜底
		if (dispH <= 0) dispH = ch;

		static DWORD lastDrawLog = 0;
		if (now - lastDrawLog > 3000) {
			static LONG lastPub = 0, lastDraws = 0;
			const LONG pub  = InterlockedCompareExchange(&g_pubCount, 0, 0);
			const LONG drw  = InterlockedCompareExchange(&g_drawCount, 0, 0);
			const DWORD age = now - g_pubTick;   // 画的时候快照已经多旧
			lastDrawLog = now;
			Log("[draw] 条数=%d 覆盖层=%dx%d@(%ld,%ld) 渲染=%dx%d 游戏窗口=%p OverlayMode=%d | 发布=%d 绘制=%d 快照年龄=%lums",
			    count, cw, ch, origin.x, origin.y, dispW, dispH,
			    reinterpret_cast<void*>(cachedGame), g_cfg.overlayMode,
			    pub - lastPub, drw - lastDraws, static_cast<unsigned long>(age));
			lastPub = pub;
			lastDraws = drw;
		}

		// 内容与上一帧完全一致就整帧跳过：静态画面下把每秒 60 次整屏提交降为 0
		bool same = (count == prevCount);
		if (same) {
			for (int i = 0; i < count; ++i) {
				const BarRect& a = bars[i];
				const BarRect& p = prevBars[i];
				if (a.x != p.x || a.y != p.y || a.w != p.w || a.h != p.h ||
				    a.frac != p.frac || a.color != p.color || a.outline != p.outline) { same = false; break; }
			}
		}
		if (same && placed && !fresh) continue;

		std::uint32_t* buf = static_cast<std::uint32_t*>(bmpBits);
		if (fresh) std::memset(bmpBits, 0, static_cast<std::size_t>(cw) * ch * 4);

		// 只擦"上一帧画过的小矩形"（50x5 像素级别），不再整屏 memset（14.4 MB/帧 → 几百字节/帧）
		auto ClearBox = [&](int x0, int y0, int x1, int y1) {
			if (x0 < 0) x0 = 0;
			if (y0 < 0) y0 = 0;
			if (x1 > cw) x1 = cw;
			if (y1 > ch) y1 = ch;
			if (x1 <= x0 || y1 <= y0) return;
			const std::size_t w = static_cast<std::size_t>(x1 - x0) * sizeof(std::uint32_t);
			for (int y = y0; y < y1; ++y) {
				std::memset(buf + static_cast<std::size_t>(y) * cw + x0, 0, w);
			}
		};
		int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
		auto MarkDirty = [&](int x0, int y0, int x1, int y1) {
			if (dirtyX1 <= dirtyX0 || dirtyY1 <= dirtyY0) {
				dirtyX0 = x0; dirtyY0 = y0; dirtyX1 = x1; dirtyY1 = y1;
				return;
			}
			if (x0 < dirtyX0) dirtyX0 = x0;
			if (y0 < dirtyY0) dirtyY0 = y0;
			if (x1 > dirtyX1) dirtyX1 = x1;
			if (y1 > dirtyY1) dirtyY1 = y1;
		};
		for (int i = 0; i < prevCount; ++i) {
			ClearBox(prevX0[i], prevY0[i], prevX1[i], prevY1[i]);
			MarkDirty(prevX0[i], prevY0[i], prevX1[i], prevY1[i]);
		}

		int newCount = 0;
		if (count > 0 && dispW > 0 && dispH > 0) {
			const float sx = static_cast<float>(cw) / static_cast<float>(dispW);
			const float sy = static_cast<float>(ch) / static_cast<float>(dispH);
			for (int i = 0; i < count; ++i) {
				const BarRect& b = bars[i];
				const int bx = static_cast<int>(std::floor(b.x * sx + 0.5f));
				const int by = static_cast<int>(std::floor(b.y * sy + 0.5f));
				int bw = static_cast<int>(std::floor(b.w * sx + 0.5f));
				int bh = static_cast<int>(std::floor(b.h * sy + 0.5f));
				if (bw < 2) bw = 2;
				if (bh < 1) bh = 1;
				const int fillW = static_cast<int>(std::floor(static_cast<float>(bw) * b.frac + 0.5f));

				const int m = b.outline ? (g_cfg.barOutlinePx > 0 ? g_cfg.barOutlinePx : 1) : 0;
				if (m > 0) {
					FillRect(buf, cw, ch, bx - m, by - m, bx + bw + m - 1, by + bh + m - 1, 0x101010, 200);
				}
				if (g_cfg.bgAlpha > 0) {
					FillRect(buf, cw, ch, bx, by, bx + bw - 1, by + bh - 1, 0x000000, g_cfg.bgAlpha);
				}
				if (fillW > 0) {
					FillRect(buf, cw, ch, bx, by, bx + fillW - 1, by + bh - 1, b.color, 255);
				}

				if (newCount < kMaxBars) {
					newX0[newCount] = bx - m;
					newY0[newCount] = by - m;
					newX1[newCount] = bx + bw + m;
					newY1[newCount] = by + bh + m;
					MarkDirty(newX0[newCount], newY0[newCount], newX1[newCount], newY1[newCount]);
					++newCount;
				}
			}
		}
		for (int i = 0; i < newCount; ++i) {
			prevX0[i] = newX0[i]; prevY0[i] = newY0[i];
			prevX1[i] = newX1[i]; prevY1[i] = newY1[i];
		}
		prevCount = newCount;
		if (count > 0) {
			std::memcpy(prevBars, bars,
			            sizeof(BarRect) * static_cast<std::size_t>(count < kMaxBars ? count : kMaxBars));
		}

		HDC screenDC = GetDC(nullptr);
		POINT ptDst = { origin.x, origin.y };
		POINT ptSrc = { 0, 0 };
		SIZE  size  = { cw, ch };
		BLENDFUNCTION bf = {};
		bf.BlendOp             = AC_SRC_OVER;
		bf.SourceConstantAlpha = 255;
		bf.AlphaFormat         = AC_SRC_ALPHA;

		// 窗口位置/尺寸没变就不调 SetWindowPos（原来每帧都调，纯浪费）
		if (!placed || origin.x != placedX || origin.y != placedY || cw != placedW || ch != placedH) {
			SetWindowPos(g_overlay, HWND_TOPMOST, origin.x, origin.y, cw, ch, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			placedX = origin.x; placedY = origin.y;
			placedW = cw; placedH = ch;
			placed = true;
		}

		// 只把脏区上传给 DWM（原来每次都是整张 14.4 MB 的表面）
		RECT dirty = { dirtyX0, dirtyY0, dirtyX1, dirtyY1 };
		UPDATELAYEREDWINDOWINFO ulw = {};
		ulw.cbSize   = sizeof ulw;
		ulw.hdcDst   = screenDC;
		ulw.pptDst   = &ptDst;
		ulw.psize    = &size;
		ulw.hdcSrc   = memDC;
		ulw.pptSrc   = &ptSrc;
		ulw.pblend   = &bf;
		ulw.dwFlags  = ULW_ALPHA;
		ulw.prcDirty = (firstUpload || dirtyX1 <= dirtyX0 || dirtyY1 <= dirtyY0) ? nullptr : &dirty;
		firstUpload = false;
		if (!UpdateLayeredWindowIndirect(g_overlay, &ulw)) {
			if (!UpdateLayeredWindow(g_overlay, screenDC, &ptDst, &size, memDC, &ptSrc, 0, &bf, ULW_ALPHA)) {
				static int warned = 0;
				if (warned++ < 3) Log("[overlay] UpdateLayeredWindow 失败 err=%lu", GetLastError());
			}
		}
		InterlockedIncrement(&g_drawCount);
		ReleaseDC(nullptr, screenDC);
	}

	if (bmp) DeleteObject(bmp);
	if (memDC) DeleteDC(memDC);
	if (g_overlay) { DestroyWindow(g_overlay); g_overlay = nullptr; }
	return 0;
}

bool OverlayStart(HWND) {
	SetProcessDPIAware();
	g_ovThread = CreateThread(nullptr, 0, OverlayThreadProc, nullptr, 0, nullptr);
	return g_ovThread != nullptr;
}

void OverlayStop() {
	InterlockedExchange(&g_ovStop, 1);
	if (g_ovThread) {
		WaitForSingleObject(g_ovThread, 2000);
		CloseHandle(g_ovThread);
		g_ovThread = nullptr;
	}
}

void OverlaySetGameWindow(HWND hwnd) {
	InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_gameHwnd), reinterpret_cast<PVOID>(hwnd));
}

HWND OverlayGameWindow() {
	return reinterpret_cast<HWND>(static_cast<LONG_PTR>(g_gameHwnd));
}
