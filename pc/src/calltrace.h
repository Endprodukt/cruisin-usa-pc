// Slow-call trace for the video backends (--perf): every OpenGL / Vulkan / swap call is timed, and one that takes longer than
// the limit (default 2 ms) is written to a log with the frame, the time since the start, the function, its duration, the
// render phase it was made in and its arguments (object ids, sizes). Off: one predictable branch per call.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

#include "perf.h"

struct CallTrace
{
	bool on = false;
	double limit_ms = 2.0;
	FILE *file = nullptr;
	std::string text;                        // the lines are kept in memory and written at the end: a file write in the frame loop
	                                         // can itself take tens of milliseconds (it did: one of the stalls measured was this log)
	double t0 = 0;                           // perf_now_ms() when the trace was opened
	std::atomic<uint32_t> frame{0};          // display frames presented so far
	// what the render thread is in right now (also read by the stall watch from its own thread)
	std::atomic<const char *> api{""}, phase{""}, call{""};
	long long res = 0, bytes = 0;            // the phase's object (page, texture...) and the size of the data it hands over
	uint64_t calls = 0, slow = 0;
	double worst_ms = 0;
	const char *worst_fn = "";

	void open(const std::string &path, double limit, const char *api_name)
	{
		if (path.empty()) return;
		if (!file) file = std::fopen(path.c_str(), "ab");
		if (!file) return;
		limit_ms = limit; t0 = perf_now_ms(); on = true;
		api.store(api_name);
		std::fprintf(file, "== %s calls of %.1f ms and more\n", api_name, limit_ms);
		std::fflush(file);
		text.reserve(1u << 20);
	}
	void slow_call(const char *fn, double ms, const long long *a, int n)
	{
		slow++;
		if (!file || text.size() > (16u << 20)) return;
		char b[400];
		int k = std::snprintf(b, sizeof(b), "frame %6u  t %8.3f s  %-28s %7.2f ms  phase %-10s object %lld  data %lld bytes  args(", frame.load(), (perf_now_ms() - ms - t0) / 1000.0, fn, ms,
		                      phase.load(), res, bytes);
		for (int i = 0; i < n && k < 360; i++) k += std::snprintf(b + k, sizeof(b) - size_t(k), i ? ", %llX" : "%llX", (unsigned long long)a[i]);
		text.append(b, size_t(k));
		text += ")\n";
	}
	void close()
	{
		if (!file) return;
		std::fwrite(text.data(), 1, text.size(), file);
		text.clear();
		std::fprintf(file, "== %llu calls timed, %llu of them slow; slowest: %s %.2f ms\n\n", (unsigned long long)calls, (unsigned long long)slow, worst_fn, worst_ms);
		std::fclose(file); file = nullptr; on = false;
	}
};
inline CallTrace g_calltrace;

struct CallScope
{
	const char *fn = nullptr;
	double t = 0;
	long long a[6];
	int n = 0;
	template <class... A> explicit CallScope(const char *f, const A &...args)
	{
		if (!g_calltrace.on) return;
		fn = f;
		(put(args), ...);
		g_calltrace.call.store(f, std::memory_order_relaxed);
		t = perf_now_ms();
	}
	~CallScope()
	{
		if (!fn) return;
		const double ms = perf_now_ms() - t;
		g_calltrace.call.store("", std::memory_order_relaxed);
		g_calltrace.calls++;
		if (ms > g_calltrace.worst_ms) { g_calltrace.worst_ms = ms; g_calltrace.worst_fn = fn; }
		if (ms >= g_calltrace.limit_ms) g_calltrace.slow_call(fn, ms, a, n);
	}
	template <class T> void put(const T &v)
	{
		if (n >= 6) return;
		if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) a[n++] = (long long)v;
		else if constexpr (std::is_pointer_v<T>) a[n++] = (long long)(uintptr_t)v;
		else a[n++] = -1;
	}
};

// the render phase the following calls belong to (restored at the end of the scope)
struct CallPhase
{
	const char *prev;
	long long prev_res, prev_bytes;
	explicit CallPhase(const char *name, long long res = 0, long long bytes = 0)
	    : prev(g_calltrace.phase.load(std::memory_order_relaxed)), prev_res(g_calltrace.res), prev_bytes(g_calltrace.bytes)
	{
		g_calltrace.phase.store(name, std::memory_order_relaxed); g_calltrace.res = res; g_calltrace.bytes = bytes;
	}
	~CallPhase() { g_calltrace.phase.store(prev, std::memory_order_relaxed); g_calltrace.res = prev_res; g_calltrace.bytes = prev_bytes; }
};

template <class F, class... A> inline auto traced_call(const char *nm, F f, A... a)
{
	CallScope s(nm, a...);
	return f(a...);
}
