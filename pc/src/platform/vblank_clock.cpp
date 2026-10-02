#include "vblank_clock.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#include "../perf.h"

namespace {

// the two kernel-mode-thunk calls of gdi32.dll that are needed (d3dkmthk.h is part of the driver kit, not of the SDK)
struct KmtOpenAdapterFromHdc { HDC hDc; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
struct KmtWaitForVBlank { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; };
struct KmtCloseAdapter { UINT hAdapter; };
using FnOpen = LONG(APIENTRY *)(KmtOpenAdapterFromHdc *);
using FnWait = LONG(APIENTRY *)(const KmtWaitForVBlank *);
using FnClose = LONG(APIENTRY *)(const KmtCloseAdapter *);

std::atomic<bool> g_run{false};
std::thread g_thread;
std::mutex g_lock;
std::vector<double> g_times;

}   // namespace

namespace vblank_clock {

bool start()
{
	if (g_run.load()) return true;
	HMODULE gdi = GetModuleHandleA("gdi32.dll");
	FnOpen open = reinterpret_cast<FnOpen>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc"));
	FnWait wait = reinterpret_cast<FnWait>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"));
	FnClose close = reinterpret_cast<FnClose>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
	if (!open || !wait) return false;
	KmtOpenAdapterFromHdc oa{};
	HDC dc = CreateDCA("DISPLAY", nullptr, nullptr, nullptr);
	oa.hDc = dc;
	const LONG st = open(&oa);
	DeleteDC(dc);
	if (st != 0) return false;
	g_times.reserve(1 << 16);
	g_run.store(true);
	g_thread = std::thread([oa, wait, close] {
		KmtWaitForVBlank w{oa.hAdapter, 0, oa.VidPnSourceId};
		while (g_run.load())
		{
			if (wait(&w) != 0) { Sleep(1); continue; }
			const double t = perf_now_ms();
			std::lock_guard<std::mutex> g(g_lock);
			g_times.push_back(t);
		}
		if (close) { KmtCloseAdapter c{oa.hAdapter}; close(&c); }
	});
	return true;
}

void stop()
{
	if (!g_run.exchange(false)) return;
	g_thread.join();
}

std::vector<double> times()
{
	std::lock_guard<std::mutex> g(g_lock);
	return g_times;
}

bool wait_next()
{
	static bool tried = false, ok = false;
	static KmtWaitForVBlank w{};
	static FnWait wait = nullptr;
	if (!tried)
	{
		tried = true;
		HMODULE gdi = GetModuleHandleA("gdi32.dll");
		FnOpen open = reinterpret_cast<FnOpen>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc"));
		wait = reinterpret_cast<FnWait>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"));
		if (open && wait)
		{
			KmtOpenAdapterFromHdc oa{};
			HDC dc = CreateDCA("DISPLAY", nullptr, nullptr, nullptr);
			oa.hDc = dc;
			ok = open(&oa) == 0;
			DeleteDC(dc);
			w = KmtWaitForVBlank{oa.hAdapter, 0, oa.VidPnSourceId};
		}
	}
	return ok && wait(&w) == 0;
}

void write(const std::string &path)
{
	const std::vector<double> t = times();
	if (FILE *f = std::fopen(path.c_str(), "wb"))
	{
		for (double v : t) std::fprintf(f, "%.3f\n", v);
		std::fclose(f);
	}
}

}   // namespace vblank_clock
