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

// The same pages inside the running game (the pause menu): drawn over the game's picture with the game's OpenGL context,
// the settings object is the one the game uses, so what the game reads every frame (bindings, force feedback strengths)
// changes at once and the frame loop applies the rest. Home, the game settings (they live in the save file the machine has
// loaded) and Network are left out.
class GameMenu
{
public:
	enum class Action { None, Continue, Attract, Exit };
	~GameMenu() { shutdown(); }
	bool init(void *hwnd, Settings &settings, const std::string &ini_path, InputHub &hub, Controls &controls);   // the game's OpenGL context is current
	void shutdown();
	bool available() const { return m_impl != nullptr; }
	bool is_open() const { return m_open; }
	void open();
	void close();                 // saves the settings when something was changed
	bool capturing() const;       // a key or button is being learnt: the keyboard belongs to that
	bool message(void *hwnd, unsigned msg, unsigned long long wp, long long lp);   // window messages while open; true = taken
	Action frame();               // builds this frame's menu; what was chosen
	void render();                // draws it (called by the video backend right before the buffer swap)

private:
	struct Impl;
	Impl *m_impl = nullptr;
	bool m_open = false;
};

// Runs the launcher modally. Edits `settings` (and saves them to `ini_path` on Save / Play).
LauncherResult launcher_run(Settings &settings, const std::string &ini_path, InputHub &hub, Controls &controls);
