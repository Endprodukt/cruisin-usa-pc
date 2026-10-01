// Cruis'n USA PC - Windows front end: settings + launcher, window, video backend, input, pacing
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../third_party/miniz/miniz.h"
#include "config/settings.h"
#include "input/controls.h"
#include "input/devices.h"
#include "launcher/launcher.h"
#include "perf.h"
#include "machine/midvunit.h"
#include "machine/telemetry.h"
#include "outputs/outputs.h"
#include "platform/audio_wasapi.h"
#include "video/video_backend.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

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
	case WM_SYSCOMMAND:
		if ((w & 0xfff0) == SC_KEYMENU) return 0;      // no menu activation on Alt
		break;
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

// ---- monitors / window modes -----------------------------------------------------------------------------

struct Mon { RECT rc; };
BOOL CALLBACK mon_cb(HMONITOR m, HDC, LPRECT, LPARAM d)
{
	MONITORINFO mi{sizeof(mi)};
	GetMonitorInfo(m, &mi);
	auto *v = reinterpret_cast<std::vector<Mon> *>(d);
	if (mi.dwFlags & MONITORINFOF_PRIMARY) v->insert(v->begin(), Mon{mi.rcMonitor}); else v->push_back(Mon{mi.rcMonitor});
	return TRUE;
}

RECT monitor_rect(int index)
{
	std::vector<Mon> mons;
	EnumDisplayMonitors(nullptr, nullptr, mon_cb, reinterpret_cast<LPARAM>(&mons));
	if (mons.empty()) return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
	return mons[size_t(std::clamp(index, 0, int(mons.size()) - 1))].rc;
}

// applies the window mode to an existing window
void apply_window_mode(HWND hwnd, const VideoSettings &v, WindowMode mode)
{
	RECT mon = monitor_rect(v.monitor);
	int mw = mon.right - mon.left, mh = mon.bottom - mon.top;
	if (mode == WindowMode::Fullscreen)
	{
		SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowPos(hwnd, HWND_TOP, mon.left, mon.top, mw, mh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		return;
	}
	DWORD style = mode == WindowMode::Borderless ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
	RECT r{0, 0, std::min<int>(v.window_w, mw), std::min<int>(v.window_h, mh)};
	AdjustWindowRect(&r, style, FALSE);
	int w = r.right - r.left, h = r.bottom - r.top;
	SetWindowLong(hwnd, GWL_STYLE, style);
	SetWindowPos(hwnd, HWND_TOP, mon.left + (mw - w) / 2, mon.top + (mh - h) / 2, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

// ---- CPU presenter: the reference rasterizer's frame through GDI ------------------------------------------------

struct CpuPresenter
{
	HWND hwnd = nullptr;
	VideoOptions opt;
	std::vector<uint32_t> buf;

	void present(const MidVUnit &m, int win_w, int win_h)
	{
		int w = m.screen_w(), h = m.screen_h();
		buf.resize(size_t(w) * h);
		for (int y = 0; y < h; y++)
			std::memcpy(&buf[size_t(y) * w], m.frame_rgba() + size_t(y) * MidVUnit::FRAME_STRIDE, size_t(w) * 4);
		HDC dc = GetDC(hwnd);
		float dw = float(win_w), dh = float(win_h), tw = dw, th = dh;
		if (opt.keep_aspect)
		{
			if (dw / dh > opt.aspect) { th = dh; tw = dh * opt.aspect; } else { tw = dw; th = dw / opt.aspect; }
			if (opt.integer_scale)
			{
				float k = std::max(1.0f, std::floor(th / float(h)));
				th = k * h; tw = th * opt.aspect;
				if (tw > dw) { tw = dw; th = tw / opt.aspect; }
			}
		}
		int x0 = int((dw - tw) / 2), y0 = int((dh - th) / 2);
		PatBlt(dc, 0, 0, win_w, win_h, BLACKNESS);
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = w;
		bi.bmiHeader.biHeight = -h;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		SetStretchBltMode(dc, opt.smooth_output ? HALFTONE : COLORONCOLOR);
		StretchDIBits(dc, x0, y0, int(tw), int(th), 0, 0, w, h, buf.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
		ReleaseDC(hwnd, dc);
	}
};

VideoOptions make_video_options(const VideoSettings &v)
{
	VideoOptions o;
	o.scale = v.internal_scale;
	o.vsync = v.vsync;
	o.filter_textures = v.texture_filter;
	o.smooth_output = v.smooth_output;
	o.aa = v.aa;
	o.wide_margin = wide_margin_for(v);
	o.integer_scale = v.integer_scale;
	o.keep_aspect = v.aspect != AspectMode::Stretch;
	o.shadow_strength = float(v.shadow_strength) / 100.0f;
	o.shadow_soft = float(v.shadow_softness) / 10.0f;
	o.aspect = v.aspect == AspectMode::Wide169 ? 16.0f / 9.0f : v.aspect == AspectMode::Wide219 ? 21.0f / 9.0f : 4.0f / 3.0f;
	return o;
}

std::string arg_value(const std::string &a, const char *key)
{
	size_t p = a.find(key);
	if (p == std::string::npos) return {};
	p += std::strlen(key);
	while (p < a.size() && a[p] == ' ') p++;
	if (p < a.size() && a[p] == '"')
	{
		size_t e = a.find('"', p + 1);
		return a.substr(p + 1, e == std::string::npos ? e : e - p - 1);
	}
	size_t e = a.find(' ', p);
	return a.substr(p, e == std::string::npos ? e : e - p);
}

std::string exe_dir()
{
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string p = path;
	return p.substr(0, p.find_last_of("\\/") + 1);
}

} // namespace

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmdline, int)
{
	std::string a = cmdline;
	std::string dir = exe_dir();
	std::string ini = dir + "cruisn.ini";
	if (std::string v = arg_value(a, "--ini"); !v.empty()) ini = v;

	Settings S;
	S.load(ini);
	if (S.nvram.empty()) S.nvram = "cruisn_usa.nv";
	S.nvram = exe_relative(S.nvram);

	// command line overrides (testing and scripting)
	std::string shot = arg_value(a, "--shot");
	int shot_frames = 600, shot_seq = 1;
	if (std::string v = arg_value(a, "--shot-frames"); !v.empty()) shot_frames = std::atoi(v.c_str());
	if (std::string v = arg_value(a, "--shot-seq"); !v.empty()) shot_seq = std::max(1, std::atoi(v.c_str()));
	bool autoplay = a.find("--autoplay") != std::string::npos;
	PerfStats perf;
	const bool bench = a.find("--bench") != std::string::npos;
	if (a.find("--perf") != std::string::npos) { perf.on = true; perf.log_path = exe_relative("perf.log"); }
	if (std::string v = arg_value(a, "--rom"); !v.empty()) S.rom = v;
	if (std::string v = arg_value(a, "--version"); !v.empty()) S.version = v;
	if (std::string v = arg_value(a, "--nvram"); !v.empty()) S.nvram = v;
	if (std::string v = arg_value(a, "--backend"); !v.empty())
		S.video.renderer = (v == "vk" || v == "vulkan") ? Renderer::Vulkan : (v == "cpu" ? Renderer::Cpu : Renderer::OpenGL);
	if (std::string v = arg_value(a, "--shadows"); !v.empty())
		S.video.shadows = v == "off" ? ShadowMode::Off : v == "original" ? ShadowMode::Original : ShadowMode::Modern;
	if (std::string v = arg_value(a, "--draw-distance"); !v.empty()) S.video.draw_distance = std::clamp(std::atoi(v.c_str()), 10, 400);
	if (std::string v = arg_value(a, "--aa"); !v.empty()) S.video.aa = std::clamp(std::atoi(v.c_str()), 0, 3);
	if (std::string v = arg_value(a, "--aspect"); !v.empty()) S.video.aspect = v == "16:9" ? AspectMode::Wide169 : v == "21:9" ? AspectMode::Wide219 : AspectMode::Native43;
	if (std::string v = arg_value(a, "--hud"); !v.empty()) S.video.hud = v == "edges" ? HudPlacement::Edges : v == "25" ? HudPlacement::Quarter25 : v == "50" ? HudPlacement::Half50 : v == "75" ? HudPlacement::Quarter75 : HudPlacement::Centre;
	if (a.find("--export-textures") != std::string::npos) S.video.export_textures = true;
	if (std::string v = arg_value(a, "--replace-textures"); !v.empty()) S.video.replace_textures = v != "0";
	if (std::string v = arg_value(a, "--scale"); !v.empty()) S.video.internal_scale = std::clamp(std::atoi(v.c_str()), 1, 8);
	if (std::string v = arg_value(a, "--vsync"); !v.empty()) S.video.vsync = v != "0";
	if (std::string v = arg_value(a, "--filter"); !v.empty()) S.video.texture_filter = v != "0";
	if (std::string v = arg_value(a, "--smooth"); !v.empty()) S.video.smooth_output = v != "0";
	if (a.find("--fullscreen") != std::string::npos) S.video.window_mode = WindowMode::Fullscreen;
	if (a.find("--no-fastboot") != std::string::npos) S.fast_boot = false;

	// ---- launcher (skippable): setting, --play, Shift held at start, or --launcher
	bool want_launcher = S.show_launcher;
	if (a.find("--play") != std::string::npos || !shot.empty() || autoplay) want_launcher = false;
	if (a.find("--launcher") != std::string::npos || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) want_launcher = true;

	if (a.find("--save-ini") != std::string::npos)      // write the effective configuration and quit (also creates a default file)
		return S.save(ini) ? 0 : 1;

	InputHub hub;
	Controls controls(S, hub);
	if (a.find("--list-devices") != std::string::npos)
	{
		hub.init(nullptr, S.controls.ignore_devices, S.controls.allow_duplicate_devices);
		if (FILE *f = std::fopen("devices.txt", "w"))
		{
			for (int i = 0; i < hub.count(); i++)
			{
				const DeviceInfo &d = hub.info(i);
				std::fprintf(f, "%s | %s | axes:", d.name.c_str(), d.backend == Backend::XInput ? "XInput" : "DirectInput");
				for (auto &ax : d.axes) std::fprintf(f, " %s", ax.c_str());
				std::fprintf(f, " | buttons %d | ffb %d | merged %d\n", d.buttons, d.ffb ? 1 : 0, d.duplicates_merged);
			}
			std::fclose(f);
		}
		return 0;
	}
	if (want_launcher)
	{
		if (launcher_run(S, ini, hub, controls) == LauncherResult::Quit) return 0;
	}

	MidVUnit m;
	std::string err;
	if (!m.load_roms(S.rom, S.version, err))
	{
		MessageBoxA(nullptr, ("ROM load failed:\n" + err + "\n\nSet the ROM path in the launcher (Home > ROM zip).").c_str(), "Cruis'n USA", MB_ICONERROR);
		return 1;
	}
	m.rom_patches.draw_distance_pct = S.video.draw_distance;
	m.rom_patches.rubberband_pct = S.rubberband;
	m.rom_patches.wide_margin = S.video.renderer == Renderer::Cpu ? 0 : wide_margin_for(S.video);
	m.set_wide_margin(m.rom_patches.wide_margin);
	m.set_hud_spread(S.video.hud == HudPlacement::Edges ? 1.0f : S.video.hud == HudPlacement::Quarter25 ? 0.25f : S.video.hud == HudPlacement::Half50 ? 0.5f : S.video.hud == HudPlacement::Quarter75 ? 0.75f : 0.0f);
	m.reset();
	if (!m.load_nvram(S.nvram))
		m.load_default_nvram();        // embedded, pre-calibrated CMOS: no calibration screen, ever
	m.inputs.dsw = S.dsw;

	timeBeginPeriod(1);
	WNDCLASSA wc{};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = hi;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "CruisnPC";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("CruisnPC", "Cruis'n USA (PC)", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, hi, nullptr);
	apply_window_mode(hwnd, S.video, S.video.window_mode);
	ShowWindow(hwnd, SW_SHOW);

	// ---- video backend
	std::unique_ptr<IVideoBackend> video;
	CpuPresenter cpu;
	VideoOptions vopt = make_video_options(S.video);
	std::string backend_note;
	if (S.video.renderer != Renderer::Cpu)
	{
		std::string e;
		if (S.video.renderer == Renderer::Vulkan)
		{
			auto be = create_vk_backend();
			if (be && be->init(hwnd, vopt, e)) video = std::move(be);
			else backend_note = "Vulkan unavailable (" + e + ") - using OpenGL";
		}
		if (!video)
		{
			// a window that failed Vulkan setup cannot take an OpenGL pixel format reliably: start over with a fresh one
			if (S.video.renderer == Renderer::Vulkan)
			{
				DestroyWindow(hwnd);
				hwnd = CreateWindowA("CruisnPC", "Cruis'n USA (PC)", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, hi, nullptr);
				apply_window_mode(hwnd, S.video, S.video.window_mode);
			}
			auto be = create_gl_backend();
			if (!be || !be->init(hwnd, vopt, e))
			{
				MessageBoxA(hwnd, ("Video backend failed:\n" + e).c_str(), "Cruis'n USA", MB_ICONERROR);
				return 1;
			}
			video = std::move(be);
		}
		RECT cr; GetClientRect(hwnd, &cr);
		video->resize(cr.right, cr.bottom);
		{
			TexRepl::Config tc;
			tc.dump = S.video.export_textures;
			tc.variants = S.video.export_variants;
			tc.replace = S.video.replace_textures;
			tc.dump_dir = exe_relative("textures/dump");
			tc.repl_dir = exe_relative("textures/replace");
			m.texrepl.configure(tc);
			std::string tlog;
			m.texrepl.load(tlog);
			if (!tlog.empty()) OutputDebugStringA(("textures: " + tlog).c_str());
		}
		m.attach_video_backend(video.get());
		m.set_shadow_mode(int(S.video.shadows));
	}
	else
	{
		cpu.hwnd = hwnd;
		cpu.opt = vopt;
	}

	// ---- input, force feedback, outputs, audio
	hub.shutdown();
	hub.init(hwnd, S.controls.ignore_devices, S.controls.allow_duplicate_devices);
	controls.ffb_start(hwnd);
	Outputs outputs;
	if (S.outputs.mode != OutputMode::Off) outputs.start(S.outputs);

	AudioOut audio;
	bool audio_ok = S.audio.enabled && audio.start();
	audio.set_volume(float(S.audio.volume) / 100.0f);
	audio.set_latency_ms(S.audio.latency_ms);
	auto hook_audio = [&](bool on) {
		if (on && audio_ok)
		{
			m.on_audio = [&audio](const int16_t *b, int n, double rate) { audio.push(b, n, rate); };
			m.on_audio_enable = [&audio](bool e) { if (!e) audio.clear(); };
		}
		else { m.on_audio = nullptr; m.on_audio_enable = nullptr; }
	};

	LARGE_INTEGER qf;
	QueryPerformanceFrequency(&qf);
	auto now_sec = [&]() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return double(n.QuadPart) / double(qf.QuadPart); };
	auto present = [&]() {
		RECT cr; GetClientRect(hwnd, &cr);
		if (video) m.present_gpu(); else cpu.present(m, cr.right, cr.bottom);
	};

	MSG msg;
	// ---- fast boot: run the boot/self tests unthrottled until the attract mode starts rendering
	if (S.fast_boot)
	{
		hook_audio(false);
		for (int f = 0; f < 6000 && !g_quit; f++)
		{
			m.inputs = MachineInputs{};
			m.inputs.dsw = S.dsw;
			m.run_frame();
			if (f > 400 && m.quads_last_frame > 50) break;
			if (f % 45 == 0)
			{
				present();
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
	double next_frame = 0;
	double fps_t = 0; int fps_n = 0; int shot_count = 0;
	bool fullscreen_now = S.video.window_mode == WindowMode::Fullscreen;
	while (!g_quit)
	{
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
		if (g_toggle_fs)
		{
			g_toggle_fs = false;
			fullscreen_now = !fullscreen_now;
			WindowMode back = S.video.window_mode == WindowMode::Fullscreen ? WindowMode::Window : S.video.window_mode;
			apply_window_mode(hwnd, S.video, fullscreen_now ? WindowMode::Fullscreen : back);
		}
		if (g_size_w > 0) { if (video) video->resize(g_size_w, g_size_h); g_size_w = 0; }

		double t = now_sec() - t_start;

		// hotkeys: F5/F6 internal resolution, F7 texture filter, F8 vsync, F9 output smoothing
		bool vchanged = false;
		if (g_pressed[VK_F11] && video) { S.video.export_textures = !S.video.export_textures; TexRepl::Config tc; tc.dump = S.video.export_textures; tc.variants = S.video.export_variants; tc.replace = S.video.replace_textures; tc.dump_dir = exe_relative("textures/dump"); tc.repl_dir = exe_relative("textures/replace"); m.texrepl.configure(tc); }
		if (g_pressed[VK_F3]) { S.video.aa = (S.video.aa + 1) % 4; vchanged = true; }
		if (g_pressed[VK_F4]) { S.video.shadows = ShadowMode((int(S.video.shadows) + 1) % 3); m.set_shadow_mode(int(S.video.shadows)); vchanged = true; }
		if (g_pressed[VK_F5]) { S.video.internal_scale = std::max(1, S.video.internal_scale - 1); vchanged = true; }
		if (g_pressed[VK_F6]) { S.video.internal_scale = std::min(8, S.video.internal_scale + 1); vchanged = true; }
		if (g_pressed[VK_F7]) { S.video.texture_filter = !S.video.texture_filter; vchanged = true; }
		if (g_pressed[VK_F8]) { S.video.vsync = !S.video.vsync; vchanged = true; }
		if (g_pressed[VK_F9]) { S.video.smooth_output = !S.video.smooth_output; vchanged = true; }
		if (vchanged)
		{
			vopt = make_video_options(S.video);
			if (video) video->set_options(vopt); else cpu.opt = vopt;
		}

		// emulation runs on its own clock (the machine's 57.9 Hz), independent of the display refresh
		bool ran = false;
		const double perf_t0 = perf.on ? perf_now_ms() : 0.0;
		const double feed0 = m.perf_feed_ms, cpu0 = m.perf_cpu_ms, dcs0 = m.perf_dcs_ms; const uint64_t draws0 = m.perf_draws, quads0 = m.perf_quads;
		for (int guard = 0; t >= next_frame && guard < (bench ? 1 : 3); guard++)
		{
			bool focused = GetForegroundWindow() == hwnd;
			controls.update(m.inputs, focused);
			if (autoplay)
			{
				static int af = 0; af++;
				auto hit = [&](int at) { return af >= at && af < at + 6; };
				if (hit(60) || hit(80) || hit(100)) m.inputs.in0 &= ~in0bit::COIN1;
				if (hit(140) || hit(320) || hit(500) || hit(680) || hit(860) || hit(1040)) m.inputs.in0 &= ~in0bit::START;
				if (af > 1300) m.inputs.accel = 255;
			}
			std::fill(std::begin(g_pressed), std::end(g_pressed), false);
			m.run_frame();
			Telemetry tele;
			const bool tele_ok = m.read_telemetry(tele);
			controls.ffb_update(m.wheel_motor, tele_ok ? &tele : nullptr);
			outputs.update(m.lamps, m.wheel_motor);
			next_frame += 1.0 / m.refresh_hz();
			ran = true;
		}
		if (t - next_frame > 0.25) next_frame = t;
		if (bench) next_frame = 0;   // --bench: run frames back to back (no pacing) to measure throughput

		if (perf.on && ran) perf.emulated(perf_now_ms() - perf_t0, m.perf_feed_ms - feed0, m.perf_draws - draws0, m.perf_quads - quads0, m.perf_cpu_ms - cpu0, m.perf_dcs_ms - dcs0);
		bool vsync = video ? S.video.vsync : false;
		if (!ran && !vsync) { Sleep(1); continue; }
		const double perf_p0 = perf.on ? perf_now_ms() : 0.0;
		present();
		if (perf.on && video) perf.gpu(video->last_gpu_ms());
		if (perf.on) { perf.presented(perf_now_ms() - perf_p0); if (!perf.summary().empty()) SetWindowTextA(hwnd, ("Cruis'n USA (PC) - " + perf.summary()).c_str()); }

		if (ran && !shot.empty() && ++shot_count >= shot_frames)
		{
			std::vector<uint32_t> px; int pw = 0, ph = 0;
			bool ok = video ? video->read_display(px, pw, ph) : false;
			int sc = video ? S.video.internal_scale : 1, w = (m.screen_w() + 2 * (video ? vopt.wide_margin : 0)) * sc, h = m.screen_h() * sc;
			if (!video) { pw = MidVUnit::FRAME_STRIDE; px.assign(m.frame_rgba(), m.frame_rgba() + size_t(MidVUnit::FRAME_STRIDE) * 512); ok = true; }
			if (ok)
			{
				std::vector<uint8_t> rgb(size_t(w) * h * 3);
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++)
					{
						uint32_t c = px[size_t(y) * pw + x];
						uint8_t *d = &rgb[(size_t(y) * w + x) * 3];
						if (video) { d[0] = uint8_t(c); d[1] = uint8_t(c >> 8); d[2] = uint8_t(c >> 16); }
						else { d[0] = uint8_t(c >> 16); d[1] = uint8_t(c >> 8); d[2] = uint8_t(c); }
					}
				size_t len = 0;
				void *png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &len);
				std::string path = shot;
				if (shot_seq > 1)
				{
					char suf[24]; std::snprintf(suf, sizeof(suf), "_%03d.png", shot_count - shot_frames);
					path = shot.substr(0, shot.rfind('.')) + suf;
				}
				if (FILE *fp = std::fopen(path.c_str(), "wb")) { std::fwrite(png, 1, len, fp); std::fclose(fp); }
				mz_free(png);
			}
			if (shot_count - shot_frames + 1 >= shot_seq) g_quit = true;
		}

		if (ran) fps_n++;
		if (t - fps_t >= 1.0)
		{
			char title[256];
			const char *rn = video ? video->name() : "CPU";
			std::snprintf(title, sizeof(title), "Cruis'n USA (PC) - %d fps  %s %dx  wheel %02X  %s%s  snd %.0fms  ffb: %s%s%s", fps_n, rn,
			              video ? S.video.internal_scale : 1, m.inputs.wheel, S.video.vsync ? "vsync " : "",
			              S.video.texture_filter ? "filtered " : "", audio.latency_ms(), controls.ffb_status().c_str(),
			              backend_note.empty() ? "" : "  ", backend_note.c_str());
			SetWindowTextA(hwnd, title);
			fps_n = 0; fps_t = t;
		}
	}

	controls.ffb_stop();
	outputs.stop();
	audio.stop();
	m.save_nvram(S.nvram);
	if (video) video->shutdown();
	hub.shutdown();
	timeEndPeriod(1);
	return 0;
}
