// Every attached controller: DirectInput devices (wheels, pedals, pads, shifters) and XInput pads.
// Polled through one interface so the bindings can name "<device> | <axis>" and "<device> | <n>".
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class Backend { DInput, XInput };

struct DeviceInfo
{
	std::string name;                 // product name; "XInput" for every Xbox pad
	Backend backend = Backend::DInput;
	int index = 0;                    // DirectInput list index / XInput user slot
	bool ffb = false;                 // has a force-feedback motor
	std::vector<std::string> axes;    // axis names present, e.g. x y z rx ry rz slider0 slider1 extra0 extra1 / lx ly rx ry lt rt
	int buttons = 0;
	bool hat = false;
	int duplicates_merged = 0;        // other enumerations of the same physical device that were folded into this one
};

struct DeviceState
{
	std::vector<float> axis;          // -1..+1 against the device's reported range, same order as DeviceInfo::axes
	std::vector<uint8_t> button;      // DeviceInfo::buttons entries
	int pov = -1;                     // hundredths of a degree, -1 centred
};

class InputHub
{
public:
	InputHub();
	~InputHub();

	// owner: window used for DirectInput cooperative levels. ignore: comma separated name fragments.
	bool init(HWND owner, const std::string &ignore, bool allow_duplicates);
	void shutdown();
	void refresh();                   // re-enumerate (hot-plug)

	int count() const { return int(m_dev.size()); }
	const DeviceInfo &info(int i) const;
	bool poll(int i, DeviceState &out);

	// resolve a stored device name; XInput matches any connected pad. -1 if not attached.
	int find(const std::string &name) const;
	static int axis_index(const DeviceInfo &d, const std::string &axis);   // -1 if absent

	// ---- force feedback (DirectInput constant force) and rumble (XInput) ---------------------------
	bool ffb_begin(int device, const std::string &steer_axis, int device_gain_pct, HWND game_window, std::string &err);
	void ffb_set(float force);        // -1..+1, call every frame
	void ffb_end();
	bool ffb_active() const;
	int  ffb_device() const;
	void rumble_all(float left, float right);   // every XInput pad, 0..1
	bool test_pulse(int device);                // short bounded test pulse; returns false if the device has no motor
	void tick();                                // advances a running test pulse

private:
	struct Impl;
	struct Dev
	{
		DeviceInfo info;
		GUID guid{};
		void *di = nullptr;                 // IDirectInputDevice8A *
		bool present[12] = {};              // DirectInput axis slots (see di_axes)
		LONG lmin[12] = {}, lmax[12] = {};
		int slot_of_axis[12] = {};          // index into DeviceInfo::axes per slot
	};
	std::vector<Dev> m_dev;
	std::unique_ptr<Impl> m_impl;
};
