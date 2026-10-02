#include "stall_watch.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "../calltrace.h"

#pragma comment(lib, "dbghelp.lib")

namespace {

constexpr int kMaxFrames = 48;
std::atomic<double> g_beat{0};
std::atomic<bool> g_run{false};
std::thread g_thread;
HANDLE g_main = nullptr;
DWORD g_main_tid = 0;
FILE *g_log = nullptr;
double g_t0 = 0, g_limit = 28.0;
// The watcher sleeps on a timer that the frame loop pushes back every frame: it only ever runs when a frame is overdue. (A
// watcher that polled every 2 ms turned out to change what it was meant to observe: see docs/PERFORMANCE.md.)
// Experiments (environment STALLWATCH, bits): 1 = load the symbols at the start instead of at the first stall, 4 = sample new
// threads' stacks as they appear, 8 = poll every 2 ms and look for new threads and modules periodically (the first version).
// STALLFROM = first frame that is watched.
HANDLE g_timer = nullptr;
uint32_t g_from = 0;
int g_opts = 0;
bool g_sym = false;
void sym_init()
{
	if (g_sym) { SymRefreshModuleList(GetCurrentProcess()); return; }
	SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
	SymInitialize(GetCurrentProcess(), nullptr, TRUE);
	g_sym = true;
}

// the call stack of a thread that is stopped for the duration (no allocation, no lock of ours while it is stopped)
int capture(HANDLE th, DWORD64 *pcs)
{
	int n = 0;
	if (SuspendThread(th) == DWORD(-1)) return 0;
	CONTEXT c;
	c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (GetThreadContext(th, &c))
	{
		__try
		{
			while (n < kMaxFrames && c.Rip)
			{
				pcs[n++] = c.Rip;
				DWORD64 base = 0;
				PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
				if (!fe) { c.Rip = *reinterpret_cast<DWORD64 *>(c.Rsp); c.Rsp += 8; }   // leaf function: the return address is on top
				else
				{
					void *hd = nullptr; DWORD64 ef = 0;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &ef, nullptr);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	ResumeThread(th);
	return n;
}

void describe(DWORD64 pc, char *out, size_t size)
{
	char mod[MAX_PATH] = "?";
	HMODULE hm = nullptr;
	DWORD64 off = pc;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(pc), &hm) && hm)
	{
		char path[MAX_PATH];
		if (GetModuleFileNameA(hm, path, MAX_PATH))
		{
			const char *b = std::strrchr(path, '\\');
			std::snprintf(mod, sizeof(mod), "%s", b ? b + 1 : path);
		}
		off = pc - reinterpret_cast<DWORD64>(hm);
	}
	char buf[sizeof(SYMBOL_INFO) + 256] = {};
	SYMBOL_INFO *si = reinterpret_cast<SYMBOL_INFO *>(buf);
	si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
	DWORD64 disp = 0;
	// (exports only for system and driver modules: a large distance means "somewhere after that export", not inside it)
	if (SymFromAddr(GetCurrentProcess(), pc, &disp, si) && disp < 0x2000) std::snprintf(out, size, "%s!%s+0x%llX", mod, si->Name, (unsigned long long)disp);
	else std::snprintf(out, size, "%s+0x%llX", mod, (unsigned long long)off);
}

void print_stack(const char *head, const DWORD64 *pcs, int n, int max_lines)
{
	std::fprintf(g_log, "%s\n", head);
	for (int i = 0; i < n && i < max_lines; i++)
	{
		char d[400];
		describe(pcs[i], d, sizeof(d));
		std::fprintf(g_log, "      %2d  %s\n", i, d);
	}
}

DWORD64 thread_start(HANDLE th)
{
	using NtQIT = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	static NtQIT q = reinterpret_cast<NtQIT>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
	DWORD64 addr = 0;
	if (q) q(th, 9 /* ThreadQuerySetWin32StartAddress */, &addr, sizeof(addr), nullptr);
	return addr;
}

std::vector<DWORD> thread_ids()
{
	std::vector<DWORD> ids;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE) return ids;
	THREADENTRY32 te; te.dwSize = sizeof(te);
	const DWORD pid = GetCurrentProcessId();
	if (Thread32First(snap, &te))
		do { if (te.th32OwnerProcessID == pid) ids.push_back(te.th32ThreadID); } while (Thread32Next(snap, &te));
	CloseHandle(snap);
	return ids;
}

void watcher()
{
	if (g_opts & 1) sym_init();
	const DWORD self = GetCurrentThreadId();
	std::set<DWORD> known;
	std::set<HMODULE> mods;
	auto since = [] { return (perf_now_ms() - g_t0) / 1000.0; };
	auto scan = [&](bool first) {
		for (DWORD id : thread_ids())
		{
			if (!known.insert(id).second) continue;
			HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, id);
			char d[400] = "?";
			DWORD64 st = 0;
			if (th) { st = thread_start(th); CloseHandle(th); }
			if (g_sym) describe(st, d, sizeof(d)); else std::snprintf(d, sizeof(d), "%llX", (unsigned long long)st);
			std::fprintf(g_log, "%s thread %5lu  start %s%s\n", first ? "   at start:" : "NEW THREAD:", (unsigned long)id, d, id == g_main_tid ? "   (frame loop)" : id == self ? "   (this watcher)" : "");
			if (!first) std::fprintf(g_log, "            ...appeared at t %.3f s, frame %u\n", since(), g_calltrace.frame.load());
			if (!first && (g_opts & 4))
				for (int k = 0; k < 3; k++)
				{
					HANDLE t2 = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
					if (!t2) break;
					static DWORD64 o[kMaxFrames];
					const int on = capture(t2, o);
					CloseHandle(t2);
					sym_init();
					print_stack("            ...its stack:", o, on, 30);
					Sleep(3);
				}
		}
		HMODULE hm[1024]; DWORD need = 0;
		// (K32EnumProcessModules without psapi.h)
		using Enum = BOOL(WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD);
		static Enum en = reinterpret_cast<Enum>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32EnumProcessModules"));
		if (en && en(GetCurrentProcess(), hm, sizeof(hm), &need))
			for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++)
				if (mods.insert(hm[i]).second && !first)
				{
					char path[MAX_PATH] = "?";
					GetModuleFileNameA(hm[i], path, MAX_PATH);
					std::fprintf(g_log, "NEW MODULE: %s at t %.3f s, frame %u\n", path, since(), g_calltrace.frame.load());
				}
		std::fflush(g_log);
	};
	if (!(g_opts & 32)) scan(true);   // (32, 64: experiments on what the watcher's presence changes)
	double last_scan = perf_now_ms(), sampled_beat = -1, last_sample = 0;
	int samples = 0;
	static DWORD64 pcs[kMaxFrames];
	bool in_stall = false;
	while (g_run.load())
	{
		WaitForSingleObject(g_timer, (g_opts & 8) ? 2 : in_stall ? 10 : INFINITE);
		const double now = perf_now_ms(), beat = g_beat.load();
		if ((g_opts & 8) && now - last_scan > ((g_opts & 4) ? 15.0 : 250.0)) { scan(false); last_scan = now; }
		in_stall = beat > 0 && now - beat >= g_limit - 1.5 && g_calltrace.frame.load() >= g_from;   // (the timer may fire a little early)
		if (!in_stall) continue;
		if (beat != sampled_beat) { sampled_beat = beat; samples = 0; last_sample = 0; }
		if (samples >= 6) { in_stall = false; continue; }
		if (now - last_sample < 9.0) continue;
		// read what the render thread says it is in before it is stopped
		const char *call = g_calltrace.call.load(), *phase = g_calltrace.phase.load(), *api = g_calltrace.api.load();
		const int n = capture(g_main, pcs);
		sym_init();
		char head[400];
		std::snprintf(head, sizeof(head), "STALL: frame %u, t %.3f s: the frame loop has not come round for %.1f ms (sample %d) | %s call in progress: %s | phase: %s",
		              g_calltrace.frame.load(), since(), now - beat, samples + 1, api, *call ? call : "(none)", *phase ? phase : "(none)");
		print_stack(head, pcs, n, kMaxFrames);
		if (samples == 0)
		{
			// once per stall: what everybody else is doing (driver threads, audio, force feedback)
			for (DWORD id : thread_ids())
			{
				if (id == self || id == g_main_tid) continue;
				HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
				if (!th) continue;
				static DWORD64 o[kMaxFrames];
				const int on = capture(th, o);
				char d[400], h2[480];
				describe(thread_start(th), d, sizeof(d));
				std::snprintf(h2, sizeof(h2), "   other thread %lu (start %s):", (unsigned long)id, d);
				print_stack(h2, o, on, 14);
				CloseHandle(th);
			}
		}
		if (samples == 0) scan(false);   // threads and modules that have appeared since
		std::fflush(g_log);
		samples++; last_sample = now;
	}
	if (g_sym) SymCleanup(GetCurrentProcess());
}

}   // namespace

namespace stallwatch {

void start(const std::string &log_path, double limit_ms)
{
	if (g_run.load() || log_path.empty()) return;
	g_log = std::fopen(log_path.c_str(), "ab");
	if (!g_log) return;
	g_limit = limit_ms; g_t0 = perf_now_ms();
	g_main_tid = GetCurrentThreadId();
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
	std::fprintf(g_log, "== stall watch: stack of the frame loop when a frame takes more than %.0f ms\n", limit_ms);
	if (const char *e = getenv("STALLWATCH")) g_opts = atoi(e);
	if (const char *e = getenv("STALLFROM")) g_from = uint32_t(atoi(e));
	g_timer = CreateWaitableTimerA(nullptr, FALSE, nullptr);
	g_run.store(true);
	g_thread = std::thread(watcher);
}

void beat()
{
	// experiment THREADAT=<frame>[,<frame>...]: a thread that does nothing is started in those frames (and ends 100 ms later)
	static std::vector<uint32_t> at = [] {
		std::vector<uint32_t> v;
		if (const char *e = getenv("THREADAT"))
			for (const char *q = e; *q;) { v.push_back(uint32_t(atoi(q))); q = std::strchr(q, ','); if (!q) break; q++; }
		return v; }();
	for (uint32_t f : at)
		if (f == g_calltrace.frame.load()) std::thread([] { Sleep(getenv("THREADMS") ? DWORD(atoi(getenv("THREADMS"))) : 100); }).detach();
	if (!g_run.load(std::memory_order_relaxed)) return;
	g_beat.store(perf_now_ms(), std::memory_order_relaxed);
	if (g_opts & 64) return;
	LARGE_INTEGER due;
	due.QuadPart = -LONGLONG(g_limit * 10000.0);   // relative, in 100 ns
	SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
}

void stop()
{
	if (!g_run.exchange(false)) return;
	LARGE_INTEGER due;
	due.QuadPart = -1;
	SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
	g_thread.join();
	CloseHandle(g_timer); g_timer = nullptr;
	if (g_log) { std::fprintf(g_log, "\n"); std::fclose(g_log); g_log = nullptr; }
	if (g_main) { CloseHandle(g_main); g_main = nullptr; }
}

}   // namespace stallwatch
