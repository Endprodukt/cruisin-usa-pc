// Drives the game's "CALIBRATE CONTROLS" screens (see DIAG.ASM SET_CONTROLS) with scripted input so
// that a fresh NVRAM ends up calibrated. Used by tools to generate src/machine/default_nvram.h.
//
// Prompts, in order: (1) hands/feet off + wheel centre (also records pedal minimum), (2) wheel left,
// (3) wheel right, (4) gas to max, (5) brake to max. Each is confirmed with the test/enter switch.
#pragma once
#include "midvunit.h"

inline void run_auto_setup(MidVUnit &m)
{
	struct Step { int at; uint8_t wheel, accel, brake; };
	const Step steps[5] = {
		{2000, 0x80, 0x00, 0x00},   // centre, pedals released
		{2110, 0x10, 0x00, 0x00},   // full left
		{2220, 0xf0, 0x00, 0x00},   // full right
		{2330, 0x80, 0xff, 0x00},   // gas max
		{2440, 0x80, 0x00, 0xff},   // brake max
	};

	MachineInputs saved = m.inputs;
	for (int f = 0; f < 4700; f++)
	{
		m.inputs = MachineInputs{};
		uint16_t press = 0;
		for (const Step &s : steps)
			if (f >= s.at - 20 && f < s.at + 30)
			{
				m.inputs.wheel = s.wheel; m.inputs.accel = s.accel; m.inputs.brake = s.brake;
				if (f >= s.at && f < s.at + 8) press = in0bit::TEST;
			}
		// afterwards: leave the diagnostics menu ("exit to game over" is 6 entries down)
		for (int i = 0; i < 6; i++)
			if (f >= 3500 + i * 15 && f < 3504 + i * 15) press = in0bit::VOLDN;
		if (f >= 3650 && f < 3656) press = in0bit::TEST;
		m.inputs.in0 = uint16_t(0xffff & ~press);
		m.run_frame();
	}
	m.inputs = saved;
}
