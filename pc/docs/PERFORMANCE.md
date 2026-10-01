# Performance notes

Measure with `cruisn_usa.exe --perf` (writes `perf.log` next to the exe every 5 s and shows the line in the title bar):
fps, frame avg/max, 1 % / 0.1 % low, emulation time split (main CPU / sound DSP / feeding the GPU), present time,
GPU time (Vulkan timestamps), draws and quads per frame, and the slowest frames. `--bench` runs frames back to back (no pacing).
`cruisn_headless` with `NORASTER=1 CPUTIME=1` is a CPU-only benchmark of the interpreter, `PROFILE=1` a sampling profiler.

Findings and changes (RTX 3060, 8x internal resolution, 21:9, FXAA):

| Problem | Cost | Change |
|---|---|---|
| debug `getenv()` calls for every drawn quad (5 per quad) | ~2-3 ms per frame of the ~7 ms emulation time | removed |
| game idles in a polling loop and in a "sort objects until the interrupt" loop | ~30 % of all time slices | recognised by instruction pattern, rest of the slice skipped (`MidVUnit::setup_idle_hooks`) |
| game code in the CPU's internal RAM went through the slow bus path | ~5 % | mapped as a fast page |
| Vulkan waited for the GPU at the end of every frame | 3-10 ms CPU stall | two frames in flight (command buffer, fence and buffer halves per frame) |
| latch copy of all 512 page rows | ~20 % of the copy | only the visible rows |
| shadow mask pass cleared the whole mask image | bandwidth | render area limited to the shadow bounding box |

Measured: emulation 7-9 ms -> ~2.5 ms per frame in the same race; Vulkan with VSync on reaches the display rate.
Whole-program optimisation (/GL /LTCG) was tried and gave nothing.
The game logic is tied to the arcade's 57.9 Hz frame clock (time step based on the vblank count); it was not changed.

## Smooth frames (withdrawn; experimental `--smooth 1` only)

**Not usable as it is:** the process dispatcher (MPROC.ASM `NEXTPRC`) counts every `SLEEP n` down by one per game frame, not by
the vblanks that passed, and ~290 SLEEP calls plus other per-frame counters time the game's sequences. With a frame per vblank
they all run twice as fast (race start countdown, text, attract sequences). A correct 57 fps display needs the game to keep running at
its own rate and the host to interpolate between game frames. The option was removed from the launcher and the ini file.

The main loop (CUSA.ASM `MAINLOOP`) waits until `INFRAMES >= FRAMRATE` before it requests the page swap, and the swap itself waits for
the next vblank, so FRAMRATE is "minimum vblanks per frame - 1". Races (`INIT_GAMELEG`) and the head-to-head logo set it to 2, the
selection screens to 1; in both cases the arcade CPU also needs more than one vblank per frame. Every movement is scaled by
`NFRAMES` (vblanks since the last frame), so the option rewrites all `LDI 1/2,R0 / STI R0,(FRAMRATE)` to 0 (the attract mode's value;
the address is taken from the MWAIT0 loop) and runs the emulated CPU at twice the clock (`cpu_overclock = 2`).

| Screen | Original | Smooth |
|---|---|---|
| race | 28.5 fps (NFRAMES always 2) | 57 fps (NFRAMES always 1) |
| car / track selection | ~19 fps | 57 fps |

Same autoplay start, speed after 100/200/300/400/500 frames: 33.9/110.2/175.1/223.1/261.3 original, 33.3/109.2/173.6/221.3/259.9
smooth (the remaining difference is the coarser original step). Cost: +0.75 ms CPU per frame. The modern force feedback skips calls
in which the game state did not change and normalises its per-frame thresholds to two vblanks, so it feels the same at either rate.

## Game cadence with a longer draw distance (`MidVUnit::update_clock`)

A longer draw distance keeps several times more scenery alive; the emulated CPU then needs 4 vblanks per game frame instead of 2
(measured: 14 instead of 28.5 game frames per second with 400 %). Everything the game counts in frames (ready - set - go, the flag
girl, logos, the end of a race) runs at half speed and the picture stutters. The CPU clock therefore follows the situation, decided
once per frame: x3 (x2 below 300 %) where the game's own governor limits the frame rate anyway (FRAMRATE >= 1: races, attract
drive), 1 + 0.25 per 100 % where it does not, x1 while no scenery is loaded. Page flips per 120 vblanks, original / 400 % before /
400 % now: race 60 / 30 / 60, first selection screen 59 / 35 / 58, end of race 40 / 40 / 40, attract drive 40 / 30 / 40.

## Frame pacing (`[video] display_sync`)

The machine draws 57.9 pictures per second. On a 60 Hz display that means a repeated picture about twice a second with VSync, or
tearing and a `Sleep(1)`-grained wait without. Display sync computes exactly one game frame per display refresh (per two or three
on faster displays) when the display rate is within 6 % of a multiple of 57.9 Hz; the game then runs at the display's pace (+3.6 % at
60 Hz, sound included). Without it the wait is now exact (sleep, then spin for the last 2 ms): frame times 16.65 +- 0.2 ms.

## Multithreading

Per-frame work (bench, race, 1x CPU): main CPU interpreter 1.29 ms, sound DSP 0.64 ms, GPU feed 0.08 ms, present 0.06 ms; the GPU
itself runs asynchronously (two frames in flight). Already on their own threads before this change: audio output, force feedback
(250 Hz), the GPU driver.

| Area | Share | Parallel? | Verdict |
|---|---|---|---|
| main CPU (TMS320C31 interpreter) | ~60 % | no: one instruction stream, every memory access can depend on the last | stays serial |
| sound DSP (ADSP-2105) | ~30 % | yes: the main CPU only writes command bytes and the reset line, it never reads the board back except "latch full" before a write | **worker thread** |
| software rasteriser (CPU renderer only) | ~0.4 ms | only with deferred drawing; the game reads video RAM back | not worth it |
| GPU feed / present | < 0.2 ms | already asynchronous | nothing to do |
| texture export (PNG encoding) | several ms per new block, hitches | yes, fire and forget | **writer thread** |
| replacement pack loading (PNG decode, resample) | startup, ~1.5 s for 60 x 1024 px | yes, per file | **parallel at startup** |
| per-frame telemetry / FFB synthesis | microseconds | | not worth it |

### 1. Sound DSP worker (`src/machine/dcs_worker.{h,cpp}`)

`MidVUnit::sync_dcs` still computes the DSP's time slice after every main CPU slice exactly as before, but hands it to a persistent
worker through a single-producer / single-consumer ring (atomic indices, no lock). Before anything that needs the board itself
(command byte with its latch check, reset, end of frame) the main thread drains the ring and then uses the board directly. The DSP
therefore sees the same operations in the same order: the sound output is bit-identical (same SHA-256 of a 6000-frame WAV with and
without the worker). The worker spins briefly while slices are coming and sleeps on a condition variable between frames; its PCM output
is buffered and handed to the audio callback on the main thread at the end of the frame (the same frame as before, so no added
latency). `drain()` runs the remaining slices itself when the worker is not inside one, so a sleeping worker never holds up a frame.
Switch: `[game] dsp_thread = -1/0/1` (auto = 4+ hardware threads), `--dcs-thread`.

**Default: off.** In back-to-back runs it is a clear gain (below), and in a paced headless run (one frame per 17.27 ms) no frame is slow
after start-up. In the app with VSync, however, single frames still take 15-18 ms of emulation every few seconds (one Windows scheduler
quantum: max frame 33 ms instead of 22 ms, 1 % low ~30 fps instead of ~46 fps). Until that interaction with the app's other threads
(audio output, force feedback, GPU driver) is understood, the inline path stays the default; the frame time at 57 Hz is far below the
budget either way (~2-3 ms of 17.3 ms).

| Measurement (i5-13600K) | Off | On |
|---|---|---|
| headless, wall time per frame | 2.19 ms | 1.63 ms |
| app `--bench`, frame time | 2.20 ms (454 fps) | 2.01 ms (499 fps) |
| app `--bench --smooth 1`, frame time | 3.35 ms (299 fps) | 2.77 ms (361 fps) |
| 1 % low (bench) | 191 fps | 193 fps |

### 2. Texture export writer

`TexRepl::write_dump` converts the block on the emulation thread and posts it to one persistent writer thread (mutex + condition
variable, only touched when a new block is exported). Same files as before (identical file names, no duplicates); the destructor
waits until everything is written.

### 3. Replacement pack loading

Decoding and resampling are independent per file and run on up to 8 threads for the duration of the load (startup only, no threads
per frame). 60 textures at 1024 px: 1497 ms on one core, 529 ms on four, 275 ms on all.
