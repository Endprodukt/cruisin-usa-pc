// Runs the DCS sound board (ADSP-2105) on its own thread, in parallel with the main CPU.
//
// The main CPU only ever writes to the sound board (command bytes, reset line); it never reads its state back except for the
// "input latch full" flag right before a command byte. So the main thread can hand out the DSP's time slices (the same per-scanline
// amounts as before, in the same order) through a single-producer / single-consumer ring and continue at once. Wherever it needs the
// board itself (a command byte, a reset, the end of the frame) it waits until the worker has caught up (drain) and then uses the
// board directly while the worker is idle. If the worker is not running a slice at that moment (asleep, or not yet scheduled by the
// OS), drain() runs the remaining slices itself, so the frame never waits for the thread. The DSP therefore sees exactly the same
// sequence of operations as when it ran inline:
// the sound output is bit-identical, and nothing about the game's timing changes.
//
// No locks while running: the ring indices are atomics. The worker spins briefly when the ring runs empty (the next slice follows
// within microseconds during a frame) and sleeps on a condition variable between frames.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class Dcs1;

class DcsWorker
{
public:
	explicit DcsWorker(Dcs1 &dcs);
	~DcsWorker();
	DcsWorker(const DcsWorker &) = delete;
	DcsWorker &operator=(const DcsWorker &) = delete;

	// main thread: queue a time slice for the DSP (ADSP cycles)
	void advance(double adsp_cycles);
	// main thread: wait until every queued slice has run; afterwards the Dcs1 may be used directly until the next advance()
	void drain();
	// main thread, after drain(): milliseconds the worker spent running the DSP since the last call
	double take_busy_ms();

	// whether a worker thread pays off on this machine (enough hardware threads for the main CPU, the DSP and the host's own threads)
	static bool worthwhile();

private:
	void run();

	Dcs1 &m_dcs;
	static constexpr uint32_t kSize = 4096;   // power of two; a frame queues ~450 slices
	std::vector<double> m_ring;
	alignas(64) std::atomic<uint32_t> m_head{0};   // written by the main thread
	alignas(64) std::atomic<uint32_t> m_tail{0};   // written by the worker
	alignas(64) std::atomic<bool> m_sleeping{false};
	alignas(64) std::atomic<bool> m_running{false};  // held by whichever thread is running the DSP (worker per slice, or drain)
	std::atomic<bool> m_quit{false};
	std::atomic<uint64_t> m_busy_ns{0};
	std::mutex m_mx;
	std::condition_variable m_cv;
	std::thread m_thread;
};
