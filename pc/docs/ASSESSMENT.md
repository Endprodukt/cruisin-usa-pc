# Cruis'n USA PC port - assessment & plan

The historicalsource repo is the original TMS320C30 game *source* (assembly); there is no
toolchain (TI C3x tools) to rebuild it, and the game needs the Midway V-Unit hardware
(TMS320C31 + video DMA rasterizer + ADSP-2105 "DCS" audio). This port therefore hosts the
original ROM code on a freestanding re-implementation of the V-Unit (hardware behaviour
per MAME's midvunit driver, BSD-3-Clause), so that we can freely add PC features.

## Architecture (pc/)
- `src/cpu/tms320c3x`   freestanding port of MAME's TMS320C3x core (page-table bus)
- `src/machine`         V-Unit: memory map, IRQ/timers, ADC, CMOS, wheel board, video DMA, soft rasterizer
- (next) `src/cpu/adsp2100` + `src/audio/dcs`  DCS sound (ADSP-2105) -> WASAPI
- (next) `src/video`    IVideoBackend: OpenGL + Vulkan (GPU quad rendering, N x internal res,
                        texture export/replace), soft renderer kept as reference
- (next) `src/input`    keyboard / XInput (rumble) / DirectInput wheel (FFB from WHLCTLZ latch)
- (next) `src/outputs`  lamps via Windows messages + network
- (next) launcher (options, ROM path, vsync, resolution)

## Feasibility
- 60 fps: the game is frame-governed (FRAMRATE min-frames, variable timestep via NFRAMES);
  native hardware runs ~29 fps in game. Achievable by patching the governor + CPU overclock.
- Upscaling to 8x: video is a list of textured quads (DMA queue) -> trivially GPU-rendered.
- Texture export/replace: 4 MB 8-bit paletted texture RAM addressed per quad -> hash by base/size.
- FFB/rumble/lamps: WHLCTLZ / DRVCTLZ latches are already decoded in `MidVUnit`.
