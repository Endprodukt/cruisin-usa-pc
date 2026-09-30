// First-run helper: drives the game's calibration + diagnostics screens with scripted
// input so that a fresh (all-ones) NVRAM ends up calibrated and in attract mode.
#pragma once
#include "midvunit.h"

inline void run_auto_setup(MidVUnit &m)
{
	struct Press { int at; uint16_t bit; };
	std::vector<Press> presses;
	auto press = [&](int at, uint16_t bit) { presses.push_back({at, bit}); };
	press(2000, in0bit::TEST);   // left lock
	press(2110, in0bit::TEST);   // right lock
	press(2210, in0bit::TEST);   // centre
	press(2300, in0bit::TEST);   // accelerator
	press(2400, in0bit::TEST);   // brake
	press(2500, in0bit::TEST);   // done -> diagnostics menu
	for (int i = 0; i < 6; i++)
		press(3500 + i * 15, in0bit::VOLDN);   // scroll to "exit to game over"
	press(3650, in0bit::TEST);

	MachineInputs saved = m.inputs;
	for (int f = 0; f < 4700; f++)
	{
		m.inputs = MachineInputs{};
		if (f >= 1990 && f < 2100) m.inputs.wheel = 16;
		else if (f >= 2100 && f < 2200) m.inputs.wheel = 240;
		if (f >= 2290 && f < 2390) m.inputs.accel = 255;
		if (f >= 2390 && f < 2490) m.inputs.brake = 255;
		for (auto &p : presses)
			if (f >= p.at && f < p.at + (p.bit == in0bit::VOLDN ? 4 : 8))
				m.inputs.in0 &= ~p.bit;
		m.run_frame();
	}
	m.inputs = saved;
}
