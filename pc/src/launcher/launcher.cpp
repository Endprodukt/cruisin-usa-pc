#include "launcher.h"

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../machine/cmos.h"
#include "../machine/default_nvram.h"
#include "../outputs/outputs.h"
#include "../video/png_io.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "comdlg32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

enum Page { P_HOME, P_VIDEO, P_AUDIO, P_CONTROLS, P_GAME, P_DIP, P_OUTPUTS, P_NETWORK, P_ABOUT, P_COUNT };
const char *const kPageNames[P_COUNT] = {"Home", "Video", "Audio", "Controls", "Game settings", "DIP Switches", "Outputs", "Network (NOT IMPLEMENTED YET)", "About"};

struct Launcher
{
	Settings &s;
	std::string ini_path;
	InputHub &hub;
	Controls &ctl;

	Page page = P_HOME;
	bool dirty = false;
	bool quit = false, play = false;
	std::string status;

	// game settings (the save file's adjustments)
	std::vector<uint32_t> nv;
	bool nv_loaded = false, nv_dirty = false, nv_from_default = false;

	// controls page
	int ctl_tab = 0;
	int selected_device = 0;
	char ignore_buf[256] = {};

	// capture state
	enum class Cap { None, Key, Pad, Axis } cap = Cap::None;
	std::string cap_action;       // action id, or "steer"/"accel"/"brake"
	bool cap_steer_left = true;
	std::vector<std::string> cap_prompt;
	float cap_live = 0;
	double cap_started = 0;

	Launcher(Settings &st, const std::string &p, InputHub &h, Controls &c) : s(st), ini_path(p), hub(h), ctl(c)
	{
		std::snprintf(ignore_buf, sizeof(ignore_buf), "%s", s.controls.ignore_devices.c_str());
	}
};

void nv_save(struct Launcher &L);

// ---- helpers -------------------------------------------------------------------------------------------

bool edited(Launcher &L, bool changed)
{
	if (changed) L.dirty = true;
	return changed;
}

template <size_t N>
bool combo(Launcher &L, const char *label, int &value, const char *const (&items)[N])
{
	bool ch = false;
	if (ImGui::BeginCombo(label, items[size_t(value)]))
	{
		for (size_t i = 0; i < N; i++)
			if (ImGui::Selectable(items[i], int(i) == value)) { value = int(i); ch = true; }
		ImGui::EndCombo();
	}
	return edited(L, ch);
}

void help(const char *text)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

double now_sec()
{
	return double(GetTickCount64()) / 1000.0;
}

bool vulkan_available()
{
	HMODULE m = LoadLibraryA("vulkan-1.dll");
	if (!m) return false;
	FreeLibrary(m);
	return true;
}

struct MonitorList
{
	std::vector<std::string> names;
};
BOOL CALLBACK monitor_cb(HMONITOR mon, HDC, LPRECT, LPARAM data)
{
	auto *l = reinterpret_cast<MonitorList *>(data);
	MONITORINFOEXA mi{};
	mi.cbSize = sizeof(mi);
	GetMonitorInfoA(mon, &mi);
	char buf[128];
	std::snprintf(buf, sizeof(buf), "Monitor %zu  %ldx%ld%s", l->names.size() + 1, mi.rcMonitor.right - mi.rcMonitor.left,
	              mi.rcMonitor.bottom - mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? "  (primary)" : "");
	l->names.push_back(buf);
	return TRUE;
}

// ---- pages ------------------------------------------------------------------------------------------------

void page_home(Launcher &L)
{
	ImGui::TextWrapped("Cruis'n USA for Windows - the original game code running on a native V-Unit, TMS320C31 and DCS sound "
	                   "implementation. Everything configured here is written to cruisn.ini next to the program.");
	ImGui::Spacing();
	ImGui::SeparatorText("Game");
	char rom[512];
	std::snprintf(rom, sizeof(rom), "%s", L.s.rom.c_str());
	ImGui::SetNextItemWidth(-230);
	if (edited(L, ImGui::InputText("##rom", rom, sizeof(rom)))) L.s.rom = rom;
	ImGui::SameLine();
	if (ImGui::Button("Browse...")) { std::string pth = L.s.rom; if (browse_rom(GetActiveWindow(), pth)) { L.s.rom = pth; L.dirty = true; } }
	ImGui::SameLine();
	ImGui::TextUnformatted("ROM zip");
	help("Path to crusnusa.zip (the MAME ROM set). It is not part of this program. The zip is only read, never modified.");
	if (L.s.rom.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "No ROM set yet: point to your crusnusa.zip to be able to play.");
	else if (GetFileAttributesA(exe_relative(L.s.rom).c_str()) == INVALID_FILE_ATTRIBUTES) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "This file does not exist.");
	static const char *const versions[] = {"4.5", "4.4", "4.1", "4.0", "2.1", "2.0", "1.1"};
	int vi = 0;
	for (int i = 0; i < 7; i++) if (L.s.version == versions[i]) vi = i;
	if (combo(L, "Version", vi, versions)) L.s.version = versions[vi];
	char nv[256];
	std::snprintf(nv, sizeof(nv), "%s", L.s.nvram.c_str());
	ImGui::SetNextItemWidth(-140);
	if (edited(L, ImGui::InputText("Save file (CMOS)", nv, sizeof(nv)))) L.s.nvram = nv;
	help("High scores, audits and operator settings. The control calibration is fixed by design.");
	edited(L, ImGui::Checkbox("Hide the power-up tests (loading picture, about two seconds)", &L.s.fast_boot));
	help("The arcade board tests its ROMs, RAM and sound board at every start and shows the results. On: they run unseen at full speed behind a loading picture. Off: they are shown as on the machine (about 25 seconds).");
	edited(L, ImGui::Checkbox("Show this launcher at startup", &L.s.show_launcher));
	help("Turn off to start the game directly. Hold Shift while starting, or pass --launcher, to open it again.");

}

void page_video(Launcher &L)
{
	VideoSettings &v = L.s.video;
	ImGui::SeparatorText("Renderer");
	static const char *const rend[] = {"CPU (software rasterizer)", "OpenGL 4.5", "Vulkan 1.1"};
	int r = int(v.renderer);
	if (combo(L, "Renderer", r, rend)) v.renderer = Renderer(r);
	help("OpenGL and Vulkan render on the GPU at the internal resolution below. The CPU renderer is the exact reference "
	     "rasterizer, always at the original 512x400.");
	if (v.renderer == Renderer::Vulkan && !vulkan_available()) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "vulkan-1.dll not found: the game falls back to OpenGL.");

	ImGui::SeparatorText("Window");
	static const char *const modes[] = {"Window", "Borderless window", "Fullscreen (borderless, desktop resolution)"};
	int m = int(v.window_mode);
	if (combo(L, "Mode", m, modes)) v.window_mode = WindowMode(m);
	static MonitorList mons;
	mons.names.clear();
	EnumDisplayMonitors(nullptr, nullptr, monitor_cb, reinterpret_cast<LPARAM>(&mons));
	if (!mons.names.empty())
	{
		v.monitor = std::clamp(v.monitor, 0, int(mons.names.size()) - 1);
		if (ImGui::BeginCombo("Monitor", mons.names[size_t(v.monitor)].c_str()))
		{
			for (size_t i = 0; i < mons.names.size(); i++)
				if (ImGui::Selectable(mons.names[i].c_str(), int(i) == v.monitor)) { v.monitor = int(i); L.dirty = true; }
			ImGui::EndCombo();
		}
	}
	ImGui::BeginDisabled(v.window_mode == WindowMode::Fullscreen);
	struct Preset { const char *name; int w, h; };
	static const Preset presets[] = {{"1024 x 768  (4:3)", 1024, 768}, {"1280 x 960  (4:3)", 1280, 960}, {"1600 x 1200 (4:3)", 1600, 1200},
	                                 {"1280 x 720  (16:9)", 1280, 720}, {"1600 x 900  (16:9)", 1600, 900}, {"1920 x 1080 (16:9)", 1920, 1080},
	                                 {"2560 x 1440 (16:9)", 2560, 1440}, {"3840 x 2160 (16:9)", 3840, 2160},
	                                 {"2560 x 1080 (21:9)", 2560, 1080}, {"3440 x 1440 (21:9)", 3440, 1440}};
	char cur[64];
	std::snprintf(cur, sizeof(cur), "%d x %d", v.window_w, v.window_h);
	if (ImGui::BeginCombo("Window resolution", cur))
	{
		for (const Preset &p : presets)
			if (ImGui::Selectable(p.name, p.w == v.window_w && p.h == v.window_h)) { v.window_w = p.w; v.window_h = p.h; L.dirty = true; }
		ImGui::EndCombo();
	}
	ImGui::SetNextItemWidth(120);
	edited(L, ImGui::InputInt("Width", &v.window_w, 0, 0));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120);
	edited(L, ImGui::InputInt("Height", &v.window_h, 0, 0));
	v.window_w = std::clamp(v.window_w, 320, 16384);
	v.window_h = std::clamp(v.window_h, 240, 16384);
	ImGui::EndDisabled();

	ImGui::SeparatorText("Image");
	bool gpu = v.renderer != Renderer::Cpu;
	ImGui::BeginDisabled(!gpu);
	int sc = v.internal_scale;
	ImGui::SetNextItemWidth(300);
	if (edited(L, ImGui::SliderInt("Internal resolution", &sc, 1, 8, "%dx"))) v.internal_scale = sc;
	ImGui::SameLine();
	ImGui::TextDisabled("%d x %d", 512 * v.internal_scale, 400 * v.internal_scale);
	help("Multiple of the arcade's 512x400. Geometry is rendered smoothly at this size; textures stay point-sampled unless filtering is on.");
	edited(L, ImGui::Checkbox("Texture filtering (bilinear)", &v.texture_filter));
	ImGui::EndDisabled();
	static const char *const aspects[] = {"4:3 (original)", "16:9", "21:9", "Stretch to window"};
	int a = int(v.aspect);
	if (combo(L, "Aspect ratio", a, aspects)) v.aspect = AspectMode(a);
	help("Shape of the picture in the window. 16:9 and 21:9 currently stretch the 4:3 image; real widescreen (wider field of view) "
	     "follows with the rendering update, see below.");
	static const char *const aas[] = {"Off", "FXAA light", "FXAA normal", "FXAA strong"};
	int aa = v.aa;
	ImGui::BeginDisabled(!gpu);
	if (combo(L, "Anti-aliasing", aa, aas)) v.aa = aa;
	ImGui::EndDisabled();
	help("Post-process smoothing of jagged edges. The internal resolution above is true supersampling and works together with it. "
	     "F3 cycles the mode in game.");
	edited(L, ImGui::Checkbox("Smooth scaling to the window", &v.smooth_output));
	edited(L, ImGui::Checkbox("Integer scaling", &v.integer_scale));
	edited(L, ImGui::Checkbox("Display sync (smoothest)", &v.display_sync));
	help("The arcade board draws 57.9 pictures per second, which fits no PC display: with VSync a picture is shown twice about twice a second, "
	     "without VSync the picture tears. Display sync computes exactly one game frame per display refresh instead, so every picture is "
	     "shown equally long. The game then runs at the display's pace: 3.6 % faster on a 60 Hz display (the sound follows). Used when the "
	     "display rate is within 6 % of a multiple of 57.9 Hz (60, 120, 175 Hz ...); otherwise the exact arcade speed is kept. Turns VSync on.");
	ImGui::BeginDisabled(v.display_sync);
	edited(L, ImGui::Checkbox("VSync", &v.vsync));
	ImGui::EndDisabled();

	ImGui::SeparatorText("Shadows");
	ImGui::BeginDisabled(!gpu);
	static const char *const shadows[] = {"Original (dithered raster quads)", "Modern (soft shadows)", "Off"};
	int sh = int(v.shadows);
	if (combo(L, "Shadow style", sh, shadows)) v.shadows = ShadowMode(sh);
	help("The arcade draws car shadows as dithered pixel patterns. Modern replaces them with a smooth blended shadow at the internal resolution. F4 cycles the style in game.");
	ImGui::BeginDisabled(v.shadows != ShadowMode::Modern);
	ImGui::SetNextItemWidth(300);
	edited(L, ImGui::SliderInt("Shadow darkness", &v.shadow_strength, 0, 100, "%d%%"));
	ImGui::SetNextItemWidth(300);
	edited(L, ImGui::SliderInt("Shadow softness", &v.shadow_softness, 0, 100, "%d"));
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	if (!gpu) ImGui::TextDisabled("Modern shadows need the OpenGL or Vulkan renderer.");

	ImGui::SeparatorText("Draw distance");
	{
		static const int steps[] = {25, 50, 75, 100, 150, 200, 300, 400};
		static const char *const names[] = {"25% (fastest, objects vanish early)", "50%", "75%", "100% (original)", "150%", "200%", "300%", "400% (maximum)"};
		int cur = 3;
		for (int i = 0; i < 8; i++) if (steps[i] == v.draw_distance) cur = i;
		if (combo(L, "Draw distance", cur, names)) v.draw_distance = steps[cur];
		help("Below 100% far objects are dropped earlier (faster). Above 100% detailed models are kept at greater distances and traffic stays "
		     "active further out (slower on weak hardware). Takes effect at the next start of the game.");
	}

	ImGui::SeparatorText("Widescreen");
	ImGui::BeginDisabled(!gpu);
	edited(L, ImGui::Checkbox("Real widescreen (show more of the world at the sides)", &v.widescreen_hack));
	ImGui::EndDisabled();
	help("With the aspect ratio set to 16:9 or 21:9 the 3D view is widened instead of stretched: the game's culling is extended and the "
	     "image gets extra room left and right (about +86 px at 16:9, 192 px at 21:9 on the arcade's 512). "
	     "Takes effect at the next start of the game. Needs the OpenGL or Vulkan renderer.");
	ImGui::BeginDisabled(!gpu);
	static const char *const huds[] = {"Centre (4:3)", "Screen edges", "25% towards the edges", "50% towards the edges", "75% towards the edges"};
	int h = int(v.hud);
	if (combo(L, "HUD placement", h, huds)) v.hud = HudPlacement(h);
	ImGui::EndDisabled();
	help("Where the race HUD sits on a wide picture: in the 4:3 centre, or spread towards the left and right edge (left items left, "
	     "right items right, time and rank stay centred). Only has an effect with real widescreen. Takes effect at the next start.");

	ImGui::SeparatorText("Textures");
	ImGui::BeginDisabled(!gpu);
	edited(L, ImGui::Checkbox("Export textures while playing", &v.export_textures));
	help("Writes every texture that gets drawn to textures/dump next to the program as a 256 x 256 PNG (tex_<hash>.png; textures that look the same share one file). "
	     "Upscale them with any tool and keep the names. The export is switched here only (there is no key for it in the game).");
	edited(L, ImGui::Checkbox("Export every palette variant (car colours etc.)", &v.export_variants));
	help("Off: one file (idx_<hash>.png) per distinct texture page, shown in the first colours seen; a replacement of it is used for all "
	     "colour variants. On: one file (tex_<hash>.png) for every colour variant, which allows per-colour replacements but exports many more files.");
	edited(L, ImGui::Checkbox("Use replacement textures", &v.replace_textures));
	help("PNGs with the same names in textures/replace (square, 256 to 2048 px; all of them are brought to the largest size) are drawn instead "
	     "of the original textures. Takes effect at the next start of the game.");
	if (ImGui::Button("Open texture folder"))
	{
		std::string d = exe_relative("textures");
		CreateDirectoryA(d.c_str(), nullptr);
		CreateDirectoryA((d + "/dump").c_str(), nullptr);
		CreateDirectoryA((d + "/replace").c_str(), nullptr);
		ShellExecuteA(nullptr, "open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::Separator();
	if (ImGui::Button("Save settings##video", ImVec2(180, 34)))
	{
		L.dirty = false;
		L.status = L.s.save(L.ini_path) ? "Saved." : "Saving failed.";
	}
	if (!L.status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", L.status.c_str()); }
}

void page_audio(Launcher &L)
{
	AudioSettings &a = L.s.audio;
	edited(L, ImGui::Checkbox("Sound enabled", &a.enabled));
	ImGui::SetNextItemWidth(300);
	edited(L, ImGui::SliderInt("Volume", &a.volume, 0, 200, "%d%%"));
	ImGui::SetNextItemWidth(300);
	edited(L, ImGui::SliderInt("Buffered audio (ms)", &a.latency_ms, 20, 300));
	help("How much sound is kept queued. Lower = less delay but more risk of crackle on a slow system.");
	ImGui::Spacing();
	ImGui::TextWrapped("The DCS sound board is emulated instruction by instruction; its stream is resampled to 48 kHz for the Windows mixer.");
}

// ---- controls ------------------------------------------------------------------------------------------------

void end_capture(Launcher &L) { L.cap = Launcher::Cap::None; L.cap_action.clear(); }

void start_capture(Launcher &L, Launcher::Cap kind, const std::string &action, bool steer_left = true)
{
	L.cap = kind;
	L.cap_action = action;
	L.cap_steer_left = steer_left;
	L.cap_started = now_sec();
	L.ctl.begin_capture();
	L.cap_live = 0;
	if (kind == Launcher::Cap::Key) L.ctl.poll_capture_key();     // arm the edge detector
}

void capture_popup(Launcher &L)
{
	if (L.cap == Launcher::Cap::None) return;
	ImGui::OpenPopup("Bind input");
	ImVec2 c = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(520, 0));
	if (!ImGui::BeginPopupModal("Bind input", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) return;

	bool analog = L.cap == Launcher::Cap::Axis;
	const char *what = L.cap_action.c_str();
	if (analog)
	{
		if (L.cap_action == "steer") ImGui::TextWrapped("Turn the wheel (or move the stick) all the way to the %s.", L.cap_steer_left ? "LEFT" : "RIGHT");
		else if (L.cap_action == "accel") ImGui::TextWrapped("Press the ACCELERATOR pedal (or trigger) fully.");
		else ImGui::TextWrapped("Press the BRAKE pedal (or trigger) fully.");
		ImGui::TextDisabled("Every attached controller is listened to. The first axis that moves clearly is bound; its direction is learnt.");
	}
	else if (L.cap == Launcher::Cap::Key)
		ImGui::Text("Press the key for \"%s\".", what);
	else
		ImGui::Text("Press a button (or hat direction) on any controller for \"%s\".", what);
	ImGui::TextDisabled("Backspace clears the binding, Esc cancels.");
	ImGui::Spacing();
	ImGui::ProgressBar(std::clamp(float((now_sec() - L.cap_started) / 15.0), 0.0f, 1.0f), ImVec2(-1, 6), "");

	bool done = false;
	if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) done = true;
	else if ((GetAsyncKeyState(VK_BACK) & 0x8000) && now_sec() - L.cap_started > 0.3)
	{
		if (analog)
		{
			if (L.cap_action == "steer") L.s.controls.steer = AxisBinding{};
			else if (L.cap_action == "accel") L.s.controls.accel = AxisBinding{};
			else L.s.controls.brake = AxisBinding{};
		}
		else if (L.cap == Launcher::Cap::Key) L.s.controls.key[L.cap_action] = "NONE";
		else L.s.controls.pad[L.cap_action] = "";
		L.dirty = true;
		done = true;
	}
	else if (L.cap == Launcher::Cap::Key && now_sec() - L.cap_started > 0.25)
	{
		std::string k = L.ctl.poll_capture_key();
		if (!k.empty()) { L.s.controls.key[L.cap_action] = k; L.dirty = true; done = true; }
	}
	else if (L.cap == Launcher::Cap::Pad && now_sec() - L.cap_started > 0.25)
	{
		std::string b, label;
		if (L.ctl.poll_capture_button(b, label))
		{
			// a controller button belongs to one action only: take it away from the previous owner
			for (auto &kv : L.s.controls.pad)
				if (kv.first != L.cap_action && kv.second == b) { kv.second.clear(); }
			L.s.controls.pad[L.cap_action] = b;
			L.dirty = true;
			done = true;
		}
	}
	else if (analog)
	{
		AxisRole role = L.cap_action == "steer" ? AxisRole::Steer : (L.cap_action == "accel" ? AxisRole::Accel : AxisRole::Brake);
		AxisBinding ab;
		if (L.ctl.poll_capture_axis(role, L.cap_steer_left, ab, L.cap_live))
		{
			(L.cap_action == "steer" ? L.s.controls.steer : (L.cap_action == "accel" ? L.s.controls.accel : L.s.controls.brake)) = ab;
			L.dirty = true;
			done = true;
		}
	}
	if (now_sec() - L.cap_started > 15.0) done = true;
	if (done) { ImGui::CloseCurrentPopup(); end_capture(L); }
	ImGui::EndPopup();
}

void controls_devices(Launcher &L)
{
	ImGui::TextWrapped("Every attached controller is read at once, alongside the keyboard. A composite device that enumerates twice "
	                   "(a Fanatec base, for example) is shown once.");
	if (ImGui::Button("Refresh devices")) L.hub.refresh();
	ImGui::SameLine();
	edited(L, ImGui::Checkbox("Keep duplicate entries", &L.s.controls.allow_duplicate_devices));
	ImGui::SetNextItemWidth(260);
	if (edited(L, ImGui::InputText("Ignore (names, comma separated)", L.ignore_buf, sizeof(L.ignore_buf)))) L.s.controls.ignore_devices = L.ignore_buf;
	ImGui::SameLine();
	if (ImGui::Button("Apply")) { L.hub.shutdown(); L.hub.init(GetActiveWindow(), L.s.controls.ignore_devices, L.s.controls.allow_duplicate_devices); }

	if (ImGui::BeginTable("devs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Device", 0, 3.0f);
		ImGui::TableSetupColumn("API");
		ImGui::TableSetupColumn("Axes");
		ImGui::TableSetupColumn("Buttons");
		ImGui::TableSetupColumn("Motor");
		ImGui::TableSetupColumn("Merged");
		ImGui::TableHeadersRow();
		for (int i = 0; i < L.hub.count(); i++)
		{
			const DeviceInfo &d = L.hub.info(i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(d.name.c_str(), L.selected_device == i, ImGuiSelectableFlags_SpanAllColumns)) L.selected_device = i;
			ImGui::TableNextColumn(); ImGui::TextUnformatted(d.backend == Backend::XInput ? "XInput" : "DirectInput");
			ImGui::TableNextColumn(); ImGui::Text("%zu", d.axes.size());
			ImGui::TableNextColumn(); ImGui::Text("%d%s", d.buttons, d.hat ? " + hat" : "");
			ImGui::TableNextColumn(); ImGui::TextUnformatted(d.ffb ? (d.backend == Backend::XInput ? "rumble" : "force feedback") : "-");
			ImGui::TableNextColumn(); if (d.duplicates_merged) ImGui::Text("%d", d.duplicates_merged); else ImGui::TextDisabled("-");
		}
		ImGui::EndTable();
	}
	if (L.hub.count() == 0) ImGui::TextDisabled("No controller found. The keyboard always works.");

	const DeviceState *st = L.ctl.device_state(L.selected_device);
	if (st && L.selected_device < L.hub.count())
	{
		const DeviceInfo &d = L.hub.info(L.selected_device);
		ImGui::SeparatorText("Live input");
		for (size_t a = 0; a < d.axes.size() && a < st->axis.size(); a++)
		{
			char lbl[32];
			std::snprintf(lbl, sizeof(lbl), "%s %+.2f", d.axes[a].c_str(), st->axis[a]);
			ImGui::ProgressBar((st->axis[a] + 1.0f) * 0.5f, ImVec2(260, 0), lbl);
		}
		std::string pressed;
		for (size_t b = 0; b < st->button.size(); b++) if (st->button[b]) pressed += std::to_string(b + 1) + " ";
		ImGui::Text("Buttons down: %s", pressed.empty() ? "-" : pressed.c_str());
	}
}

void controls_bindings(Launcher &L)
{
	ControlSettings &c = L.s.controls;
	ImGui::SeparatorText("Wheel, pedals and sticks");
	ImGui::TextWrapped("Move the control when asked: the axis and its direction are learnt. A pedal also learns whether it rests at one end "
	                   "of its axis or at the centre (a combined-pedal axis, where it covers one half). Unbound controls fall back to every "
	                   "device's default.");
	struct Row { const char *id, *label; AxisBinding *b; float live; };
	Row rows[] = {{"steer", "Steering", &c.steer, L.ctl.steer_value()}, {"accel", "Accelerator", &c.accel, L.ctl.accel_value() * 2 - 1},
	              {"brake", "Brake", &c.brake, L.ctl.brake_value() * 2 - 1}};
	for (Row &r : rows)
	{
		ImGui::PushID(r.id);
		ImGui::Text("%-12s", r.label);
		ImGui::SameLine(120);
		if (ImGui::Button((L.ctl.axis_label(*r.b) + "##a").c_str(), ImVec2(420, 0)))
		{
			bool left = true;
			start_capture(L, Launcher::Cap::Axis, r.id, left);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Clear")) { *r.b = AxisBinding{}; L.dirty = true; }
		ImGui::SameLine();
		ImGui::ProgressBar((r.live + 1.0f) * 0.5f, ImVec2(140, 0), "");
		ImGui::PopID();
	}

	ImGui::SeparatorText("Keys and buttons");
	ImGui::TextWrapped("Bind a keyboard key and/or a controller button per action. Controller buttons are tied to the device that "
	                   "pressed them (name + button), so a button on the wheel and the same number on a shifter never mix.");
	static const char *const shifters[] = {"Buttons (sticky)", "Buttons (toggling)", "Sequential", "H-Pattern"};
	int sm = int(c.shifter);
	ImGui::SetNextItemWidth(260);
	if (combo(L, "Shifter type", sm, shifters)) c.shifter = ShifterMode(sm);
	help("Sticky: a gear button keeps that gear until another (or Neutral) is pressed.\n"
	     "Toggling: pressing the engaged gear again returns to neutral.\n"
	     "Sequential: shift up / down buttons or paddles.\n"
	     "H-Pattern: for a real shifter - a gear is engaged only while its button is held, neutral otherwise.");
	ImGui::SameLine();
	ImGui::TextDisabled("  current gear: %s", L.ctl.gear() == 0 ? "N" : std::to_string(L.ctl.gear()).c_str());
	auto visible = [&](const std::string &id) {
		if (id == "neutral") return c.shifter == ShifterMode::Sticky;
		if (id == "shift_up" || id == "shift_down") return c.shifter == ShifterMode::Sequential;
		if (id.rfind("gear", 0) == 0) return c.shifter != ShifterMode::Sequential;
		return true;
	};
	const char *group = "";
	if (ImGui::BeginTable("bind", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Action", 0, 2.0f);
		ImGui::TableSetupColumn("Keyboard", 0, 2.0f);
		ImGui::TableSetupColumn("Controller", 0, 3.0f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 10.0f);
		ImGui::TableHeadersRow();
		for (const ActionInfo &a : action_table())
		{
			if (!visible(a.id)) continue;
			if (std::string(a.group) != group)
			{
				group = a.group;
				ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", a.group);
			}
			ImGui::PushID(a.id);
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(a.label);
			ImGui::TableNextColumn();
			std::string key = key_display_name(c.key[a.id]);
			// a key shared with another action (MAME itself shares Ctrl and Alt) is flagged, not refused
			bool shared = false;
			std::string with;
			if (mame_key_to_vk(c.key[a.id]))
				for (const ActionInfo &o : action_table())
					if (std::string(o.id) != a.id && c.key[o.id] == c.key[a.id]) { shared = true; with = o.label; break; }
			if (ImGui::Button((key + "##k").c_str(), ImVec2(150, 0))) start_capture(L, Launcher::Cap::Key, a.id);
			if (shared) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "also: %s", with.c_str()); }
			ImGui::TableNextColumn();
			std::string pl = L.ctl.pad_label(c.pad[a.id]);
			bool present = c.pad[a.id].empty();
			if (!present)
			{
				size_t bar = c.pad[a.id].rfind('|');
				present = bar != std::string::npos && L.ctl.binding_device_present(c.pad[a.id].substr(0, bar == 0 ? 0 : bar - 1));
			}
			if (ImGui::Button((pl + "##p").c_str(), ImVec2(260, 0))) start_capture(L, Launcher::Cap::Pad, a.id);
			if (!present) { ImGui::SameLine(); ImGui::TextDisabled("(not attached)"); }
			ImGui::TableNextColumn();
			if (L.ctl.action_active(a.id)) ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "*");
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::Spacing();
	if (ImGui::Button("Reset keyboard + buttons to defaults (MAME)"))
	{
		AxisBinding st = c.steer, ac = c.accel, br = c.brake;
		L.s.reset_controls_to_defaults();
		L.s.controls.steer = st; L.s.controls.accel = ac; L.s.controls.brake = br;
		L.dirty = true;
	}
}

void controls_analog(Launcher &L)
{
	ControlSettings &c = L.s.controls;
	ImGui::TextWrapped("Keyboard steering and pedals use MAME's analog-input model: each frame a held key adds 'key delta' to an accumulator, "
	                   "the reported value is the accumulator times 'sensitivity', and a released key returns by 'centre delta' per frame. "
	                   "Defaults are the Cruis'n USA driver's (sensitivity 25, key delta 20).");
	struct Item { const char *label; AnalogKeys *k; };
	Item items[] = {{"Steering", &c.steer_keys}, {"Accelerator", &c.accel_keys}, {"Brake", &c.brake_keys}};
	for (Item &it : items)
	{
		ImGui::PushID(it.label);
		ImGui::SeparatorText(it.label);
		ImGui::SetNextItemWidth(260);
		edited(L, ImGui::SliderInt("Sensitivity", &it.k->sensitivity, 1, 400, "%d%%"));
		ImGui::SetNextItemWidth(260);
		edited(L, ImGui::SliderInt("Key delta", &it.k->key_delta, 1, 200));
		ImGui::SetNextItemWidth(260);
		edited(L, ImGui::SliderInt("Centre delta", &it.k->center_delta, 0, 200));
		ImGui::PopID();
	}
	if (ImGui::Button("MAME defaults"))
	{
		c.steer_keys = c.accel_keys = c.brake_keys = AnalogKeys{25, 20, 20};
		L.dirty = true;
	}
	ImGui::SeparatorText("Wheel / stick axis");
	ImGui::SetNextItemWidth(260);
	edited(L, ImGui::SliderInt("Steering deadzone", &c.steer_deadzone, 0, 30, "%d%%"));
	ImGui::SetNextItemWidth(260);
	edited(L, ImGui::SliderInt("Steering range (full lock at)", &c.steer_range, 20, 200, "%d%%"));
	help("Percent of the wheel's travel that gives full lock. Lower it if the wheel base rotation is larger than you want to use.");
	ImGui::SetNextItemWidth(260);
	edited(L, ImGui::SliderInt("Pedal deadzone", &c.pedal_deadzone, 0, 30, "%d%%"));
	edited(L, ImGui::Checkbox("Read keyboard and controllers when the window is in the background", &c.background_input));
	ImGui::SeparatorText("Monitor");
	ImGui::ProgressBar((L.ctl.steer_value() + 1) * 0.5f, ImVec2(300, 0), "steer");
	ImGui::ProgressBar(L.ctl.accel_value(), ImVec2(300, 0), "accelerator");
	ImGui::ProgressBar(L.ctl.brake_value(), ImVec2(300, 0), "brake");
}

void controls_ffb(Launcher &L)
{
	FfbSettings &f = L.s.ffb;
	ImGui::TextWrapped("The game computes the force (a position servo with damping, up to +/-126) and the host carries it to the wheel as a "
	                   "DirectInput constant force along the steering axis. The axis is taken from the steering binding, or the wheel's x axis.");
	edited(L, ImGui::Checkbox("Force feedback", &f.enabled));
	static const char *const modes[] = {"Vanilla (the game's own force only)", "Modern (adds effects from the game's car state)"};
	int fm = int(f.mode);
	if (combo(L, "Mode", fm, modes)) f.mode = FfbMode(fm);
	help("Vanilla is the arcade's position servo. Modern keeps that force and adds surface, kerb, bump, collision, spin-out, jump and "
	     "tyre effects read from the game's physics while you drive. Takes effect at the next start of the game.");
	std::string cur = f.device.empty() ? "Auto (steering device, else first wheel with a motor)" : f.device;
	if (ImGui::BeginCombo("Device", cur.c_str()))
	{
		if (ImGui::Selectable("Auto (steering device, else first wheel with a motor)", f.device.empty())) { f.device.clear(); L.dirty = true; }
		for (int i = 0; i < L.hub.count(); i++)
			if (L.hub.info(i).backend == Backend::DInput && L.hub.info(i).ffb)
				if (ImGui::Selectable(L.hub.info(i).name.c_str(), f.device == L.hub.info(i).name)) { f.device = L.hub.info(i).name; L.dirty = true; }
		ImGui::EndCombo();
	}
	int tgt = L.ctl.ffb_target_device();
	ImGui::TextDisabled("Will use: %s", tgt >= 0 ? L.hub.info(tgt).name.c_str() : "no wheel with a motor found");
	ImGui::SetNextItemWidth(300);
	edited(L, ImGui::SliderInt("Strength", &f.strength, 0, 200, "%d%%"));
	help("Overall strength of everything sent to the wheel: the game's force, and in Modern mode every effect below. The wheel's own driver strength still applies on top.");
	ImGui::SetNextItemWidth(300);
	edited(L, ImGui::SliderInt("Device gain", &f.device_gain, 0, 100, "%d%%"));
	edited(L, ImGui::Checkbox("Invert direction", &f.invert));
	ImGui::SameLine();
	if (ImGui::Button("Detect direction"))
	{
		bool inv = f.invert;
		std::string msg;
		if (L.ctl.ffb_detect_direction(GetActiveWindow(), inv, msg)) { f.invert = inv; L.dirty = true; }
		L.status = msg;
	}
	help("Briefly pushes the wheel and watches which way the steering axis moves, then sets the direction. Keep your hands off the wheel.");
	help("Turn on if the wheel pulls away from the road instead of towards it.");
	if (ImGui::Button("Test wheel (short pulse)", ImVec2(220, 0)))
		L.status = (tgt >= 0 && L.hub.test_pulse(tgt)) ? "Test pulse sent to " + L.hub.info(tgt).name : "No wheel with a motor found.";
	if (f.mode == FfbMode::Modern)
	{
		ImGui::SeparatorText("Modern effects");
		if (ImGui::CollapsingHeader("Effect strengths", ImGuiTreeNodeFlags_DefaultOpen))
		{
			auto fx = [&](const char *label, int &v, const char *tip) {
				ImGui::SetNextItemWidth(300);
				edited(L, ImGui::SliderInt(label, &v, 0, 200, "%d%%"));
				help(tip);
			};
			fx("Self-aligning torque", f.fx_aligning, "The core of the model: the front tyres' force from the game's physics. Resists you in corners, "
			                                       "gets light when the tyres give up, and throws the wheel into counter-steer in slides, spins and after hits.");
			fx("Self-centring", f.fx_centering, "A light centring force that grows with speed (the aligning torque already centres the wheel while the car grips).");
			fx("Menu effects", f.fx_menu, "Strength of the arcade's own force outside a race (selection screens, results). In a race the force comes from the car state only; the attract mode has no force.");
			fx("Impact kick", f.fx_impact, "Directional kick when the car's direction changes abruptly: a hit from the left jerks the wheel left, "
			                                   "a car spinning right throws the wheel to the left.");
			fx("Off-road surface", f.fx_offroad, "Grass, dirt and gravel beside the road: a coarse rumble that gets stronger and faster with speed.");
			fx("Roadside objects", f.fx_object, "Running into signs, posts, lamps, bushes, barrels, barriers, cones and animals: a knock and a push from the side the object stood on.");
			fx("Standstill resistance", f.fx_standstill, "Weight of the wheel while the car stands or crawls: a smooth resistance against turning (the wheel's own damper effect) and a soft pull to the centre. Fades out by about 20 mph.");
			fx("Kerb tug", f.fx_kerb, "A sideways tug when a wheel drops off the edge of the road.");
			fx("Bumps", f.fx_bump, "Bumps, road seams and rails.");
			fx("Collisions", f.fx_collision, "Other cars hitting the car from any side (also from behind), walls, trees and poles.");
			fx("Spin-out", f.fx_spin, "The wheel is thrown to one side while the car spins out, and torn from side to side while it somersaults.");
			fx("Landing", f.fx_landing, "A thump when the car touches down after a jump.");
			fx("Engine", f.fx_engine, "Vibration that follows the engine revs.");
			ImGui::SetNextItemWidth(300);
			edited(L, ImGui::SliderInt("Engine pulse at idle", &f.engine_ms_idle, 4, 200, "%d ms"));
			help("Time between two engine pulses at idle. Larger = coarser, slower throb; smaller = finer buzz.");
			ImGui::SetNextItemWidth(300);
			edited(L, ImGui::SliderInt("Engine pulse at full revs", &f.engine_ms_max, 4, 200, "%d ms"));
			help("Time between two engine pulses at the rev limit. The pulse moves between the two values with the revs.");
			fx("Tyre rattle", f.fx_skid, "Vibration while the tyres slide.");
			fx("Air time lightness", f.fx_air, "The wheel goes light while the car is airborne.");
			fx("Understeer lightness", f.fx_understeer, "The wheel lightens when the tyres lose grip.");
			if (ImGui::Button("Reset effect strengths"))
			{
				f.reset_effects(); L.dirty = true;
			}
		}
	}
	ImGui::SeparatorText("Gamepad rumble");
	edited(L, ImGui::Checkbox("Rumble on XInput pads", &f.rumble));
	ImGui::SetNextItemWidth(300);
	edited(L, ImGui::SliderInt("Rumble strength", &f.rumble_strength, 0, 200, "%d%%"));
	ImGui::Spacing();
	if (ImGui::Button("Test rumble (short pulse)", ImVec2(220, 0)))
	{
		// the gamepads only: the wheel has its own test button above
		int sent = 0; std::string names;
		for (int i = 0; i < L.hub.count(); i++)
			if (L.hub.info(i).backend == Backend::XInput && L.hub.info(i).ffb && L.hub.test_pulse(i)) { names = L.hub.info(i).name; sent = 1; break; }
		L.status = sent ? "Rumble pulse sent to " + names : "No XInput pad connected.";
	}
	if (!L.status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", L.status.c_str()); }
	L.hub.tick();
}

void page_controls(Launcher &L)
{
	if (ImGui::BeginTabBar("ctl"))
	{
		if (ImGui::BeginTabItem("Bindings")) { controls_bindings(L); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Analog")) { controls_analog(L); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Force feedback")) { controls_ffb(L); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Devices")) { controls_devices(L); ImGui::EndTabItem(); }
		ImGui::EndTabBar();
	}
	capture_popup(L);
}

// ---- game settings (service-menu adjustments, edited in the save file) ---------------------------------------------

void nv_load(Launcher &L)
{
	std::string path = exe_relative(L.s.nvram);
	L.nv_from_default = false;
	if (!cmos::load_file(path, L.nv) || !cmos::checksum_ok(L.nv))
	{
		L.nv.assign(cmos::WORDS, 0xffffffffu);
		for (const NvPair &p : kDefaultNvram) if (p.index < L.nv.size()) L.nv[p.index] = p.value;
		cmos::set(L.nv, cmos::ADJ_FREE_PLAY, 1);   // same default as MidVUnit::load_default_nvram
		L.nv_from_default = true;
	}
	L.nv_loaded = true;
	L.nv_dirty = false;
}

void nv_save(Launcher &L)
{
	if (!L.nv_loaded || !L.nv_dirty) return;
	cmos::fix_checksum(L.nv);
	L.status = cmos::save_file(exe_relative(L.s.nvram), L.nv) ? "Game settings written to the save file." : "Could not write the save file.";
	L.nv_dirty = false;
	L.nv_from_default = false;
}

void page_game(Launcher &L)
{
	if (!L.nv_loaded) nv_load(L);
	ImGui::SeparatorText("Opponents");
	{
		static const int steps[] = {0, 25, 50, 100, 150};
		static const char *const names[] = {"None (no rubber banding)", "25%", "Reduced (50%)", "Original (100%)", "Stronger (150%)"};
		int cur = 3;
		for (int i = 0; i < 5; i++) if (steps[i] == L.s.rubberband) cur = i;
		if (combo(L, "Rubber banding", cur, names)) L.s.rubberband = steps[cur];
		help("The game gives the opponents more engine power when they are behind you (at least +20 %, up to +40 %, more the longer you lead) and "
		     "less when they are ahead. This scales that boost; the random power surges and the driving AI stay as they are. Takes effect at the next start.");
	}
	ImGui::SeparatorText("Operator adjustments");
	ImGui::TextWrapped("The operator adjustments the game's service menu offers, edited directly in the save file so you never need the "
	                   "service menu. They take effect at the next start. The control calibration is fixed and not listed.");
	if (L.nv_from_default) ImGui::TextDisabled("No save file yet (or it was rejected): showing the factory values; the file is created when you apply.");
	ImGui::Spacing();
	for (const cmos::AdjInfo &a : cmos::adjustments())
	{
		ImGui::PushID(a.index);
		int v = int(cmos::get(L.nv, a.index));
		v = std::clamp(v, a.min, a.max);
		int nvv = v;
		ImGui::SetNextItemWidth(260);
		switch (a.kind)
		{
		case cmos::AdjInfo::OnOff:
		{
			bool on = v != 0;
			if (ImGui::Checkbox(a.label, &on)) nvv = on ? 1 : 0;
			break;
		}
		case cmos::AdjInfo::Speed:
		{
			static const char *const units[] = {"MPH", "KPH"};
			int u = v;
			if (ImGui::BeginCombo(a.label, units[std::clamp(u, 0, 1)]))
			{
				for (int i = 0; i < 2; i++) if (ImGui::Selectable(units[i], i == u)) nvv = i;
				ImGui::EndCombo();
			}
			break;
		}
		case cmos::AdjInfo::StartTime:
		{
			char fmt[24];
			std::snprintf(fmt, sizeof(fmt), "%d s", 60 + 5 * v);
			if (ImGui::SliderInt(a.label, &nvv, a.min, a.max, fmt)) {}
			break;
		}
		default:
			if (a.step > 1)
			{
				int steps = nvv / a.step;
				if (ImGui::SliderInt(a.label, &steps, a.min / a.step, a.max / a.step, "%d x 1000")) nvv = steps * a.step;
			}
			else ImGui::SliderInt(a.label, &nvv, a.min, a.max);
			break;
		}
		if (a.help && *a.help) help(a.help);
		if (nvv != v) { cmos::set(L.nv, a.index, uint32_t(nvv)); L.nv_dirty = true; L.dirty = true; }
		ImGui::PopID();
	}
	ImGui::Spacing();
	if (ImGui::Button("Restore factory values"))
		for (const cmos::AdjInfo &a : cmos::adjustments()) { cmos::set(L.nv, a.index, uint32_t(a.def)); L.nv_dirty = true; L.dirty = true; }
	ImGui::SameLine();
	if (ImGui::Button("Apply to save file")) { nv_save(L); }
	ImGui::SameLine();
	ImGui::TextDisabled("(also written when you press Play or Save settings)");
	if (!L.status.empty()) ImGui::TextDisabled("%s", L.status.c_str());
}

// ---- DIP switches ---------------------------------------------------------------------------------------------

struct Coinage { uint16_t value; const char *name; };
const Coinage kCoinage[] = {
	{0xfe00, "USA-1"}, {0xfa00, "USA-3"}, {0xfc00, "USA-7"}, {0xf800, "USA-8"}, {0xf600, "Norway-1"}, {0xee00, "Australia-1"},
	{0xea00, "Australia-2"}, {0xec00, "Australia-3"}, {0xe800, "Australia-4"}, {0xde00, "Swiss-1"}, {0xda00, "Swiss-2"},
	{0xdc00, "Swiss-3"}, {0xce00, "Belgium-1"}, {0xca00, "Belgium-2"}, {0xcc00, "Belgium-3"}, {0xbe00, "French-1"},
	{0xba00, "French-2"}, {0xbc00, "French-3"}, {0xb800, "French-4"}, {0xb600, "Hungary-1"}, {0xae00, "Taiwan-1"},
	{0xaa00, "Taiwan-2"}, {0xac00, "Taiwan-3"}, {0x9e00, "UK-1"}, {0x9a00, "UK-2"}, {0x9c00, "UK-3"}, {0x8e00, "Finland-1"},
	{0x7e00, "German-1"}, {0x7a00, "German-2"}, {0x7c00, "German-3"}, {0x7800, "German-4"}, {0x7600, "Denmark-1"},
	{0x6e00, "Japan-1"}, {0x6a00, "Japan-2"}, {0x6c00, "Japan-3"}, {0x5e00, "Italy-1"}, {0x5a00, "Italy-2"},
	{0x5c00, "Italy-3"}, {0x4e00, "Sweden-1"}, {0x3e00, "Canada-1"}, {0x3a00, "Canada-2"}, {0x3c00, "Canada-3"},
	{0x3600, "General-1"}, {0x3200, "General-3"}, {0x3400, "General-5"}, {0x3000, "General-7"}, {0x2e00, "Austria-1"},
	{0x2a00, "Austria-2"}, {0x2c00, "Austria-3"}, {0x2800, "Austria-4"}, {0x1e00, "Spain-1"}, {0x1a00, "Spain-2"},
	{0x1c00, "Spain-3"}, {0x1800, "Spain-4"}, {0x0e00, "Netherland-1"},
};

bool dip_bit(Launcher &L, const char *label, uint16_t bit, const char *on, const char *off, const char *tip)
{
	// the switch reads 0 when "On": the bit set means the named 'off' state
	bool isset = (L.s.dsw & bit) != 0;
	int idx = isset ? 0 : 1;       // 0 = off-name, 1 = on-name
	const char *items[2] = {off, on};
	bool ch = false;
	ImGui::SetNextItemWidth(220);
	if (ImGui::BeginCombo(label, items[idx]))
	{
		for (int i = 0; i < 2; i++)
			if (ImGui::Selectable(items[i], i == idx))
			{
				if (i == 0) L.s.dsw |= bit; else L.s.dsw &= uint16_t(~bit);
				ch = true;
			}
		ImGui::EndCombo();
	}
	if (tip && *tip) help(tip);
	return edited(L, ch);
}

void page_dip(Launcher &L)
{
	ImGui::TextWrapped("The cabinet's DIP switch blocks SW2 (operator) and SW3 (coinage), as listed by the game's driver. "
	                   "Changes apply at the next start.");
	ImGui::SeparatorText("SW2");
	dip_bit(L, "Link status", 0x0001, "Slave", "Master", "Dual-cabinet link role (link play is not implemented yet).");
	dip_bit(L, "Linking", 0x0004, "On", "Off", "");
	dip_bit(L, "Freeze", 0x0010, "On", "Off", "");
	dip_bit(L, "Cabinet", 0x0020, "Upright", "Sitdown", "Sitdown enables the seat / motion related screens.");
	dip_bit(L, "Motion", 0x0040, "Off", "On", "Enable motion platform (the game then tests the motion hardware at boot).");
	dip_bit(L, "Service mode", 0x0080, "Service", "Normal", "");
	ImGui::SeparatorText("SW3");
	dip_bit(L, "Coin counters", 0x0100, "2", "1", "");
	uint16_t coin = L.s.dsw & 0xfe00;
	const char *curname = "Custom";
	for (const Coinage &c : kCoinage) if (c.value == coin) curname = c.name;
	ImGui::SetNextItemWidth(220);
	if (ImGui::BeginCombo("Coinage", curname))
	{
		for (const Coinage &c : kCoinage)
			if (ImGui::Selectable(c.name, c.value == coin)) { L.s.dsw = uint16_t((L.s.dsw & ~0xfe00) | c.value); L.dirty = true; }
		ImGui::EndCombo();
	}
	ImGui::Spacing();
	ImGui::Text("DSW = 0x%04X", L.s.dsw);
	if (ImGui::Button("Defaults")) { L.s.dsw = 0xf9bf; L.dirty = true; }
}

// ---- outputs / network -------------------------------------------------------------------------------------------

void page_outputs(Launcher &L)
{
	OutputSettings &o = L.s.outputs;
	ImGui::TextWrapped("Cabinet lamps and the wheel motor for lamp tools, sent the way MAME sends them.");
	static const char *const modes[] = {"Off", "Windows (MAMEOutput window - MameHooker)", "Network (TCP - Hook Of The Reaper)"};
	int m = int(o.mode);
	if (combo(L, "Output", m, modes)) o.mode = OutputMode(m);
	ImGui::BeginDisabled(o.mode != OutputMode::Network);
	ImGui::SetNextItemWidth(160);
	edited(L, ImGui::InputInt("TCP port", &o.port, 0, 0));
	o.port = std::clamp(o.port, 1, 65535);
	ImGui::EndDisabled();
	char gn[64];
	std::snprintf(gn, sizeof(gn), "%s", o.game_name.c_str());
	ImGui::SetNextItemWidth(260);
	if (edited(L, ImGui::InputText("Game name", gn, sizeof(gn)))) o.game_name = gn;
	help("What the tools see as the running game. MAME's name for this game is crusnusa.");
	ImGui::SeparatorText("Outputs (named like the game's lamp test)");
	size_t n = 0;
	const char *const *names = Outputs::names(n);
	for (size_t i = 0; i < n; i++) ImGui::BulletText("%s", names[i]);
	ImGui::TextDisabled("Lamps are 0/1. WHEEL MOTOR is the signed force byte the game sends (-126..126).");
}

void page_network(Launcher &L)
{
	ImGui::TextWrapped("Link play (two cabinets, the game's own 'Head 2 Head' mode) is part of the game and is configured through the DIP "
	                   "switches; the network transport for it is not implemented yet.");
	ImGui::BeginDisabled(true);
	bool off = true;
	ImGui::Checkbox("Enable link play", &off);
	ImGui::EndDisabled();
	ImGui::Spacing();
	ImGui::TextDisabled("Output network settings (lamps over TCP) are on the Outputs page.");
	(void)L;
}

void page_about(Launcher &)
{
	auto head = [](const char *t) { ImGui::Spacing(); ImGui::SeparatorText(t); };
	auto line = [](const char *what, const char *text) {
		ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1), "%s", what);
		ImGui::Indent();
		ImGui::TextWrapped("%s", text);
		ImGui::Unindent();
	};
	ImGui::TextWrapped("Cruis'n USA for Windows");
	ImGui::TextDisabled("PC port by Endprodukt, 2026   -   github.com/Endprodukt/cruisin-usa-pc");
	ImGui::Spacing();
	ImGui::TextWrapped("The original game program runs unchanged on a native implementation of the arcade board (Midway V-Unit: TMS320C31 "
	                   "CPU, DCS sound board), with a GPU renderer, widescreen, force feedback and this launcher around it. The game's ROMs "
	                   "are not included: you need your own crusnusa.zip.");

	head("The game");
	line("Cruis'n USA (1994)", "Created by Eugene Jarvis and the team at TV Games, Inc., built and published by Midway Manufacturing Company "
	                           "under licence from Nintendo. All rights to the game, its name, artwork and ROMs belong to their owners. "
	                           "This port is a fan project and is not affiliated with or endorsed by them.");
	line("Game source code", "\"COPYRIGHT (C) 1994 BY TV GAMES, INC.\" - the original TMS320C31 assembly source, preserved at "
	                         "github.com/historicalsource/cruisin-usa. It was the reference for every fix, the widescreen and draw "
	                         "distance changes and the force feedback; this repository is a fork of it.");
	line("Launcher artwork", "Original Cruis'n USA logo and cabinet artwork (signed Youssi), (C) Midway / Nintendo.");

	head("Made possible by");
	line("MAME - mamedev.org", "The V-Unit driver (midvunit), the TMS3203x and ADSP-21xx CPU cores and the DCS audio emulation by Aaron Giles "
	                           "and the MAME contributors are the foundation of the hardware side of this port. Without two decades of "
	                           "their documentation of this board there would be no port. Licence: BSD-3-Clause.");
	line("historicalsource", "For preserving and publishing the game's source code.");

	head("Libraries and licences");
	line("MAME cores and hardware behaviour", "BSD-3-Clause. Copyright (c) Aaron Giles and the MAME team.");
	line("Dear ImGui 1.91", "MIT licence. Copyright (c) 2014-2025 Omar Cornut. (this launcher)");
	line("miniz", "MIT licence. Copyright (c) 2013-2014 RAD Game Tools and Valve Software, 2010-2014 Rich Geldreich and Tenacious Software LLC. (zip and PNG)");
	line("volk", "MIT licence. Copyright (c) 2018-2025 Arseny Kapoulkine. (Vulkan loader)");
	line("Vulkan headers", "Apache-2.0 OR MIT. Copyright (c) The Khronos Group Inc.");
	line("Windows APIs", "OpenGL, Vulkan, WASAPI, DirectInput 8 and XInput as shipped with Windows and the graphics driver.");
	ImGui::Spacing();
	ImGui::TextDisabled("The full licence texts are in LICENSES.txt next to the program.");
}

// the launcher's pictures are resources of the program (assets/cruisn.rc)
GLuint load_picture(const char *name, int &w, int &h)
{
	HRSRC res = FindResourceA(nullptr, name, MAKEINTRESOURCEA(10));   // RT_RCDATA
	if (!res) return 0;
	HGLOBAL mem = LoadResource(nullptr, res);
	const uint8_t *data = mem ? static_cast<const uint8_t *>(LockResource(mem)) : nullptr;
	std::vector<uint8_t> rgba;
	if (!data || !png_decode_rgba(data, SizeofResource(nullptr, res), w, h, rgba)) return 0;
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);   // GL_CLAMP_TO_EDGE
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	return tex;
}

// ---- window ----------------------------------------------------------------------------------------------------------

LRESULT CALLBACK launcher_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
	switch (msg)
	{
	case WM_CLOSE: PostQuitMessage(0); return 0;
	case WM_DESTROY: return 0;
	}
	return DefWindowProcA(h, msg, wp, lp);
}

void apply_style()
{
	ImGuiStyle &st = ImGui::GetStyle();
	ImGui::StyleColorsDark(&st);
	st.WindowRounding = 0; st.FrameRounding = 4; st.GrabRounding = 4; st.TabRounding = 4; st.ChildRounding = 4;
	st.FramePadding = ImVec2(8, 5); st.ItemSpacing = ImVec2(8, 7);
	ImVec4 *c = st.Colors;
	c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 0);       // the root window: the picture behind it shows
	c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.13f, 0.80f);    // the panels: see-through
	c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.97f);
	c[ImGuiCol_Border] = ImVec4(0.45f, 0.55f, 0.75f, 0.35f);
	c[ImGuiCol_Button] = ImVec4(0.20f, 0.24f, 0.32f, 1);
	c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.36f, 0.52f, 1);
	c[ImGuiCol_ButtonActive] = ImVec4(0.33f, 0.46f, 0.70f, 1);
	c[ImGuiCol_Header] = ImVec4(0.22f, 0.30f, 0.45f, 1);
	c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.38f, 0.56f, 1);
	c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.17f, 0.21f, 0.90f);
	c[ImGuiCol_TableRowBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.55f);
	c[ImGuiCol_TableRowBgAlt] = ImVec4(0.16f, 0.18f, 0.22f, 0.55f);
	c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.75f, 1.0f, 1);
	c[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.65f, 0.95f, 1);
	c[ImGuiCol_PlotHistogram] = ImVec4(0.30f, 0.60f, 0.95f, 1);
}

} // namespace

bool browse_rom(void *owner_hwnd, std::string &path)
{
	char file[MAX_PATH] = {};
	std::snprintf(file, sizeof(file), "%s", path.c_str());
	for (char &ch : file) if (ch == '/') ch = '\\';
	OPENFILENAMEA ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = static_cast<HWND>(owner_hwnd);
	ofn.lpstrFilter = "Cruis'n USA ROM set (crusnusa.zip)\0crusnusa.zip\0Zip files (*.zip)\0*.zip\0All files\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = sizeof(file);
	ofn.lpstrTitle = "Where is your crusnusa.zip?";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameA(&ofn))
	{
		// a path that does not exist any more makes the dialog refuse to open: try again without it
		if (CommDlgExtendedError() == 0 || file[0] == 0) return false;
		file[0] = 0;
		if (!GetOpenFileNameA(&ofn)) return false;
	}
	path = file;
	return true;
}

bool create_default_save(const std::string &path)
{
	if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
	std::vector<uint32_t> nv(cmos::WORDS, 0xffffffffu);
	for (const NvPair &p : kDefaultNvram) if (p.index < nv.size()) nv[p.index] = p.value;
	cmos::set(nv, cmos::ADJ_FREE_PLAY, 1);   // same default as MidVUnit::load_default_nvram
	cmos::fix_checksum(nv);
	return cmos::save_file(path, nv);
}

LauncherResult launcher_run(Settings &settings, const std::string &ini_path, InputHub &hub, Controls &controls)
{
	SetProcessDPIAware();
	HINSTANCE hi = GetModuleHandle(nullptr);
	WNDCLASSA wc{};
	wc.lpfnWndProc = launcher_proc;
	wc.hInstance = hi;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(1));
	wc.lpszClassName = "CruisnLauncher";
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);
	UINT dpi = 96;
	if (HDC dc = GetDC(nullptr)) { dpi = UINT(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(nullptr, dc); }
	float scale = float(dpi) / 96.0f;
	RECT r{0, 0, LONG(1120 * scale), LONG(760 * scale)};
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowA("CruisnLauncher", "Cruis'n USA - Launcher", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
	                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
	HDC dc = GetDC(hwnd);
	PIXELFORMATDESCRIPTOR pfd{};
	pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
	SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
	HGLRC rc = wglCreateContext(dc);
	wglMakeCurrent(dc, rc);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	if (FILE *f = std::fopen("C:/Windows/Fonts/segoeui.ttf", "rb")) { std::fclose(f); io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 16.0f * scale); }
	else { ImFontConfig fc; fc.SizePixels = 15.0f * scale; io.Fonts->AddFontDefault(&fc); }
	apply_style();
	ImGui::GetStyle().ScaleAllSizes(scale);
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplOpenGL3_Init("#version 130");

	hub.shutdown();
	hub.init(hwnd, settings.controls.ignore_devices, settings.controls.allow_duplicate_devices);

	int logo_w = 0, logo_h = 0, bg_w = 0, bg_h = 0;
	const GLuint logo = load_picture("LOGO", logo_w, logo_h), backdrop = load_picture("BACKGROUND", bg_w, bg_h);

	Launcher L(settings, ini_path, hub, controls);
	if (const char *pg = std::getenv("CRUISN_PAGE")) L.page = Page(std::clamp(std::atoi(pg), 0, int(P_COUNT) - 1));      // testing aid
	if (const char *tb = std::getenv("CRUISN_TAB")) L.ctl_tab = std::atoi(tb);
	MachineInputs scratch;
	bool running = true;
	while (running && !L.quit && !L.play)
	{
		MSG msg;
		while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT) { running = false; L.quit = true; }
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		controls.update(scratch, GetForegroundWindow() == hwnd);
		hub.tick();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		ImGuiViewport *vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->Pos);
		ImGui::SetNextWindowSize(vp->Size);
		ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

		if (backdrop)
		{
			// the picture covers the window (cropped, never stretched)
			const float wa = vp->Size.x / vp->Size.y, pa = float(bg_w) / float(bg_h);
			ImVec2 uv0(0, 0), uv1(1, 1);
			if (wa > pa) { const float k = pa / wa; uv0.y = (1 - k) * 0.5f; uv1.y = 1 - uv0.y; }
			else { const float k = wa / pa; uv0.x = (1 - k) * 0.5f; uv1.x = 1 - uv0.x; }
			ImGui::GetBackgroundDrawList()->AddImage(ImTextureID(intptr_t(backdrop)), vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), uv0, uv1);
		}

		ImGui::BeginChild("nav", ImVec2(250 * scale, 0), true);
		if (logo)
		{
			const float lw = ImGui::GetContentRegionAvail().x * 0.86f, lh = lw * float(logo_h) / float(logo_w);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - lw) * 0.5f);
			ImGui::Image(ImTextureID(intptr_t(logo)), ImVec2(lw, lh));
		}
		else ImGui::TextDisabled("CRUIS'N USA");
		ImGui::Separator();
		for (int i = 0; i < P_COUNT; i++)
			if (ImGui::Selectable(kPageNames[i], L.page == i, 0, ImVec2(0, 30 * scale))) L.page = Page(i);
		// PLAY / SAVE / QUIT at the bottom
		const float bh = 36 * scale, gap = ImGui::GetStyle().ItemSpacing.y;
		ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 3 * (bh + gap) - 2 * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().WindowPadding.y));
		if (L.dirty) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "unsaved changes");
		else if (!L.status.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", L.status.c_str()); ImGui::PopTextWrapPos(); }
		else ImGui::TextUnformatted("");
		ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 3 * (bh + gap) - ImGui::GetStyle().WindowPadding.y);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.26f, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.58f, 0.33f, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.70f, 0.40f, 1));
		if (ImGui::Button("PLAY", ImVec2(-1, bh)))
		{
			if (L.s.rom.empty() || GetFileAttributesA(exe_relative(L.s.rom).c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				std::string pth = L.s.rom;
				if (browse_rom(hwnd, pth)) { L.s.rom = pth; L.dirty = true; L.play = true; }
				else { L.page = P_HOME; L.status = "Set the ROM zip first."; }
			}
			else L.play = true;
		}
		ImGui::PopStyleColor(3);
		if (ImGui::Button("SAVE", ImVec2(-1, bh)))
		{
			L.dirty = false;
			nv_save(L);
			L.status = L.s.save(L.ini_path) ? "Saved." : "Saving failed.";
		}
		if (ImGui::Button("QUIT", ImVec2(-1, bh))) L.quit = true;
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("page", ImVec2(0, 0), true);
		switch (L.page)
		{
		case P_HOME: page_home(L); break;
		case P_VIDEO: page_video(L); break;
		case P_AUDIO: page_audio(L); break;
		case P_CONTROLS: page_controls(L); break;
		case P_GAME: page_game(L); break;
		case P_DIP: page_dip(L); break;
		case P_OUTPUTS: page_outputs(L); break;
		case P_NETWORK: page_network(L); break;
		case P_ABOUT: page_about(L); break;
		default: break;
		}
		ImGui::EndChild();
		ImGui::End();

		ImGui::Render();
		RECT cr;
		GetClientRect(hwnd, &cr);
		glViewport(0, 0, cr.right, cr.bottom);
		glClearColor(0.09f, 0.10f, 0.12f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SwapBuffers(dc);
		Sleep(8);
	}

	nv_save(L);
	if (L.dirty || L.play) L.s.save(L.ini_path);
	if (logo) glDeleteTextures(1, &logo);
	if (backdrop) glDeleteTextures(1, &backdrop);
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	wglMakeCurrent(nullptr, nullptr);
	wglDeleteContext(rc);
	ReleaseDC(hwnd, dc);
	DestroyWindow(hwnd);
	UnregisterClassA("CruisnLauncher", hi);
	return L.play ? LauncherResult::Play : LauncherResult::Quit;
}
