// Cruis'n USA PC - Windows front end: settings + launcher, window, video backend, input, pacing
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <dwmapi.h>
#include <shlobj.h>

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
#include <thread>

#include "perf.h"
#include "pause_menu.h"
#include "calltrace.h"
#include "platform/stall_watch.h"
#include "platform/vblank_clock.h"
#include "machine/midvunit.h"
#include "machine/telemetry.h"
#include "outputs/outputs.h"
#include "platform/audio_wasapi.h"
#include "platform/splash.h"
#include "video/video_backend.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

namespace {

bool g_keys[256];
bool g_pressed[256];           // edge-triggered key presses
bool g_quit = false;
bool g_toggle_fs = false;
bool g_hide_mouse = true;      // no pointer over the game's picture
bool g_menu_esc = false;       // Esc while the options menu is open: closes it
GameMenu *g_menu = nullptr;    // the options menu of the running game, while it exists
int  g_size_w = 0, g_size_h = 0;

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	const bool menu_open = g_menu && g_menu->is_open();
	if (menu_open && g_menu->message(h, m, w, l)) return 1;
	switch (m)
	{
	case WM_SETCURSOR:
		if (LOWORD(l) == HTCLIENT && g_hide_mouse && !menu_open) { SetCursor(nullptr); return TRUE; }
		break;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (w < 256) { if (!g_keys[w]) g_pressed[w] = true; g_keys[w] = true; }
		if (w == VK_F11 || (w == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) g_toggle_fs = true;
		if (w == VK_ESCAPE && !menu_open) g_quit = true;   // (in the options menu Esc is "back")
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
	o.frame_ahead_limit = v.frame_ahead_limit;
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

// "Cruis'n USA.lnk" on the user's desktop, pointing at this program (asked for once, at the first start)
bool create_desktop_shortcut()
{
	char exe[MAX_PATH], desk[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	if (FAILED(SHGetFolderPathA(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk))) return false;
	std::string dir = exe;
	dir = dir.substr(0, dir.find_last_of("\\/"));
	const std::string lnk = std::string(desk) + "\\Cruis'n USA.lnk";
	const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
	bool ok = false;
	IShellLinkA *link = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkA, reinterpret_cast<void **>(&link))) && link)
	{
		link->SetPath(exe);
		link->SetWorkingDirectory(dir.c_str());
		link->SetIconLocation(exe, 0);
		link->SetDescription("Cruis'n USA for Windows");
		IPersistFile *file = nullptr;
		if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file))) && file)
		{
			wchar_t wide[MAX_PATH];
			MultiByteToWideChar(CP_ACP, 0, lnk.c_str(), -1, wide, MAX_PATH);
			ok = SUCCEEDED(file->Save(wide, TRUE));
			file->Release();
		}
		link->Release();
	}
	if (com) CoUninitialize();
	return ok;
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
	const bool first_start = GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES;

	// command line overrides (testing and scripting)
	std::string shot = arg_value(a, "--shot");
	int shot_frames = 600, shot_seq = 1, shot_step = 1;
	if (std::string v = arg_value(a, "--shot-frames"); !v.empty()) shot_frames = std::atoi(v.c_str());
	if (std::string v = arg_value(a, "--shot-seq"); !v.empty()) shot_seq = std::max(1, std::atoi(v.c_str()));
	if (std::string v = arg_value(a, "--shot-step"); !v.empty()) shot_step = std::max(1, std::atoi(v.c_str()));   // every n-th frame
	bool autoplay = a.find("--autoplay") != std::string::npos;
	const bool autopilot = a.find("--autopilot") != std::string::npos;   // with --autoplay: steers along the road
	PerfStats perf;
	const bool bench = a.find("--bench") != std::string::npos;
	if (a.find("--perf") != std::string::npos) { perf.on = true; perf.log_path = exe_relative("perf.log"); }
	// --perf also records every frame: perf_spikes.log gets each slow frame with its neighbours and, at the end, the percentiles;
	// --perf-spike <ms> sets the limit (default 1.5 x median), --perf-csv <file> writes all frames, --perf-seconds <n> quits after n s
	FrameProf prof;
	prof.on = perf.on;
	prof.spike_path = exe_relative("perf_spikes.log");
	if (std::string v = arg_value(a, "--perf-spike"); !v.empty()) prof.spike_ms = std::atof(v.c_str());
	if (std::string v = arg_value(a, "--perf-csv"); !v.empty()) prof.csv_path = v;
	if (std::string v = arg_value(a, "--perf-log"); !v.empty()) prof.spike_path = v;
	double perf_seconds = 0;
	if (std::string v = arg_value(a, "--perf-seconds"); !v.empty()) perf_seconds = std::atof(v.c_str());
	if (std::string v = arg_value(a, "--rom"); !v.empty()) S.rom = v;
	if (std::string v = arg_value(a, "--version"); !v.empty()) S.version = v;
	if (std::string v = arg_value(a, "--nvram"); !v.empty()) S.nvram = v;
	if (std::string v = arg_value(a, "--backend"); !v.empty())
		S.video.renderer = (v == "vk" || v == "vulkan") ? Renderer::Vulkan : (v == "cpu" ? Renderer::Cpu : Renderer::OpenGL);
	if (std::string v = arg_value(a, "--shadows"); !v.empty())
		S.video.shadows = v == "off" ? ShadowMode::Off : v == "original" ? ShadowMode::Original : ShadowMode::Modern;
	if (std::string v = arg_value(a, "--frame-unlock"); !v.empty()) S.smooth_frames = v != "0";   // experimental, breaks the game's timing
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

	const bool scripted = !shot.empty() || autoplay || a.find("--save-ini") != std::string::npos || a.find("--list-devices") != std::string::npos;
	if (!scripted)
	{
		// The very first start (no configuration yet): create the configuration and the save file next to the program, so that
		// both exist and point there, and ask for the one thing that cannot be shipped: the game's ROM set.
		if (S.rom.empty())
			for (const char *guess : {"crusnusa.zip", "roms/crusnusa.zip", "roms\\crusnusa.zip"})
				if (GetFileAttributesA(exe_relative(guess).c_str()) != INVALID_FILE_ATTRIBUTES) { S.rom = guess; break; }
		if (first_start)
		{
			create_default_save(exe_relative(S.nvram));
			S.save(ini);
		}
		const bool rom_ok = !S.rom.empty() && GetFileAttributesA(exe_relative(S.rom).c_str()) != INVALID_FILE_ATTRIBUTES;
		if (!rom_ok && (first_start || !want_launcher))
		{
			const int r = MessageBoxA(nullptr,
			    "Welcome to Cruis'n USA for Windows.\n\n"
			    "The game itself is not part of this program: it needs the ROM set crusnusa.zip (the MAME set of Cruis'n USA).\n\n"
			    "Press OK to point to your crusnusa.zip now, or Cancel to do it later in the launcher (Home > ROM zip).",
			    "Cruis'n USA - ROM set needed", MB_OKCANCEL | MB_ICONINFORMATION);
			std::string pth;
			if (r == IDOK && browse_rom(nullptr, pth)) { S.rom = pth; S.save(ini); }
			want_launcher = true;
		}
		// ...and, the first time only, whether a shortcut on the desktop is wanted. Nothing is put there unasked.
		if (first_start &&
		    MessageBoxA(nullptr, "Would you like a shortcut to Cruis'n USA on your desktop?", "Cruis'n USA - desktop shortcut", MB_YESNO | MB_ICONQUESTION) == IDYES &&
		    !create_desktop_shortcut())
			MessageBoxA(nullptr, "The shortcut could not be created.", "Cruis'n USA", MB_OK | MB_ICONWARNING);
	}

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
				std::fprintf(f, " | buttons %d | ffb %d | same hardware %d\n", d.buttons, d.ffb ? 1 : 0, d.siblings);
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
	if (!m.load_roms(exe_relative(S.rom), S.version, err))
	{
		MessageBoxA(nullptr, ("ROM load failed:\n" + err + "\n\nSet the ROM path in the launcher (Home > ROM zip).").c_str(), "Cruis'n USA", MB_ICONERROR);
		return 1;
	}
	m.rom_patches.draw_distance_pct = S.video.draw_distance;
	m.rom_patches.rubberband_pct = S.rubberband;
	m.rom_patches.smooth_frames = S.smooth_frames;
	m.rom_patches.cruise_leg_rate = S.steady_cadence && arg_value(a, "--steady") != "0";   // (steady_cadence = false: the machine as it is)
	m.cpu_overclock = S.smooth_frames ? 2 : 1;   // a frame per vblank needs the frame's work done within one vblank
	m.dcs_thread = S.dsp_thread;
	m.steady_cadence = S.steady_cadence ? 1 : 0;
	if (std::string v = arg_value(a, "--steady"); !v.empty()) m.steady_cadence = std::clamp(std::atoi(v.c_str()), 0, 2);
	if (std::string v = arg_value(a, "--dcs-thread"); !v.empty()) m.dcs_thread = std::atoi(v.c_str());   // -1 auto, 0 off, 1 on
	m.rom_patches.wide_margin = S.video.renderer == Renderer::Cpu ? 0 : wide_margin_for(S.video);
	m.set_wide_margin(m.rom_patches.wide_margin);
	m.set_hud_spread(S.video.hud == HudPlacement::Edges ? 1.0f : S.video.hud == HudPlacement::Quarter25 ? 0.25f : S.video.hud == HudPlacement::Half50 ? 0.5f : S.video.hud == HudPlacement::Quarter75 ? 0.75f : 0.0f);
	m.reset();
	if (!m.load_nvram(exe_relative(S.nvram)))
		m.load_default_nvram();        // embedded, pre-calibrated CMOS: no calibration screen, ever
	m.inputs.dsw = S.dsw;

	timeBeginPeriod(1);
	WNDCLASSA wc{};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = hi;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(1));
	wc.lpszClassName = "CruisnPC";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("CruisnPC", "Cruis'n USA (PC)", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, hi, nullptr);
	apply_window_mode(hwnd, S.video, S.video.window_mode);
	ShowWindow(hwnd, SW_SHOW);

	// --perf: slow graphics calls (--call-ms <limit>, default 2) and the stack of the frame loop when a frame stalls
	// (--stall-ms <limit>, default 28, 0 = off) go to perf_calls.log / --call-log <file>. The watcher thread is started before
	// the video backend exists: a thread that appears while the driver is already presenting is itself an event (PERFORMANCE.md).
	std::string call_log = arg_value(a, "--call-log");
	if (call_log.empty()) call_log = exe_relative("perf_calls.log");
	if (prof.on)
	{
		const std::string sm = arg_value(a, "--stall-ms");
		if (sm != "0") stallwatch::start(call_log, sm.empty() ? 28.0 : std::atof(sm.c_str()));
		if (!prof.csv_path.empty()) vblank_clock::start();   // the display's vblanks, written next to the csv (<csv>.vblank)
	}

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

	// Display sync: one game frame per sync_k display refreshes instead of the machine's own 57.9 Hz clock. sync_speed is the
	// resulting game speed (1.036 at 59.94 Hz); the sound is played at the same factor.
	// (testing: --fake-refresh <hz> behaves as if the display ran at that rate: VSync off, and every present is followed by a
	// wait for the next "vblank" of a clock at that rate. For checking the pacing logic at rates this display does not have.)
	const double fake_hz = arg_value(a, "--fake-refresh").empty() ? 0.0 : std::atof(arg_value(a, "--fake-refresh").c_str());
	if (fake_hz > 0) S.video.vsync = false;
	double sync_speed = 1.0, display_hz = 60.0;
	int sync_k = 0;   // 0 = off (the machine's own clock)
	if (video && S.video.display_sync && !bench)
	{
		MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
		DEVMODEW dm{}; dm.dmSize = sizeof(dm);
		if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
		{
			double hz = double(dm.dmDisplayFrequency);
			if (dm.dmDisplayFrequency % 60 == 59) hz = (hz + 1.0) * 1000.0 / 1001.0;   // 59 / 119 = the NTSC rates 59.94 / 119.88
			if (fake_hz > 0) hz = fake_hz;
			display_hz = hz;
			const int k = std::max(1, int(hz / m.refresh_hz() + 0.5));
			const double speed = hz / k / m.refresh_hz();
			if (speed > 0.94 && speed < 1.06) { sync_k = k; sync_speed = speed; }
		}
	}

	AudioOut audio;
	bool audio_ok = S.audio.enabled && audio.start();
	audio.set_volume(float(S.audio.volume) / 100.0f);
	audio.set_latency_ms(S.audio.latency_ms);
	auto hook_audio = [&](bool on) {
		if (on && audio_ok)
		{
			m.on_audio = [&audio, &sync_speed](const int16_t *b, int n, double rate) { audio.push(b, n, rate * sync_speed); };
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

	// The first thread that starts in the process after the OpenGL driver has begun to present makes the driver's next buffer
	// swap wait for 50 to 100 ms, once (measured: NVIDIA; the driver's own thread sits in a wait for a GPU synchronisation
	// object, D3DKMTWaitForSynchronizationObjectFromCpu). Left alone, that thread is one of Windows' thread pool workers about
	// 30 s after the start, in the middle of the first race. So it is given one here, on the first picture, before anything
	// moves. What counts is that a thread is *created* (a work item for the thread pool, whose threads exist already, does
	// nothing: 8 of 8 runs still stalled), not how long it lives or how many there are.
	// The warm-up checks itself: the driver's reaction shows as a present that takes much longer than two refreshes right
	// after the thread has started. Threads are started, one per three presents, until two of them in a row have caused no
	// such present: then a new thread no longer disturbs the driver. (The first version started one thread and hoped: the
	// reaction came later in 1 of 63 runs.)
	// --no-warmup leaves it out (to measure the stall). Experiments: --warmup-kind 1 a thread that ends at once / 2 a thread
	// pool work item / 3 three threads / 9 presents only; --warmup-log <file>: the presents' times.
	const bool driver_warmup_on = a.find("--no-warmup") == std::string::npos;
	const int warm_kind = arg_value(a, "--warmup-kind").empty() ? 0 : std::atoi(arg_value(a, "--warmup-kind").c_str());
	const std::string warm_log = arg_value(a, "--warmup-log");
	auto driver_warmup = [&]() {
		if (!video || !driver_warmup_on) return;
		std::string log;
		auto timed_present = [&]() {
			const double t0 = perf_now_ms();
			present();
			const double ms = perf_now_ms() - t0;
			char b[32]; std::snprintf(b, sizeof(b), " %.1f", ms); log += b;
			return ms;
		};
		if (warm_kind == 0)
		{
			const double limit = std::max(25.0, 2600.0 / std::max(30.0, display_hz));   // "much longer than two refreshes"
			for (int i = 0; i < 4; i++) timed_present();                                // (the first presents have a long one of their own)
			int clean = 0;
			for (int attempt = 0; attempt < 6 && clean < 2; attempt++)
			{
				HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
				std::thread th([done] { WaitForSingleObject(done, INFINITE); });
				double worst = 0;
				for (int i = 0; i < 3; i++) worst = std::max(worst, timed_present());
				SetEvent(done);
				th.join();
				CloseHandle(done);
				clean = worst < limit ? clean + 1 : 0;
				log += worst < limit ? " | clean" : " | reaction";
			}
		}
		else
		{
			HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr), started = CreateEventA(nullptr, TRUE, FALSE, nullptr);
			std::vector<std::thread> th;
			if (warm_kind == 1) { std::thread([] {}).join(); }
			else if (warm_kind == 2) { TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void *e) { SetEvent(HANDLE(e)); }, started, nullptr); WaitForSingleObject(started, 1000); }
			else if (warm_kind == 3) for (int k = 0; k < 3; k++) th.emplace_back([done] { WaitForSingleObject(done, INFINITE); });
			for (int i = 0; i < 6; i++) timed_present();
			SetEvent(done);
			for (std::thread &t : th) t.join();
			CloseHandle(done); CloseHandle(started);
		}
		if (!warm_log.empty())
			if (FILE *f = std::fopen(warm_log.c_str(), "ab")) { std::fprintf(f, "warm-up kind %d:%s\n", warm_kind, log.c_str()); std::fclose(f); }
	};

	MSG msg;
	// ---- The arcade board's power-up tests (ROM checksums, RAM, sound board, then the result screens with their fixed waits)
	// are part of the game program and take about 1300 frames. They are run unthrottled, silent and unseen behind a loading
	// picture (about two seconds), until the attract mode draws its first picture. (Also after "return to attract" in the
	// pause menu, which resets the machine.)
	auto boot_to_attract = [&]() {
		if (!S.fast_boot) return;
		Splash splash;
		if (shot.empty()) splash.show(hwnd, "LOADING ...");
		hook_audio(false);
		for (int f = 0; f < 6000 && !g_quit; f++)
		{
			m.inputs = MachineInputs{};
			m.inputs.dsw = S.dsw;
			m.run_frame();
			if (f > 400 && m.quads_last_frame > 50) break;
			if (f % 45 == 0)
				while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
		}
		audio.clear();
		if (video) video->set_pillarbox(!m.world_shown());
		present();          // the game's first picture is in the window before the loading picture goes
		splash.hide();
		SetForegroundWindow(hwnd);
	};
	boot_to_attract();
	hook_audio(true);
	std::fill(std::begin(g_pressed), std::end(g_pressed), false);

	// wait until `until` (seconds on the now_sec clock): sleep while more than 2 ms remain, then spin (Sleep alone is only
	// accurate to a millisecond or two, which shows as uneven frame times)
	timeBeginPeriod(1);
	auto wait_until = [&](double until) {
		for (;;)
		{
			const double left = until - now_sec();
			if (left <= 0) break;
			if (left > 0.002) Sleep(DWORD((left - 0.0015) * 1000.0)); else YieldProcessor();
		}
	};
	if (sync_k && fake_hz <= 0) { S.video.vsync = true; vopt = make_video_options(S.video); if (video) video->set_options(vopt); }
	int sync_fast = 0, sync_passes = 0;
	double sync_emu_t = 0;     // when the last emulated frame of the display sync was started
	double sync_last = 0, sync_done = 0;
	bool sync_clock = false;   // display sync paced by the clock because the swap does not wait

	if (video) video->set_profiling(prof.on);
	driver_warmup();
	if (prof.on && video)
	{
		const std::string cm = arg_value(a, "--call-ms");
		g_calltrace.open(call_log, cm.empty() ? 2.0 : std::atof(cm.c_str()), video->name());   // (its own handle on the file; both append whole lines)
		if (std::string v = arg_value(a, "--gl-debug"); !v.empty()) video->set_debug(std::atoi(v.c_str()));   // see IVideoBackend::set_debug
	}
	double prof_last = 0;
	uint64_t prof_flips = m.page_flips, prof_late = m.catchup_fails;
	uint32_t prof_n = 0;
	double t_start = now_sec();
	double next_frame = 0;
	double fps_t = 0; int fps_n = 0; int shot_count = 0;
	bool fullscreen_now = S.video.window_mode == WindowMode::Fullscreen;
	PauseMenu pause;
	// the options menu inside the game (OpenGL): the launcher's pages over the picture
	GameMenu menu;
	if (video && shot.empty() && video->set_overlay([&menu] { menu.render(); }))
	{
		if (menu.init(hwnd, S, ini, hub, controls)) g_menu = &menu;
		else video->set_overlay(nullptr);
	}
	g_hide_mouse = S.video.hide_mouse;
	// settings the menu changed, applied while it is open (the game keeps the margin and aspect it was started with)
	const VideoOptions vopt_start = vopt;
	auto apply_live = [&]() {
		VideoOptions o = make_video_options(S.video);
		o.wide_margin = vopt_start.wide_margin; o.aspect = vopt_start.aspect; o.keep_aspect = vopt_start.keep_aspect;
		if (sync_k && fake_hz <= 0) o.vsync = true;
		if (std::memcmp(&o, &vopt, sizeof(o)) != 0) { vopt = o; if (video) video->set_options(vopt); else cpu.opt = vopt; }
		m.set_shadow_mode(int(S.video.shadows));
		m.set_hud_spread(S.video.hud == HudPlacement::Edges ? 1.0f : S.video.hud == HudPlacement::Quarter25 ? 0.25f : S.video.hud == HudPlacement::Half50 ? 0.5f : S.video.hud == HudPlacement::Quarter75 ? 0.75f : 0.0f);
		audio.set_volume(float(S.audio.volume) / 100.0f);
		audio.set_latency_ms(S.audio.latency_ms);
		m.inputs.dsw = S.dsw;
		m.steady_cadence = S.steady_cadence ? 1 : 0;
		g_hide_mouse = S.video.hide_mouse;
		static WindowMode shown_mode = S.video.window_mode;
		if (S.video.window_mode != shown_mode)
		{
			shown_mode = S.video.window_mode;
			fullscreen_now = shown_mode == WindowMode::Fullscreen;
			apply_window_mode(hwnd, S.video, shown_mode);
		}
	};
	bool pause_held = false, pause_sel_held = false;
	int pause_zone = 0;
	while (!g_quit)
	{
		FrameRec rec;
		if (prof.on) stallwatch::beat();
		const double pr0 = prof.on ? perf_now_ms() : 0.0;
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
		if (prof.on) rec.msg = float(perf_now_ms() - pr0);
		if (g_toggle_fs)
		{
			g_toggle_fs = false;
			fullscreen_now = !fullscreen_now;
			WindowMode back = S.video.window_mode == WindowMode::Fullscreen ? WindowMode::Window : S.video.window_mode;
			apply_window_mode(hwnd, S.video, fullscreen_now ? WindowMode::Fullscreen : back);
		}
		if (g_size_w > 0) { if (video) video->resize(g_size_w, g_size_h); g_size_w = 0; sync_clock = false; sync_fast = 0; }   // (another display, perhaps: look again whether the swap waits)

		// ---- pause (the "pause" action, P by default): the machine stands still, no sound, no force. With OpenGL the launcher's
		// options are shown over the picture and what is changed there applies at once; the other backends show the small
		// menu drawn with the game's font (continue, return to attract, exit).
		if (!pause.is_open() && !menu.is_open())
		{
			// (testing: --pause-at <frame> opens the menu by itself, --pause-sel <n> moves the small menu's selection down n times)
			static const int pause_at = arg_value(a, "--pause-at").empty() ? 0 : std::atoi(arg_value(a, "--pause-at").c_str());
			static int pause_frames = 0;
			const bool pause_test = pause_at > 0 && ++pause_frames == pause_at;
			const bool ph = controls.action_active("pause");
			if ((ph && !pause_held) || pause_test)
			{
				if (menu.available()) { menu.open(); SetCursor(LoadCursor(nullptr, IDC_ARROW)); }
				else
				{
					pause.open(m);
					if (pause_test) for (int k = std::atoi(arg_value(a, "--pause-sel").c_str()); k > 0; k--) pause.update(m, false, true, false);
				}
				audio.clear();
				controls.ffb_update(0, nullptr);
				pause_sel_held = true;                                    // a pedal that is down has to come up first
				pause_zone = m.inputs.wheel < 88 ? -1 : m.inputs.wheel > 168 ? 1 : 0;
				g_menu_esc = false;
			}
			pause_held = ph;
		}
		if (pause.is_open() || menu.is_open())
		{
			MachineInputs pin;
			controls.update(pin, GetForegroundWindow() == hwnd);
			const bool ph = controls.action_active("pause");
			int choice = 0;   // 1 continue, 2 return to attract, 3 exit
			if (menu.is_open())
			{
				const GameMenu::Action act = menu.frame();
				choice = act == GameMenu::Action::Continue ? 1 : act == GameMenu::Action::Attract ? 2 : act == GameMenu::Action::Exit ? 3 : 0;
				if (!menu.capturing() && ph && !pause_held) choice = 1;
				apply_live();
			}
			else
			{
				// the wheel, every hat / d-pad and the arrow keys move; accept, Start or the accelerator choose; back continues
				const Controls::MenuNav nav = controls.menu_nav();
				const int zone = (pin.wheel < 88 || nav.up) ? -1 : (pin.wheel > 168 || nav.down) ? 1 : (pin.wheel > 108 && pin.wheel < 148) ? 0 : pause_zone;
				const bool up = g_pressed[VK_UP] || (zone == -1 && pause_zone != -1), down = g_pressed[VK_DOWN] || (zone == 1 && pause_zone != 1);
				pause_zone = zone;
				const bool sel_now = !(pin.in0 & in0bit::START) || pin.accel > 160 || nav.accept;
				static bool small_back = false;
				const bool back_edge = nav.back && !small_back;
				small_back = nav.back;
				const bool select = (g_pressed[VK_RETURN] && !(GetKeyState(VK_MENU) & 0x8000)) || (sel_now && !pause_sel_held);
				pause_sel_held = sel_now;
				const PauseMenu::Choice pc = pause.update(m, up, down, select);
				choice = pc == PauseMenu::Continue ? 1 : pc == PauseMenu::Attract ? 2 : pc == PauseMenu::Exit ? 3 : 0;
				if ((ph && !pause_held) || back_edge) choice = 1;
			}
			{   // (testing: --pause-do continue|attract|exit chooses by itself two seconds after --pause-at opened the menu)
				static const std::string pause_do = arg_value(a, "--pause-do");
				static int pause_open_frames = 0;
				if (!pause_do.empty() && ++pause_open_frames == 120) choice = pause_do == "attract" ? 2 : pause_do == "exit" ? 3 : 1;
			}
			pause_held = ph;
			std::fill(std::begin(g_pressed), std::end(g_pressed), false);
			if (choice == 0)
			{
				present();
				if (!video || !S.video.vsync) Sleep(10);
				continue;
			}
			const bool was_menu = menu.is_open();
			menu.close();
			pause.close();
			if (choice == 3) { g_quit = true; continue; }
			if (was_menu)
			{
				// what the menu may have changed and the game does not read every frame: the wheel's motor, the cabinet outputs
				controls.ffb_stop();
				controls.ffb_start(hwnd);
				outputs.stop();
				if (S.outputs.mode != OutputMode::Off) outputs.start(S.outputs);
			}
			if (choice == 2)
			{
				m.save_nvram(exe_relative(S.nvram));
				m.reset();
				m.inputs.dsw = S.dsw;
				boot_to_attract();
				hook_audio(true);
			}
			// back to the game: the pacing starts from now
			next_frame = now_sec() - t_start;
			sync_last = now_sec(); sync_done = 0; sync_fast = 0; sync_emu_t = 0;
			prof_last = 0;
			continue;
		}

		double t = now_sec() - t_start;

		// hotkeys: F5/F6 internal resolution, F7 texture filter, F8 vsync, F9 output smoothing
		bool vchanged = false;
		if (g_pressed[VK_F3]) { S.video.aa = (S.video.aa + 1) % 4; vchanged = true; }
		if (g_pressed[VK_F4]) { S.video.shadows = ShadowMode((int(S.video.shadows) + 1) % 3); m.set_shadow_mode(int(S.video.shadows)); vchanged = true; }
		if (g_pressed[VK_F5]) { S.video.internal_scale = std::max(1, S.video.internal_scale - 1); vchanged = true; }
		if (g_pressed[VK_F6]) { S.video.internal_scale = std::min(8, S.video.internal_scale + 1); vchanged = true; }
		if (g_pressed[VK_F7]) { S.video.texture_filter = !S.video.texture_filter; vchanged = true; }
		if (g_pressed[VK_F8]) { S.video.vsync = !S.video.vsync; vchanged = true; if (!S.video.vsync) { sync_k = 0; sync_speed = 1.0; next_frame = now_sec() - t_start; } }
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
		// display sync: exactly one frame every sync_k presents (each present waits for the display's vblank)
		// One emulated frame per sync_k refreshes, counted in time and not in presents. On a 60 Hz display (sync_k 1) that is
		// every pass of this loop, as before. On a faster display a pass that computes a game picture can take longer than one
		// refresh (8.3 ms at 120 Hz): counting presents then loses that refresh for good and the game runs slow (measured
		// with a simulated display: 0.92 x at 120 Hz, 0.82 x at 165 Hz, 0.86 x at 240 Hz). Counted in time, the next frame
		// is simply due sooner.
		const double sync_T = sync_k ? 1.0 / (m.refresh_hz() * sync_speed * sync_k) : 0.0;   // one display refresh
		const double sync_now = now_sec();
		// (Every sync_k-th pass is due in any case, as before: the passes do not begin at even distances, so time alone
		// would skip a frame now and then. Time only adds the frame that counting passes would lose.)
		sync_passes++;
		const bool sync_due = sync_k && (sync_emu_t <= 0 || sync_passes >= sync_k || sync_now - sync_emu_t >= (double(sync_k) - 0.5) * sync_T);
		if (sync_due) { sync_emu_t = sync_now; sync_passes = 0; }
		for (int guard = 0; (sync_k ? (sync_due && guard == 0) : t >= next_frame) && guard < (bench ? 1 : 3); guard++)
		{
			const double pi0 = prof.on ? perf_now_ms() : 0.0;
			bool focused = GetForegroundWindow() == hwnd;
			controls.update(m.inputs, focused);
			if (prof.on) rec.input += float(perf_now_ms() - pi0);
			if (autoplay)
			{
				static int af = 0; af++;
				auto hit = [&](int at) { return af >= at && af < at + 6; };
				if (hit(60) || hit(80) || hit(100)) m.inputs.in0 &= ~in0bit::COIN1;
				if (hit(140) || hit(320) || hit(500) || hit(680) || hit(860) || hit(1040)) m.inputs.in0 &= ~in0bit::START;
				if (af > 1300) m.inputs.accel = 255;
				// --menu-wheel n: wheel position (0..255) held in the selection screens, to test the other choices
				// (a list "a,b,c" steps through the positions, 60 frames each, and leaves the car selection to its timer)
				static const std::vector<int> menu_wheel = [&] {
					std::vector<int> w; std::string v = arg_value(a, "--menu-wheel");
					for (size_t p = 0; p < v.size();) { w.push_back(std::atoi(v.c_str() + p)); p = v.find(',', p); if (p == std::string::npos) break; p++; }
					return w; }();
				if (!menu_wheel.empty() && af > 520 && af < 1300)
				{
					m.inputs.wheel = uint8_t(menu_wheel[std::min(menu_wheel.size() - 1, size_t(std::max(0, af - 560) / 60))]);
					if (menu_wheel.size() > 1) m.inputs.in0 |= in0bit::START;
				}
				// --track n (0..13): that race is chosen whatever the selection screen shows (testing)
				static const int test_track = [&] { std::string v = arg_value(a, "--track"); return v.empty() ? -1 : std::atoi(v.c_str()); }();
				static bool rolling = false;
				if (test_track >= 0 && !rolling)
				{
					m.ram_poke(0xE664, uint32_t(test_track));   // CHOSEN_RACE
					Telemetry t;
					if (m.read_telemetry(t) && t.speed > 30) rolling = true;
				}
				if (autopilot)
				{
					static float prev = 0, dfilt = 0;
					Telemetry t;
					if (m.read_telemetry(t))
					{
						if (m.ram_peek(0xE634) < 40) m.ram_poke(0xE634, 60);   // _countdown: the time limit does not end the test
						const float d = t.dist_to_center;
						if (d != prev) { dfilt = dfilt * 0.6f + (d - prev) * 0.4f; prev = d; }
						m.inputs.wheel = uint8_t(std::clamp(128.0f + std::clamp(0.10f * d + 1.2f * dfilt, -100.0f, 100.0f), 16.0f, 240.0f));
						m.inputs.accel = uint8_t(std::fabs(d) > 700.0f ? 120 : 255);
					}
					else if (af > 2600 && (af / 6) % 30 == 0) m.inputs.in0 &= ~in0bit::START;
				}
			}
			std::fill(std::begin(g_pressed), std::end(g_pressed), false);
			const double pe0 = prof.on ? perf_now_ms() : 0.0;
			const double cs0 = perf_now_ms();
			m.run_frame();
			// (testing: --cpu-slow <factor> spends factor - 1 times the emulation's time again: a host that much slower)
			static const double cpu_slow = arg_value(a, "--cpu-slow").empty() ? 1.0 : std::atof(arg_value(a, "--cpu-slow").c_str());
			if (cpu_slow > 1.0) { const double until = cs0 + (perf_now_ms() - cs0) * cpu_slow; while (perf_now_ms() < until) YieldProcessor(); }
			const double pe1 = prof.on ? perf_now_ms() : 0.0;
			rec.t_emu = pe1;
			Telemetry tele;
			const bool tele_ok = m.read_telemetry(tele);
			// no force while the attract mode runs: the game keeps its wheel servo on there, which is felt as a constant drag
			controls.ffb_update(m.in_attract() ? uint8_t(0) : m.wheel_motor, tele_ok ? &tele : nullptr);
			outputs.update(m.lamps, m.wheel_motor);
			if (prof.on) { rec.emu += float(pe1 - pe0); rec.ffb += float(perf_now_ms() - pe1); rec.ran++; }
			next_frame += 1.0 / m.refresh_hz();
			ran = true;
		}
		if (t - next_frame > 0.25) next_frame = t;
		if (bench) next_frame = 0;   // --bench: run frames back to back (no pacing) to measure throughput

		if (perf.on && ran) perf.emulated(perf_now_ms() - perf_t0, m.perf_feed_ms - feed0, m.perf_draws - draws0, m.perf_quads - quads0, m.perf_cpu_ms - cpu0, m.perf_dcs_ms - dcs0);
		bool vsync = video ? (S.video.vsync || fake_hz > 0) : false;
		if (!sync_k && !ran && !vsync) { wait_until(t_start + next_frame); continue; }
		const double pw0 = prof.on ? perf_now_ms() : 0.0;
		const double sync_period = sync_k ? 1.0 / (m.refresh_hz() * sync_speed) / sync_k : 0.0;
		if (sync_k)
		{
			// The buffer swap waits for the display's vblank, and that wait is the pace: every picture is shown for exactly one
			// refresh. (A timer set to the nominal refresh rate, as before, drifts against the real one: every 15 to 20 seconds
			// the two cross, and for a moment pictures are shown for 10 and 22 ms in turn.) Only when the swap turns out not
			// to wait (VSync forced off in the driver, window on a faster display) the clock paces instead.
			if (sync_clock) wait_until(sync_last + sync_period);
			sync_last = now_sec();
			next_frame = now_sec() - t_start;   // keeps the clock mode's bookkeeping current for a switch (F8)
		}
		if (prof.on) rec.wait = float(perf_now_ms() - pw0);
		const double perf_p0 = perf.on ? perf_now_ms() : 0.0;
		rec.t_top = pr0; rec.t_present = perf_p0;
		if (prof.on && video) rec.gl_frame = video->present_index();
		if (video) video->set_pillarbox(!m.world_shown());   // menus and 2D screens: the arcade's 4:3 picture, black bars beside it
		// (experiment --pace 1 / 2: the display's vblank itself is the pace, waited for after the present; 2: a picture that
		// has not changed is not presented again, so that the GPU has two refreshes for a game picture instead of one)
		static const int pace_mode = arg_value(a, "--pace").empty() ? 0 : std::atoi(arg_value(a, "--pace").c_str());
		static bool pace_pillar = false; static int pace_skipped = 0;
		bool pace_show = true;
		if (pace_mode == 2 && sync_k && video)
		{
			const bool pillar = !m.world_shown();
			pace_show = m.take_display_changed() || vchanged || pillar != pace_pillar || pace_skipped >= 30 || prof_n < 4;
			pace_pillar = pillar;
			pace_skipped = pace_show ? 0 : pace_skipped + 1;
		}
		if (pace_show) present(); else if (video) video->flush();   // (the frame's polygons start on the GPU now, not with the next swap)
		if (pace_mode && sync_k && video) vblank_clock::wait_next();
		if (fake_hz > 0) { const double n = now_sec(); wait_until(std::ceil(n * fake_hz + 1e-6) / fake_hz); }
		if (perf.on && video) perf.gpu(video->last_gpu_ms());
		if (perf.on) { perf.presented(perf_now_ms() - perf_p0); if (!perf.summary().empty()) SetWindowTextA(hwnd, ("Cruis'n USA (PC) - " + perf.summary()).c_str()); }
		const double pp1 = prof.on ? perf_now_ms() : 0.0;
		if (sync_k && !sync_clock && !pace_mode)
		{
			// eight pictures in a row faster than three quarters of a refresh: nothing waits for the display
			const double done = now_sec();
			sync_fast = (sync_done > 0 && done - sync_done < sync_period * 0.75) ? sync_fast + 1 : 0;
			sync_done = done;
			if (sync_fast >= 8) sync_clock = true;
		}

		if (ran && !shot.empty() && ++shot_count >= shot_frames && (shot_count - shot_frames) % shot_step == 0)
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
					char suf[24]; std::snprintf(suf, sizeof(suf), "_%03d.png", (shot_count - shot_frames) / shot_step);
					path = shot.substr(0, shot.rfind('.')) + suf;
				}
				if (FILE *fp = std::fopen(path.c_str(), "wb")) { std::fwrite(png, 1, len, fp); std::fclose(fp); }
				mz_free(png);
			}
			if ((shot_count - shot_frames) / shot_step + 1 >= shot_seq) g_quit = true;
		}

		if (prof.on)
		{
			const double pend = perf_now_ms();
			rec.frame = prof_n++;
			rec.present = float(pp1 - perf_p0);
			rec.swap = video ? float(video->last_swap_ms()) : -1.0f;
			if (video)
			{
				video->swap_times(rec.t_swap0, rec.t_swap1);
				uint32_t gf = 0; double gm = 0;
				while (video->pop_gpu_done(gf, gm)) prof.gpu_done(gf, gm);
				double ph[IVideoBackend::kGpuPhases];
				while (video->pop_gpu_phases(gf, ph)) prof.gpu_phases(gf, ph);   // (the frame's own GPU times replace "a recent frame's")
			}
			rec.gpu = video ? float(video->last_gpu_ms()) : -1.0f;
			if (video) for (int i = 0; i < 6; i++) rec.gpu_ph[i] = float(video->last_gpu_phase_ms(i));
			rec.cpu = float(m.perf_cpu_ms - cpu0); rec.dsp = float(m.perf_dcs_ms - dcs0); rec.feed = float(m.perf_feed_ms - feed0) - rec.present;
			if (rec.feed < 0) rec.feed = 0;
			rec.quads = uint32_t(m.perf_quads - quads0); rec.draws = uint32_t(m.perf_draws - draws0);
			rec.mode = m.ram_word(0xC8F5);
			rec.flips = uint8_t(std::min<uint64_t>(255, m.page_flips - prof_flips)); prof_flips = m.page_flips;
			rec.clock_q4 = uint8_t(m.clock_q4());
			rec.late = uint8_t(std::min<uint64_t>(255, m.catchup_fails - prof_late)); prof_late = m.catchup_fails;
			rec.audio_ms = float(audio.latency_ms());
			{
				DWM_TIMING_INFO ti{};
				ti.cbSize = sizeof(ti);
				if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod)
				{
					// refreshes since the reported vblank are added, so that the count is the one at the moment the swap returned
					LARGE_INTEGER nowq; QueryPerformanceCounter(&nowq);
					const long long since = nowq.QuadPart - (long long)ti.qpcVBlank;
					const long long per = (long long)ti.qpcRefreshPeriod;
					const long long extra = since >= 0 ? since / per : -((-since + per - 1) / per);   // floor: the reported vblank may lie ahead
					rec.refresh = uint32_t(ti.cRefresh + extra);
					rec.phase = float(double(since - extra * (long long)ti.qpcRefreshPeriod) * 1000.0 / double(qf.QuadPart));
				}
			}
			rec.dt = prof_last > 0 ? float(pp1 - prof_last) : 0.0f;
			rec.other = float(pend - pr0) - rec.msg - rec.input - rec.emu - rec.ffb - rec.wait - rec.present;
			prof_last = pp1;
			if (prof_last > 0 && rec.frame > 0) prof.add(rec);
			if (perf_seconds > 0 && t > perf_seconds) g_quit = true;
		}
		if (ran) fps_n++;
		if (t - fps_t >= 1.0 && !prof.on && fullscreen_now) { fps_n = 0; fps_t = t; }   // no title bar to write to
		if (t - fps_t >= 1.0 && !prof.on)
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

	{
		char title[200];
		std::snprintf(title, sizeof(title), "%s %dx, draw distance %d%%, %s%s", video ? video->name() : "CPU", S.video.internal_scale, S.video.draw_distance,
		              sync_k ? "display sync" : (S.video.vsync ? "vsync" : "no vsync"), bench ? ", bench" : "");
		prof.finish(title);
		if (!prof.csv_path.empty()) { vblank_clock::stop(); vblank_clock::write(prof.csv_path + ".vblank"); }
		stallwatch::stop();
		g_calltrace.close();
	}
	g_menu = nullptr;
	if (video) video->set_overlay(nullptr);
	menu.shutdown();
	controls.ffb_stop();
	outputs.stop();
	audio.stop();
	m.save_nvram(exe_relative(S.nvram));
	if (video) video->shutdown();
	hub.shutdown();
	timeEndPeriod(1);
	return 0;
}
