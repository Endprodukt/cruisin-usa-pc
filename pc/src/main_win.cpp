// Cruis'n USA PC - Windows front end (window, video backend, input, pacing)
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "machine/midvunit.h"
#include "platform/audio_wasapi.h"
#include "video/video_backend.h"
#include "../third_party/miniz/miniz.h"

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "winmm.lib")

namespace {

struct Options
{
	std::string rom = "D:/Mame/roms/crusnusa.zip";
	std::string version = "4.5";
	std::string nvram = "cruisn_usa.nv";
	std::string backend = "gl";     // gl | vk
	bool fastboot = true;
	bool fullscreen = false;
	std::string shot;           // --shot file.png : save the internal-resolution image and exit
	int shot_frames = 600;      // frames after boot before the screenshot
	VideoOptions video;
};

bool g_keys[256];
bool g_pressed[256];           // edge-triggered key presses
bool g_quit = false;
bool g_toggle_fs = false;
int  g_size_w = 0, g_size_h = 0;

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (w < 256) { if (!g_keys[w]) g_pressed[w] = true; g_keys[w] = true; }
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
	case WM_SIZE:
		g_size_w = LOWORD(l); g_size_h = HIWORD(l);
		return 0;
	case WM_ERASEBKGND: return 1;
	case WM_CLOSE: g_quit = true; return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcA(h, m, w, l);
}

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
		SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
		             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
	}
	else
	{
		SetWindowLong(hwnd, GWL_STYLE, style);
		SetWindowPos(hwnd, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top,
		             SWP_FRAMECHANGED | SWP_NOZORDER);
	}
}

// ---- input -------------------------------------------------------------------------------

struct InputState
{
	float steer = 0;   // -1..1
	int gear = 0;
};

inline float dz(float v, float d) { return std::fabs(v) < d ? 0.0f : (v - (v > 0 ? d : -d)) / (1.0f - d); }

// calibration range embedded in the default CMOS: wheel 0x10..0xf0 (centre 0x80), pedals 0..0xff
constexpr int WHEEL_CENTER = 0x80, WHEEL_SPAN = 0x70;

void update_inputs(MidVUnit &m, InputState &st, double dt)
{
	auto K = [](int vk) { return g_keys[vk & 255]; };
	auto P = [](int vk) { return g_pressed[vk & 255]; };
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

	float target = 0;
	if (K(VK_LEFT) || K('A')) target -= 1;
	if (K(VK_RIGHT) || K('D')) target += 1;
	float rate = (target == 0) ? 5.0f : 3.5f;
	float diff = target - st.steer;
	float step = rate * float(dt);
	st.steer += (std::fabs(diff) <= step) ? diff : (diff > 0 ? step : -step);
	float steer = st.steer;
	float accel = (K(VK_UP) || K('W')) ? 1.0f : 0.0f;
	float brake = (K(VK_DOWN) || K('S')) ? 1.0f : 0.0f;

	for (int g = 1; g <= 4; g++)
		if (P('0' + g)) st.gear = g;
	if (P('G')) st.gear = 0;
	if (P(VK_PRIOR)) st.gear = std::min(4, st.gear + 1);
	if (P(VK_NEXT)) st.gear = std::max(0, st.gear - 1);

	XINPUT_STATE xs{};
	static WORD prevb = 0;
	if (XInputGetState(0, &xs) == ERROR_SUCCESS)
	{
		const XINPUT_GAMEPAD &p = xs.Gamepad;
		float lx = dz(p.sThumbLX / 32768.0f, 0.10f);
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
		WORD nb = p.wButtons & ~prevb;
		if (nb & XINPUT_GAMEPAD_RIGHT_SHOULDER) st.gear = std::min(4, st.gear + 1);
		if (nb & XINPUT_GAMEPAD_LEFT_SHOULDER) st.gear = std::max(0, st.gear - 1);
		prevb = p.wButtons;
	}

	m.inputs.in0 = in0;
	m.inputs.in1 = in1;
	m.inputs.gear = st.gear;
	m.inputs.wheel = uint8_t(std::clamp(int(std::lround(WHEEL_CENTER + std::clamp(steer, -1.0f, 1.0f) * WHEEL_SPAN)), 0x10, 0xf0));
	m.inputs.accel = uint8_t(std::lround(std::clamp(accel, 0.0f, 1.0f) * 255));
	m.inputs.brake = uint8_t(std::lround(std::clamp(brake, 0.0f, 1.0f) * 255));
}

std::string arg_value(const std::string &a, const char *key)
{
	size_t p = a.find(key);
	if (p == std::string::npos) return {};
	p += std::strlen(key);
	while (p < a.size() && a[p] == ' ') p++;
	size_t e = a.find(' ', p);
	return a.substr(p, e == std::string::npos ? e : e - p);
}

} // namespace

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmdline, int)
{
	Options opt;
	{
		std::string a = cmdline;
		std::string v;
		if (!(v = arg_value(a, "--rom")).empty()) opt.rom = v;
		if (!(v = arg_value(a, "--version")).empty()) opt.version = v;
		if (!(v = arg_value(a, "--nvram")).empty()) opt.nvram = v;
		if (!(v = arg_value(a, "--backend")).empty()) opt.backend = v;
		if (!(v = arg_value(a, "--vsync")).empty()) opt.video.vsync = v != "0";
		if (!(v = arg_value(a, "--scale")).empty()) opt.video.scale = std::clamp(std::atoi(v.c_str()), 1, 8);
		if (!(v = arg_value(a, "--filter")).empty()) opt.video.filter_textures = v != "0";
		if (!(v = arg_value(a, "--smooth")).empty()) opt.video.smooth_output = v != "0";
		if (!(v = arg_value(a, "--shot")).empty()) opt.shot = v;
		if (!(v = arg_value(a, "--shot-frames")).empty()) opt.shot_frames = std::atoi(v.c_str());
		if (a.find("--fullscreen") != std::string::npos) opt.fullscreen = true;
		if (a.find("--no-fastboot") != std::string::npos) opt.fastboot = false;
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
		m.load_default_nvram();        // embedded, pre-calibrated CMOS: no calibration screen, ever

	timeBeginPeriod(1);
	WNDCLASSA wc{};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = hi;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "CruisnPC";
	RegisterClassA(&wc);

	RECT r{0, 0, 640, 480};
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowA("CruisnPC", "Cruis'n USA (PC)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
	                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);

	// ---- video backend
	std::unique_ptr<IVideoBackend> video;
	{
		auto be = (opt.backend == "vk" || opt.backend == "vulkan") ? create_vk_backend() : create_gl_backend();
		std::string e;
		if (!be || !be->init(hwnd, opt.video, e))
		{
			MessageBoxA(hwnd, ("Video backend failed:\n" + e).c_str(), "Cruis'n USA", MB_ICONERROR);
			return 1;
		}
		video = std::move(be);
	}
	{
		RECT cr; GetClientRect(hwnd, &cr);
		video->resize(cr.right, cr.bottom);
	}
	m.attach_video_backend(video.get());

	AudioOut audio;
	bool audio_ok = audio.start();
	auto hook_audio = [&](bool on) {
		if (on && audio_ok)
		{
			m.on_audio = [&audio](const int16_t *b, int n, double rate) { audio.push(b, n, rate); };
			m.on_audio_enable = [&audio](bool e) { if (!e) audio.clear(); };
		}
		else
		{
			m.on_audio = nullptr;
			m.on_audio_enable = nullptr;
		}
	};

	RECT saved{};
	bool fs = false;
	if (opt.fullscreen) { set_fullscreen(hwnd, true, saved); fs = true; }

	InputState st;
	LARGE_INTEGER qf;
	QueryPerformanceFrequency(&qf);
	auto now_sec = [&]() {
		LARGE_INTEGER n; QueryPerformanceCounter(&n);
		return double(n.QuadPart) / double(qf.QuadPart);
	};

	MSG msg;
	// ---- fast boot: run the boot/self tests unthrottled until the attract mode starts rendering
	if (opt.fastboot)
	{
		hook_audio(false);
		for (int f = 0; f < 6000 && !g_quit; f++)
		{
			m.inputs = MachineInputs{};
			m.run_frame();
			if (f > 400 && m.quads_last_frame > 50)
				break;
			if (f % 45 == 0)
			{
				m.present_gpu();
				while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
				char t[96]; std::snprintf(t, sizeof(t), "Cruis'n USA (PC) - booting... %d%%", std::min(99, f * 100 / 1400));
				SetWindowTextA(hwnd, t);
			}
		}
		audio.clear();
	}
	hook_audio(true);
	std::fill(std::begin(g_pressed), std::end(g_pressed), false);

	double t_start = now_sec();
	double next_frame = 0, last_time = 0;
	double fps_t = 0; int fps_n = 0; int shot_count = 0;
	while (!g_quit)
	{
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
		if (g_toggle_fs) { g_toggle_fs = false; fs = !fs; set_fullscreen(hwnd, fs, saved); }
		if (g_size_w > 0) { video->resize(g_size_w, g_size_h); g_size_w = 0; }

		double t = now_sec() - t_start;

		// hotkeys: F5/F6 internal resolution, F7 texture filter, F8 vsync, F9 output smoothing
		bool vchanged = false;
		if (g_pressed[VK_F5]) { opt.video.scale = std::max(1, opt.video.scale - 1); vchanged = true; }
		if (g_pressed[VK_F6]) { opt.video.scale = std::min(8, opt.video.scale + 1); vchanged = true; }
		if (g_pressed[VK_F7]) { opt.video.filter_textures = !opt.video.filter_textures; vchanged = true; }
		if (g_pressed[VK_F8]) { opt.video.vsync = !opt.video.vsync; vchanged = true; }
		if (g_pressed[VK_F9]) { opt.video.smooth_output = !opt.video.smooth_output; vchanged = true; }
		if (vchanged) video->set_options(opt.video);

		// emulation runs on its own clock (the machine's 57.9 Hz), independent of the display refresh
		bool ran = false;
		for (int guard = 0; t >= next_frame && guard < 3; guard++)
		{
			update_inputs(m, st, std::min(t - last_time, 0.1));
			std::fill(std::begin(g_pressed), std::end(g_pressed), false);
			last_time = t;
			m.run_frame();
			next_frame += 1.0 / m.refresh_hz();
			ran = true;
		}
		if (t - next_frame > 0.25) next_frame = t;

		if (!ran && !opt.video.vsync)
		{
			Sleep(1);
			continue;
		}
		m.present_gpu();

		if (ran && !opt.shot.empty() && ++shot_count >= opt.shot_frames)
		{
			std::vector<uint32_t> px; int pw = 0, ph = 0;
			if (video->read_page(m.display_page(), px, pw, ph))
			{
				int S = opt.video.scale, w = m.screen_w() * S, h = m.screen_h() * S;
				std::vector<uint8_t> rgb(size_t(w) * h * 3);
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++)
					{
						uint32_t c = px[size_t(y) * pw + x];
						uint8_t *d = &rgb[(size_t(y) * w + x) * 3];
						d[0] = uint8_t(c); d[1] = uint8_t(c >> 8); d[2] = uint8_t(c >> 16);
					}
				size_t len = 0;
				void *png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &len);
				if (FILE *fp = std::fopen(opt.shot.c_str(), "wb")) { std::fwrite(png, 1, len, fp); std::fclose(fp); }
				mz_free(png);
			}
			g_quit = true;
		}

		if (ran) fps_n++;
		if (t - fps_t >= 1.0)
		{
			char title[192];
			std::snprintf(title, sizeof(title), "Cruis'n USA (PC) - %d fps  %s %dx  gear %d  %s%s  snd %.0fms", fps_n, video->name(),
			              opt.video.scale, st.gear, opt.video.vsync ? "vsync " : "", opt.video.filter_textures ? "filtered" : "",
			              audio.latency_ms());
			SetWindowTextA(hwnd, title);
			fps_n = 0; fps_t = t;
		}
	}

	audio.stop();
	m.save_nvram(opt.nvram);
	video->shutdown();
	timeEndPeriod(1);
	return 0;
}
