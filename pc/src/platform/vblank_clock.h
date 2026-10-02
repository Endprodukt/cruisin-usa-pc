// The display's vertical blanks as times on the performance counter (--perf): a thread that sleeps in
// D3DKMTWaitForVerticalBlankEvent on the primary display and notes when it wakes. With these, a frame's place inside its
// refresh can be computed afterwards: how long before the vblank it was ready (the present margin, see docs/PERFORMANCE.md).
#pragma once

#include <string>
#include <vector>

namespace vblank_clock {
bool start();                       // false if the display's adapter cannot be opened
void stop();
std::vector<double> times();        // perf_now_ms() of every vblank seen so far
void write(const std::string &path);   // one time per line
bool wait_next();                   // blocks the calling thread until the primary display's next vblank; false: not available
}
