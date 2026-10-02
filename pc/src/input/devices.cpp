#include "devices.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "xinput.lib")

namespace {

// DirectInput axis slots (DIJOYSTATE2 members in layout order, plus two catch-all slots: a Fanatec base
// exposes an axis that has no named slot)
enum { S_NONE = 0, S_X, S_Y, S_Z, S_RX, S_RY, S_RZ, S_SLIDER0, S_SLIDER1, S_EXTRA0, S_EXTRA1, S_COUNT };
const char *const kSlotName[S_COUNT] = {"none", "x", "y", "z", "rx", "ry", "rz", "slider0", "slider1", "extra0", "extra1"};
constexpr LONG DI_RANGE = 10000;

struct DiState
{
	DIJOYSTATE2 js;
	LONG extra[2];
};

DWORD slot_offset(int s)
{
	switch (s)
	{
	case S_X: return DIJOFS_X;
	case S_Y: return DIJOFS_Y;
	case S_Z: return DIJOFS_Z;
	case S_RX: return DIJOFS_RX;
	case S_RY: return DIJOFS_RY;
	case S_RZ: return DIJOFS_RZ;
	case S_SLIDER0: return DWORD(DIJOFS_SLIDER(0));
	case S_SLIDER1: return DWORD(DIJOFS_SLIDER(1));
	case S_EXTRA0: return DWORD(offsetof(DiState, extra[0]));
	case S_EXTRA1: return DWORD(offsetof(DiState, extra[1]));
	}
	return DIJOFS_X;
}

LONG slot_value(const DiState &st, int s)
{
	const DIJOYSTATE2 &j = st.js;
	switch (s)
	{
	case S_X: return j.lX;
	case S_Y: return j.lY;
	case S_Z: return j.lZ;
	case S_RX: return j.lRx;
	case S_RY: return j.lRy;
	case S_RZ: return j.lRz;
	case S_SLIDER0: return j.rglSlider[0];
	case S_SLIDER1: return j.rglSlider[1];
	case S_EXTRA0: return st.extra[0];
	case S_EXTRA1: return st.extra[1];
	}
	return 0;
}

// c_dfDIJoystick2 plus two anonymous axes
const DIDATAFORMAT *data_format()
{
	static DIOBJECTDATAFORMAT odf[256];
	static DIDATAFORMAT df;
	if (df.dwSize) return &df;
	DWORD n = c_dfDIJoystick2.dwNumObjs;
	if (n > 254) return &c_dfDIJoystick2;
	std::memcpy(odf, c_dfDIJoystick2.rgodf, n * sizeof(odf[0]));
	for (DWORD k = 0; k < 2; k++)
	{
		odf[n + k].pguid = nullptr;
		odf[n + k].dwOfs = DWORD(offsetof(DiState, extra[0])) + k * DWORD(sizeof(LONG));
		odf[n + k].dwType = DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL;
		odf[n + k].dwFlags = DIDOI_ASPECTPOSITION;
	}
	df = c_dfDIJoystick2;
	df.dwDataSize = sizeof(DiState);
	df.dwNumObjs = n + 2;
	df.rgodf = odf;
	return &df;
}

void dword_prop(IDirectInputDevice8A *dev, DWORD obj, REFGUID prop, DWORD value)
{
	DIPROPDWORD dw{};
	dw.diph.dwSize = sizeof(dw);
	dw.diph.dwHeaderSize = sizeof(dw.diph);
	dw.diph.dwHow = DIPH_BYID;
	dw.diph.dwObj = obj;
	dw.dwData = value;
	dev->SetProperty(prop, &dw.diph);
}

struct AxisEnum { IDirectInputDevice8A *dev; DWORD ff_type; };

BOOL CALLBACK enum_axis_cb(LPCDIDEVICEOBJECTINSTANCEA obj, LPVOID ctx)
{
	auto *ae = static_cast<AxisEnum *>(ctx);
	auto *dev = ae->dev;
	if ((obj->dwFlags & DIDOI_FFACTUATOR) && !ae->ff_type) ae->ff_type = obj->dwType;   // the axis the motor acts on
	DIPROPRANGE range{};
	range.diph.dwSize = sizeof(range);
	range.diph.dwHeaderSize = sizeof(range.diph);
	range.diph.dwHow = DIPH_BYID;
	range.diph.dwObj = obj->dwType;
	range.lMin = -DI_RANGE;
	range.lMax = DI_RANGE;
	dev->SetProperty(DIPROP_RANGE, &range.diph);
	dword_prop(dev, obj->dwType, DIPROP_DEADZONE, 0);       // the host applies its own deadzone
	dword_prop(dev, obj->dwType, DIPROP_SATURATION, 10000);
	return DIENUM_CONTINUE;
}

bool is_xinput_device(IDirectInput8A *di, const GUID &instance)
{
	IDirectInputDevice8A *dev = nullptr;
	if (FAILED(di->CreateDevice(instance, &dev, nullptr)) || !dev) return false;
	DIPROPGUIDANDPATH gp{};
	gp.diph.dwSize = sizeof(gp);
	gp.diph.dwHeaderSize = sizeof(gp.diph);
	gp.diph.dwHow = DIPH_DEVICE;
	bool yes = SUCCEEDED(dev->GetProperty(DIPROP_GUIDANDPATH, &gp.diph)) && (wcsstr(gp.wszPath, L"IG_") || wcsstr(gp.wszPath, L"ig_"));
	dev->Release();
	return yes;
}

bool name_contains(const std::string &hay, const std::string &needle)
{
	if (needle.empty()) return false;
	auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
	                      [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
	return it != hay.end();
}

bool name_in_list(const std::string &name, const std::string &list)
{
	size_t pos = 0;
	while (pos <= list.size())
	{
		size_t e = list.find(',', pos);
		std::string item = list.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
		size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
		if (a != std::string::npos && name_contains(name, item.substr(a, b - a + 1))) return true;
		if (e == std::string::npos) break;
		pos = e + 1;
	}
	return false;
}

bool iequals(const std::string &a, const std::string &b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
}

const char *const kXiAxes[6] = {"lx", "ly", "rx", "ry", "lt", "rt"};

// XInputGetState does not report the Home (Guide) button; the same call under ordinal 100 of xinput1_4.dll does
// (undocumented, in every Windows since Vista and used the same way by SDL and Steam). Falls back to the documented call.
DWORD xi_get_state(DWORD index, XINPUT_STATE &xs)
{
	using Fn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
	static Fn ex = [] {
		HMODULE h = LoadLibraryA("xinput1_4.dll");
		if (!h) h = LoadLibraryA("xinput1_3.dll");
		return h ? reinterpret_cast<Fn>(GetProcAddress(h, reinterpret_cast<LPCSTR>(100))) : nullptr;
	}();
	return ex ? ex(index, &xs) : XInputGetState(index, &xs);
}

} // namespace

struct InputHub::Impl
{
	HWND owner = nullptr;
	IDirectInput8A *di = nullptr;
	std::string ignore;
	bool allow_dupes = false;

	// force feedback
	int ffb_dev = -1;
	IDirectInputEffect *effect = nullptr;
	IDirectInputEffect *damper = nullptr;
	float last_damper = -1.0f;
	DWORD ffb_axis_offset = 0;        // the axis the effects act on: an object id (DIEFF_OBJECTIDS) or a data format offset
	DWORD ffb_axis_flags = 0;
	float last_force = 2.0f;
	// test pulse
	int test_dev = -1;
	DWORD test_end = 0, test_toggle = 0;
	int test_sign = 0;
	bool test_owns_effect = false;
};

InputHub::InputHub() : m_impl(new Impl) {}
InputHub::~InputHub() { shutdown(); }

void InputHub::shutdown()
{
	ffb_end();
	for (Dev &d : m_dev)
		if (d.di)
		{
			auto *dev = static_cast<IDirectInputDevice8A *>(d.di);
			dev->Unacquire();
			dev->Release();
			d.di = nullptr;
		}
	m_dev.clear();
	if (m_impl->di) { m_impl->di->Release(); m_impl->di = nullptr; }
}

bool InputHub::init(HWND owner, const std::string &ignore, bool allow_duplicates)
{
	m_impl->owner = owner;
	m_impl->ignore = ignore;
	m_impl->allow_dupes = allow_duplicates;
	if (!m_impl->di)
	{
		HRESULT hr = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
		                                reinterpret_cast<void **>(&m_impl->di), nullptr);
		if (FAILED(hr)) m_impl->di = nullptr;
	}
	refresh();
	return true;
}

const DeviceInfo &InputHub::info(int i) const { return m_dev[size_t(i)].info; }

void InputHub::refresh()
{
	ffb_end();
	for (Dev &d : m_dev)
		if (d.di)
		{
			auto *dev = static_cast<IDirectInputDevice8A *>(d.di);
			dev->Unacquire();
			dev->Release();
			d.di = nullptr;
		}
	m_dev.clear();

	if (m_impl->di)
	{
		struct Ctx { InputHub *hub; } ctx{this};
		auto cb = [](LPCDIDEVICEINSTANCEA inst, LPVOID c) -> BOOL {
			auto *hub = static_cast<Ctx *>(c)->hub;
			IDirectInput8A *di = hub->m_impl->di;
			if (name_in_list(inst->tszProductName, hub->m_impl->ignore) || is_xinput_device(di, inst->guidInstance))
				return DIENUM_CONTINUE;
			IDirectInputDevice8A *dev = nullptr;
			if (FAILED(di->CreateDevice(inst->guidInstance, &dev, nullptr)) || !dev) return DIENUM_CONTINUE;
			if (FAILED(dev->SetDataFormat(data_format())) ||
			    FAILED(dev->SetCooperativeLevel(hub->m_impl->owner, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND)))
			{
				dev->Release();
				return DIENUM_CONTINUE;
			}
			Dev d;
			// (trimmed: cheap encoder boards report "Generic   USB  Joystick  " with spaces at the end, and a binding is
			// "<name> | <button>" with the name trimmed when it is read: the device was found when binding and not when playing)
			{
				std::string nm = inst->tszProductName;
				const size_t a = nm.find_first_not_of(" \t"), e = nm.find_last_not_of(" \t");
				d.info.name = d.info.base = a == std::string::npos ? std::string("device") : nm.substr(a, e - a + 1);
			}
			d.info.backend = Backend::DInput;
			d.guid = inst->guidInstance;
			d.di = dev;
			AxisEnum ae{dev, 0};
			dev->EnumObjects(enum_axis_cb, &ae, DIDFT_AXIS);
			d.ff_axis_type = ae.ff_type;
			for (int s = S_X; s < S_COUNT; s++)
			{
				DIPROPRANGE r{};
				r.diph.dwSize = sizeof(r);
				r.diph.dwHeaderSize = sizeof(r.diph);
				r.diph.dwHow = DIPH_BYOFFSET;
				r.diph.dwObj = slot_offset(s);
				if (FAILED(dev->GetProperty(DIPROP_RANGE, &r.diph))) continue;
				d.present[s] = true;
				d.lmin[s] = -DI_RANGE; d.lmax[s] = DI_RANGE;
				if (r.lMax > r.lMin) { d.lmin[s] = r.lMin; d.lmax[s] = r.lMax; }
				d.slot_of_axis[s] = int(d.info.axes.size());
				d.info.axes.push_back(kSlotName[s]);
			}
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(caps);
			if (SUCCEEDED(dev->GetCapabilities(&caps)))
			{
				d.info.buttons = int(std::min<DWORD>(caps.dwButtons, 128));
				d.info.hat = caps.dwPOVs > 0;
				d.info.ffb = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0 || d.ff_axis_type != 0;
			}
			dev->Acquire();
			hub->m_dev.push_back(std::move(d));
			return DIENUM_CONTINUE;
		};
		m_impl->di->EnumDevices(DI8DEVCLASS_GAMECTRL, cb, &ctx, DIEDFL_ATTACHEDONLY);
	}

	// One piece of hardware can be several DirectInput devices with the same product name: a Fanatec base is two "FANATEC
	// Wheel"s, one with the steering axis and one for what is plugged into the wheel, and the motor is on one of them (not
	// necessarily the one that steers). All of them are kept; they are told apart by their shape, so that a binding means one
	// of them whatever order Windows lists them in: "FANATEC Wheel [A8 B108]". The force feedback then looks for the motor
	// among the devices of the same name (Controls::ffb_start).
	for (size_t i = 0; i < m_dev.size(); i++)
	{
		int same = 0;
		for (size_t k = 0; k < m_dev.size(); k++) same += k != i && iequals(m_dev[k].info.base, m_dev[i].info.base);
		m_dev[i].info.siblings = same;
	}
	for (size_t i = 0; i < m_dev.size(); i++)
	{
		Dev &d = m_dev[i];
		if (!d.info.siblings) continue;
		char tag[40];
		std::snprintf(tag, sizeof(tag), " [A%zu B%d]", d.info.axes.size(), d.info.buttons);
		d.info.name = d.info.base + tag;
		int n = 1;
		for (size_t k = 0; k < i; k++) n += iequals(m_dev[k].info.name, d.info.name) || m_dev[k].info.name.rfind(d.info.name + " #", 0) == 0;
		if (n > 1) d.info.name += " #" + std::to_string(n);   // the same shape twice: two wheels of one kind
	}
	int di_index = 0;
	for (Dev &d : m_dev) d.info.index = di_index++;

	// XInput pads (all of them bind as "XInput": slots change between sessions)
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
	{
		XINPUT_STATE xs{};
		if (XInputGetState(i, &xs) != ERROR_SUCCESS) continue;
		Dev d;
		d.info.name = "XInput";
		d.info.backend = Backend::XInput;
		d.info.index = int(i);
		d.info.ffb = true;           // rumble
		for (const char *a : kXiAxes) d.info.axes.push_back(a);
		d.info.buttons = 11;         // ...the eleventh is Home
		d.info.hat = true;
		m_dev.push_back(std::move(d));
	}
}

int InputHub::axis_index(const DeviceInfo &d, const std::string &axis)
{
	for (size_t i = 0; i < d.axes.size(); i++)
		if (iequals(d.axes[i], axis)) return int(i);
	return -1;
}

int InputHub::find(const std::string &name) const
{
	for (size_t i = 0; i < m_dev.size(); i++)
		if (iequals(m_dev[i].info.name, name)) return int(i);
	for (size_t i = 0; i < m_dev.size(); i++)
		if (iequals(m_dev[i].info.base, name)) return int(i);
	return -1;
}

int InputHub::find_axis(const std::string &name, const std::string &axis) const
{
	for (size_t i = 0; i < m_dev.size(); i++)
		if (iequals(m_dev[i].info.name, name)) return int(i);
	for (size_t i = 0; i < m_dev.size(); i++)
		if (iequals(m_dev[i].info.base, name) && axis_index(m_dev[i].info, axis) >= 0) return int(i);
	return find(name);
}

bool InputHub::same_hardware(int a, int b) const
{
	if (a < 0 || b < 0 || a >= count() || b >= count()) return false;
	const DeviceInfo &x = m_dev[size_t(a)].info, &y = m_dev[size_t(b)].info;
	return x.backend == Backend::DInput && y.backend == Backend::DInput && !x.base.empty() && iequals(x.base, y.base);
}

bool InputHub::poll(int i, DeviceState &out)
{
	if (i < 0 || i >= count()) return false;
	Dev &d = m_dev[size_t(i)];
	out.axis.assign(d.info.axes.size(), 0.0f);
	out.button.assign(size_t(d.info.buttons), 0);
	out.pov = -1;

	if (d.info.backend == Backend::XInput)
	{
		XINPUT_STATE xs{};
		if (xi_get_state(DWORD(d.info.index), xs) != ERROR_SUCCESS) return false;
		const XINPUT_GAMEPAD &g = xs.Gamepad;
		auto norm = [](SHORT v) { return float(v) / (v < 0 ? 32768.0f : 32767.0f); };
		out.axis[0] = norm(g.sThumbLX);
		out.axis[1] = -norm(g.sThumbLY);      // up is negative like a DirectInput y axis
		out.axis[2] = norm(g.sThumbRX);
		out.axis[3] = -norm(g.sThumbRY);
		out.axis[4] = float(g.bLeftTrigger) / 255.0f * 2.0f - 1.0f;
		out.axis[5] = float(g.bRightTrigger) / 255.0f * 2.0f - 1.0f;
		static const WORD btn[11] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
		                             XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK,
		                             XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, 0x0400 /* Home */};
		for (int b = 0; b < 11 && size_t(b) < out.button.size(); b++) out.button[size_t(b)] = (g.wButtons & btn[b]) ? 1 : 0;
		if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) out.pov = 0;
		else if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) out.pov = 9000;
		else if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) out.pov = 18000;
		else if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) out.pov = 27000;
		return true;
	}

	auto *dev = static_cast<IDirectInputDevice8A *>(d.di);
	if (!dev) return false;
	if (FAILED(dev->Poll())) { dev->Acquire(); dev->Poll(); }
	DiState st{};
	if (FAILED(dev->GetDeviceState(sizeof(st), &st))) { dev->Acquire(); return false; }
	for (int s = S_X; s < S_COUNT; s++)
	{
		if (!d.present[s]) continue;
		float span = float(d.lmax[s] - d.lmin[s]);
		float f = span > 0 ? (float(slot_value(st, s) - d.lmin[s]) / span) * 2.0f - 1.0f : 0.0f;
		out.axis[size_t(d.slot_of_axis[s])] = std::clamp(f, -1.0f, 1.0f);
	}
	for (int b = 0; b < d.info.buttons; b++) out.button[size_t(b)] = (st.js.rgbButtons[b] & 0x80) ? 1 : 0;
	out.pov = (st.js.rgdwPOV[0] & 0xFFFF) == 0xFFFF ? -1 : int(st.js.rgdwPOV[0]);
	return true;
}

// ---- force feedback --------------------------------------------------------------------------------

bool InputHub::ffb_active() const { return m_impl->effect != nullptr; }
int InputHub::ffb_device() const { return m_impl->ffb_dev; }

bool InputHub::ffb_begin(int device, const std::string &steer_axis, int gain_pct, HWND game_window, std::string &err)
{
	ffb_end();
	if (device < 0 || device >= count()) { err = "no device"; return false; }
	Dev &d = m_dev[size_t(device)];
	if (d.info.backend != Backend::DInput || !d.di) { err = "device has no force-feedback motor"; return false; }
	auto *dev = static_cast<IDirectInputDevice8A *>(d.di);

	// the motor pushes along the steering axis: the bound one, else x, else the first axis
	int slot = S_NONE;
	for (int s = S_X; s < S_COUNT; s++)
		if (d.present[s] && kSlotName[s] == steer_axis) slot = s;
	if (slot == S_NONE) slot = d.present[S_X] ? S_X : (d.present[S_EXTRA0] ? S_EXTRA0 : S_NONE);
	// ...but when the device says which axis its motor is on, that one. (On a wheel base that is two devices the motor can
	// be on the part that does not steer, where the steering binding's axis means nothing.) Always ONE axis: an effect across
	// every force-feedback axis a device reports is refused by some bases.
	// Both are tried, the motor's own axis first.
	struct AxisSpec { DWORD flags, axis; };
	std::vector<AxisSpec> specs;
	if (d.ff_axis_type) specs.push_back({DIEFF_OBJECTIDS, d.ff_axis_type});
	if (slot != S_NONE) specs.push_back({DIEFF_OBJECTOFFSETS, slot_offset(slot)});
	if (specs.empty()) { err = "device has no axis to push on"; return false; }

	dev->Unacquire();
	HWND w = game_window ? game_window : m_impl->owner;
	if (FAILED(dev->SetCooperativeLevel(w, DISCL_EXCLUSIVE | DISCL_BACKGROUND)) || FAILED(dev->Acquire()))
	{
		dev->SetCooperativeLevel(m_impl->owner, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
		dev->Acquire();
		err = "exclusive access refused (another program holds the wheel?)";
		return false;
	}
	dword_prop(dev, 0, DIPROP_AUTOCENTER, DIPROPAUTOCENTER_OFF);
	{
		DIPROPDWORD dw{};
		dw.diph.dwSize = sizeof(dw);
		dw.diph.dwHeaderSize = sizeof(dw.diph);
		dw.diph.dwHow = DIPH_DEVICE;
		dw.diph.dwObj = 0;
		dw.dwData = DWORD(std::clamp(gain_pct, 0, 100)) * 100;
		dev->SetProperty(DIPROP_FFGAIN, &dw.diph);
		dw.dwData = DIPROPAUTOCENTER_OFF;
		dev->SetProperty(DIPROP_AUTOCENTER, &dw.diph);
	}

	DICONSTANTFORCE cf{};
	LONG dir = 0;
	HRESULT hr = E_FAIL;
	for (const AxisSpec &sp : specs)
	{
		m_impl->ffb_axis_offset = sp.axis;
		m_impl->ffb_axis_flags = sp.flags;
		DIEFFECT eff{};
		eff.dwSize = sizeof(eff);
		eff.dwFlags = DIEFF_CARTESIAN | sp.flags;
		eff.dwDuration = INFINITE;
		eff.dwGain = DI_FFNOMINALMAX;
		eff.dwTriggerButton = DIEB_NOTRIGGER;
		eff.cAxes = 1;
		eff.rgdwAxes = &m_impl->ffb_axis_offset;
		eff.rglDirection = &dir;
		eff.cbTypeSpecificParams = sizeof(cf);
		eff.lpvTypeSpecificParams = &cf;
		m_impl->effect = nullptr;
		hr = dev->CreateEffect(GUID_ConstantForce, &eff, &m_impl->effect, nullptr);
		if (SUCCEEDED(hr) && m_impl->effect && SUCCEEDED(m_impl->effect->Start(1, 0))) break;
		if (m_impl->effect) { m_impl->effect->Release(); m_impl->effect = nullptr; }
	}
	if (!m_impl->effect)
	{
		dev->Unacquire();
		dev->SetCooperativeLevel(m_impl->owner, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
		dev->Acquire();
		char code[40];
		std::snprintf(code, sizeof(code), " (0x%08lX)", (unsigned long)hr);
		err = std::string(d.info.ffb ? "constant-force effect could not be created" : "no force-feedback motor") + code;
		return false;
	}
	m_impl->ffb_dev = device;
	m_impl->last_force = 2.0f;

	// optional second effect: a damper (resistance proportional to the wheel's speed), started at zero
	{
		DICONDITION cond{};
		DIEFFECT de{};
		de.dwSize = sizeof(de);
		de.dwFlags = DIEFF_CARTESIAN | m_impl->ffb_axis_flags;
		de.dwDuration = INFINITE;
		de.dwGain = DI_FFNOMINALMAX;
		de.dwTriggerButton = DIEB_NOTRIGGER;
		de.cAxes = 1;
		de.rgdwAxes = &m_impl->ffb_axis_offset;
		de.rglDirection = &dir;
		de.cbTypeSpecificParams = sizeof(cond);
		de.lpvTypeSpecificParams = &cond;
		m_impl->damper = nullptr;
		m_impl->last_damper = -1.0f;
		if (FAILED(dev->CreateEffect(GUID_Damper, &de, &m_impl->damper, nullptr)) || !m_impl->damper || FAILED(m_impl->damper->Start(1, 0)))
		{
			if (m_impl->damper) m_impl->damper->Release();
			m_impl->damper = nullptr;
		}
	}
	return true;
}

void InputHub::ffb_set_damper(float amount)
{
	if (!m_impl->damper) return;
	amount = std::clamp(amount, 0.0f, 1.0f);
	if (std::fabs(amount - m_impl->last_damper) < 0.02f && !(amount == 0.0f && m_impl->last_damper != 0.0f)) return;
	m_impl->last_damper = amount;
	DICONDITION cond{};
	cond.lPositiveCoefficient = cond.lNegativeCoefficient = LONG(amount * 10000.0f);
	cond.dwPositiveSaturation = cond.dwNegativeSaturation = 10000;
	DIEFFECT eff{};
	eff.dwSize = sizeof(eff);
	eff.cbTypeSpecificParams = sizeof(cond);
	eff.lpvTypeSpecificParams = &cond;
	m_impl->damper->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
}

void InputHub::ffb_set(float force)
{
	if (!m_impl->effect) return;
	force = std::clamp(force, -1.0f, 1.0f);
	if (force == m_impl->last_force) return;
	m_impl->last_force = force;
	DICONSTANTFORCE cf{};
	cf.lMagnitude = LONG(force * 10000.0f);
	DIEFFECT eff{};
	eff.dwSize = sizeof(eff);
	eff.cbTypeSpecificParams = sizeof(cf);
	eff.lpvTypeSpecificParams = &cf;
	m_impl->effect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
}

void InputHub::ffb_end()
{
	if (m_impl->damper)
	{
		m_impl->damper->Stop();
		m_impl->damper->Release();
		m_impl->damper = nullptr;
	}
	if (m_impl->effect)
	{
		DICONSTANTFORCE cf{};
		DIEFFECT eff{};
		eff.dwSize = sizeof(eff);
		eff.cbTypeSpecificParams = sizeof(cf);
		eff.lpvTypeSpecificParams = &cf;
		m_impl->effect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
		m_impl->effect->Stop();
		m_impl->effect->Release();
		m_impl->effect = nullptr;
	}
	if (m_impl->ffb_dev >= 0 && m_impl->ffb_dev < count())
	{
		Dev &d = m_dev[size_t(m_impl->ffb_dev)];
		if (auto *dev = static_cast<IDirectInputDevice8A *>(d.di))
		{
			dev->Unacquire();
			dev->SetCooperativeLevel(m_impl->owner, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
			dev->Acquire();
		}
	}
	m_impl->ffb_dev = -1;
	m_impl->test_dev = -1;
}

void InputHub::rumble_all(float left, float right)
{
	static float last_l = -1, last_r = -1;
	if (left == last_l && right == last_r) return;
	last_l = left; last_r = right;
	XINPUT_VIBRATION v{};
	v.wLeftMotorSpeed = WORD(std::clamp(left, 0.0f, 1.0f) * 65535.0f);
	v.wRightMotorSpeed = WORD(std::clamp(right, 0.0f, 1.0f) * 65535.0f);
	for (const Dev &d : m_dev)
		if (d.info.backend == Backend::XInput) XInputSetState(DWORD(d.info.index), &v);
}

bool InputHub::test_pulse(int device)
{
	if (device < 0 || device >= count()) return false;
	const Dev &d = m_dev[size_t(device)];
	m_impl->test_end = GetTickCount() + 850;
	m_impl->test_toggle = GetTickCount() + 65;
	m_impl->test_sign = 1;
	if (d.info.backend == Backend::XInput)
	{
		XINPUT_VIBRATION v{42000, 52000};
		XInputSetState(DWORD(d.info.index), &v);
		m_impl->test_dev = device;
		return true;
	}
	std::string err;
	if (!m_impl->effect)
	{
		if (!ffb_begin(device, "", 100, m_impl->owner, err)) return false;
		m_impl->test_owns_effect = true;
	}
	ffb_set(0.35f);
	m_impl->test_dev = device;
	return true;
}

void InputHub::tick()
{
	if (m_impl->test_dev < 0) return;
	DWORD now = GetTickCount();
	const Dev &d = m_dev[size_t(m_impl->test_dev)];
	if (LONG(now - m_impl->test_end) >= 0)
	{
		if (d.info.backend == Backend::XInput)
		{
			XINPUT_VIBRATION v{};
			XInputSetState(DWORD(d.info.index), &v);
			m_impl->test_dev = -1;
		}
		else
		{
			bool own = m_impl->test_owns_effect;
			m_impl->test_owns_effect = false;
			ffb_set(0.0f);
			if (own) ffb_end();
			m_impl->test_dev = -1;
		}
		return;
	}
	if (d.info.backend == Backend::DInput && LONG(now - m_impl->test_toggle) >= 0)
	{
		m_impl->test_sign = -m_impl->test_sign;
		ffb_set(0.35f * float(m_impl->test_sign));
		m_impl->test_toggle = now + 65;
	}
}
