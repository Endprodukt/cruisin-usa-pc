Cruis'n USA for Windows
=======================

Windows version by Endprodukt, 2026. An emulator made for this one game: the original arcade
program runs on an emulation of its board; it is not a port of the game's code.  https://github.com/Endprodukt/cruisin-usa-pc

What you need
  * crusnusa.zip - the MAME ROM set of Cruis'n USA. It is NOT included. The first start asks where it is; you can also
    set it later in the launcher (Home > ROM zip), or simply put the zip next to the program.
  * Windows 10 or 11, a graphics card with OpenGL 4.5 (or Vulkan 1.1).

Start
  Run cruisn_usa.exe. The launcher opens: PLAY starts the game, SAVE writes the settings, QUIT leaves.
  Settings are kept in cruisn.ini, high scores and operator settings in cruisn_usa.nv. Both are created next to the
  program at the first start. The first start also asks whether you want a shortcut on the desktop; nothing is put there
  without asking.
  To skip the launcher turn off "Show this launcher at startup" (hold Shift while starting to get it back).

Keys in the game (defaults)
  5 coin, 1 start, Left / Right steer, Left Ctrl accelerate, Left Alt brake, Z X C V gears (Up / Down with the sequential
  shifter), R radio, A S D views, F2 test menu, Esc quit.
  P pauses the game and opens the options over the picture: video, audio, controls, force feedback, DIP switches and
  outputs can be changed there and apply at once (a few video settings need a restart and say so). CONTINUE, P or Esc go
  back to the game; RETURN TO ATTRACT and EXIT GAME are there too. The pause button can be bound to a wheel or pad button
  (Controls). With the Vulkan or CPU renderer a small menu (continue, return to attract, exit game) is shown instead.
  The menu works without the mouse: arrow keys or any device's hat / d-pad move, Enter (pad: A) accepts, Esc or Backspace
  (pad: B) goes back, from the list of pages out of the menu; Tab jumps between the list of pages and the page. The Xbox
  pad's Home button opens the menu (Windows' Game Bar also listens to that button unless it is turned off in Windows).
  Pause, accept and back can be bound to other keys and buttons under Controls (group "Menu").
  The mouse pointer is hidden over the game (Video > "Hide the mouse pointer"); it is there while the options are open.
  F3 anti-aliasing, F4 shadow style, F5 / F6 internal resolution, F7 texture filter, F8 VSync, F9 smooth scaling.
  Wheels, pedals, shifters and pads are bound in the launcher under Controls.

Folders the program creates when asked to
  textures/dump     textures exported while playing (Video > Textures; not ready yet, switched off in this version)
  textures/replace  your replacement textures (same file names)

Credits and licences: see the launcher's About page and LICENSES.txt.
