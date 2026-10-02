# Cruis'n USA for Windows

A native Windows port of Midway's 1994 arcade racer. The original game program runs unchanged on a freestanding
implementation of its arcade board (Midway V-Unit), with a GPU renderer, real widescreen, force feedback and an
options menu around it. No MAME installation needed.

> **Work in progress.** This is the first public build. It is playable from coin to credits, but expect rough edges -
> and please [report them](#help-wanted).

## Quick start

1. Download `Cruis'n Windows.zip` from [Releases](https://github.com/Endprodukt/cruisin-usa-pc/releases) and unpack it anywhere.
2. Get your own `crusnusa.zip` (the MAME ROM set). **It is not included.**
3. Run `cruisn_usa.exe`, point it to the zip when asked, press **PLAY**.
4. `5` coin, `1` start, arrow keys steer, `Left Ctrl` gas, `Left Alt` brake, `Z X C V` gears, `P` options, `Esc` quit.

Needs Windows 10 / 11 (64 bit) and a graphics card with OpenGL 4.5. Wheels, pedals, shifters and pads are bound under
**Controls**.

## Status

**Works**

- All 14 tracks and the complete "Cruise the USA" tour, with sound; high scores and operator settings are saved
- OpenGL renderer at up to 8x the arcade resolution, in 4:3, 16:9 and 21:9
- Keyboard, Xbox pads with rumble, DirectInput wheels, pedals and shifters, several devices at once
- Force feedback on wheels (developed on a Fanatec ClubSport base)
- Options menu inside the game, usable with keyboard, pad or wheel alone

**Does not work yet**

- Link play (Head 2 Head): the network transport is missing
- Texture export and texture packs: unfinished, switched off in the menu
- Higher frame rates: a race is drawn at the arcade's rate of about 29 pictures per second; there is no 60 fps mode
- Vulkan and the CPU renderer run, but are less tested and get a small pause menu instead of the options
- ROM revisions other than 4.5 can be selected but are largely untested
- Motion cabinet hardware, other V-Unit games (Cruis'n World, Off Road Challenge, War Gods), other operating systems

**Known problems**

- NVIDIA cards at 2x-4x internal resolution: now and then a picture arrives one refresh late (0.1-0.4 %)
- Not yet run on real hardware: pad-only menu operation, displays above 60 Hz, AMD and Intel graphics

## What the port adds

**Picture**

- GPU rendering at 1x-8x of the arcade's 512x400, FXAA, optional texture filtering
- Real widescreen: the 3D view is widened and the game's culling extended instead of stretching the image; the HUD
  can stay in the centre or move to the edges
- Draw distance from 25 % to 400 %
- Soft shadows in place of the arcade's dithered ones (the original style is selectable)
- Display sync: the game follows the display's refresh, so every picture is shown equally long - no judder from the
  board's 57.9 Hz
- Low input lag: the graphics driver stays at most one frame behind the game
- Window, borderless or fullscreen on any monitor; the CPU renderer is the pixel-exact reference

**Game**

- Steady cadence: the emulated CPU gets the time a picture needs, so the arcade's slowdowns in busy scenes are gone
  (can be turned off in `cruisn.ini` for the original machine, slowdowns included)
- Rubber banding of the opponents adjustable from none to 150 %
- Operator adjustments (difficulty, start time, free play, MPH / KPH ...) and DIP switches edited in the menu, no
  service menu needed
- Power-up tests skipped on request

**Controls**

- Free binding of keys, buttons, hats and axes per device; dead zones, steering range, keyboard steering tuning
- Shifter modes: sticky, toggling, sequential, H-pattern
- Force feedback in two modes: *Vanilla* sends exactly the force the arcade computes; *Modern* builds it from the
  game's car physics - aligning torque, kerbs, bumps, collisions from every side, roadside objects, off-road surface,
  spin-outs, landings, engine, understeer - each adjustable

**Menu and cabinet**

- A launcher, and the same options inside the game (`P` or the pad's Home button). Most changes apply at once, so
  force feedback or picture settings can be tried without leaving the race
- Cabinet lamps and wheel motor as outputs for MameHooker (Windows messages) and Hook Of The Reaper (TCP)
- Hotkeys: `F3` anti-aliasing, `F4` shadows, `F5` / `F6` resolution, `F7` texture filter, `F8` VSync, `F9` scaling,
  `F2` test menu

**Under the hood**

- TMS320C31 interpreter, V-Unit video and DCS sound (ADSP-2105) as freestanding code, WASAPI audio
- Changes to the game are small patches in the RAM copy of the program, located by instruction pattern with the
  original source as the reference; the ROM files stay untouched
- Built-in profiling; the measurements behind the timing decisions are in [pc/docs/PERFORMANCE.md](pc/docs/PERFORMANCE.md)

## Help wanted

This port has been tested on very few machines, so every other setup is news. Please open an
[issue](https://github.com/Endprodukt/cruisin-usa-pc/issues) for

- crashes, hangs, graphics or sound faults (track, place and a screenshot help a lot)
- wheels, pedals, shifters and pads: what works, what does not, how the force feedback feels
- performance on your hardware, especially AMD and Intel graphics and displays above 60 Hz
- differences to the real cabinet

Include your graphics card, the renderer and your `cruisn.ini`. Pull requests are welcome too.

## Building

Visual Studio 2022 and CMake 3.20 or newer; all libraries are in the repository.

```
cmake -S pc -B pc/build
cmake --build pc/build --config Release
```

The port lives in [`pc/`](pc). Everything else in this repository is the game's original 1994 source code, unchanged.

## Credits

- **Cruis'n USA** was created by **Eugene Jarvis** and the team at **TV Games, Inc.**, built and published by
  **Midway Manufacturing Company** under licence from **Nintendo**. Logo and cabinet artwork (signed Youssi) are theirs.
- **[historicalsource](https://github.com/historicalsource/cruisin-usa)** preserved and published the game's
  TMS320C31 assembly source. This repository is a fork of it; the source was the reference for every fix, the
  widescreen, the draw distance and the force feedback.
- **[MAME](https://www.mamedev.org)** - **Aaron Giles** and the MAME contributors. Their V-Unit driver, TMS3203x and
  ADSP-21xx CPU cores and DCS audio emulation are the foundation of the hardware side. Without their decades of
  documenting this board there would be no port.
- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut (menus),
  **[miniz](https://github.com/richgel999/miniz)** by Rich Geldreich (zip, PNG),
  **[volk](https://github.com/zeux/volk)** by Arseny Kapoulkine and the Khronos **Vulkan headers**.
- Port by **Endprodukt**, 2026, written with [Claude Code](https://claude.com/claude-code).

## Licence

- The port's code in `pc/` is under the **BSD 3-Clause** licence, like the MAME code it builds on: see [LICENSE](LICENSE).
- Third-party libraries keep their own licences: [pc/docs/release/LICENSES.txt](pc/docs/release/LICENSES.txt).
- The game is **not** covered by any of this. Its source code in this repository is © 1994 TV Games, Inc.; its ROMs,
  name and artwork belong to their owners, and no ROM is distributed here.

This is a fan project, not affiliated with or endorsed by Midway, Nintendo or their successors.
