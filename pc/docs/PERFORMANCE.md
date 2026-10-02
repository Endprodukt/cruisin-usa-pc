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
| 3 | The emulated CPU did not finish a game frame within its two vblanks | game cadence (emulated time) | pictures 3 vblanks apart: at the original draw distance up to 7 % of a race's frames (San Francisco 272 of 3700; the machine does the same), at 400 % 5 % on San Francisco even at three times the clock | `MidVUnit::catch_up`: a frame that is due and would be late gets the instructions it still needs, in no emulated time (reworked in the second pass, see the audit below; the first version acted before every vblank within 10 ms of host time). All 14 tracks at 100 / 300 / 400 %: 1-3 slow frames per race (the start). The frames after the finish line stay at 3 vblanks: there the game sets its governor to that |
| 4 | Chicago did not start as a single race with a draw distance above 100 % | bug found on the way | the car never left the garage (the wider object window put other streets' road pieces under it) | the original window applies until the race mode is set |
| 5 | One stall of 50-70 ms about 30 s after the game window opens | OpenGL driver (NVIDIA), once per start | one picture 5 refreshes late | found in the second pass (below): the driver's reaction to the first new thread in the process; taken before the first frame now |
| 6 | Vulkan backend: stalls of 45-65 ms inside the polygon hand-over, in bursts | backend | not smooth at 6x | found and fixed in the second pass (below): `vkWaitForFences` at a frame's first upload |

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

Per display frame: main CPU 3.5-5.9 ms on average, up to 10-12 ms in the frame that computes a game picture and about
0.4 ms in the other one; sound DSP 0.8 ms; everything else on the host below 0.2 ms. The game is CPU-bound on the emulated
side (the interpreter), GPU-bound only through the shadow pass that is now gone, and was pacing-bound through the timer.
With the internal resolution at 3x instead of 6x the GPU time barely changed before the shadow fix, which is what pointed at
the shadow pass instead of the polygons.

### Left as it is, with reasons

* Polygons are drawn as their bounding rectangles and cut to shape in the fragment shader. The polygons cover 62-84 % of
  those rectangles (QUADSTAT), so drawing the polygon's outline instead would save a fifth to a third of the fragment work.
  The GPU has room at 6x on this card; at 8x or on a weaker card it would be the next thing to do.
* `[game] steady_cadence = false` restores the machine's own slowdowns.

## Second pass: timing audit, the two stalls, polygons, scaling (October 2026)

A critical look at the first pass. Nothing here speeds up the emulated hardware; where the port deviates from the machine it
says so. All numbers are from this machine (RTX 3060, i5-13600K, 3440x1440 at 60 Hz) and the configuration named above.

### New tools

* `src/calltrace.h`: every OpenGL / Vulkan call and the buffer swap is timed; a call over the limit (`--call-ms`, default 2) is
  logged with frame, time since the start, function, duration, render phase, the phase's object and data size, and the call's
  arguments (`perf_calls.log`, `--call-log <file>`).
* `src/platform/stall_watch.{h,cpp}`: a watcher thread that sleeps on a timer the frame loop pushes back every frame. When a
  frame is overdue (`--stall-ms`, default 28) it stops the frame loop's thread for a moment, walks its stack (module + nearest
  export per frame), does the same once for every other thread of the process, and lists threads and modules that have
  appeared since. `THREADAT=<frame>,..` starts a do-nothing thread in those frames (to reproduce the driver stall below).
* `STATELOG=<csv>` in the headless tool: every `STATEEVERY` vblanks the emulated cycles, executed instructions, pictures,
  mode, the game's timers, its random number, the player's car and hashes of the work RAM, to compare runs bit by bit.
* `QUADSTAT=1` / `POLYLOG=<csv>` in the headless tool: per game picture the polygons by shape and, for 1x to 8x, the pixels
  of their bounding rectangles and of the polygons themselves, computed with the fragment shader's own coverage rules.
* The profiler's own logs are now written at the end of the run. One of the stalls it reported was its own `fflush`.

### 1. Timing audit of the steady cadence

What the first pass did was too much. It gave the CPU extra instructions before *every* vblank and limited them by host time.
That kept the cadence, but it moved a frame's work in front of the frame's first vblank (the interrupt's side effects were
then seen a vblank later by code that used to run after it), and its result depended on how fast the host was. The audit
below showed the difference from frame 2050 on, in the countdown before the first race.

`MidVUnit::catch_up` now acts only on a game frame that is due at this vblank (INFRAMES >= FRAMRATE) and has not asked for
its page swap yet, i.e. a frame the machine would deliver a vblank late. It runs the instructions that frame still needs (at
most four vblanks' worth, counted in instructions) while the emulated clock stands still. A frame that is in time is not
touched. With a draw distance above 100 % the fixed clock of the section "Game cadence with a longer draw distance" applies
again (x2, x3 from 300 %), because the frames there are late by more than a rescue should cover.

The questions, with the measurements (headless, default save file, autopilot, the same inputs every run, 10 500 vblanks,
draw distance 100 %, state logged every 25 vblanks; `--steady 0 / 1 / 2`):

| | machine timing | steady cadence (due frames) | first version (every vblank) |
|---|---|---|---|
| **Golden Gate Park**: frames of 3 vblanks in the race | 1 | 0 | 0 |
| instructions executed | 4 522 856 721 | 4 536 551 024 (+0.30 %) | 5 753 843 190 (+27.2 %) |
| emulated cycles | 4 522 837 209 | 4 522 666 720 (-0.004 %) | 4 523 074 746 (+0.005 %) |
| state (RAM hashes) first differs from the machine at vblank | | 7150 (the frame the machine delivers late) | 2050 |
| at vblank 5000: random number, car position | C4065DFD, (-561257, 5326, 373236) | identical | 377881BE, (-652777, 3656, 507783) |
| at vblank 10 000: game timer / pictures shown | 2.248636 / 4436 | 2.248636 / 4439 | 2.249221 / 4301 |
| **San Francisco**: frames of 3 vblanks in the race | 270 | 0 | 0 |
| instructions executed | 4 523 222 682 | 4 965 715 736 (+9.8 %) | 6 168 368 675 (+36.4 %) |
| state first differs at vblank | | 2050 (the machine is already late before the start) | 2050 |
| game timer per 2500 vblanks (5000 minus 2500) | 0.730947 | 0.730947 | 0.730947 |
| at vblank 10 000: pictures shown | 4097 | 4304 | 4295 |

* **Is the emulated clock faster?** No. Emulated time only advances in `run_cpu`, by the same cycles per scanline as before;
  the rescue runs with the clock stopped. The cycle totals agree to 0.004 % (a time slice ends on an instruction boundary).
  Sound board, A-D converter and the CPU's timer registers read that clock.
* **More emulated work per real second?** More instructions, yes: 0.3 % (Golden Gate Park) to 9.8 % (San Francisco) at the
  original draw distance, all of them in frames that would have been late. That is the one deviation, and it is deliberate.
* **Do gameplay, AI, physics, timers or random numbers run faster?** Timers: no. The vblank interrupt counts INFRAMES, the
  one-second timer, the race countdown and GAME_TIMER, the same number of times per second (the table's game timer per 2500
  vblanks). Physics and AI: the game scales every movement by NFRAMES, the vblanks the last frame took, so a stretch that the
  machine integrates as one step of 3 vblanks is integrated as steps of 2: the same race time, a slightly different
  trajectory. Random numbers are drawn per game frame, so their sequence, and with it the traffic, differs from the first
  rescued frame on. Until then the run is bit-identical to the machine (Golden Gate Park: 7150 vblanks, two minutes).
* **Interrupt order?** Unchanged in a frame that is in time. In a rescued frame the vblank interrupt comes after the frame's
  work instead of in the middle of it, which is where it comes in every frame that is in time.
* **Extra emulated time?** None: no vblank and no cycle is added.
* **Deterministic?** Yes: two runs of each mode are identical in every logged value. The limit is counted in instructions;
  host time no longer enters (the 10 ms budget is gone).
* **Same race duration?** Yes: the race clock ticks per vblank. The San Francisco race *starts* six vblanks (0.1 s) earlier,
  because the machine is slow in the frames before the green light as well.
* **CPU cycle counts used for timing?** The two timer registers of the TMS320C31 (the game uses them for its own profiling
  record only), the sound board's command timing and the A-D converter. All read emulated time; none sees the rescue.

So the steady cadence is not the machine: it is the machine without its own slowdowns. `[game] steady_cadence = false`
(`--steady 0`) is the machine exactly as it is, slow frames included (San Francisco: 7 % of the race's frames).

Cadence with the reworked rescue, all 14 tracks, frames of 3 or more vblanks in 12 000 vblanks: 100 %: 476, 300 %: 420,
400 %: 428, of which 460 / 396 / 400 are LA Freeway's frames after the finish line (there the game sets its governor to 3
vblanks; that is the machine's pace, not a slowdown). Every other track: 1 to 3 per race. Stability: all 14 tracks at 100 %
and 400 % and the whole tour (14 legs) at 100 / 300 / 400 % run through without a reset.

### 2. The OpenGL stall (one frame of 50 to 100 ms, about 30 s after the start)

**The call that blocks is `SwapBuffers`.** Every other OpenGL call of the frame returns in microseconds (376 000 calls
timed per run; apart from the swap, which waits for the display, none takes 2 ms after the first frames).

**What it waits for** (stall watch, the stacks at the moment of the stall):

    frame loop:     SwapBuffers > OPENGL32!wglSwapBuffers > nvoglv64!DrvSwapLayerBuffers > WaitForSingleObject
    driver thread:  nvoglv64!DrvSwapLayerBuffers > ... > win32u!NtGdiDdDDIWaitForSynchronizationObjectFromCpu

The NVIDIA driver runs OpenGL on a thread of its own; the application's swap waits for that thread, and that thread waits in
the kernel for a GPU synchronisation object (D3DKMTWaitForSynchronizationObjectFromCpu) for four to six refreshes.

**What triggers it** is not anything the game does, and not a timer of ours (nothing in the port runs on a 30 s or
1800-frame period). It is the first thread that starts in the process after the driver has begun to present:

| Run | Result |
|---|---|
| as before | `SwapBuffers` 70 to 100 ms at 27.5 s, in every run; at that moment two Windows thread pool workers appear in the process (every run, to within 10 ms) |
| a do-nothing thread started in frame 600 (`THREADAT=600,900`) | the stall is in frame 600; none at frame 900, none at 27.5 s |
| the same in frame 5 | the stall is in frame 7 (52 to 55 ms); none later |
| an idle thread that exists from the first frame on | no stall at 27.5 s (the thread pool workers still appear) |
| a thread started before the video backend exists | stall at 27.5 s as before |
| a polling stall watcher (the first version of the tool) | no stall: the tool's own thread had triggered it in the first frames |

So about 30 s after the start Windows starts thread pool workers in the process (for whom was not determined: they were
idle again within 15 ms), the driver reacts to the first new thread it sees, once, and the reaction costs one swap of 50 to
100 ms. Independent of window mode, sound, force feedback and the game's state, as found in the first pass; not present with
Vulkan.

**Change:** `driver_warmup` in `main_win.cpp` starts a thread that does nothing, keeps it alive across six presents of the
first picture, and ends it, right before the frame loop begins (after the loading picture has gone, with the swap interval
set; earlier, behind the loading picture, it has no effect). The driver's reaction happens there, on a still picture.
`--no-warmup` leaves it out. After: no frame over 29 ms in three races of 82 s, every game picture two refreshes after the
one before (Chicago 2457 of 2457, LA Freeway 2457 of 2457, San Francisco 2456 of 2456).

What the driver does in that wait is not known; trigger, call and wait are measured.

### 3. Vulkan

**The call that blocks is `vkWaitForFences`**, in `begin_cb()` at the first upload of a game frame (the palette), where the
backend waited for the fence of the frame submitted two presents earlier before reusing that frame's buffers:

    Chicago, 6x, 62 s of race: 333 waits of more than 20 ms in the palette upload, 39 in the polygon hand-over;
    average 39 ms, longest 59 ms; `vkQueuePresentKHR`, `vkAcquireNextImageKHR` and `vkQueueSubmit` never over 2 ms.
    Game pictures: 408 after 1 refresh, 1188 after 2, 124 after 3, 136 after 4. 38 % off the pace by more than 3 ms.

With a FIFO swapchain neither acquire nor present waits for the display on this driver; the wait is deferred into the
semaphore the frame's command buffer needs for its swapchain image, and so into that frame's fence. With two frames queued
the fence was released in bursts: nothing for seven frames, then 40 to 60 ms, in the middle of the emulated frame. No
`vkDeviceWaitIdle` or other global wait is in the frame path, and the forced mid-frame submits (a per-frame buffer running
full) are instrumented and do not appear among the slow calls. The implicit layers on this machine (RivaTuner, OBS) are not involved:
the same with `VK_LOADER_LAYERS_DISABLE=~implicit~`.

Measured alternatives (Chicago, 6x, 42 s of race, 1256 pictures):

| Where the wait is | pictures at 2 refreshes | longest frame | emulation over 12 ms |
|---|---|---|---|
| at the next-but-one frame's first upload (before) | 824 | 66 ms | 1554 frames |
| after present, for the frame before (two in flight) | 828 | 61 ms | 52 |
| after present, for this frame (none in flight) | 1243 | 54 ms | 1 |
| **in present, before acquiring: the frame before** (now) | **1254** | **30 ms** | 1 |

**Change:** `present()` waits for the previous frame's fence before it acquires the next image. The wait is then the
frame's pace (it ends with the display's vblank, once per frame, in the present phase); the GPU still renders one frame
while the CPU emulates the next. After: LA Freeway 2456 of 2457 pictures at 2 refreshes, San Francisco 400 % 2458 of 2458.
(`set_debug(16)` / `--gl-debug 16` restores the old behaviour for comparison.)

### 4. How the polygons are drawn, and what it costs

Both GPU backends draw one instance per hardware polygon: four vertices that span the polygon's bounding rectangle (grown by
one arcade pixel above 1x), and a fragment shader that decides per pixel whether it belongs to the polygon.

**Why rectangles.** The V-Unit's rasteriser is not a triangle rasteriser. It walks the polygon scanline by scanline, takes
the leftmost and rightmost edge crossing of each line, interpolates the texture coordinates along those two edges and then
linearly across the line, and fills the pixels whose centre lies in between, with its own rule for which edge pixels count.
A GPU's triangles interpolate over the plane of each triangle and use the top-left rule: a quad split into two triangles
gets a different texture mapping (the kink along the diagonal) and different edge pixels. So the fragment shader repeats
the hardware's walk for its own pixel (`scan()` in `shader_src.h`), and the geometry only has to *cover* the polygon. The
bounding rectangle is the simplest cover: no CPU work per polygon, one instanced draw call per page.

**What shapes there are** (race pictures, at 1x; share of the polygons / of the rectangles' pixels / how much of its
rectangles the shape fills):

| Shape | Chicago | LA Freeway | San Francisco |
|---|---|---|---|
| axis-parallel rectangle | 13 % / 31 % / 100 % | 9 % / 42 % / 100 % | 22 % / 47 % / 100 % |
| triangle (two corners equal, or three in line) | 14 % / 39 % / 9 % | 21 % / 11 % / 16 % | 10 % / 8 % / 25 % |
| convex quad | 18 % / 28 % / 64 % | 31 % / 46 % / 60 % | 17 % / 44 % / 76 % |
| concave quad | 0.3 % / 0.0 % / 15 % | 0.6 % / 0.1 % / 14 % | 0.3 % / 0.1 % / 23 % |
| self-crossing ("bowtie") | 0.1 % / 0.0 % / 6 % | 0.2 % / 0.1 % / 6 % | 0.2 % / 0.1 % / 12 % |
| no area (a line or a point: far objects) | 54 % / 1.8 % / 7 % | 38 % / 0.8 % / 24 % | 51 % / 1.3 % / 8 % |

(draw distance 300 %; at 100 % the shares of the pixels are the same within two points and the polygons without area are
35 / 21 / 27 % of the polygons.) Not all polygons are convex, but the ones that are not are at most 1.4 % of the polygons and 0.1 to
0.2 % of the pixels. The hardware's walk fills them from the leftmost to the rightmost crossing, so on the machine, too,
they are filled as if convex per scanline.

**The measurement** (`QUADSTAT=1`, `POLYLOG=<csv>`: one line per game picture with the polygon count, the shapes, and
rectangle and polygon pixels for 1x to 8x). Averages per race picture, in megapixels, page 896 x 512 arcade pixels (21:9):

| | polygons per picture | rectangles 1x / 6x / 8x | polygons 1x / 6x / 8x | discarded at 6x | polygons / rectangles (average; per picture min .. median) |
|---|---|---|---|---|---|
| Chicago 100 % | 2206 | 2.15 / 82.9 / 147.3 | 1.14 / 41.8 / 74.2 | 41.1 | 50 % (25 .. 49 %) |
| Chicago 300 % | 5000 | 2.20 / 86.7 / 154.2 | 1.16 / 42.6 / 75.8 | 44.1 | 49 % (28 .. 48 %) |
| Chicago 400 % | 5340 | 2.23 / 87.8 / 156.0 | 1.16 / 42.6 / 75.7 | 45.2 | 48 % (25 .. 48 %) |
| LA Freeway 100 % | 2262 | 1.61 / 63.4 / 112.7 | 1.16 / 42.5 / 75.5 | 20.9 | 67 % (53 .. 68 %) |
| LA Freeway 300 % | 5003 | 1.64 / 66.2 / 117.7 | 1.17 / 42.9 / 76.3 | 23.3 | 65 % (52 .. 66 %) |
| LA Freeway 400 % | 5922 | 1.65 / 66.9 / 119.0 | 1.18 / 43.4 / 77.2 | 23.5 | 65 % (53 .. 66 %) |
| San Francisco 100 % | 3464 | 1.42 / 56.7 / 100.8 | 1.17 / 42.8 / 76.1 | 13.9 | 76 % (50 .. 78 %) |
| San Francisco 300 % | 9012 | 1.46 / 60.9 / 108.2 | 1.19 / 43.9 / 78.0 | 17.0 | 72 % (49 .. 74 %) |
| San Francisco 400 % | 11621 | 1.49 / 63.3 / 112.6 | 1.21 / 44.5 / 79.1 | 18.8 | 70 % (48 .. 72 %) |

What follows from it:

* The polygons themselves cover the picture 3.2 times (1.15 Mpx of 0.36 visible), and that number hardly moves with the
  draw distance: what a longer draw distance adds is small on the screen. **The draw distance is paid for by the emulated
  CPU (2.4 to 3.4 times the polygons at 400 %), not by the GPU's pixels (polygons +2 to +4 %, rectangles +6 to +12 %).**
* The pixel work grows with the square of the internal resolution, and the share that is discarded is the same at every
  resolution above 1x (at 1x it is 3 to 10 points lower: no dilation border).
* Chicago is the worst case because of its triangles: 14 % of the polygons, 39 % of all shaded pixels, of which 91 % are
  thrown away. Long thin triangles (the diagonal of a rectangle) are what a bounding rectangle fits worst.

**Could the polygon's outline be drawn instead?** Yes, without touching the result: the fragment shader would still decide
every pixel, the geometry would only stop offering pixels that cannot belong. For that the cover must be conservative: the
convex hull of the four corners (not the polygon: concave and self-crossing ones are filled across), grown by one arcade
pixel (half a pixel of dilation above 1x, the hardware's inclusive right and bottom edge pixels, rounding to pixel
centres), and clamped to the page like the rectangle is now.

* Triangulation: the hull of four points is a triangle or a quad, i.e. 3 to 4 corners, 1 or 2 triangles; grown by a pixel
  with square corners it has at most 8. It can be built in the vertex shader from the same instance data (no CPU cost, no
  change to the draw calls) or on the CPU (four cross products and a sort per polygon; not measured, an estimate is well under 0.1 ms for 5000).
* Transparency, order and edges: unaffected. There is no depth buffer: the order is the order of the instances, and it
  stays one instance per polygon. Transparent texels and the dither pattern are `discard`s in the fragment shader, the edge
  rules are its scanline test; a tighter cover changes none of these, as long as it is a cover. The risk is exactly there:
  a hull that is too tight by a fraction of a pixel drops edge pixels, visible as seams between road pieces. It would have
  to be verified by comparing pictures pixel by pixel against the rectangle version (the headless tool can do that).
* What it would save: the discarded pixels above, 14 to 45 Mpx per picture at 6x (a quarter to a half of the fragments),
  minus the grown border. In time it is less than that share, because a discarded pixel is the cheap kind: it runs the
  scanline test and stops, it does not read the texture, the palette or a replacement. What the GPU's time per megapixel
  actually is, the scaling runs below show.

The renderer has not been changed.

### 5. Scaling: internal resolution 1x to 8x, draw distance 100 / 300 / 400 %

63 races in the real app (OpenGL, fullscreen 3440x1440, 21:9, modern shadows, display sync, autopilot; 33 s of race each,
about 957 game pictures), on LA Freeway, Chicago and San Francisco.

**How the GPU's time is measured, and why it has to be read with the clock.** The first pass measured one timer query from
swap to swap. That also counts the time the GPU waits for the CPU to hand over the next call, so it read 10 ms per picture
at 1x, where the GPU has next to nothing to do. `--perf` now puts a query around each render phase's own calls (polygons,
shadows, latch, present, uploads; columns `g_*` of the csv). And the GPU does not run at a fixed speed: the driver clocks it
to the load. During these runs `nvidia-smi` showed 210 MHz at 1x, about 450 MHz at 3x, 900 MHz at 6x and 1000 to 1150 MHz
at 8x, of 2160 MHz, at 28 to 40 % utilisation in every case and 17 to 37 W. So a phase's milliseconds are milliseconds at
that clock (the same present pass takes 8.4 ms at 1x and 1.2 ms at 8x), and the measure of how much of the card is used is
utilisation times clock: the column "load at full clock".

The columns: emulation per game picture (two display frames; in brackets the longest single frame, which has to fit into
one refresh of 16.7 ms); GPU time per picture by phase; the GPU's state; game pictures that did not come two refreshes
after the one before; 1 % and 0.1 % low of the picture rate (30 is perfect); the longest interval between two pictures
(33.4 ms is perfect).

**LA Freeway, draw distance 100 %** (2201 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 4.1 (3.7) + 1.5 ms | 0.7 + 0.6 + 0.4 + 8.4 = 10.2 (12.3 / 13.4) ms | 37 %, 210 MHz, 17 W: 4 % | 0 of 957 | 27.5 / 27.4 fps | 36.5 ms |
| 2x | 4.3 (3.9) + 1.6 ms | 1.6 + 0.7 + 1.1 + 7.2 = 10.6 (15.2 / 16.2) ms | 36 %, 379 MHz, 18 W: 6 % | 2 of 955 | 24.9 / 19.8 fps | 50.4 ms |
| 3x | 4.1 (4.0) + 1.5 ms | 2.6 + 0.6 + 1.2 + 4.4 = 8.8 (18.0 / 19.2) ms | 35 %, 451 MHz, 19 W: 7 % | 2 of 955 | 24.3 / 20.0 fps | 50.0 ms |
| 4x | 4.1 (5.2) + 1.5 ms | 3.7 + 0.6 + 1.6 + 4.3 = 10.2 (19.8 / 21.1) ms | 33 %, 525 MHz, 20 W: 8 % | 2 of 955 | 24.9 / 20.3 fps | 49.2 ms |
| 5x | 4.1 (4.3) + 1.5 ms | 4.9 + 0.5 + 2.1 + 4.4 = 12.0 (15.7 / 18.7) ms | 39 %, 615 MHz, 21 W: 11 % | 0 of 957 | 28.6 / 28.3 fps | 35.3 ms |
| 6x | 4.2 (3.9) + 1.6 ms | 4.7 + 0.4 + 2.5 + 4.1 = 11.7 (16.6 / 20.7) ms | 36 %, 907 MHz, 24 W: 15 % | 0 of 957 | 27.7 / 24.9 fps | 40.1 ms |
| 8x | 4.0 (3.7) + 1.5 ms | 7.4 + 0.3 + 1.3 + 1.3 = 10.2 (20.6 / 29.6) ms | 32 %, 969 MHz, 35 W: 14 % | 1 of 955 | 25.7 / 20.3 fps | 49.2 ms |

**LA Freeway, draw distance 300 %** (4728 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 8.4 (10.0) + 1.6 ms | 0.8 + 1.0 + 0.4 + 8.3 = 10.6 (13.0 / 16.1) ms | 38 %, 210 MHz, 17 W: 4 % | 0 of 957 | 27.5 / 27.4 fps | 36.6 ms |
| 2x | 8.2 (10.0) + 1.6 ms | 1.5 + 0.9 + 1.0 + 6.2 = 9.7 (14.9 / 16.6) ms | 35 %, 391 MHz, 18 W: 6 % | 3 of 955 | 24.4 / 20.0 fps | 50.0 ms |
| 3x | 8.1 (9.7) + 1.5 ms | 2.6 + 0.9 + 1.2 + 4.4 = 9.1 (18.9 / 26.3) ms | 32 %, 475 MHz, 19 W: 7 % | 4 of 955 | 22.6 / 17.9 fps | 56.0 ms |
| 4x | 8.6 (9.8) + 1.6 ms | 3.9 + 0.8 + 1.6 + 4.2 = 10.6 (20.5 / 22.2) ms | 36 %, 531 MHz, 20 W: 9 % | 1 of 956 | 25.8 / 19.9 fps | 50.2 ms |
| 5x | 8.2 (11.4) + 1.6 ms | 4.7 + 0.7 + 2.2 + 4.4 = 12.1 (18.4 / 22.0) ms | 39 %, 668 MHz, 21 W: 12 % | 0 of 957 | 28.6 / 28.5 fps | 35.1 ms |
| 6x | 8.1 (10.2) + 1.5 ms | 4.7 + 0.5 + 2.0 + 3.2 = 10.4 (16.9 / 24.7) ms | 33 %, 949 MHz, 26 W: 14 % | 0 of 958 | 26.3 / 25.0 fps | 40.0 ms |
| 8x | 8.0 (9.4) + 1.5 ms | 7.7 + 0.5 + 1.2 + 1.2 = 10.6 (21.5 / 31.5) ms | 34 %, 1024 MHz, 35 W: 16 % | 0 of 957 | 26.3 / 25.4 fps | 39.3 ms |

**LA Freeway, draw distance 400 %** (5653 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 9.3 (9.8) + 1.6 ms | 0.8 + 1.0 + 0.4 + 8.3 = 10.7 (13.1 / 13.9) ms | 39 %, 211 MHz, 17 W: 4 % | 0 of 958 | 27.5 / 27.4 fps | 36.5 ms |
| 2x | 9.2 (9.9) + 1.5 ms | 1.6 + 0.9 + 0.9 + 6.2 = 9.8 (15.3 / 16.9) ms | 33 %, 404 MHz, 18 W: 6 % | 2 of 957 | 24.4 / 18.8 fps | 53.2 ms |
| 3x | 9.6 (9.8) + 1.6 ms | 2.6 + 0.8 + 1.2 + 4.3 = 9.0 (18.2 / 23.8) ms | 27 %, 463 MHz, 19 W: 6 % | 4 of 955 | 22.4 / 17.8 fps | 56.1 ms |
| 4x | 9.3 (10.3) + 1.6 ms | 3.9 + 0.8 + 1.5 + 4.2 = 10.6 (20.4 / 28.1) ms | 34 %, 522 MHz, 20 W: 8 % | 3 of 956 | 23.6 / 18.2 fps | 55.0 ms |
| 5x | 9.3 (10.2) + 1.5 ms | 4.7 + 0.7 + 2.2 + 4.4 = 12.0 (14.4 / 19.2) ms | 39 %, 670 MHz, 21 W: 12 % | 0 of 956 | 28.6 / 28.5 fps | 35.0 ms |
| 6x | 9.5 (9.8) + 1.6 ms | 4.7 + 0.5 + 2.2 + 3.6 = 11.1 (15.5 / 22.2) ms | 35 %, 950 MHz, 25 W: 15 % | 0 of 957 | 26.9 / 25.4 fps | 39.4 ms |
| 8x | 9.3 (10.4) + 1.5 ms | 8.6 + 0.5 + 1.2 + 1.2 = 11.5 (27.6 / 31.4) ms | 32 %, 1018 MHz, 35 W: 15 % | 1 of 957 | 25.9 / 20.5 fps | 48.7 ms |

**Chicago, draw distance 100 %** (1866 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 4.3 (6.6) + 1.6 ms | 0.7 + 0.5 + 0.3 + 8.4 = 10.1 (12.3 / 13.7) ms | 37 %, 210 MHz, 17 W: 4 % | 0 of 956 | 27.5 / 27.4 fps | 36.5 ms |
| 2x | 4.3 (5.8) + 1.6 ms | 1.7 + 0.6 + 1.0 + 7.4 = 10.9 (15.2 / 19.3) ms | 37 %, 397 MHz, 18 W: 7 % | 0 of 956 | 27.0 / 25.0 fps | 39.9 ms |
| 3x | 4.3 (5.7) + 1.6 ms | 2.9 + 0.5 + 1.1 + 4.4 = 9.0 (17.4 / 18.1) ms | 28 %, 457 MHz, 19 W: 6 % | 5 of 952 | 21.6 / 18.0 fps | 55.4 ms |
| 4x | 4.4 (6.3) + 1.6 ms | 4.2 + 0.5 + 1.3 + 4.2 = 10.3 (19.2 / 21.9) ms | 34 %, 518 MHz, 20 W: 8 % | 0 of 956 | 26.8 / 25.7 fps | 38.9 ms |
| 5x | 4.2 (5.6) + 1.5 ms | 5.4 + 0.5 + 1.9 + 4.3 = 12.1 (17.2 / 19.5) ms | 39 %, 617 MHz, 21 W: 11 % | 0 of 956 | 28.5 / 27.7 fps | 36.1 ms |
| 6x | 4.2 (6.2) + 1.6 ms | 5.2 + 0.4 + 2.1 + 3.8 = 11.6 (17.5 / 20.2) ms | 36 %, 922 MHz, 24 W: 15 % | 0 of 956 | 27.7 / 25.7 fps | 39.0 ms |
| 8x | 4.3 (6.0) + 1.6 ms | 7.6 + 0.3 + 1.2 + 1.2 = 10.2 (21.1 / 31.1) ms | 32 %, 1147 MHz, 36 W: 17 % | 1 of 954 | 25.6 / 20.0 fps | 49.9 ms |

**Chicago, draw distance 300 %** (4440 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 8.5 (10.3) + 1.6 ms | 0.9 + 0.9 + 0.4 + 8.2 = 10.5 (12.8 / 14.1) ms | 38 %, 210 MHz, 17 W: 4 % | 0 of 957 | 27.5 / 27.2 fps | 36.7 ms |
| 2x | 8.3 (9.8) + 1.6 ms | 1.7 + 0.8 + 0.9 + 6.4 = 10.0 (15.4 / 16.0) ms | 34 %, 408 MHz, 18 W: 6 % | 2 of 956 | 24.3 / 20.0 fps | 50.1 ms |
| 3x | 8.7 (10.1) + 1.6 ms | 2.9 + 0.8 + 1.0 + 4.3 = 9.1 (17.8 / 23.3) ms | 28 %, 486 MHz, 19 W: 6 % | 6 of 954 | 21.9 / 17.9 fps | 55.9 ms |
| 4x | 8.6 (10.1) + 1.6 ms | 4.5 + 0.8 + 1.4 + 4.1 = 10.8 (19.3 / 21.6) ms | 35 %, 510 MHz, 20 W: 8 % | 0 of 957 | 26.9 / 25.7 fps | 38.8 ms |
| 5x | 8.5 (10.0) + 1.6 ms | 5.4 + 0.7 + 1.9 + 4.3 = 12.4 (19.2 / 21.0) ms | 39 %, 662 MHz, 21 W: 12 % | 0 of 958 | 28.4 / 27.9 fps | 35.8 ms |
| 6x | 8.8 (10.6) + 1.7 ms | 5.3 + 0.5 + 1.9 + 3.4 = 11.1 (18.6 / 23.5) ms | 35 %, 940 MHz, 26 W: 15 % | 0 of 957 | 26.6 / 25.4 fps | 39.4 ms |
| 8x | 8.5 (10.7) + 1.6 ms | 8.4 + 0.4 + 1.1 + 1.1 = 11.1 (27.7 / 30.6) ms | 30 %, 1137 MHz, 37 W: 16 % | 0 of 957 | 26.8 / 26.0 fps | 38.4 ms |

**Chicago, draw distance 400 %** (4843 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 8.0 (10.6) + 1.5 ms | 0.9 + 1.1 + 0.4 + 8.3 = 10.8 (13.1 / 17.0) ms | 38 %, 212 MHz, 17 W: 4 % | 0 of 957 | 27.4 / 27.3 fps | 36.7 ms |
| 2x | 8.3 (10.1) + 1.6 ms | 1.7 + 0.9 + 0.9 + 5.9 = 9.6 (15.1 / 19.8) ms | 33 %, 396 MHz, 18 W: 6 % | 0 of 956 | 26.8 / 25.7 fps | 38.9 ms |
| 3x | 8.2 (10.2) + 1.6 ms | 2.9 + 0.9 + 1.0 + 4.2 = 9.2 (17.9 / 24.5) ms | 31 %, 464 MHz, 19 W: 7 % | 4 of 955 | 23.0 / 18.2 fps | 54.9 ms |
| 4x | 8.0 (10.8) + 1.6 ms | 4.6 + 0.9 + 1.4 + 4.1 = 11.0 (20.6 / 25.2) ms | 36 %, 508 MHz, 20 W: 8 % | 1 of 957 | 26.1 / 20.1 fps | 49.6 ms |
| 5x | 8.6 (10.1) + 1.7 ms | 5.3 + 0.8 + 1.9 + 4.2 = 12.3 (19.3 / 21.4) ms | 39 %, 686 MHz, 21 W: 12 % | 0 of 957 | 28.5 / 27.8 fps | 36.0 ms |
| 6x | 8.2 (10.2) + 1.6 ms | 5.4 + 0.6 + 1.7 + 2.9 = 10.5 (19.7 / 24.2) ms | 32 %, 946 MHz, 27 W: 14 % | 0 of 957 | 26.6 / 25.6 fps | 39.0 ms |
| 8x | 8.1 (10.1) + 1.6 ms | 9.2 + 0.6 + 1.1 + 1.1 = 12.0 (25.9 / 30.5) ms | 33 %, 1099 MHz, 37 W: 16 % | 0 of 958 | 26.7 / 26.1 fps | 38.3 ms |

**San Francisco, draw distance 100 %** (3275 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 5.3 (5.3) + 1.5 ms | 0.7 + 0.4 + 0.4 + 8.4 = 10.0 (12.3 / 13.5) ms | 36 %, 210 MHz, 17 W: 4 % | 0 of 955 | 27.6 / 27.4 fps | 36.4 ms |
| 2x | 5.3 (5.7) + 1.5 ms | 1.7 + 0.5 + 1.1 + 8.3 = 11.8 (14.0 / 15.2) ms | 40 %, 346 MHz, 17 W: 6 % | 1 of 956 | 26.6 / 20.2 fps | 49.5 ms |
| 3x | 5.4 (5.2) + 1.6 ms | 2.4 + 0.4 + 1.1 + 4.7 = 8.7 (16.9 / 17.5) ms | 31 %, 443 MHz, 19 W: 6 % | 2 of 955 | 24.0 / 18.2 fps | 54.8 ms |
| 4x | 5.4 (5.3) + 1.6 ms | 3.2 + 0.4 + 1.4 + 4.4 = 9.5 (19.4 / 20.3) ms | 32 %, 530 MHz, 19 W: 8 % | 0 of 956 | 26.4 / 25.7 fps | 39.0 ms |
| 5x | 5.6 (5.3) + 1.6 ms | 5.3 + 0.4 + 1.9 + 4.3 = 12.0 (13.7 / 19.0) ms | 39 %, 488 MHz, 21 W: 9 % | 0 of 956 | 28.6 / 28.3 fps | 35.3 ms |
| 6x | 5.4 (5.5) + 1.6 ms | 4.9 + 0.3 + 2.5 + 4.5 = 12.3 (13.9 / 18.8) ms | 39 %, 763 MHz, 22 W: 14 % | 3 of 956 | 24.8 / 12.0 fps | 83.5 ms |
| 8x | 5.3 (4.9) + 1.5 ms | 6.3 + 0.2 + 1.4 + 1.5 = 9.4 (19.5 / 21.1) ms | 28 %, 1004 MHz, 34 W: 13 % | 0 of 956 | 26.8 / 26.2 fps | 38.1 ms |

**San Francisco, draw distance 300 %** (8640 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 12.2 (10.0) + 1.7 ms | 0.9 + 0.6 + 0.3 + 8.3 = 10.3 (12.7 / 13.7) ms | 37 %, 210 MHz, 17 W: 4 % | 0 of 958 | 27.5 / 27.2 fps | 36.8 ms |
| 2x | 11.9 (10.6) + 1.6 ms | 1.7 + 0.6 + 1.0 + 7.4 = 10.8 (14.4 / 15.3) ms | 38 %, 380 MHz, 18 W: 6 % | 0 of 957 | 27.6 / 27.2 fps | 36.8 ms |
| 3x | 11.8 (12.1) + 1.6 ms | 2.6 + 0.5 + 1.1 + 4.6 = 8.9 (17.5 / 18.2) ms | 31 %, 452 MHz, 19 W: 6 % | 3 of 954 | 23.3 / 18.2 fps | 55.1 ms |
| 4x | 11.6 (9.7) + 1.6 ms | 3.6 + 0.5 + 1.4 + 4.3 = 9.9 (20.2 / 21.6) ms | 33 %, 532 MHz, 20 W: 8 % | 3 of 955 | 23.2 / 17.9 fps | 55.8 ms |
| 5x | 12.1 (10.0) + 1.6 ms | 5.3 + 0.5 + 1.9 + 4.3 = 12.0 (13.8 / 19.5) ms | 39 %, 543 MHz, 21 W: 10 % | 0 of 957 | 28.3 / 27.8 fps | 35.9 ms |
| 6x | 12.0 (10.2) + 1.6 ms | 4.8 + 0.4 + 2.5 + 4.5 = 12.3 (13.7 / 17.8) ms | 39 %, 856 MHz, 22 W: 16 % | 0 of 958 | 28.4 / 27.9 fps | 35.8 ms |
| 8x | 12.1 (10.1) + 1.6 ms | 7.3 + 0.3 + 1.3 + 1.3 = 10.2 (25.5 / 29.0) ms | 31 %, 1014 MHz, 34 W: 14 % | 0 of 958 | 26.6 / 25.8 fps | 38.8 ms |

**San Francisco, draw distance 400 %** (11002 polygons per picture)

| internal | emulation: main CPU avg (longest frame) + sound | GPU per picture: polygons + shadows + latch + present = total (p99 / max) | GPU busy, clock, power: load at full clock | pictures off the 2-refresh pace | 1 % / 0.1 % low | longest interval |
|---|---|---|---|---|---|---|
| 1x | 14.4 (11.1) + 1.6 ms | 1.0 + 0.6 + 0.3 + 8.3 = 10.4 (12.8 / 14.2) ms | 38 %, 210 MHz, 17 W: 4 % | 0 of 957 | 27.4 / 26.7 fps | 37.5 ms |
| 2x | 13.9 (10.1) + 1.6 ms | 1.8 + 0.6 + 0.9 + 7.3 = 10.8 (14.5 / 15.1) ms | 37 %, 383 MHz, 18 W: 6 % | 2 of 956 | 25.7 / 20.8 fps | 48.1 ms |
| 3x | 14.6 (10.8) + 1.7 ms | 2.7 + 0.5 + 1.0 + 4.5 = 8.8 (17.6 / 18.4) ms | 29 %, 468 MHz, 19 W: 6 % | 2 of 956 | 25.0 / 20.0 fps | 50.1 ms |
| 4x | 14.2 (10.1) + 1.6 ms | 3.7 + 0.5 + 1.4 + 4.3 = 9.9 (20.2 / 21.9) ms | 32 %, 553 MHz, 20 W: 8 % | 1 of 957 | 25.5 / 20.3 fps | 49.2 ms |
| 5x | 14.3 (10.6) + 1.6 ms | 5.3 + 0.5 + 1.9 + 4.3 = 12.0 (13.6 / 14.3) ms | 39 %, 554 MHz, 21 W: 10 % | 0 of 957 | 28.8 / 27.9 fps | 35.8 ms |
| 6x | 13.9 (9.9) + 1.6 ms | 4.9 + 0.4 + 2.5 + 4.5 = 12.3 (13.7 / 13.8) ms | 39 %, 861 MHz, 22 W: 15 % | 0 of 957 | 28.7 / 27.6 fps | 36.3 ms |
| 8x | 14.8 (11.1) + 1.7 ms | 6.6 + 0.3 + 1.3 + 1.3 = 9.5 (19.7 / 21.3) ms | 28 %, 1056 MHz, 35 W: 13 % | 0 of 956 | 26.7 / 25.9 fps | 38.5 ms |

Not in the tables: the emulation cannot be split into road, objects and traffic. The game computes all three in one pass
over its object list, and the port has no symbols for the game program; it would take a profile of the emulated program by
address (the headless tool samples it, `PCHIST=1`) mapped onto the routines of the source. What the tables do show is the
sum, and that it follows the polygon count.

What the matrix says:

* **The emulated CPU** costs 4 to 5 ms per picture at 100 %, 8 to 9.5 ms at 300 and 400 % on LA Freeway and Chicago, 12 and
  14.4 ms on San Francisco, independent of the internal resolution. The frame that computes a picture takes up to 10 to
  12 ms of its 16.7 ms. That is the tighter of the two limits: a host with two thirds of this CPU's single-thread speed
  would start to miss refreshes at 300 % on San Francisco.
* **The GPU** is never the limit on this card. Its load, at full clock, goes from 3.6 % at 1x to 14 to 16 % at 6x and 8x.
  The polygons are the part that grows (0.7 to 1.0 ms per picture at 1x, 4.7 to 5.4 ms at 6x, 6.4 to 9.2 ms at 8x, each at that
  resolution's clock); the shadows are 0.3 to 1.1 ms since the first pass; latch and present together 9 ms at 1x and 2.4 ms at 8x, falling
  as the clock rises. The draw distance moves the GPU's numbers by a few percent, as the polygon statistics predicted.
* *(Corrected in the third pass: the late pictures are tied to periodic dips of the GPU's clock, not to its changes as
  such, and the swap call is not the display. See there.)*
* **What is left is pacing, and it is not the same at every resolution.** Game pictures that came a refresh late, of about
  8600 per resolution: 1x: 0, 2x: 12, 3x: 32, 4x: 11, 5x: 0, 6x: 3 (one stall, below), 8x: 3. The late pictures at 2x to
  4x come every 2 to 4 seconds or a multiple of that, in a present that takes one refresh longer, and the GPU time of the
  frames after such a present is twice or half what it was before: the driver has changed the GPU's clock. At 2x to 4x the
  load sits where the driver keeps switching between power states (the clock samples spread over 300 to 650 MHz); at 1x
  it stays in the lowest state and from 5x on it stays up. `glFinish` or `glFlush` before the swap does not change it
  (`--gl-debug 1 / 2`: 6 and 2 late pictures of 955 against 3). This is the driver's power management ("Power management
  mode" in the NVIDIA control panel; "Prefer maximum performance" for `cruisn_usa.exe` is the setting that holds the clock).
  The port does not touch driver settings.
* **The stall of section 2 came back once in the 63 runs** (San Francisco, 6x, 100 %: 66 ms at 27.5 s). The warm-up takes the
  driver's reaction in the other 62, and did in all runs before; why it did not in that one is not known.
* At 8x a display frame takes 29 ms now and then, every 2.4 s in some runs, without a game picture being late: the swap's
  return drifts against the display's vblank while the pictures stay on their refreshes.

So, for 4x, 6x and 8x on this machine: neither CPU nor GPU limits the frame rate. The CPU has a factor of 1.4 to 4 in hand
depending on track and draw distance, the GPU a factor of 6. Where pictures are late, it is the hand-over to the display.

### Next, in order of what the measurements say matters

1. **GPU power states at 2x to 4x** (0.1 to 0.4 % of the pictures late): a driver setting, see above. Nothing in the port
   should try to hold the GPU's clock up with artificial load.
2. **The interpreter's worst frame** (10 to 12 ms of 16.7): the only place where a slower host would lose refreshes. A
   profile of the interpreter on San Francisco at 400 % would be the starting point; nothing has been changed here.
3. **The warm-up's one miss in 63**: an idle thread that stays for the whole session had no miss in a dozen runs either; with as
   few events as this, telling the two apart needs a few hundred starts.
4. **A tighter cover for the polygons** (section 4): saves a quarter to a half of the fragments, which on this card is a
   saving on 16 % load. Worth it for integrated graphics, not here; needs a pixel-exact comparison first.
5. **Vulkan** now paces as evenly as OpenGL (one frame of 45 ms in three races). OpenGL stays the default; the per-phase GPU
   times are OpenGL only so far (Vulkan reports the frame's total, which is a true GPU time there: one command buffer).
6. **The steady cadence's default** is a decision, not a measurement: on, the machine's own slow frames are gone and the
   run differs from the machine from the first rescued frame; off, it is the machine exactly.

## Third pass: the remaining late pictures, input lag, the CPU's worst case, weaker hosts (October 2026)

No general optimisation round. What was left after the second pass, looked at one by one. Two statements of the second pass
are corrected here: the late pictures at 2x to 4x are tied to the GPU's clock, but not in the way described there, and the
"present margin" could not be judged from the swap call at all.

### New in the tools

* `--perf-csv` now carries the frame's timeline on the performance counter: top of the frame loop (the inputs are read
  there), emulation done, present called, swap called, swap returned, and the moment the GPU had finished the frame (a
  `GL_TIMESTAMP` query after the frame's last call, tied to the performance counter once when profiling starts). The GPU's
  time by phase is attached to the frame it belongs to. `<csv>.vblank` holds the display's vblanks on the same clock
  (`platform/vblank_clock`: a thread in `D3DKMTWaitForVerticalBlankEvent`).
* From these: **display cadence** (the first vblank after the GPU had finished the frame that shows a game picture; the
  driver's own swap waits for that vblank), **present margin** (how long before that vblank the GPU was finished) and
  **latency** (from the top of the frame loop to that vblank). This is what can be seen from inside the process; the
  moment the panel actually changes is one scan-out later and is not measured.
* Test switches (never set by the launcher): `--gl-debug 1 / 2 / 64 / 128` (glFinish, glFlush, extra GPU load, frame-ahead
  limit off), `--pace 1 / 2` (pace by the vblank itself, with or without presenting unchanged pictures),
  `--fake-refresh <hz>`, `--cpu-slow <factor>`, `--warmup-kind <n>`, `--pause-at <frame>`.
* Headless, profiling build (`cmake -DC3X_PROFILE=ON`): `PCPROF=<prefix>` writes the instructions per game picture and by
  address, `PCDUMP=<picture>,..` the same for single pictures, `PCTAG=<pc>,<lo>,<hi>` splits the display routine's
  instructions by the class of the object being displayed. `PROFILE=1` (normal build) samples the host.

### 1. "Cruise the USA" at two thirds of the frame rate (a fault of the game)

Reported as stutter at the start of US 101 when reached in the tour, not when chosen directly. Cause: the frame governor.
`INIT_GAMELEG` sets FRAMRATE to 2 (a picture every 3 vblanks at best), and `ALL_JOINUP` sets it to 1 for the race. Every
way into a race runs through both, except the next leg of the tour started from the map screen (`BONUS.ASM`: `CLRI R0 /
STI R0,@DID_TIMED_OUT / CALL INIT_GAMELEG / DIE`). So every leg after the first runs at 19.3 instead of 28.9 pictures per
second, on the machine as well (measured with the machine's timing, `--steady 0`: 200 of 200 frames at 3 vblanks from the
second leg on).

`rom_patches.cpp: cruise_leg_rate` rewrites those four words in the RAM copy to `CALL INIT_GAMELEG / LDI 1,R0 / STI
R0,@FRAMRATE / DIE` (the two instructions it replaces are redundant: INIT_GAMELEG clears DID_TIMED_OUT itself). After: legs
0 to 3 of the tour, 100 % and 300 %: every frame at 2 vblanks. With `steady_cadence = false` the patch is not applied: that
setting stays the machine as it is.

### 2. The late pictures at 2x to 4x: what the GPU's clock has to do with them

The second pass blamed "clock changes". Sampling the clock through the driver (NVML, every 16 ms) did not confirm it: of
9 late pictures in three races none was within 250 ms of a sampled change, and 27 sampled changes had no late picture.
The driver's reading is too coarse and too late. The frames themselves are a better clock: the present pass (the final
blit) of a frame that draws no polygons does the same work every time, so its GPU time is the inverse of the GPU's speed.

Seen that way (Chicago, 3x, 42 s of race per run):

* Every 2.0 s the GPU runs 2 to 3.9 times slower for a few frames, then returns: 19 or 20 such dips per run, 2.0 s apart
  with a deviation of a tenth. The driver tries a lower clock, finds it too low and goes back.
* **All late pictures are at a dip**: 5 of 5, 3 of 3, 4 of 4, 6 of 6 in four runs (of different variants below), while the dips' windows (ten frames
  either side) cover 24 % of the race. About one dip in four costs a picture one refresh.
* At 5x and 6x there are no dips and no late pictures. At 1x the GPU stays in its lowest state.
* Cause or coincidence: with extra GPU load (`--gl-debug 1088`: the present pass four more times, for measuring only) the
  clock stays up, the GPU's time per frame *falls* (3.5 ms instead of 5.1 ms in the median) and no picture is late.

So the trigger is the driver's periodic step down, at loads low enough for it to try. What then makes a picture late is
not simply that the GPU's work no longer fits (it takes 10 to 13 ms in a dip, of 16.7; the same GPU times at 6x cost
nothing). In the runs with a one-frame queue, where it can be told apart, the lost refresh was at the frame in which the
GPU's time halved again (3 of 3): the way back up, not the slow frames.

**Can the port avoid it without a driver profile and without burning GPU time?** Tried, each measured on the display
side:

| Variant (Chicago, 3x) | pictures off the pace | present margin min / median | latency |
|---|---|---|---|
| as before | 5 of 1253 (4, 6 in other runs) | 6.4 / 11.8 ms | 80 ms |
| glFinish before the swap | 1 to 6 | | |
| the frame before finished before the swap (the limit of section 3) | 3 of 1253 | 6.3 / 12.0 ms | 47 ms |
| pace by the vblank, present every refresh | 26 of 1250 | 0.3 / 11.9 ms | 67 ms |
| pace by the vblank, present new pictures only | 74 and 84 of 1255 | 0.0 / 12.1 ms | 33 ms |
| extra GPU load (not a solution) | 2 of 1256 | 6.3 / 11.6 ms | 33 ms |

None of the clean ones removes the dips' cost. Presenting only new pictures looked perfect by the swap call (0 late) and
was the worst on the display: the GPU then finishes around the vblank, sometimes before and sometimes after it. That is
the reason the display-side numbers were built. **Answer: no.** The port can make the dips rarer only by the load it has
anyway: from 5x on (on this card) they do not occur, and 1x is below them.

### 3. Frame queue, present margin, input lag

The timeline of a frame showed something the frame times had hidden: **the NVIDIA OpenGL driver ran four frames behind
the game.** Its thread executes a frame's calls, then waits in its own swap for the vblank; the game's thread meanwhile
hands over further frames and its `SwapBuffers` returns as soon as there is room in that queue.

    as before, 6x:  GPU finished a frame 62.5 ms (median) after its swap was called: 3 or 4 vblanks later
                    from the top of the frame loop (inputs read) to the vblank that shows the picture: 80 ms

Consequences: more than 30 ms of input lag that buy nothing; swap calls that return at times unrelated to the display (the
alternating 11 / 22 ms frame times of the second pass, harmless in themselves); and at 8x, where the GPU's work per frame
is longest, frames whose work ended right on the vblank:

    Chicago 8x, as before: pictures on the display after 1 / 2 / 3 vblanks: 14 / 1227 / 14 (2.2 % off the pace),
    present margin: minimum 0.06 ms, 13 pictures under 2 ms. By the swap call: 1255 of 1255 on the pace.

The second pass' scaling table, which judged by the swap call, did not see these.

**Change (`[video] frame_ahead_limit`, default on; launcher: "Low input lag"):** before a frame is swapped, the GPU must
have finished the frame before it (`glFenceSync` after each frame, `glClientWaitSync` on the previous one before the
swap). The driver is then at most one frame behind. The Vulkan backend has had the same rule since the second pass.

| 75 s per run, OpenGL, 300 % | pictures off the pace on the display | present margin min / 1st percentile / median | latency median |
|---|---|---|---|
| Chicago 8x | 0 of 1707 (before: 28 of 1255) | 6.6 / 8.7 / 13.2 ms (before: 0.06 / 1.6 / 13.6) | 48.7 ms (before 81.5) |
| LA Freeway 8x | 1 of 1706 | 7.3 / 8.5 / 13.1 ms | 48.5 ms |
| San Francisco 8x | 0 of 1707 | 2.5 / 3.7 / 9.5 ms | 44.7 ms |
| Chicago 6x | 0 of 1707 | 7.7 / 9.0 / 11.6 ms | 46.3 ms (before 79.5) |
| LA Freeway 6x | 1 of 1706 | 8.4 / 9.0 / 11.4 ms | 46.0 ms |
| San Francisco 6x | 0 of 1707 | 5.2 / 6.3 / 7.8 ms | 43.2 ms (before 76.3) |
| Chicago 3x | 4 of 1704 (the dips of section 2) | 6.3 / 7.6 / 11.6 ms | 47.4 ms |
| LA Freeway 4x | 5 of 1700 | 0.8 / 7.0 / 11.6 ms | 46.9 ms |
| San Francisco 2x | 1 of 1704 | 5.2 / 5.8 / 8.0 ms | 43.5 ms |

**The margin of a normal frame** is therefore 8 to 13 ms of the 16.7 ms refresh; the first percentile is 6 to 9 ms
(San Francisco at 8x: 3.7 ms, the tightest case measured). Of the remaining 46 ms of latency, 33 are two refreshes: the
driver executes a frame's calls after the swap before it has returned at a vblank, and shows the result at the vblank
after that. The rest is the emulation and the place of the swap call inside its refresh. Waiting for the GPU to finish
*this* frame before going on would take another refresh off, at the price of the CPU standing still while the GPU works;
not done.

### 4. San Francisco at 400 %: where the emulated CPU's instructions go

Profiling build, autopilot, 3280 race pictures; every executed instruction counted by address, and inside the display
routine by the class of the object being displayed. Routines are the call targets of the disassembly; the names come from
the game's source by matching the code (`DIRQ.ASM`, `COLLA.ASM`, the sort in the background module).

| per game picture | 100 % | 400 % |
|---|---|---|
| instructions: median / 99th percentile / maximum | 0.72 / 1.00 / 1.09 million | 2.21 / 2.60 / 2.89 million |
| display (transform, clip, hand polygons to the video hardware) | 52 % | 66 % |
| ...of which objects of class 4 (the track's scenery: buildings) | 28 % | 40 % |
| ...road pieces | 14 % | 19 % |
| ...traffic and racers / player's car / trees and signs | 7 / 2 / 1 % | 5 / 1 / 2 % |
| collision (cars against road pieces and objects: `_obj_coll`, the scans) | 28 % | 16 % |
| sorting the object list by distance (ZSORT) | 7 % | 10 % |
| waiting (delay loop, video FIFO) | 3 % | 2.5 % |
| everything else (game logic, sound commands, text, interrupt) | 10 % | 5 % |

The two halves of the display routine: transforming the vertices (`DISPLAY` / `VECTOR_TRANSFORMATION`, 33 % of all
instructions at 400 %) and clipping and plotting the polygons (`PLOTPOLY`, 29 %).

**What the longer draw distance buys is scenery**: three times the instructions, and the growth is almost entirely the
display of buildings and road, most of it for polygons that end up as a line or a point on the screen (58 % of the
polygons have no area, section 4 of the second pass): 0.38 million instructions per picture become 1.29 million. The
collision code grows by half (0.20 to 0.31 million), the sort from 0.05 to 0.19 million.

**A normal picture and the slowest one** (400 %):

| | median picture (no. 1701) | slowest picture (no. 2393) | difference |
|---|---|---|---|
| instructions | 2 211 491 | 2 889 249 | +677 758 |
| display | 78.2 % = 1.73 million | 66.3 % = 1.92 million | +187 000 |
| ...buildings / road / traffic | 60.9 / 12.3 / 2.4 % | 48.3 / 11.0 / 3.8 % | |
| sort | 12.0 % = 265 000 | 16.0 % = 462 000 | +197 000 |
| collision | 5.2 % = 115 000 | 12.2 % = 352 000 | +237 000 |
| everything else, waiting | 4.6 % | 5.5 % | +57 000 |

The slowest picture is not slow because more is drawn (+11 %). Two things come together in it: the collision code runs
(it is 5 % of the median picture and 16 % of the average one; `_obj_coll` alone is 140 000 instructions more), and the sort has more to do: ZSORT is a bubble sort over the list of active objects, cheap while the
list stays in order and expensive in the pictures in which many objects change their order, with three times the objects
in the list at 400 %. The second slowest picture (no. 2032) is of the other kind: 14 071 polygons, display +380 000.

So the worst frame at 400 % is: the scenery's display as the base load, plus collision and sort in the same picture. With
the clock at x3 these 2.9 million instructions are what `catch_up` has to fit before the vblank.

### 5. Where the interpreter spends the host's time

Sampling profile of the same run (10 386 samples), with the instruction counts of the profiling build:

* TMS320C31 interpreter 79.9 % of the host's time, sound DSP (ADSP-2105) 13.8 %, everything else 6.3 %.
* 5.64 ns per emulated instruction back to back (177 million per second). In the paced app the same work takes about
  7.4 ns: the core does not stay at full clock between the frames.
* **Dispatch: 36 % of the interpreter's time, 2.0 ns per instruction** (the run loop and `execute_one`): by source line,
  the indirect call through the table of 2048 handlers 15 %, the repeat-block check 7 %, the hook filter and comparison
  6 %, opcode fetch and cycle count 3.5 %, loop condition 2 %, trace flag 1.6 %.
* The floating point helpers (`mpyf`, `addf`, `subf`, `int2float`, `float2int`: the DSP's own format, computed bit-exactly
  with integers) 19.5 %.
* The rest is the handlers themselves:

| function | host time (interpreter = 100 %) | calls | ns per call |
|---|---|---|---|
| run (the loop) + execute_one | 36.0 % | 10.6 billion | 2.0 |
| mpyf (helper of the multiply instructions) | 7.5 % | | |
| addf (helper) | 5.9 % | | |
| ldi_ind | 4.2 % | 869 million | 2.9 |
| subf (helper) | 4.1 % | | |
| mpyf3stf | 2.6 % | 307 million | 5.1 |
| ldi_dir | 2.1 % | 377 million | 3.3 |
| lsh_imm | 2.1 % | 566 million | 2.2 |
| fixsti | 2.0 % | 323 million | 3.8 |
| mpyaddf_0 | 1.9 % | 197 million | 5.7 |
| subf3_indind | 1.6 % | 281 million | 3.4 |
| brcd_imm / brc_imm | 1.1 / 0.9 % | 470 / 627 million | 1.4 / 0.8 |

(the handlers' ns per call are without the helpers they call and without the dispatch.) No memory wrapper, bounds check or
endian conversion shows up: memory is reached through a page table of host pointers, one load and one test per access;
the only diagnostic in the loop is the trace flag.

**One change, measured:** the hook filter looked at the low 8 bits of the program counter; with 14 hooks one instruction
in 18 passed it and was compared with all hooks. It now looks at 12 bits. San Francisco 400 %, 8000 vblanks, three runs
each: 37.36 / 37.58 / 37.90 s before, 37.01 / 37.27 / 37.31 s after: **1.1 % faster, about the size of the run-to-run
spread**. Cycles, instructions and RAM hashes of the two builds are identical.

**Would a faster dispatch be worth it?**

(estimates from the shares above, not measured:)

| Option | what it could remove | realistic gain (interpreter) | risk |
|---|---|---|---|
| computed goto / threaded handlers | part of the indirect call (15 %) | 3 to 6 % | MSVC has no computed goto: a second compiler (clang-cl) for the release, or a giant switch; debugging unchanged |
| decode cache (handler pointer per address) | the table index, 1 to 2 % | 1 to 2 % | code is in RAM and patched at start: needs invalidation; small |
| folding the per-instruction checks (repeat block, hooks, trace) into one test | up to 10 % | 4 to 8 % | the repeat-block end test is part of the CPU's semantics: has to stay exact; medium |
| basic-block cache or JIT | most of the 36 % and some of the helpers | 1.3 to 2 times | interrupts are taken between instructions at exact cycle counts, delayed branches and repeat blocks carry state across instructions, hooks stop at single addresses: all of that has to be reproduced; large, and the hardest to keep bit-identical |
| host floating point for the DSP's floats (`USE_FP` exists in the source) | most of the 19.5 % | 10 to 15 % | not bit-exact: rejected |

Nothing here is needed on this machine (the worst frame takes 12 ms of 16.7). For a host half as fast the third row is
the place to start; a JIT is not justified by these numbers.

### 6. The OpenGL warm-up: the mechanism, and a warm-up that checks itself

A thread is started in a chosen frame of the race ("probe"); if the swap stalls there, the warm-up before had not taken
the driver's reaction. 6x, the frame-ahead limit on:

| Before the first frame | probe stalls |
|---|---|
| nothing (`--no-warmup`) | 4 of 4 |
| presents only, no thread | 6 of 6 |
| a work item for the thread pool, then presents | 8 of 8 |
| a thread that ends at once, then presents | 0 of 8 |
| a thread kept alive across the presents (the version of the second pass) | 0 of 12 |
| three threads kept alive | 0 of 8 |

(From the second pass: a thread started before the video backend exists, or behind the loading picture, does not help
either.) So it is the *creation* of a thread, seen by the driver once it presents for real: a new thread attaches to every
DLL of the process, the driver's among them. Work handed to threads that already exist does nothing, which is also why
the stall waited for the thread pool to *grow* at 30 s. How long the thread lives and how many there are does not matter.

The driver's reaction can be seen while it happens: the present right after the thread has started takes 50 to 79 ms
instead of at most two refreshes. That makes a warm-up possible that does not hope: after four presents (the first
presents have a long one of their own, with or without a thread), threads are started, one per three presents, until two
in a row have caused no long present. In 36 of 36 starts: reaction at the first thread, the next two clean, no stall at
the probe. Cost: thirteen presents of the first picture, about a quarter of a second. Why the single thread of the second
pass missed once in 63 runs was not found; the new version would have started another thread in that case, and says in
`--warmup-log` what it saw.

### 7. Other display rates

The game is never made faster or slower than the display sync's rule allows: one emulated vblank per `k` refreshes when
the display's rate is within 6 % of `k` times the machine's 57.93 Hz, the machine's own clock otherwise. Nothing in the
emulator knows the display's rate; the steady cadence counts emulated vblanks.

| Display | k | game speed | game pictures |
|---|---|---|---|
| 60 Hz (59.94) | 1 | 1.036 (1.035) | every 2 refreshes |
| 120 Hz | 2 | 1.036 | every 4 |
| 144 Hz | no sync (2.49 times the machine's rate) | 1.000, machine clock | every 5, now and then 4 |
| 165 Hz | 3 | 0.950 | every 6 |
| 180 / 240 / 360 Hz | 3 / 4 / 6 | 1.036 | every 6 / 8 / 12 |
| 50, 75, 90, 100 Hz | no sync | 1.000 | uneven (2 or 3 refreshes per emulated vblank) |

One assumption of 60 Hz was in it and is gone: the loop counted *presents* to find the refresh in which the next emulated
frame is due. On a 60 Hz display that is every pass. On a faster display the pass that computes a game picture can take
longer than a refresh (10 ms against 8.3 ms at 120 Hz); the present then comes a refresh later, and counting presents the
loop never made up for it. Measured with a simulated display (`--fake-refresh`, VSync off, a wait for the next tick of a
clock at that rate after every present; this display runs at 60 Hz only):

| simulated rate | before: emulated vblanks per second, speed | now |
|---|---|---|
| 60 Hz | 60.00, 1.036 | 60.00, 1.036 |
| 120 Hz | 53.13, **0.917** | 60.02, 1.036 (959 of 959 pictures after 4 refreshes) |
| 144 Hz | 57.93, 1.000 | 57.93, 1.000 |
| 165 Hz | 47.69, **0.823** | 54.78, 0.946 |
| 240 Hz | 50.07, **0.864** | 60.02, 1.036 (942 of 959 pictures after 8 refreshes) |

The emulated frame is now due after `k` passes as before, or earlier when `k - 0.5` refreshes of time have passed since
the last one was started. At 60 Hz nothing changes (time alone is not enough there: the passes do not begin at even
distances, and a rule by time only skipped frames, 29 of 1692 pictures late in a test). Not tested on a real display of those rates. Known limits: the display's rate is read once at the start
(a window moved to another display keeps the old rule until the next start), and a variable-refresh display is treated
as its maximum rate.

### 8. A slower host

`--cpu-slow <n>`: after every emulated frame the loop spins until n times the frame's emulation time has passed (main
CPU, sound DSP and the hand-over to the driver alike). 6x, 70 s per run, on the display side:

| | emulation x 1 | x 2 | x 3 |
|---|---|---|---|
| San Francisco 100 % | | | 0 of 1555 pictures off the pace |
| Chicago 300 % | | 0 of 1557 | 249 of 1418 (18 %) |
| LA Freeway 300 % | | | 173 of 1460 (12 %) |
| San Francisco 300 % | 0 of 1557 | 6 of 1555 (0.4 %) | 685 of 1181 (58 %), the game slows down, the sound runs dry |
| San Francisco 400 % | | 88 of 1522 (6 %) | |

* **The CPU breaks the cadence first**, in the frame that computes a game picture: at half this CPU's speed everything up
  to 300 % still holds and San Francisco at 400 % begins to fail; at a third 300 % fails on every track and 100 % holds.
  (The paced app already runs the interpreter at about three quarters of its back-to-back speed, section 5.)
* **The GPU**: its load at full clock is 16 % at 6x and 4 % at 1x on this card (second pass). A card half as fast changes
  nothing. Integrated graphics of a tenth of this card's speed would be at its limit around 3x and comfortable at 1x and
  2x; there the polygons' bounding rectangles (a quarter to a half of the fragments are thrown away) become worth
  cutting. That is derived from the load, not measured on such a card.
* The frame-ahead limit makes the game wait for the GPU when the GPU is slower than the display, instead of queueing
  frames: on a weak GPU the frame rate then follows the GPU directly.

So on weaker hardware the order is: (1) the interpreter's worst frame at draw distances above 100 %, (2) the GPU's
fragment load at internal resolutions above 2x to 3x on integrated graphics. At the original draw distance and 1x to 2x a
host a third as fast as this one keeps the cadence.

### Answers, in short

1. **GPU clock and the late pictures at 2x to 4x:** yes, but not through "changes" as such: the driver steps the clock
   down every 2.0 s at these loads, and every late picture (18 of 18) lies in such a dip. With the load held up there are
   none.
2. **Without a driver profile:** not by pacing; four variants measured, none removes it, two make it worse. Not needed at
   1x and from 5x on.
3. **Present margin:** 8 to 13 ms of 16.7 ms for a normal frame, 6 to 9 ms at the first percentile (3.7 ms in the
   tightest case). The larger finding on the way: the driver ran four frames behind; limited to one, the input lag falls
   from 80 to 46 ms and the pictures at 8x come on time.
4. **San Francisco 400 %:** the display of the scenery (61 % of a normal picture's instructions); the slowest picture is
   that plus collision and sort in the same picture, not more polygons.
5. **The interpreter:** 36 % dispatch, 20 % float helpers, the rest handlers; 5.6 ns per instruction.
6. **The warm-up:** thread creation is the trigger; the warm-up now repeats until the driver no longer reacts: 36 of 36.
   A proof for all cases it is not.
7. **Weaker hardware:** the CPU first (interpreter, draw distance above 100 %), then fragments on integrated graphics.
