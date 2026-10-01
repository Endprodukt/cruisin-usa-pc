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
