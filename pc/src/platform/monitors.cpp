#include "monitors.h"

#include <cstdio>

namespace {

// the monitors' own names (what Windows' display settings show) by GDI device name
std::vector<std::pair<std::string, std::string>> friendly_names()
{
	std::vector<std::pair<std::string, std::string>> out;
	UINT32 np = 0, nm = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return out;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return out;
	for (UINT32 i = 0; i < np; i++)
	{
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof(src);
		src.header.adapterId = paths[i].sourceInfo.adapterId;
		src.header.id = paths[i].sourceInfo.id;
		DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
		tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tgt.header.size = sizeof(tgt);
		tgt.header.adapterId = paths[i].targetInfo.adapterId;
		tgt.header.id = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
		char dev[64], name[128];
		WideCharToMultiByte(CP_UTF8, 0, src.viewGdiDeviceName, -1, dev, sizeof(dev), nullptr, nullptr);
		WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1, name, sizeof(name), nullptr, nullptr);
		out.emplace_back(dev, name);
	}
	return out;
}

BOOL CALLBACK collect(HMONITOR mon, HDC, LPRECT, LPARAM data)
{
	auto *v = reinterpret_cast<std::vector<MonitorDesc> *>(data);
	MONITORINFOEXA mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoA(mon, &mi)) return TRUE;
	MonitorDesc m;
	m.device = mi.szDevice;
	m.rc = mi.rcMonitor;
	m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
	m.width = mi.rcMonitor.right - mi.rcMonitor.left;
	m.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
	DEVMODEA dm{};
	dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
	{
		m.width = int(dm.dmPelsWidth);   // the display mode itself, whatever the scaling
		m.height = int(dm.dmPelsHeight);
		m.hz = dm.dmDisplayFrequency > 1 ? int(dm.dmDisplayFrequency) : 0;
	}
	v->push_back(m);
	return TRUE;
}

} // namespace

std::vector<MonitorDesc> list_monitors()
{
	std::vector<MonitorDesc> mons;
	EnumDisplayMonitors(nullptr, nullptr, collect, reinterpret_cast<LPARAM>(&mons));
	for (const auto &[dev, name] : friendly_names())
		for (MonitorDesc &m : mons)
			if (m.device == dev) m.name = name;
	for (MonitorDesc &m : mons)
		if (m.name.empty())
		{
			DISPLAY_DEVICEA dd{};
			dd.cb = sizeof(dd);
			if (EnumDisplayDevicesA(m.device.c_str(), 0, &dd, 0)) m.name = dd.DeviceString;   // e.g. "Generic PnP Monitor"
		}
	return mons;
}

int find_monitor(const std::vector<MonitorDesc> &mons, const std::string &device, int index)
{
	if (!device.empty())
		for (size_t i = 0; i < mons.size(); i++)
			if (mons[i].device == device) return int(i);
	if (index >= 0 && index < int(mons.size()) && device.empty()) return index;
	for (size_t i = 0; i < mons.size(); i++)
		if (mons[i].primary) return int(i);
	return 0;
}

std::string monitor_label(const MonitorDesc &m, int index)
{
	char buf[256];
	std::snprintf(buf, sizeof(buf), "%d: %s  -  %d x %d%s%s", index + 1, m.name.empty() ? "Monitor" : m.name.c_str(), m.width, m.height,
	              m.hz ? (", " + std::to_string(m.hz) + " Hz").c_str() : "", m.primary ? "  (main display)" : "");
	return buf;
}
