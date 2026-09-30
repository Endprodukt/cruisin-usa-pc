// Settings launcher shown before the game starts (skippable).
#pragma once

#include <string>

#include "../config/settings.h"
#include "../input/controls.h"
#include "../input/devices.h"

enum class LauncherResult { Play, Quit };

// Runs the launcher modally. Edits `settings` (and saves them to `ini_path` on Save / Play).
LauncherResult launcher_run(Settings &settings, const std::string &ini_path, InputHub &hub, Controls &controls);
