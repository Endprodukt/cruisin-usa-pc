// Turns keyboard + every attached controller into the cabinet's inputs, following the bindings.
#pragma once

#include <string>
#include <vector>

#include "../config/settings.h"
#include "../machine/midvunit.h"
#include "devices.h"

// MAME key names <-> Windows virtual keys ("LEFT", "LCONTROL", "F2", "5", ...; KEYCODE_ prefix accepted)
int mame_key_to_vk(const std::string &name);          // 0 = NONE / unknown
std::string vk_to_mame_key(int vk);                   // "" if the key has no MAME name
std::string key_display_name(const std::string &mame);   // "Left Ctrl"

// role of an analog control that is being learnt
enum class AxisRole { Steer, Accel, Brake };

class Controls
{
public:
	Controls(Settings &s, InputHub &hub) : m_s(s), m_hub(hub) {}

	// one emulated frame: polls the devices once and fills the machine's inputs
	void update(MachineInputs &out, bool window_focused);

	// ---- live monitor for the launcher ---------------------------------------------------------
	int gear() const { return m_gear; }                       // 0 = neutral
	float steer_value() const { return m_steer_out; }        // -1..1 as sent to the game
	float accel_value() const { return m_accel_out; }
	float brake_value() const { return m_brake_out; }
	bool action_active(const std::string &id) const;
	const DeviceState *device_state(int i) const { return i >= 0 && i < int(m_states.size()) ? &m_states[size_t(i)] : nullptr; }

	// ---- binding capture (launcher) -------------------------------------------------------------
	// Keyboard: returns the MAME key name of the next key that goes down, "" while waiting.
	std::string poll_capture_key();
	// Buttons / hats: next button or hat direction on any device that goes down (Backspace handled by caller).
	void begin_capture();                                     // snapshot the resting state of every device
	bool poll_capture_button(std::string &binding, std::string &label);
	// Axes: the first axis that moves clearly away from where it rested. Learns the direction:
	//  steer - 'turn left' must be the direction asked for (want_negative); pedals - pressed direction,
	//  and whether the pedal rests at one end (full) or at the centre (half).
	bool poll_capture_axis(AxisRole role, bool steer_left_asked, AxisBinding &out, float &live);

	// human readable binding text
	std::string pad_label(const std::string &binding) const;
	std::string axis_label(const AxisBinding &b) const;
	bool binding_device_present(const std::string &device) const { return m_hub.find(device) >= 0; }

	// ---- force feedback / rumble ------------------------------------------------------------------
	// start (or restart) the motor on the right device; returns a status text for the UI
	std::string ffb_start(HWND game_window);
	void ffb_update(uint8_t motor_byte, const struct Telemetry *telemetry = nullptr);   // call every emulated frame with the wheel latch
	void ffb_stop();
	bool ffb_detect_direction(HWND owner, bool &invert_out, std::string &msg);   // wheel must be free to move
	const std::string &ffb_status() const { return m_ffb_status; }
	int ffb_target_device() const;
	// the devices the motor is looked for on, best first: the chosen or the steering device, then the other devices of the
	// same hardware (a wheel base that is two devices: the motor can be on the part that does not steer)
	std::vector<int> ffb_candidates() const;
	~Controls();

private:
	bool button_bound(const std::string &binding) const;
	bool key_down(const std::string &mame, bool focused) const;
	bool pad_down(const std::string &binding) const;

	Settings &m_s;
	InputHub &m_hub;
	std::vector<DeviceState> m_states;

	// MAME analog accumulators (port units before sensitivity)
	struct Acc { float acc = 0; bool lastdigital = false; };
	Acc m_steer_acc, m_accel_acc, m_brake_acc;
	int m_gear = 0;
	bool m_prev_up = false, m_prev_down = false, m_prev_neutral = false;
	bool m_prev_gear[4] = {};

	float m_steer_out = 0, m_accel_out = 0, m_brake_out = 0;
	std::vector<std::string> m_active;

	// capture
	std::vector<DeviceState> m_rest;
	std::vector<uint8_t> m_rest_valid;

	void fx_thread_start();
	void fx_thread_stop();
	struct FxState;
	FxState *m_fx = nullptr;

	std::string m_ffb_status = "off";
	HWND m_game_window = nullptr;
	int m_ffb_dev = -1;
};
