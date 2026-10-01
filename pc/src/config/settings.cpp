#include <cmath>
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <windows.h>

namespace {

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a])) a++;
	while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
	return s.substr(a, b - a);
}

std::string lower(std::string s)
{
	for (char &c : s) c = char(std::tolower((unsigned char)c));
	return s;
}

using Section = std::map<std::string, std::string>;
using Ini = std::map<std::string, Section>;

// values may carry inline ';' / '#' comments when whitespace precedes the marker
Ini parse_ini(const std::string &text)
{
	Ini ini;
	std::istringstream in(text);
	std::string line, section;
	while (std::getline(in, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == ';' || line[0] == '#') continue;
		if (line[0] == '[')
		{
			size_t e = line.find(']');
			section = lower(trim(line.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = lower(trim(line.substr(0, eq)));
		std::string val = line.substr(eq + 1);
		for (size_t i = 1; i < val.size(); i++)
			if ((val[i] == ';' || val[i] == '#') && std::isspace((unsigned char)val[i - 1])) { val.resize(i); break; }
		ini[section][key] = trim(val);
	}
	return ini;
}

struct Reader
{
	const Ini &ini;
	const std::string *get(const char *sec, const std::string &key) const
	{
		auto s = ini.find(sec);
		if (s == ini.end()) return nullptr;
		auto k = s->second.find(key);
		return k == s->second.end() ? nullptr : &k->second;
	}
	void str(const char *sec, const char *key, std::string &out) const { if (auto v = get(sec, key)) out = *v; }
	void boolean(const char *sec, const char *key, bool &out) const
	{
		if (auto v = get(sec, key)) { std::string l = lower(*v); out = (l == "1" || l == "true" || l == "yes" || l == "on"); }
	}
	void integer(const char *sec, const char *key, int &out, int lo, int hi) const
	{
		if (auto v = get(sec, key)) { char *e; long n = std::strtol(v->c_str(), &e, 0); if (e != v->c_str()) out = int(std::clamp<long>(n, lo, hi)); }
	}
	template <typename E, size_t N>
	void choice(const char *sec, const char *key, E &out, const char *const (&names)[N]) const
	{
		if (auto v = get(sec, key))
		{
			std::string l = lower(*v);
			for (size_t i = 0; i < N; i++) if (l == names[i]) out = E(i);
		}
	}
};

const char *const kRenderer[] = {"cpu", "opengl", "vulkan"};
const char *const kWindow[] = {"window", "borderless", "fullscreen"};
const char *const kAspect[] = {"4:3", "16:9", "21:9", "stretch"};
const char *const kHud[] = {"centre", "edges", "25", "50", "75"};
const char *const kShadow[] = {"original", "modern", "off"};
const char *const kFfbMode[] = {"vanilla", "modern"};
const char *const kOutput[] = {"off", "windows", "network"};
const char *const kShifter[] = {"sticky", "toggling", "sequential", "h-pattern"};

AxisBinding parse_axis(const std::string &text)
{
	AxisBinding b;
	size_t bar = text.rfind('|');
	if (bar == std::string::npos) return b;
	b.device = trim(text.substr(0, bar));
	std::istringstream words(text.substr(bar + 1));
	std::string w;
	bool first = true;
	while (words >> w)
	{
		if (first) { b.axis = lower(w); first = false; }
		else if (lower(w) == "inverted") b.inverted = true;
		else if (lower(w) == "half") b.half = true;
	}
	if (b.axis.empty()) b.device.clear();
	return b;
}

std::string format_axis(const AxisBinding &b)
{
	if (!b.bound()) return "";
	return b.device + " | " + b.axis + (b.inverted ? " inverted" : "") + (b.half ? " half" : "");
}

} // namespace

const char *to_string(Renderer r) { return kRenderer[int(r)]; }
const char *to_string(WindowMode m) { return kWindow[int(m)]; }
const char *to_string(AspectMode a) { return kAspect[int(a)]; }
const char *to_string(HudPlacement h) { return kHud[int(h)]; }
const char *to_string(ShadowMode s) { return kShadow[int(s)]; }

int wide_margin_for(const VideoSettings &v)
{
	if (!v.widescreen_hack) return 0;
	// the 512 x 400 picture is 4:3 on the arcade monitor (pixels 1.04 wider than tall): full width for aspect A is 384 * A
	double a = v.aspect == AspectMode::Wide169 ? 16.0 / 9.0 : v.aspect == AspectMode::Wide219 ? 21.0 / 9.0 : 0.0;
	if (a <= 0.0) return 0;
	return int(std::ceil((384.0 * a - 512.0) / 2.0));
}
const char *to_string(FfbMode m) { return kFfbMode[int(m)]; }
const char *to_string(OutputMode o) { return kOutput[int(o)]; }
const char *to_string(ShifterMode m) { return kShifter[int(m)]; }

std::string exe_relative(const std::string &path)
{
	if (path.empty() || (path.size() > 1 && path[1] == ':') || path[0] == '/' || path[0] == '\\') return path;
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string d = exe;
	return d.substr(0, d.find_last_of("\\/") + 1) + path;
}

const std::vector<ActionInfo> &action_table()
{
	static const std::vector<ActionInfo> t = {
#define ACTION(id, label, key, pad, group) {#id, label, key, pad, #group},
#include "../input/actions.def"
#undef ACTION
	};
	return t;
}

Settings::Settings() { reset_controls_to_defaults(); }

void Settings::reset_controls_to_defaults()
{
	controls = ControlSettings{};
	for (const ActionInfo &a : action_table())
	{
		controls.key[a.id] = a.def_key;
		controls.pad[a.id] = a.def_pad;
	}
}

bool Settings::load(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::stringstream ss;
	ss << f.rdbuf();
	Ini ini = parse_ini(ss.str());
	Reader r{ini};

	r.str("game", "rom", rom);
	r.str("game", "version", version);
	r.str("game", "nvram", nvram);
	r.boolean("game", "show_launcher", show_launcher);
	r.boolean("game", "fast_boot", fast_boot);
	r.integer("game", "rubberband", rubberband, 0, 300);
	r.integer("game", "dsp_thread", dsp_thread, -1, 1);

	r.choice("video", "renderer", video.renderer, kRenderer);
	r.choice("video", "window_mode", video.window_mode, kWindow);
	r.integer("video", "window_width", video.window_w, 320, 16384);
	r.integer("video", "window_height", video.window_h, 240, 16384);
	r.integer("video", "monitor", video.monitor, 0, 15);
	r.integer("video", "internal_scale", video.internal_scale, 1, 8);
	r.choice("video", "aspect", video.aspect, kAspect);
	r.boolean("video", "integer_scale", video.integer_scale);
	r.boolean("video", "vsync", video.vsync);
	r.boolean("video", "texture_filter", video.texture_filter);
	r.boolean("video", "smooth_output", video.smooth_output);
	r.integer("video", "aa", video.aa, 0, 3);
	r.boolean("video", "export_textures", video.export_textures);
	r.boolean("video", "export_variants", video.export_variants);
	r.boolean("video", "replace_textures", video.replace_textures);
	r.boolean("video", "widescreen_hack", video.widescreen_hack);
	r.choice("video", "hud", video.hud, kHud);
	r.integer("video", "draw_distance", video.draw_distance, 25, 400);
	r.choice("video", "shadows", video.shadows, kShadow);
	r.integer("video", "shadow_strength", video.shadow_strength, 0, 100);
	r.integer("video", "shadow_softness", video.shadow_softness, 0, 100);

	r.boolean("audio", "enabled", audio.enabled);
	r.integer("audio", "volume", audio.volume, 0, 200);
	r.integer("audio", "latency_ms", audio.latency_ms, 20, 400);

	for (const ActionInfo &a : action_table())
	{
		r.str("digital", a.id, controls.key[a.id]);
		r.str("gamepad", a.id, controls.pad[a.id]);
	}
	if (auto v = r.get("analog", "steer_axis")) controls.steer = parse_axis(*v);
	if (auto v = r.get("analog", "accel_axis")) controls.accel = parse_axis(*v);
	if (auto v = r.get("analog", "brake_axis")) controls.brake = parse_axis(*v);
	r.integer("analog", "steer_deadzone", controls.steer_deadzone, 0, 50);
	r.integer("analog", "steer_range", controls.steer_range, 10, 200);
	r.integer("analog", "pedal_deadzone", controls.pedal_deadzone, 0, 50);
	struct { const char *p; AnalogKeys *k; } ak[] = {{"steer", &controls.steer_keys}, {"accel", &controls.accel_keys}, {"brake", &controls.brake_keys}};
	for (auto &e : ak)
	{
		r.integer("keyboard_analog", (std::string(e.p) + "_sensitivity").c_str(), e.k->sensitivity, 1, 400);
		r.integer("keyboard_analog", (std::string(e.p) + "_key_delta").c_str(), e.k->key_delta, 1, 400);
		r.integer("keyboard_analog", (std::string(e.p) + "_center_delta").c_str(), e.k->center_delta, 0, 400);
	}
	r.choice("controls", "shifter", controls.shifter, kShifter);
	r.boolean("controls", "background_input", controls.background_input);
	r.boolean("controls", "allow_duplicate_devices", controls.allow_duplicate_devices);
	r.str("controls", "ignore_devices", controls.ignore_devices);

	r.boolean("ffb", "enabled", ffb.enabled);
	r.str("ffb", "device", ffb.device);
	r.integer("ffb", "strength", ffb.strength, 0, 200);
	r.integer("ffb", "device_gain", ffb.device_gain, 0, 100);
	r.boolean("ffb", "invert", ffb.invert);
	r.boolean("ffb", "rumble", ffb.rumble);
	r.integer("ffb", "rumble_strength", ffb.rumble_strength, 0, 200);
	r.choice("ffb", "mode", ffb.mode, kFfbMode);
	r.integer("ffb_modern", "master", ffb.fx_master, 0, 200);
	r.integer("ffb_modern", "aligning", ffb.fx_aligning, 0, 200);
	r.integer("ffb_modern", "centering", ffb.fx_centering, 0, 200);
	r.integer("ffb_modern", "menu", ffb.fx_menu, 0, 200);
	r.integer("ffb_modern", "impact", ffb.fx_impact, 0, 200);
	r.integer("ffb_modern", "surface", ffb.fx_surface, 0, 200);
	r.integer("ffb_modern", "kerb", ffb.fx_kerb, 0, 200);
	r.integer("ffb_modern", "bump", ffb.fx_bump, 0, 200);
	r.integer("ffb_modern", "collision", ffb.fx_collision, 0, 200);
	r.integer("ffb_modern", "spin", ffb.fx_spin, 0, 200);
	r.integer("ffb_modern", "landing", ffb.fx_landing, 0, 200);
	r.integer("ffb_modern", "engine", ffb.fx_engine, 0, 200);
	r.integer("ffb_modern", "skid", ffb.fx_skid, 0, 200);
	r.integer("ffb_modern", "air", ffb.fx_air, 0, 200);
	r.integer("ffb_modern", "understeer", ffb.fx_understeer, 0, 200);

	r.choice("outputs", "mode", outputs.mode, kOutput);
	r.integer("outputs", "port", outputs.port, 1, 65535);
	r.str("outputs", "game_name", outputs.game_name);

	int d = dsw;
	r.integer("dip", "switches", d, 0, 0xffff);
	dsw = uint16_t(d);
	return true;
}

bool Settings::save(const std::string &path) const
{
	std::ostringstream o;
	auto b = [](bool v) { return v ? "true" : "false"; };
	o << "; Cruis'n USA PC -- written by the launcher. Every key is optional; defaults apply.\n\n";
	o << "[game]\nrom = " << rom << "\nversion = " << version << "\nnvram = " << nvram
	  << "\nshow_launcher = " << b(show_launcher) << "\nfast_boot = " << b(fast_boot) << "\n; opponents' rubber band (catch-up boost) in percent of the original: 100 original, 50 reduced, 0 none\nrubberband = " << rubberband	  << "\n; sound DSP emulated on its own thread (same sound, less time per frame): -1 auto (4+ CPU threads), 0 off, 1 on\ndsp_thread = " << dsp_thread << "\n\n";

	o << "[video]\n; renderer: cpu | opengl | vulkan      window_mode: window | borderless | fullscreen\n"
	  << "renderer = " << to_string(video.renderer) << "\nwindow_mode = " << to_string(video.window_mode)
	  << "\nwindow_width = " << video.window_w << "\nwindow_height = " << video.window_h << "\nmonitor = " << video.monitor
	  << "\n; internal_scale 1..8 x the arcade's 512x400\ninternal_scale = " << video.internal_scale
	  << "\n; aspect: 4:3 | 16:9 | 21:9 | stretch\naspect = " << to_string(video.aspect)
	  << "\ninteger_scale = " << b(video.integer_scale) << "\nvsync = " << b(video.vsync)
	  << "\n; aa: 0 off | 1..3 FXAA light/normal/strong (post filter; internal_scale above 1 is supersampling)\naa = " << video.aa << "\n; textures: export_textures writes every drawn texture to textures/dump (tex_<hash>.png); files with the same\n; name in textures/replace are drawn instead (any square size up to 2048)\nexport_textures = " << b(video.export_textures) << "\nexport_variants = " << b(video.export_variants) << "\nreplace_textures = " << b(video.replace_textures)
	  << "\ntexture_filter = " << b(video.texture_filter) << "\nsmooth_output = " << b(video.smooth_output)
	  << "\n; reserved for the widescreen / rendering stage\nwidescreen_hack = " << b(video.widescreen_hack)
	  << "\nhud = " << to_string(video.hud) << "\ndraw_distance = " << video.draw_distance
	  << "\n; shadows: original (the arcade's dithered quads) | modern (soft blended) | off\nshadows = " << to_string(video.shadows)
	  << "\nshadow_strength = " << video.shadow_strength << "\nshadow_softness = " << video.shadow_softness << "\n\n";

	o << "[audio]\nenabled = " << b(audio.enabled) << "\nvolume = " << audio.volume << "\nlatency_ms = " << audio.latency_ms << "\n\n";

	o << "[controls]\n; shifter: sticky | toggling | sequential | h-pattern (MAME's shifter types)\nshifter = " << to_string(controls.shifter) << "\nbackground_input = " << b(controls.background_input) << "\nallow_duplicate_devices = "
	  << b(controls.allow_duplicate_devices) << "\nignore_devices = " << controls.ignore_devices << "\n\n";

	o << "[digital]\n; MAME key names (LEFT, LCONTROL, SPACE, F2, ...); NONE = unbound\n";
	for (const ActionInfo &a : action_table())
	{
		auto it = controls.key.find(a.id);
		o << a.id << " = " << (it != controls.key.end() && !it->second.empty() ? it->second : "NONE") << "\n";
	}
	o << "\n[gamepad]\n; <device> | <button n>   or   <device> | pov up|right|down|left   (device = product name, XInput = any Xbox pad)\n";
	for (const ActionInfo &a : action_table())
	{
		auto it = controls.pad.find(a.id);
		o << a.id << " = " << (it != controls.pad.end() ? it->second : "") << "\n";
	}
	o << "\n[analog]\n; <device> | <axis>[ inverted][ half]   written by the launcher's Bind\n"
	  << "steer_axis = " << format_axis(controls.steer) << "\naccel_axis = " << format_axis(controls.accel)
	  << "\nbrake_axis = " << format_axis(controls.brake) << "\nsteer_deadzone = " << controls.steer_deadzone
	  << "\nsteer_range = " << controls.steer_range << "\npedal_deadzone = " << controls.pedal_deadzone << "\n\n";

	o << "[keyboard_analog]\n; MAME analog-input semantics for keyboard steering and pedals\n";
	struct { const char *p; const AnalogKeys *k; } ak[] = {{"steer", &controls.steer_keys}, {"accel", &controls.accel_keys}, {"brake", &controls.brake_keys}};
	for (auto &e : ak)
		o << e.p << "_sensitivity = " << e.k->sensitivity << "\n" << e.p << "_key_delta = " << e.k->key_delta
		  << "\n" << e.p << "_center_delta = " << e.k->center_delta << "\n";

	o << "\n[ffb]\nenabled = " << b(ffb.enabled) << "\ndevice = " << ffb.device << "\nstrength = " << ffb.strength
	  << "\ndevice_gain = " << ffb.device_gain << "\ninvert = " << b(ffb.invert) << "\nrumble = " << b(ffb.rumble)
	  << "\nrumble_strength = " << ffb.rumble_strength
	  << "\n; mode: vanilla (the game's own force only) | modern (adds effects from the game's car state, see [ffb_modern])"
	  << "\nmode = " << to_string(ffb.mode) << "\n\n"
	  << "[ffb_modern]\n; strength of each effect in percent of its default, 0 = off\nmaster = " << ffb.fx_master << "\naligning = " << ffb.fx_aligning << "\ncentering = " << ffb.fx_centering << "\narcade = " << ffb.fx_menu << "\nimpact = " << ffb.fx_impact << "\nsurface = " << ffb.fx_surface
	  << "\nkerb = " << ffb.fx_kerb << "\nbump = " << ffb.fx_bump << "\ncollision = " << ffb.fx_collision << "\nspin = " << ffb.fx_spin
	  << "\nlanding = " << ffb.fx_landing << "\nengine = " << ffb.fx_engine << "\nskid = " << ffb.fx_skid << "\nair = " << ffb.fx_air
	  << "\nundersteer = " << ffb.fx_understeer << "\n\n";

	o << "[outputs]\n; mode: off | windows (MAMEOutput window, MameHooker) | network (TCP, Hook Of The Reaper)\nmode = "
	  << to_string(outputs.mode) << "\nport = " << outputs.port << "\ngame_name = " << outputs.game_name << "\n\n";

	char dsw_hex[16];
	std::snprintf(dsw_hex, sizeof(dsw_hex), "0x%04X", dsw);
	o << "[dip]\nswitches = " << dsw_hex << "\n";

	// atomic replace: write a temp file, keep a .bak of the previous one
	std::string tmp = path + ".tmp", bak = path + ".bak";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f << o.str();
		if (!f) return false;
	}
	std::remove(bak.c_str());
	std::rename(path.c_str(), bak.c_str());
	return std::rename(tmp.c_str(), path.c_str()) == 0;
}
