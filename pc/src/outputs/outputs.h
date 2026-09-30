// Cabinet lamps (and the wheel motor) for MameHooker / Hook Of The Reaper style tools.
//
//  windows  MAME's win32 output server: a window "MAMEOutput" and the registered messages
//           MAMEOutputStart/Stop/UpdateState/Register/Unregister/GetIDString (names via WM_COPYDATA).
//  network  MAME's network output server: TCP (default 8000), one "name = value\r" line per change,
//           opened by "mame_start = <game>\r".
//
// The outputs are named like the game's own lamp test lists them.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "../config/settings.h"

class Outputs
{
public:
	Outputs();
	~Outputs();

	bool start(const OutputSettings &cfg);     // no-op for mode Off
	void stop();
	bool running() const;

	// call once per emulated frame
	void update(const uint8_t lamps[8], uint8_t wheel_motor);

	static const char *const *names(size_t &count);   // lamp names in output order

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
