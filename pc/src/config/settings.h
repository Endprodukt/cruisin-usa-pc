// All user settings: one ini file (cruisn.ini) next to the executable, edited by the launcher.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum class Renderer { Cpu, OpenGL, Vulkan };
enum class WindowMode { Window, Borderless, Fullscreen };
enum class AspectMode { Native43, Wide169, Wide219, Stretch };
enum class HudPlacement { Centre, Edges, Quarter25, Half50, Quarter75 };
enum class ShadowMode { Original, Modern, Off };
enum class FfbMode { Vanilla, Modern };
enum class OutputMode { Off, Windows, Network };
// MAME's "Shifter Type": buttons keep the last gear (sticky) or toggle it, two buttons step up/down
// (sequential), or the gear is only engaged while its button is held (H-pattern, neutral otherwise)
enum class ShifterMode { Sticky, Toggling, Sequential, HPattern };

struct VideoSettings
{
	Renderer renderer = Renderer::OpenGL;
	WindowMode window_mode = WindowMode::Window;
	int window_w = 1280, window_h = 960;     // client size in window / borderless mode
	int monitor = 0;                          // which monitor for borderless / fullscreen (0 = primary)
	int internal_scale = 2;                   // 1..8 x the arcade's 512x400
	AspectMode aspect = AspectMode::Native43;
	bool integer_scale = false;
	bool vsync = true;
	bool texture_filter = false;              // bilinear texture filtering on the GPU
	bool export_textures = false;
	bool export_variants = false;             // one file per palette variant (e.g. every car colour) instead of one per page             // write every drawn texture to textures/dump
	bool replace_textures = true;             // draw textures from textures/replace when a matching file exists
	int aa = 0;                               // 0 off, 1..3 FXAA light/normal/strong
	bool smooth_output = true;                // linear scaling of the final image

	// reserved for the widescreen / rendering stage (stored now, applied when implemented)
	bool widescreen_hack = true;              // with a 16:9 / 21:9 aspect: show more of the world at the sides instead of stretching
	HudPlacement hud = HudPlacement::Centre;
	int draw_distance = 100;                  // percent of the original
	ShadowMode shadows = ShadowMode::Modern;
	int shadow_strength = 55;                 // percent darkness of modern shadows
	int shadow_softness = 30;                 // penumbra radius in tenths of an arcade pixel
};

struct AudioSettings
{
	bool enabled = true;
	int volume = 100;       // percent
	int latency_ms = 60;    // target buffered audio
};

// <device> | <axis>[ inverted][ half]; empty device = unbound (fall back to every device's default)
struct AxisBinding
{
	std::string device, axis;
	bool inverted = false, half = false;
	bool bound() const { return !device.empty() && !axis.empty(); }
};

// MAME analog-input semantics for the keyboard increments (see MAME's analog_field)
struct AnalogKeys
{
	int sensitivity = 25;      // percent
	int key_delta = 20;        // port units added per frame while a key is held
	int center_delta = 20;     // port units returned per frame when released
};

struct ControlSettings
{
	std::map<std::string, std::string> key;    // action id -> MAME key name
	std::map<std::string, std::string> pad;    // action id -> "<device> | <n>" / "<device> | pov <dir>"
	AxisBinding steer, accel, brake;
	AnalogKeys steer_keys{25, 20, 20}, accel_keys{25, 20, 20}, brake_keys{25, 20, 20};
	int steer_deadzone = 3;    // percent
	int steer_range = 100;     // percent of the axis travel that maps to full lock
	int pedal_deadzone = 2;
	ShifterMode shifter = ShifterMode::Sticky;
	bool background_input = false;
	bool allow_duplicate_devices = false;
	std::string ignore_devices = "vJoy";
};

struct FfbSettings
{
	bool enabled = true;
	std::string device;        // empty = the device steering is bound to, else the first wheel with a motor
	int strength = 100;        // percent of the game's force (0..200)
	int device_gain = 100;     // percent, DirectInput device gain
	bool invert = false;
	bool rumble = true;        // XInput pads
	int rumble_strength = 100;
	FfbMode mode = FfbMode::Modern;   // vanilla: only the force the game computes; modern: plus effects from the game's car state
	// modern effect strengths, percent of their default (0 = off)
	int fx_master = 100, fx_surface = 100, fx_kerb = 70, fx_bump = 100, fx_collision = 100, fx_spin = 100;
	int fx_landing = 100, fx_engine = 25, fx_skid = 100, fx_air = 100, fx_understeer = 100;
	int fx_aligning = 100, fx_centering = 35, fx_menu = 100, fx_impact = 100;
};

struct OutputSettings
{
	OutputMode mode = OutputMode::Off;
	int port = 8000;           // network output port
	std::string game_name = "crusnusa";
};

struct Settings
{
	// paths / game
	std::string rom = "D:/Mame/roms/crusnusa.zip";
	std::string version = "4.5";
	std::string nvram = "cruisn_usa.nv";
	bool show_launcher = true;
	bool fast_boot = true;
	bool smooth_frames = false; // up to 57 game frames per second instead of 28.5 (frame governor off, CPU clock x2)
	int rubberband = 100;      // opponents' catch-up boost in percent of the original (0 = none)
	int dsp_thread = 0;        // sound DSP on its own thread: -1 auto (4+ hardware threads), 0 off (default, see PERFORMANCE.md), 1 on

	VideoSettings video;
	AudioSettings audio;
	ControlSettings controls;
	FfbSettings ffb;
	OutputSettings outputs;
	uint16_t dsw = 0xf9fe;     // cabinet DIP switches (see the DIP page)

	Settings();
	void reset_controls_to_defaults();

	bool load(const std::string &path);
	bool save(const std::string &path) const;
};

// names used in the ini and the UI
const char *to_string(Renderer r);
const char *to_string(WindowMode m);
const char *to_string(AspectMode a);
const char *to_string(HudPlacement h);
const char *to_string(ShadowMode s);
const char *to_string(FfbMode m);
// extra 3D view per side (arcade pixels) for the chosen aspect ratio; 0 for 4:3 / stretch or with the hack off
int wide_margin_for(const VideoSettings &v);
const char *to_string(OutputMode o);
const char *to_string(ShifterMode m);

// relative paths are taken relative to the executable (so the launcher and the game always agree)
std::string exe_relative(const std::string &path);

// list of actions (from actions.def)
struct ActionInfo { const char *id, *label, *def_key, *def_pad, *group; };
const std::vector<ActionInfo> &action_table();
