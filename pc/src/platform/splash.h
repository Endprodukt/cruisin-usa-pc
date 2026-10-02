// Loading picture shown over the game window while the arcade board runs its power-up tests.
#pragma once

#include <windows.h>

class Splash
{
public:
	~Splash() { hide(); }
	// covers the client area of `game` with the picture (resource BACKGROUND, cropped to fit) and a caption
	void show(HWND game, const char *caption);
	void hide();
	bool shown() const { return m_wnd != nullptr; }

private:
	HWND m_wnd = nullptr;
};
