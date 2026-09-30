#include "controls.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace {

struct KeyEntry { const char *name; int vk; const char *display; };

const std::vector<KeyEntry> &key_table()
{
	static std::vector<KeyEntry> t;
	if (!t.empty()) return t;
	static char names[64][4];
	int n = 0;
	for (char c = 'A'; c <= 'Z'; c++)
	{
		names[n][0] = c; names[n][1] = 0;
		t.push_back({names[n], c, names[n]});
		n++;
	}
	for (char c = '0'; c <= '9'; c++)
	{
		names[n][0] = c; names[n][1] = 0;
		t.push_back({names[n], c, names[n]});
		n++;
	}
	static char fnames[12][4];
	for (int i = 1; i <= 12; i++)
	{
		std::snprintf(fnames[i - 1], 4, "F%d", i);
		t.push_back({fnames[i - 1], VK_F1 + i - 1, fnames[i - 1]});
	}
	const KeyEntry extra[] = {
		{"LEFT", VK_LEFT, "Left"}, {"RIGHT", VK_RIGHT, "Right"}, {"UP", VK_UP, "Up"}, {"DOWN", VK_DOWN, "Down"},
		{"LCONTROL", VK_LCONTROL, "Left Ctrl"}, {"RCONTROL", VK_RCONTROL, "Right Ctrl"},
		{"LSHIFT", VK_LSHIFT, "Left Shift"}, {"RSHIFT", VK_RSHIFT, "Right Shift"},
		{"LALT", VK_LMENU, "Left Alt"}, {"RALT", VK_RMENU, "Right Alt"},
		{"SPACE", VK_SPACE, "Space"}, {"ENTER", VK_RETURN, "Enter"}, {"TAB", VK_TAB, "Tab"}, {"BACKSPACE", VK_BACK, "Backspace"},
		{"MINUS", VK_OEM_MINUS, "-"}, {"EQUALS", VK_OEM_PLUS, "="}, {"COMMA", VK_OEM_COMMA, ","}, {"STOP", VK_OEM_PERIOD, "."},
		{"SLASH", VK_OEM_2, "/"}, {"COLON", VK_OEM_1, ";"}, {"QUOTE", VK_OEM_7, "'"}, {"OPENBRACE", VK_OEM_4, "["},
		{"CLOSEBRACE", VK_OEM_6, "]"}, {"BACKSLASH", VK_OEM_5, "\\"}, {"TILDE", VK_OEM_3, "`"},
		{"INSERT", VK_INSERT, "Insert"}, {"DEL", VK_DELETE, "Delete"}, {"HOME", VK_HOME, "Home"}, {"END", VK_END, "End"},
		{"PGUP", VK_PRIOR, "Page Up"}, {"PGDN", VK_NEXT, "Page Down"}, {"CAPSLOCK", VK_CAPITAL, "Caps Lock"},
		{"0_PAD", VK_NUMPAD0, "Num 0"}, {"1_PAD", VK_NUMPAD1, "Num 1"}, {"2_PAD", VK_NUMPAD2, "Num 2"},
		{"3_PAD", VK_NUMPAD3, "Num 3"}, {"4_PAD", VK_NUMPAD4, "Num 4"}, {"5_PAD", VK_NUMPAD5, "Num 5"},
		{"6_PAD", VK_NUMPAD6, "Num 6"}, {"7_PAD", VK_NUMPAD7, "Num 7"}, {"8_PAD", VK_NUMPAD8, "Num 8"},
		{"9_PAD", VK_NUMPAD9, "Num 9"}, {"PLUS_PAD", VK_ADD, "Num +"}, {"MINUS_PAD", VK_SUBTRACT, "Num -"},
		{"ASTERISK", VK_MULTIPLY, "Num *"}, {"SLASH_PAD", VK_DIVIDE, "Num /"}, {"DEL_PAD", VK_DECIMAL, "Num ."},
	};
	for (const KeyEntry &e : extra) t.push_back(e);
	return t;
}

std::string upper(std::string s)
{
	for (char &c : s) c = char(std::toupper((unsigned char)c));
	return s;
}

bool iequals(const std::string &a, const std::string &b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
}

// "<device> | <what>" -> device, what
bool split_binding(const std::string &b, std::string &device, std::string &what)
{
	size_t bar = b.rfind('|');
	if (bar == std::string::npos) return false;
	auto trim = [](std::string s) {
		size_t a = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
		return a == std::string::npos ? std::string() : s.substr(a, e - a + 1);
	};
	device = trim(b.substr(0, bar));
	what = trim(b.substr(bar + 1));
	return !device.empty() && !what.empty();
}

int pov_dir(const std::string &w)
{
	std::string l;
	for (char c : w) l += char(std::tolower((unsigned char)c));
	if (l == "pov up") return 0;
	if (l == "pov right") return 9000;
	if (l == "pov down") return 18000;
	if (l == "pov left") return 27000;
	return -1;
}

bool pov_matches(int pov, int dir)
{
	if (pov < 0 || dir < 0) return false;
	int d = std::abs(pov - dir);
	if (d > 18000) d = 36000 - d;
	return d <= 4500;      // diagonals count for both neighbours
}

float apply_deadzone(float v, float dz)
{
	float a = std::fabs(v);
	if (a <= dz) return 0.0f;
	return (v < 0 ? -1.0f : 1.0f) * std::min(1.0f, (a - dz) / (1.0f - dz));
}

} // namespace

int mame_key_to_vk(const std::string &name)
{
	std::string n = upper(name);
	if (n.rfind("KEYCODE_", 0) == 0) n = n.substr(8);
	if (n.empty() || n == "NONE") return 0;
	for (const KeyEntry &e : key_table()) if (n == e.name) return e.vk;
	return 0;
}

std::string vk_to_mame_key(int vk)
{
	for (const KeyEntry &e : key_table()) if (e.vk == vk) return e.name;
	return "";
}

std::string key_display_name(const std::string &mame)
{
	std::string n = upper(mame);
	if (n.rfind("KEYCODE_", 0) == 0) n = n.substr(8);
	if (n.empty() || n == "NONE") return "-";
	for (const KeyEntry &e : key_table()) if (n == e.name) return e.display;
	return mame;
}

// ---- state evaluation -------------------------------------------------------------------------------

bool Controls::key_down(const std::string &mame, bool focused) const
{
	int vk = mame_key_to_vk(mame);
	if (!vk) return false;
	if (!focused && !m_s.controls.background_input) return false;
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool Controls::pad_down(const std::string &binding) const
{
	std::string device, what;
	if (!split_binding(binding, device, what)) return false;
	int hat = pov_dir(what);
	int n = 0;
	if (hat < 0) n = std::atoi(what.c_str());
	for (int i = 0; i < m_hub.count(); i++)
	{
		const DeviceInfo &di = m_hub.info(i);
		if (!iequals(di.name, device) || size_t(i) >= m_states.size()) continue;
		const DeviceState &st = m_states[size_t(i)];
		if (hat >= 0) { if (pov_matches(st.pov, hat)) return true; }
		else if (n >= 1 && n <= int(st.button.size()) && st.button[size_t(n - 1)]) return true;
	}
	return false;
}

bool Controls::action_active(const std::string &id) const
{
	return std::find(m_active.begin(), m_active.end(), id) != m_active.end();
}

void Controls::update(MachineInputs &out, bool focused)
{
	m_states.assign(size_t(m_hub.count()), DeviceState{});
	for (int i = 0; i < m_hub.count(); i++) m_hub.poll(i, m_states[size_t(i)]);

	const ControlSettings &c = m_s.controls;
	m_active.clear();
	auto active = [&](const char *id) {
		auto k = c.key.find(id);
		auto p = c.pad.find(id);
		bool on = (k != c.key.end() && key_down(k->second, focused)) || (p != c.pad.end() && !p->second.empty() && pad_down(p->second));
		if (on) m_active.push_back(id);
		return on;
	};

	uint16_t in0 = 0xffff, in1 = 0x007f;
	auto hold0 = [&](bool on, uint16_t bit) { if (on) in0 &= ~bit; };
	auto hold1 = [&](bool on, uint16_t bit) { if (on) in1 &= ~bit; };
	hold0(active("coin1"), in0bit::COIN1);
	hold0(active("coin2"), in0bit::COIN2);
	hold0(active("start"), in0bit::START);
	hold0(active("service"), in0bit::SERVICE);
	hold0(active("test"), in0bit::TEST);
	hold0(active("vol_down"), in0bit::VOLDN);
	hold0(active("vol_up"), in0bit::VOLUP);
	hold1(active("radio"), in1bit::RADIO);
	hold1(active("view1"), in1bit::VIEW1);
	hold1(active("view2"), in1bit::VIEW2);
	hold1(active("view3"), in1bit::VIEW3);

	// gears: sticky H-pattern buttons (MAME's default shifter mode) plus sequential up/down
	bool g[4] = {active("gear1"), active("gear2"), active("gear3"), active("gear4")};
	for (int i = 0; i < 4; i++)
		if (g[i] && !m_prev_gear[i]) m_gear = i + 1;
	for (int i = 0; i < 4; i++) m_prev_gear[i] = g[i];
	bool up = active("shift_up"), down = active("shift_down");
	if (up && !m_prev_up) m_gear = std::min(4, m_gear + 1);
	if (down && !m_prev_down) m_gear = std::max(0, m_gear - 1);
	m_prev_up = up; m_prev_down = down;

	// ---- analog controls ----------------------------------------------------------------------------
	auto axis_value = [&](const AxisBinding &b, bool &ok) -> float {
		ok = false;
		if (!b.bound()) return 0.0f;
		int dev = m_hub.find(b.device);
		if (dev < 0 || size_t(dev) >= m_states.size()) return 0.0f;
		int ai = InputHub::axis_index(m_hub.info(dev), b.axis);
		if (ai < 0 || size_t(ai) >= m_states[size_t(dev)].axis.size()) return 0.0f;
		ok = true;
		return m_states[size_t(dev)].axis[size_t(ai)];
	};

	// keyboard increments with MAME's analog accumulator
	auto key_axis = [&](Acc &a, const AnalogKeys &k, float range, bool neg_key, bool pos_key, bool centres_to_zero) -> float {
		float maxacc = range * 100.0f / float(std::max(1, k.sensitivity));
		bool pressed = neg_key || pos_key;
		if (neg_key) a.acc -= float(k.key_delta);
		if (pos_key) a.acc += float(k.key_delta);
		if (pressed) a.lastdigital = true;
		a.acc = centres_to_zero ? std::clamp(a.acc, -maxacc, maxacc) : std::clamp(a.acc, 0.0f, maxacc);
		if (!pressed && a.lastdigital)
		{
			float d = float(k.center_delta);
			if (a.acc > 0) { a.acc = std::max(0.0f, a.acc - d); if (a.acc == 0) a.lastdigital = false; }
			else if (a.acc < 0) { a.acc = std::min(0.0f, a.acc + d); if (a.acc == 0) a.lastdigital = false; }
			else a.lastdigital = false;
		}
		float port = std::clamp(a.acc * float(k.sensitivity) / 100.0f, centres_to_zero ? -range : 0.0f, range);
		return port / range;     // -1..1 (or 0..1)
	};

	bool sl = active("steer_left"), sr = active("steer_right");
	float key_steer = key_axis(m_steer_acc, c.steer_keys, 112.0f, sl, sr, true);      // 0x80 +/- 0x70
	float key_accel = key_axis(m_accel_acc, c.accel_keys, 255.0f, false, active("accel"), false);
	float key_brake = key_axis(m_brake_acc, c.brake_keys, 255.0f, false, active("brake"), false);

	// steering from a device
	float steer = 0;
	bool ok = false;
	float raw = axis_value(c.steer, ok);
	if (ok)
	{
		float v = c.steer.inverted ? -raw : raw;
		v = apply_deadzone(v, float(c.steer_deadzone) / 100.0f) * (100.0f / float(std::max(1, c.steer_range)));
		steer = std::clamp(v, -1.0f, 1.0f);
	}
	else
	{
		// unbound: an Xbox pad's left stick steers. DirectInput devices (wheels, pedals, sticks) must be bound
		// explicitly: their "x" axis is not reliably the wheel, and an unbound motor must never fight the game.
		for (int i = 0; i < m_hub.count(); i++)
		{
			const DeviceInfo &di = m_hub.info(i);
			if (di.backend != Backend::XInput) continue;
			int ai = InputHub::axis_index(di, "lx");
			if (ai < 0 || size_t(ai) >= m_states[size_t(i)].axis.size()) continue;
			float v = apply_deadzone(m_states[size_t(i)].axis[size_t(ai)], 0.12f);
			if (std::fabs(v) > std::fabs(steer)) steer = v;
		}
	}
	if (std::fabs(key_steer) > std::fabs(steer)) steer = key_steer;

	// pedals
	auto pedal = [&](const AxisBinding &b, bool &bound) -> float {
		float v = axis_value(b, bound);
		if (!bound) return 0.0f;
		if (b.inverted) v = -v;
		float p = b.half ? std::max(0.0f, v) : (v + 1.0f) * 0.5f;
		return apply_deadzone(p, float(c.pedal_deadzone) / 100.0f);
	};
	bool ab = false, bb = false;
	float accel = pedal(c.accel, ab);
	float brake = pedal(c.brake, bb);
	if (!ab)
		for (int i = 0; i < m_hub.count(); i++)
		{
			const DeviceInfo &di = m_hub.info(i);
			if (di.backend == Backend::XInput)
			{
				int rt = InputHub::axis_index(di, "rt");
				if (rt >= 0 && size_t(rt) < m_states[size_t(i)].axis.size()) accel = std::max(accel, (m_states[size_t(i)].axis[size_t(rt)] + 1.0f) * 0.5f);
			}
		}
	if (!bb)
		for (int i = 0; i < m_hub.count(); i++)
		{
			const DeviceInfo &di = m_hub.info(i);
			if (di.backend == Backend::XInput)
			{
				int lt = InputHub::axis_index(di, "lt");
				if (lt >= 0 && size_t(lt) < m_states[size_t(i)].axis.size()) brake = std::max(brake, (m_states[size_t(i)].axis[size_t(lt)] + 1.0f) * 0.5f);
			}
		}
	accel = std::max(accel, key_accel);
	brake = std::max(brake, key_brake);

	m_steer_out = std::clamp(steer, -1.0f, 1.0f);
	m_accel_out = std::clamp(accel, 0.0f, 1.0f);
	m_brake_out = std::clamp(brake, 0.0f, 1.0f);

	out.in0 = in0;
	out.in1 = in1;
	out.dsw = m_s.dsw;
	out.gear = m_gear;
	out.wheel = uint8_t(std::clamp(int(std::lround(0x80 + m_steer_out * 0x70)), 0x10, 0xf0));
	out.accel = uint8_t(std::lround(m_accel_out * 255.0f));
	out.brake = uint8_t(std::lround(m_brake_out * 255.0f));
}

// ---- binding capture ---------------------------------------------------------------------------------------

std::string Controls::poll_capture_key()
{
	static bool prev[256] = {};
	std::string found;
	for (const KeyEntry &e : key_table())
	{
		bool down = (GetAsyncKeyState(e.vk) & 0x8000) != 0;
		if (down && !prev[e.vk & 255] && e.vk != VK_BACK && e.vk != VK_ESCAPE && found.empty()) found = e.name;
		prev[e.vk & 255] = down;
	}
	return found;
}

void Controls::begin_capture()
{
	m_rest.assign(size_t(m_hub.count()), DeviceState{});
	m_rest_valid.assign(size_t(m_hub.count()), 0);
	for (int i = 0; i < m_hub.count(); i++) m_rest_valid[size_t(i)] = m_hub.poll(i, m_rest[size_t(i)]) ? 1 : 0;
}

bool Controls::poll_capture_button(std::string &binding, std::string &label)
{
	for (int i = 0; i < m_hub.count(); i++)
	{
		DeviceState st;
		if (!m_hub.poll(i, st)) continue;
		const DeviceInfo &di = m_hub.info(i);
		const DeviceState *rest = (size_t(i) < m_rest.size() && m_rest_valid[size_t(i)]) ? &m_rest[size_t(i)] : nullptr;
		// the next button that goes down, whatever is already held (a shifter's neutral may itself be a held button)
		for (size_t b = 0; b < st.button.size(); b++)
			if (st.button[b] && !(rest && b < rest->button.size() && rest->button[b]))
			{
				binding = di.name + " | " + std::to_string(b + 1);
				label = di.name + " button " + std::to_string(b + 1);
				return true;
			}
		if (st.pov >= 0 && !(rest && rest->pov >= 0))
		{
			const char *dir = st.pov < 4500 || st.pov >= 31500 ? "up" : st.pov < 13500 ? "right" : st.pov < 22500 ? "down" : "left";
			binding = di.name + " | pov " + dir;
			label = di.name + " hat " + dir;
			return true;
		}
	}
	return false;
}

bool Controls::poll_capture_axis(AxisRole role, bool steer_left_asked, AxisBinding &out, float &live)
{
	live = 0;
	int best_dev = -1, best_axis = -1;
	float best_delta = 0.45f;       // must move clearly away from rest
	for (int i = 0; i < m_hub.count(); i++)
	{
		DeviceState st;
		if (!m_hub.poll(i, st) || size_t(i) >= m_rest.size() || !m_rest_valid[size_t(i)]) continue;
		const DeviceState &rest = m_rest[size_t(i)];
		for (size_t a = 0; a < st.axis.size() && a < rest.axis.size(); a++)
		{
			float d = st.axis[a] - rest.axis[a];
			if (std::fabs(d) > best_delta) { best_delta = std::fabs(d); best_dev = i; best_axis = int(a); }
		}
	}
	if (best_dev < 0) return false;

	DeviceState st;
	m_hub.poll(best_dev, st);
	const DeviceState &rest = m_rest[size_t(best_dev)];
	float r = rest.axis[size_t(best_axis)], v = st.axis[size_t(best_axis)];
	live = v;
	out = AxisBinding{};
	out.device = m_hub.info(best_dev).name;
	out.axis = m_hub.info(best_dev).axes[size_t(best_axis)];
	float moved = v - r;
	if (role == AxisRole::Steer)
	{
		// the player turned the wheel the way that was asked for; a positive reading for "left" means inverted
		out.inverted = steer_left_asked ? (moved > 0) : (moved < 0);
	}
	else
	{
		// pressed direction = the direction it moved; a pedal resting near the centre shares its axis (half)
		out.inverted = moved < 0;
		out.half = std::fabs(r) < 0.3f;
	}
	return true;
}

std::string Controls::pad_label(const std::string &binding) const
{
	if (binding.empty()) return "-";
	std::string dev, what;
	if (!split_binding(binding, dev, what)) return binding;
	int hat = pov_dir(what);
	std::string w = hat >= 0 ? "Hat " + what.substr(4) : "Button " + what;
	return dev + "  " + w;
}

std::string Controls::axis_label(const AxisBinding &b) const
{
	if (!b.bound()) return "auto (every device's default)";
	return b.device + "  " + b.axis + (b.inverted ? "  inverted" : "") + (b.half ? "  half axis" : "");
}

// ---- force feedback --------------------------------------------------------------------------------------------

int Controls::ffb_target_device() const
{
	const FfbSettings &f = m_s.ffb;
	auto usable = [&](int i) { return i >= 0 && m_hub.info(i).backend == Backend::DInput && m_hub.info(i).ffb; };
	// the motor may only act on a wheel that actually steers the game: the device the steering axis is bound to,
	// or one chosen explicitly
	if (!f.device.empty())
	{
		int i = m_hub.find(f.device);
		if (usable(i)) return i;
	}
	if (m_s.controls.steer.bound())
	{
		int i = m_hub.find(m_s.controls.steer.device);
		if (usable(i)) return i;
	}
	return -1;
}

std::string Controls::ffb_start(HWND game_window)
{
	m_game_window = game_window;
	m_hub.ffb_end();
	m_ffb_dev = -1;
	if (!m_s.ffb.enabled) return m_ffb_status = "disabled";
	int dev = ffb_target_device();
	if (dev < 0) return m_ffb_status = m_s.controls.steer.bound() ? "steering device has no motor" : "bind the wheel's steering axis to enable force feedback";
	std::string axis = m_s.controls.steer.bound() && iequals(m_s.controls.steer.device, m_hub.info(dev).name) ? m_s.controls.steer.axis : "";
	std::string err;
	if (!m_hub.ffb_begin(dev, axis, m_s.ffb.device_gain, game_window, err)) return m_ffb_status = "failed: " + err;
	m_ffb_dev = dev;
	return m_ffb_status = "active on " + m_hub.info(dev).name;
}

void Controls::ffb_update(uint8_t motor)
{
	// WHLCTLZ byte: signed force, the game limits it to +/-126
	float f = float(int8_t(motor)) / 126.0f;
	f = std::clamp(f * float(m_s.ffb.strength) / 100.0f * (m_s.ffb.invert ? -1.0f : 1.0f), -1.0f, 1.0f);
	if (m_s.ffb.enabled && m_hub.ffb_active()) m_hub.ffb_set(f);
	if (m_s.ffb.enabled && m_s.ffb.rumble)
	{
		float mag = std::fabs(f) * float(m_s.ffb.rumble_strength) / 100.0f;
		m_hub.rumble_all(std::min(1.0f, mag * 0.7f), std::min(1.0f, mag));
	}
	else
		m_hub.rumble_all(0, 0);
}

void Controls::ffb_stop()
{
	m_hub.ffb_end();
	m_hub.rumble_all(0, 0);
	m_ffb_dev = -1;
}

// Drives the wheel with a known positive force and watches which way the steering axis moves, then decides
// whether the game's force needs inverting: the game's positive force must push towards 'steer right'.
bool Controls::ffb_detect_direction(HWND owner, bool &invert_out, std::string &msg)
{
	m_hub.ffb_end();
	int dev = ffb_target_device();
	if (dev < 0) { msg = "Bind the wheel's steering axis first."; return false; }
	const AxisBinding &sb = m_s.controls.steer;
	std::string axis = sb.bound() ? sb.axis : "x";
	std::string err;
	if (!m_hub.ffb_begin(dev, axis, m_s.ffb.device_gain, owner, err)) { msg = "Force feedback could not start: " + err; return false; }
	int ai = InputHub::axis_index(m_hub.info(dev), axis);
	DeviceState st;
	if (ai < 0 || !m_hub.poll(dev, st)) { m_hub.ffb_end(); msg = "The steering axis cannot be read."; return false; }
	float rest = st.axis[size_t(ai)], best = 0;
	m_hub.ffb_set(0.55f);
	DWORD t0 = GetTickCount();
	while (GetTickCount() - t0 < 450)
	{
		Sleep(10);
		if (m_hub.poll(dev, st)) { float d = st.axis[size_t(ai)] - rest; if (std::fabs(d) > std::fabs(best)) best = d; }
	}
	m_hub.ffb_set(0.0f);
	m_hub.ffb_end();
	if (std::fabs(best) < 0.03f) { msg = "The wheel did not move (hold it loosely, or raise the strength / device gain)."; return false; }
	// direction of +force on the axis, against the direction that means 'steer right'
	bool force_moves_positive = best > 0;
	bool right_is_positive = !sb.inverted;
	invert_out = force_moves_positive != right_is_positive;
	msg = std::string("Detected: positive force moves the axis ") + (force_moves_positive ? "up" : "down") + "; direction " +
	      (invert_out ? "inverted" : "normal") + ".";
	return true;
}
