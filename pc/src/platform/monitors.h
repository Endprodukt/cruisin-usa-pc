// The attached monitors, listed once for the launcher and the game: one list in one order, so the monitor picked in the
// launcher is the one the game opens on. The choice is stored by device name (\\.\DISPLAYn) as well as by position.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct MonitorDesc
{
	std::string device;        // \\.\DISPLAYn
	std::string name;          // the monitor's own name (e.g. "LG TV SSCR2"), empty if Windows does not know it
	RECT rc{};                 // desktop rectangle in real pixels (the process is DPI aware per monitor)
	int width = 0, height = 0, hz = 0;   // current display mode
	bool primary = false;
};

std::vector<MonitorDesc> list_monitors();
// the monitor with this device name; without a stored name (older ini) the one at this position; else the main display
int find_monitor(const std::vector<MonitorDesc> &mons, const std::string &device, int index);
std::string monitor_label(const MonitorDesc &m, int index);
