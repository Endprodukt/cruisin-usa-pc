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

## Frame time spikes (October 2026)

Question: why does the port still hitch now and then, at a high internal resolution and a long draw distance, although the
average frame takes a few milliseconds? Measured in the real app, paced, with the user's configuration (RTX 3060, i5-13600K,
3440x1440 at 60 Hz, OpenGL, 6x internal resolution, 21:9, draw distance 300 %, modern shadows, display sync), an autopilot
driving one race per run on Chicago, LA Freeway, San Francisco and Golden Gate Park.

### The tools

`--perf` now records every displayed frame (`FrameProf` in `src/perf.h`): frame time, window messages, input, emulation split
into main CPU / sound DSP / GPU hand-over, force feedback, pacing wait, present with the swap call on its own, GPU time (OpenGL
timer query, Vulkan timestamps), queued audio, polygons, draw calls, the game's mode, whether a new game picture was shown,
the emulated clock and game pictures that were late. A frame over the limit (`--perf-spike <ms>`, default 1.5 x median) goes to
`perf_spikes.log` with the eight frames before and six after it; at the end the log gets the percentiles, the phase averages
and maxima and the cadence of the game's pictures. `--perf-csv <file>` writes every frame, `--perf-seconds <n>` ends the run,
`--track <n>` and `--autopilot` (with `--autoplay`) pick and drive a race. The headless tool has `NFLOG=1` (vblanks per game
frame), `QUADSTAT=1` (screen area of the polygons and of their bounding boxes) and `SURFLOG`.

What counts is not the frame time of the host but the interval between two new game pictures on the display: the game draws
a picture every second refresh (28.9 per second on the machine, 30 with display sync at 60 Hz).

### Findings, by weight

| # | Cause | Kind | Effect before | Change |
|---|---|---|---|---|
| 1 | Soft shadows: every pixel of a shadow batch's rectangle read the mask 49 times, at the full internal resolution | GPU, in traffic and with cars near the camera | GPU time up to 22 ms at 6x: runs of pictures 3 refreshes apart (LA Freeway: 11 in 92 s, audio buffer down to 0) | mask at no more than twice the arcade resolution, blurred there, read once per page pixel: GPU max 11.9 ms, no late picture |
| 2 | Display sync was paced by a timer set to the nominal refresh rate, not by the display | pacing | timer and display drift against each other; every 15-20 s they cross and pictures are shown for 10 and 22 ms in turn for a moment | the buffer swap's wait for the vblank is the pace; the clock only takes over when the swap turns out not to wait |
| 3 | The emulated CPU did not finish a game frame within its two vblanks | game cadence (emulated time) | pictures 3 vblanks apart: at the original draw distance up to 7 % of a race's frames (San Francisco 272 of 3700; the machine does the same), at 400 % 5 % on San Francisco even at three times the clock | `MidVUnit::catch_up`: before each vblank the CPU gets the instructions the frame still needs, in no emulated time, within 10 ms of host time per display frame. All 14 tracks at 100 / 300 / 400 %: 1-3 slow frames per race (the start). The frames after the finish line stay at 3 vblanks: there the game sets its governor to that |
| 4 | Chicago did not start as a single race with a draw distance above 100 % | bug found on the way | the car never left the garage (the wider object window put other streets' road pieces under it) | the original window applies until the race mode is set |
| 5 | One stall of 50-70 ms about 30 s after the game window opens | OpenGL driver (NVIDIA), once per start | one picture 5 refreshes late | not ours: independent of window mode, sound, force feedback and game state, absent with Vulkan. Left as it is |
| 6 | Vulkan backend: stalls of 45-65 ms inside the polygon hand-over, in bursts | backend | not smooth at 6x | open (OpenGL is the default) |

Not a cause (measured): window messages (avg 0.03 ms, max 2.8), input polling (avg 0.08, max 4.4 once), force feedback and
outputs (0.00), sound DSP (0.8, max 3.1), GPU hand-over (0.00, max 0.3), allocations in the frame loop (none: the polygon
lists are reserved once), texture replacement and export (inactive unless used), release flags (`/O2 /Oi`, whole program
optimisation measured earlier: no gain).

### After (same runs, 92 s of race each)

| Track | pictures | at 2 refreshes | 3 refreshes | off the 33.4 ms pace by > 3 ms | emulation max | GPU max |
|---|---|---|---|---|---|---|
| Chicago 300 % | 2755 | 2750 | 2 | 1.2 % | 10.7 ms | 14.6 ms |
| LA Freeway 300 % | 2758 | 2756 | 0 | 1.5 % | 10.3 ms | 12.5 ms |
| San Francisco 300 % | 2757 | 2753 | 0 | 0.9 % | 10.7 ms | 10.9 ms |
| Golden Gate Park 300 % | 2758 | 2755 | 0 | 0.2 % | 9.8 ms | 10.8 ms |
| San Francisco 400 % | 2757 | 2753 | 1 | 0.3 % | 10.7 ms | 10.1 ms |
| Chicago 400 % | 2757 | 2755 | 0 | 1.1 % | 10.6 ms | 13.7 ms |

(Each run also contains the one driver stall of finding 5.) Before, with the old shadows: LA Freeway 11 pictures at 3
refreshes, 2.5 % off pace, GPU max 22.4 ms.

### Where the time goes now

Per display frame: main CPU 3.5-5.9 ms on average, up to the 10 ms budget in the frame that computes a game picture and about
0.4 ms in the other one; sound DSP 0.8 ms; everything else on the host below 0.2 ms. The game is CPU-bound on the emulated
side (the interpreter), GPU-bound only through the shadow pass that is now gone, and was pacing-bound through the timer.
With the internal resolution at 3x instead of 6x the GPU time barely changed before the shadow fix, which is what pointed at
the shadow pass instead of the polygons.

### Left as it is, with reasons

* Polygons are drawn as their bounding rectangles and cut to shape in the fragment shader. The polygons cover 62-84 % of
  those rectangles (QUADSTAT), so drawing the polygon's outline instead would save a fifth to a third of the fragment work.
  The GPU has room at 6x on this card; at 8x or on a weaker card it would be the next thing to do.
* `steady_budget_ms` (10 ms) is the share of a display frame the interpreter may use. A host that cannot do a game frame's work
  in two such shares shows the picture a vblank later, as the machine would; the display frame itself is never made late.
* `[game] steady_cadence = false` restores the machine's own slowdowns (and the fixed clock of the section above).

