// Stall watch (--perf): a watcher thread that looks at the frame loop from outside. When the thread that called start() has not
// called beat() for longer than the limit, the watcher stops it for a moment, walks its call stack and writes it to the log
// (module + nearest exported symbol per frame), together with the graphics call it was in (calltrace.h) - and, once per stall,
// the stacks of the process' other threads. It also notes every thread that appears while the game runs, with the module its
// start address lies in. This is how a stall inside a driver call can be traced to what the driver was waiting for.
#pragma once

#include <string>

namespace stallwatch {
void start(const std::string &log_path, double limit_ms);
void beat();   // once per frame, from the thread that called start()
void stop();
}
