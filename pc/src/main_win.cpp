// Cruis'n USA PC - Windows front end (window, OpenGL present, input, pacing)
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#include <GL/gl.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "machine/autosetup.h"
#include "machine/midvunit.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "winmm.lib")

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace {

struct Options
{
	std::string rom = "D:/Mame/roms/crusnusa.zip";
	std::string version = "4.5";
	std::string nvram = "cruisn_usa.nv";
	bool vsync = false;
	bool fullscreen = false;
	bool integer_scale = false;
	int scale = 2;
};

bool g_keys[256];
bool g_quit = false;
bool g_toggle_fs = false;

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (w < 256) g_keys[w] = true;
		if (w == VK_F11 || (w == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) g_toggle_fs = true;
		if (w == VK_ESCAPE) g_quit = true;
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (w < 256) g_keys[w] = false;
		return 0;
	case WM_KILLFOCUS:
		std::fill(std::begin(g_keys), std::end(g_keys), false);
		return 0;
	case WM_CLOSE: g_quit = true; return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcA(h, m, w, l);
}

typedef BOOL(WINAPI *PFNWGLSWAPINTERVALEXT)(int);

void set_fullscreen(HWND hwnd, bool fs, RECT &saved)
{
	static DWORD style = 0;
	if (fs)
	{
		GetWindowRect(hwnd, &saved);
		style = GetWindowLong(hwnd, GWL_STYLE);
		MONITORINFO mi{sizeof(mi)};
		GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
		SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
		SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
		             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
		             SWP_FRAMECHANGED);
	}
	else
	{
		SetWindowLong(hwnd, GWL_STYLE, style);
		SetWindowPos(hwnd, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top,
		             SWP_FRAMECHANGED | SWP_NOZORDER);
	}
}

// ---- input --------------------------------------------------------------------------------

struct InputState
{
	float steer = 0;   // -1..1 keyboard-smoothed
	int gear = 0;
};

inline float dz(float v, float d) { return std::fabs(v) < d ? 0.0f : (v - (v > 0 ? d : -d)) / (1.0f - d); }

void update_inputs(MidVUnit &m, InputState &st, bool &prev_gear_key_state, double dt)
{
	auto K = [](int vk) { return g_keys[vk & 255]; };
	uint16_t in0 = 0xffff, in1 = 0x007f;
	auto hold0 = [&](bool c, uint16_t bit) { if (c) in0 &= ~bit; };
	auto hold1 = [&](bool c, uint16_t bit) { if (c) in1 &= ~bit; };

	hold0(K('5') || K(VK_F3), in0bit::COIN1);
	hold0(K('6'), in0bit::COIN2);
	hold0(K(VK_RETURN) || K(VK_SPACE), in0bit::START);
	hold0(K(VK_F2), in0bit::TEST);
	hold0(K(VK_F1), in0bit::SERVICE);
	hold0(K(VK_OEM_MINUS), in0bit::VOLDN);
	hold0(K(VK_OEM_PLUS), in0bit::VOLUP);

	hold1(K('Z'), in1bit::VIEW1);
	hold1(K('X'), in1bit::VIEW2);
	hold1(K('C'), in1bit::VIEW3);
	hold1(K('R'), in1bit::RADIO);

	// analog: keyboard
	float target = 0;
	if (K(VK_LEFT) || K('A')) target -= 1;
	if (K(VK_RIGHT) || K('D')) target += 1;
	float rate = (target == 0) ? 4.0f : 3.0f;   // return-to-centre a bit faster
	float diff = target - st.steer;
	float step = rate * float(dt);
	st.steer += (std::fabs(diff) <= step) ? diff : (diff > 0 ? step : -step);
	float accel = (K(VK_UP) || K('W')) ? 1.0f : 0.0f;
	float brake = (K(VK_DOWN) || K('S')) ? 1.0f : 0.0f;
	float steer = st.steer;

	// gears: 1-4 select, G = neutral
	static bool prev[6];
	bool cur[6] = {false, K('1'), K('2'), K('3'), K('4'), K('G')};
	for (int g = 1; g <= 4; g++)
		if (cur[g] && !prev[g]) st.gear = g;
	if (cur[5] && !prev[5]) st.gear = 0;
	if (K(VK_PRIOR) && !prev_gear_key_state) st.gear = std::min(4, st.gear + 1);
	if (K(VK_NEXT) && !prev_gear_key_state) st.gear = std::max(0, st.gear - 1);
	prev_gear_key_state = K(VK_PRIOR) || K(VK_NEXT);
	for (int i = 0; i < 6; i++) prev[i] = cur[i];

	// XInput
	XINPUT_STATE xs{};
	if (XInputGetState(0, &xs) == ERROR_SUCCESS)
	{
		const XINPUT_GAMEPAD &p = xs.Gamepad;
		float lx = dz(p.sThumbLX / 32768.0f, 0.12f);
		if (lx != 0) steer = lx;
		accel = std::max(accel, p.bRightTrigger / 255.0f);
		brake = std::max(brake, p.bLeftTrigger / 255.0f);
		if (p.wButtons & XINPUT_GAMEPAD_A) accel = 1.0f;
		if (p.wButtons & XINPUT_GAMEPAD_B) brake = 1.0f;
		hold0(p.wButtons & XINPUT_GAMEPAD_START, in0bit::START);
		hold0(p.wButtons & XINPUT_GAMEPAD_BACK, in0bit::COIN1);
		hold1(p.wButtons & XINPUT_GAMEPAD_X, in1bit::VIEW1);
		hold1(p.wButtons & XINPUT_GAMEPAD_Y, in1bit::VIEW2);
		hold1(p.wButtons & XINPUT_GAMEPAD_LEFT_THUMB, in1bit::VIEW3);
		hold1(p.wButtons & XINPUT_GAMEPAD_DPAD_UP, in1bit::RADIO);
		static WORD prevb = 0;
		if ((p.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) && !(prevb & XINPUT_GAMEPAD_RIGHT_SHOULDER)) st.gear = std::min(4, st.gear + 1);
		if ((p.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) && !(prevb & XINPUT_GAMEPAD_LEFT_SHOULDER)) st.gear = std::max(0, st.gear - 1);
		prevb = p.wButtons;
	}

	m.inputs.in0 = in0;
	m.inputs.in1 = in1;
	m.inputs.gear = st.gear;
	m.inputs.wheel = uint8_t(std::lround(0x80 + steer * 0x60));   // 0x20..0xe0 (calibrated range 0x10..0xf0)
	m.inputs.accel = uint8_t(std::lround(accel * 255));
	m.inputs.brake = uint8_t(std::lround(brake * 255));
}

} // namespace

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmdline, int)
{
	Options opt;
	{
		std::string a = cmdline;
		auto next = [&](const char *key, std::string &out) {
			size_t p = a.find(key);
			if (p == std::string::npos) return;
			p += std::strlen(key);
			while (p < a.size() && a[p] == ' ') p++;
			size_t e = a.find(' ', p);
			out = a.substr(p, e == std::string::npos ? e : e - p);
		};
		std::string v;
		next("--rom", opt.rom);
		next("--version", opt.version);
		next("--nvram", opt.nvram);
		v.clear(); next("--vsync", v); if (!v.empty()) opt.vsync = v != "0";
		v.clear(); next("--scale", v); if (!v.empty()) opt.scale = std::max(1, std::atoi(v.c_str()));
		if (a.find("--fullscreen") != std::string::npos) opt.fullscreen = true;
	}

	MidVUnit m;
	std::string err;
	if (!m.load_roms(opt.rom, opt.version, err))
	{
		MessageBoxA(nullptr, ("ROM load failed:\n" + err + "\n\nUse --rom <path\\crusnusa.zip>").c_str(), "Cruis'n USA", MB_ICONERROR);
		return 1;
	}
	m.reset();
	if (!m.load_nvram(opt.nvram))
	{
		run_auto_setup(m);          // fresh NVRAM: calibrate + leave diagnostics automatically
		m.save_nvram(opt.nvram);
	}

	timeBeginPeriod(1);
	WNDCLASSA wc{};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = hi;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "CruisnPC";
	RegisterClassA(&wc);

	RECT r{0, 0, 512 * opt.scale, 400 * opt.scale};
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowA("CruisnPC", "Cruis'n USA (PC)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
	                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);

	HDC dc = GetDC(hwnd);
	PIXELFORMATDESCRIPTOR pfd{};
	pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
	SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
	HGLRC rc = wglCreateContext(dc);
	wglMakeCurrent(dc, rc);
	auto swap_interval = (PFNWGLSWAPINTERVALEXT)wglGetProcAddress("wglSwapIntervalEXT");
	if (swap_interval) swap_interval(opt.vsync ? 1 : 0);

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	std::vector<uint32_t> bgra(512 * 512);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, bgra.data());

	RECT saved{};
	bool fs = false;
	if (opt.fullscreen) { set_fullscreen(hwnd, true, saved); fs = true; }

	InputState st;
	bool gearprev = false;
	LARGE_INTEGER qf, t0;
	QueryPerformanceFrequency(&qf);
	QueryPerformanceCounter(&t0);
	double next_frame = 0, last_time = 0;
	double fps_t = 0; int fps_n = 0;

	MSG msg;
	while (!g_quit)
	{
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		if (g_toggle_fs) { g_toggle_fs = false; fs = !fs; set_fullscreen(hwnd, fs, saved); }

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		double t = double(now.QuadPart - t0.QuadPart) / double(qf.QuadPart);

		if (!opt.vsync)
		{
			if (t < next_frame)
			{
				double wait = next_frame - t;
				if (wait > 0.002) Sleep(1);
				continue;
			}
			next_frame += 1.0 / m.refresh_hz();
			if (t - next_frame > 0.25) next_frame = t;   // don't spiral after stalls
		}

		update_inputs(m, st, gearprev, std::min(t - last_time, 0.1));
		last_time = t;
		m.run_frame();

		// upload the visible part of the frame
		const uint32_t *src = m.frame_rgba();
		int w = m.screen_w(), h = m.screen_h();
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				uint32_t c = src[y * MidVUnit::FRAME_STRIDE + x];
				bgra[y * 512 + x] = ((c >> 16) & 0xff) | (c & 0xff00) | ((c & 0xff) << 16) | 0xff000000u;
			}
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, h, GL_RGBA, GL_UNSIGNED_BYTE, bgra.data());

		RECT cr;
		GetClientRect(hwnd, &cr);
		int cw = cr.right, ch = cr.bottom;
		glViewport(0, 0, cw, ch);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		// keep 512:400 aspect (letterbox)
		float aspect = float(w) / float(h), win_aspect = float(cw) / float(std::max(ch, 1));
		float sx = 1, sy = 1;
		if (win_aspect > aspect) sx = aspect / win_aspect; else sy = win_aspect / aspect;
		glEnable(GL_TEXTURE_2D);
		glBegin(GL_QUADS);
		float u1 = float(w) / 512.0f, v1 = float(h) / 512.0f;
		glTexCoord2f(0, 0);   glVertex2f(-sx, sy);
		glTexCoord2f(u1, 0);  glVertex2f(sx, sy);
		glTexCoord2f(u1, v1); glVertex2f(sx, -sy);
		glTexCoord2f(0, v1);  glVertex2f(-sx, -sy);
		glEnd();
		SwapBuffers(dc);

		fps_n++;
		if (t - fps_t >= 1.0)
		{
			char title[128];
			std::snprintf(title, sizeof(title), "Cruis'n USA (PC) - %d fps  gear %d  [%s]", fps_n, st.gear, opt.vsync ? "vsync" : "paced");
			SetWindowTextA(hwnd, title);
			fps_n = 0; fps_t = t;
		}
	}

	m.save_nvram(opt.nvram);
	wglMakeCurrent(nullptr, nullptr);
	wglDeleteContext(rc);
	timeEndPeriod(1);
	return 0;
}
