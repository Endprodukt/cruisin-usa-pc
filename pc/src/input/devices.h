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
	std::string name;                 // unique name used in bindings: the product name, plus "[A<axes> B<buttons>]" when several
	                                  // devices share it (one wheel base can be two devices); "XInput" for every Xbox pad
	std::string base;                 // the product name alone: devices with the same base belong together
	Backend backend = Backend::DInput;
	int index = 0;                    // DirectInput list index / XInput user slot
	bool ffb = false;                 // has a force-feedback motor
	std::vector<std::string> axes;    // axis names present, e.g. x y z rx ry rz slider0 slider1 extra0 extra1 / lx ly rx ry lt rt
	int buttons = 0;
	bool hat = false;
	int siblings = 0;                 // other devices with the same product name (parts of the same hardware)
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
	bool init(HWND owner, const std::string &ignore, bool unused_allow_duplicates = false);
	void shutdown();
	void refresh();                   // re-enumerate (hot-plug)

	int count() const { return int(m_dev.size()); }
	const DeviceInfo &info(int i) const;
	bool poll(int i, DeviceState &out);

	// resolve a stored device name; XInput matches any connected pad. -1 if not attached.
	// (a name stored before devices sharing a product name were told apart matches the first of them)
	int find(const std::string &name) const;
	// the device a stored axis binding means: the named one, else the first with that product name that has the axis
	int find_axis(const std::string &name, const std::string &axis) const;
	bool same_hardware(int a, int b) const;   // same product name (two parts of one wheel base)
	static int axis_index(const DeviceInfo &d, const std::string &axis);   // -1 if absent

	// ---- force feedback (DirectInput constant force) and rumble (XInput) ---------------------------
	bool ffb_begin(int device, const std::string &steer_axis, int device_gain_pct, HWND game_window, std::string &err);
	void ffb_set(float force);        // -1..+1, call every frame
	// resistance against turning the wheel, 0..1: a damper effect that the wheel's own electronics compute from the wheel's
	// speed (smooth at any speed, unlike a force computed here from 60 position samples a second). No-op if unsupported.
	void ffb_set_damper(float amount);
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
		DWORD ff_axis_type = 0;             // object id of the axis the motor acts on (the device's force-feedback actuator), 0 = none reported
	};
	std::vector<Dev> m_dev;
	std::unique_ptr<Impl> m_impl;
};
