#include "dcs_worker.h"

#include <chrono>

#include "../audio/dcs.h"

#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_relax() { _mm_pause(); }
#else
static inline void cpu_relax() {}
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using clk = std::chrono::steady_clock;
uint64_t ns_since(clk::time_point t0) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count()); }
}

DcsWorker::DcsWorker(Dcs1 &dcs) : m_dcs(dcs), m_ring(kSize, 0.0)
{
	m_thread = std::thread([this] { run(); });
}

DcsWorker::~DcsWorker()
{
	drain();
	{
		std::lock_guard<std::mutex> lk(m_mx);
		m_quit.store(true);
	}
	m_cv.notify_one();
	if (m_thread.joinable()) m_thread.join();
}

bool DcsWorker::worthwhile()
{
	return std::thread::hardware_concurrency() >= 4;
}

void DcsWorker::advance(double adsp_cycles)
{
	const uint32_t h = m_head.load(std::memory_order_relaxed);
	if (h - m_tail.load(std::memory_order_acquire) >= kSize) drain();   // ring full: the worker is far behind, catch up here
	m_ring[h & (kSize - 1)] = adsp_cycles;
	m_head.store(h + 1, std::memory_order_seq_cst);
	// the worker announces its sleep before it checks the ring a last time (both seq_cst), so either it sees this slice or we see it asleep
	if (m_sleeping.load(std::memory_order_seq_cst))
	{
		std::lock_guard<std::mutex> lk(m_mx);
		m_cv.notify_one();
	}
}

void DcsWorker::drain()
{
	const uint32_t h = m_head.load(std::memory_order_relaxed);
	for (;;)
	{
		if (m_tail.load(std::memory_order_acquire) == h) return;
		// The worker is not inside a slice right now (asleep, not yet woken by the OS, or between two slices): run the rest here.
		// This way the frame never waits for the thread to be scheduled; whoever holds m_running runs the DSP, one slice at a time,
		// in ring order, so the result is the same either way.
		if (!m_running.exchange(true, std::memory_order_acquire))
		{
			const auto t0 = clk::now();
			uint32_t t = m_tail.load(std::memory_order_relaxed);
			for (; t != h; t++) m_dcs.advance(m_ring[t & (kSize - 1)]);
			m_tail.store(t, std::memory_order_release);
			m_running.store(false, std::memory_order_release);
			m_busy_ns.fetch_add(ns_since(t0), std::memory_order_relaxed);
			return;
		}
		cpu_relax();   // the worker is in the middle of one slice (a few microseconds)
	}
}

double DcsWorker::take_busy_ms()
{
	return double(m_busy_ns.exchange(0, std::memory_order_relaxed)) * 1e-6;
}

void DcsWorker::run()
{
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
	int idle = 0;
	for (;;)
	{
		if (m_tail.load(std::memory_order_relaxed) != m_head.load(std::memory_order_acquire) && !m_running.exchange(true, std::memory_order_acquire))
		{
			// one slice per claim, so that a drain on the main thread never waits longer than one slice
			const auto t0 = clk::now();
			const uint32_t t = m_tail.load(std::memory_order_relaxed);
			const bool work = t != m_head.load(std::memory_order_acquire);
			if (work)
			{
				m_dcs.advance(m_ring[t & (kSize - 1)]);
				m_tail.store(t + 1, std::memory_order_release);
			}
			m_running.store(false, std::memory_order_release);
			if (work) m_busy_ns.fetch_add(ns_since(t0), std::memory_order_relaxed);
			idle = 0;
			continue;
		}
		if (m_quit.load(std::memory_order_relaxed)) return;
		// during a frame the next slice comes within microseconds: spin a little before sleeping
		if (++idle < 3000) { cpu_relax(); continue; }
		std::unique_lock<std::mutex> lk(m_mx);
		m_sleeping.store(true, std::memory_order_seq_cst);
		m_cv.wait(lk, [&] { return m_quit.load() || m_head.load(std::memory_order_seq_cst) != m_tail.load(std::memory_order_relaxed); });
		m_sleeping.store(false, std::memory_order_seq_cst);
		idle = 0;
	}
}
