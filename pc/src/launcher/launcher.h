// Settings launcher shown before the game starts (skippable).
#pragma once

#include <string>

#include "../config/settings.h"
#include "../input/controls.h"
#include "../input/devices.h"

enum class LauncherResult { Play, Quit };

// "Open" dialog for crusnusa.zip; false when cancelled
bool browse_rom(void *owner_hwnd, std::string &path);
// writes the factory save file (CMOS) when there is none at `path`
bool create_default_save(const std::string &path);

// Runs the launcher modally. Edits `settings` (and saves them to `ini_path` on Save / Play).
LauncherResult launcher_run(Settings &settings, const std::string &ini_path, InputHub &hub, Controls &controls);
