// Optional performance statistics (--perf): frame pacing, phase timings and GPU feed counters. Costs a few clock reads per frame.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

inline double perf_now_ms()
{
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
}

class PerfStats
{
public:
	bool on = false;
	std::string log_path;

	void begin_frame() { if (on) m_t0 = perf_now_ms(); }
	// call after emulation (ms spent emulating + feeding the GPU), after present, once per displayed frame
	void gpu(double ms) { if (on && ms >= 0) { m_gpu += ms; m_ngpu++; } }
	void emulated(double emu_ms, double feed_ms, uint64_t draws, uint64_t quads, double cpu_ms = 0, double dcs_ms = 0)
	{
		if (!on) return;
		m_cur_emu = float(emu_ms);
		m_cpu += cpu_ms; m_dcs += dcs_ms;
		m_emu += emu_ms; m_feed += feed_ms; m_draws += draws; m_quads += quads; m_nemu++;
	}
	void presented(double present_ms)
	{
		if (!on) return;
		double now = perf_now_ms();
		if (m_last > 0)
		{
			float dt = float(now - m_last);
			m_ft.push_back(dt);
			if (dt > 24.0f && m_out.size() < 6) { char b[96]; std::snprintf(b, sizeof b, "[%.1f ms: emu %.1f present %.1f]", dt, m_cur_emu, float(present_ms)); m_out += b; m_out.push_back(' '); }
			m_worst = std::max(m_worst, dt);
		}
		m_last = now;
		m_pres += present_ms; m_npres++;
		if (now - m_window > 5000.0) report(now);
	}
	std::string summary() const { return m_summary; }

private:
	void report(double now)
	{
		if (m_ft.empty()) { m_window = now; return; }
		std::vector<float> s = m_ft;
		std::sort(s.begin(), s.end());
		double sum = 0;
		for (float v : s) sum += v;
		size_t n = s.size(), k1 = std::max<size_t>(1, n / 100), k01 = std::max<size_t>(1, n / 1000);
		double w1 = 0, w01 = 0;
		for (size_t i = 0; i < k1; i++) w1 += s[n - 1 - i];
		for (size_t i = 0; i < k01; i++) w01 += s[n - 1 - i];
		char buf[512];
		std::snprintf(buf, sizeof buf,
		              "fps %.1f | frame avg %.2f ms max %.2f | 1%% low %.1f fps 0.1%% low %.1f fps | emu %.2f ms (cpu %.2f dsp %.2f gpu feed %.2f) present %.2f ms | gpu %.2f ms | draws/frame %.1f quads/frame %.0f",
		              1000.0 * double(n) / sum, sum / double(n), double(m_worst), 1000.0 / (w1 / double(k1)), 1000.0 / (w01 / double(k01)),
		              m_nemu ? m_emu / double(m_nemu) : 0.0, m_nemu ? m_cpu / double(m_nemu) : 0.0, m_nemu ? m_dcs / double(m_nemu) : 0.0, m_nemu ? m_feed / double(m_nemu) : 0.0, m_npres ? m_pres / double(m_npres) : 0.0, m_ngpu ? m_gpu / double(m_ngpu) : -1.0,
		              m_nemu ? double(m_draws) / double(m_nemu) : 0.0, m_nemu ? double(m_quads) / double(m_nemu) : 0.0);
		m_summary = buf;
		if (!m_out.empty()) { m_summary += " | slow frames: " + m_out; m_out.clear(); }
		if (!log_path.empty())
			if (FILE *f = std::fopen(log_path.c_str(), "ab")) { std::fprintf(f, "%s\n", m_summary.c_str()); std::fclose(f); }
		m_ft.clear(); m_worst = 0; m_emu = m_feed = m_pres = m_cpu = m_dcs = m_gpu = 0; m_ngpu = 0; m_nemu = m_npres = 0; m_draws = m_quads = 0;
		m_window = now;
	}

	double m_t0 = 0, m_last = 0, m_window = 0;
	double m_gpu = 0; int m_ngpu = 0;
	double m_emu = 0, m_feed = 0, m_pres = 0, m_cpu = 0, m_dcs = 0;
	uint64_t m_draws = 0, m_quads = 0;
	int m_nemu = 0, m_npres = 0;
	float m_worst = 0, m_cur_emu = 0;
	std::string m_out;
	std::vector<float> m_ft;
	std::string m_summary;
};
