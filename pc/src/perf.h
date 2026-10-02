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
	~PerfStats()
	{
		if (!log_path.empty() && !m_log_text.empty())
			if (FILE *f = std::fopen(log_path.c_str(), "ab")) { std::fwrite(m_log_text.data(), 1, m_log_text.size(), f); std::fclose(f); }
	}
	std::string m_log_text;

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
		if (!log_path.empty()) { m_log_text += m_summary; m_log_text += "\n"; }   // (written at the end: no file access in the frame loop)
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

// Per-frame recorder (--perf): every displayed frame with the time of each phase, kept in a ring. A frame that takes longer
// than the limit is written to perf_spikes.log together with the frames before and after it, so that a hitch can be traced to
// its phase. --perf-csv <file> additionally writes every frame for offline analysis.
struct FrameRec
{
	uint32_t frame = 0;
	float dt = 0;          // from the end of the previous present to the end of this one: what the eye sees
	float msg = 0;         // window messages
	float input = 0;       // keyboard / wheel / pad polling
	float emu = 0;         // the whole emulated frame...
	float cpu = 0, dsp = 0, feed = 0;   // ...of which: main CPU, sound DSP, handing polygons to the GPU driver
	float ffb = 0;         // telemetry, force feedback, cabinet outputs
	float wait = 0;        // the pacing wait
	float present = 0;     // final blit and buffer swap (with VSync: includes waiting for the display)
	float swap = 0;        // of which the swap call itself
	float gpu = 0;         // GPU time of a recent frame (-1: not measured)
	// the frame's timeline, perf_now_ms(): top of the frame loop, emulation done, present called, swap called, swap returned,
	// GPU finished the frame (0 = not known); gl_frame: the backend's number of this present
	double t_top = 0, t_emu = 0, t_present = 0, t_swap0 = 0, t_swap1 = 0, t_gpu_done = 0;
	uint32_t gl_frame = 0;
	float gpu_ph[6] = {-1, -1, -1, -1, -1, -1};   // ...by phase (IVideoBackend::GpuPhase): polygons, shadows, overlay, latch, present, uploads
	float other = 0;       // window title, bookkeeping
	float audio_ms = 0;    // sound queued in the output buffer
	uint32_t quads = 0, draws = 0;
	uint32_t mode = 0;     // the game's mode word
	uint8_t flips = 0;     // new game pictures shown in this frame (page flips)
	uint8_t clock_q4 = 4;  // emulated CPU clock in quarters
	uint8_t ran = 0;       // emulated frames run in this display frame
	uint8_t late = 0;      // game pictures that were due in this frame but not finished (host time budget used up)
	uint32_t refresh = 0;  // the display's refresh count (desktop compositor) when the swap returned: the picture shows from the next one
	float phase = 0;       // ms since the display's last vblank at that moment
};

class FrameProf
{
public:
	bool on = false;
	double spike_ms = 0;          // 0 = automatic: 1.5 x the median frame time
	std::string spike_path, csv_path;

	void gpu_done(uint32_t gl_frame, double ms)
	{
		for (size_t i = m_all.size(), k = 0; i-- > 0 && k < 32; k++)
			if (m_all[i].gl_frame == gl_frame) { m_all[i].t_gpu_done = ms; return; }
	}

	void gpu_phases(uint32_t gl_frame, const double *ms)
	{
		for (size_t i = m_all.size(), k = 0; i-- > 0 && k < 32; k++)
			if (m_all[i].gl_frame == gl_frame)
			{
				float sum = 0;
				for (int p = 0; p < 6; p++) { m_all[i].gpu_ph[p] = float(ms[p]); sum += float(ms[p]); }
				m_all[i].gpu = sum;
				return;
			}
	}

	void add(const FrameRec &r)
	{
		if (!on) return;
		m_all.push_back(r);
		const size_t n = m_all.size();
		if (n < 240) return;                                    // start-up is not judged
		if (n % 120 == 0 || m_median <= 0)
		{
			std::vector<float> s;
			for (size_t i = n - 240; i < n; i++) s.push_back(m_all[i].dt);
			std::nth_element(s.begin(), s.begin() + 120, s.end());
			m_median = s[120];
		}
		const double limit = spike_ms > 0 ? spike_ms : double(m_median) * 1.5;
		if (r.dt > limit && n - m_last_spike > 8) { m_pending.push_back(n - 1); m_last_spike = n - 1; m_spikes++; }
		while (!m_pending.empty() && n - 1 >= m_pending.front() + 6) { write_spike(m_pending.front(), limit); m_pending.erase(m_pending.begin()); }
	}

	// percentiles and the cadence of the game's own pictures, then the csv
	void finish(const char *title)
	{
		if (!on || m_all.size() < 300) return;
		std::vector<float> dt;
		for (size_t i = 240; i < m_all.size(); i++) dt.push_back(m_all[i].dt);
		std::sort(dt.begin(), dt.end());
		auto pct = [&](double q) { return dt[std::min(dt.size() - 1, size_t(q * double(dt.size())))]; };
		const float med = pct(0.5);
		size_t over125 = 0, over150 = 0, over200 = 0;
		for (float v : dt) { over125 += v > med * 1.25f; over150 += v > med * 1.5f; over200 += v > med * 2.0f; }
		// cadence: display frames per game picture
		uint32_t hist[8] = {};
		int since = 0;
		for (size_t i = 240; i < m_all.size(); i++)
		{
			since++;
			if (m_all[i].flips) { hist[std::min(since, 7)]++; since = 0; }
		}
		double sum[9] = {}; float mx[9] = {};
		uint32_t late = 0, race = 0, race_h[5] = {};
		int rsince = 0;
		// what the display shows: refreshes each presented frame stays up (0 = replaced before it was ever shown), and refreshes
		// between two new game pictures in the race (2 = the game's own pace; anything else is a visible hitch)
		uint32_t shown[5] = {}, pic[8] = {};
		uint32_t pic_ref = 0; bool pic_have = false;
		for (size_t i = 241; i < m_all.size(); i++)
		{
			const FrameRec &r = m_all[i], &q = m_all[i - 1];
			if (!r.refresh || !q.refresh) continue;
			shown[std::min<uint32_t>(r.refresh - q.refresh, 4)]++;
			if ((r.mode & 0xf) == 4 && (r.mode & 0x200))
			{
				if (r.flips) { if (pic_have) pic[std::min<uint32_t>(r.refresh - pic_ref, 7)]++; pic_ref = r.refresh; pic_have = true; }
			}
			else pic_have = false;
		}
		for (size_t i = 240; i < m_all.size(); i++)
		{
			const FrameRec &r = m_all[i];
			late += r.late;
			if ((r.mode & 0xf) == 4 && (r.mode & 0x200))   // in the race proper (MGO): display frames per game picture
			{
				race++; rsince++;
				if (r.flips) { race_h[std::min(rsince, 4)]++; rsince = 0; }
			}
			else rsince = 0;
			const float v[9] = {r.msg, r.input, r.cpu, r.dsp, r.feed, r.ffb, r.wait, r.present, r.other};
			for (int k = 0; k < 9; k++) { sum[k] += v[k]; mx[k] = std::max(mx[k], v[k]); }
		}
		const double nn = double(m_all.size() - 240);
		if (!spike_path.empty())
			if (FILE *f = std::fopen(spike_path.c_str(), "ab"))
			{
				std::fwrite(m_spike_text.data(), 1, m_spike_text.size(), f);
				std::fprintf(f, "== %s: %zu frames | median %.2f ms, p99 %.2f, p99.9 %.2f, max %.2f | over 1.25x median: %zu, 1.5x: %zu, 2x: %zu | spikes logged %zu\n",
				             title, dt.size(), med, pct(0.99), pct(0.999), dt.back(), over125, over150, over200, m_spikes);
				static const char *const nm[9] = {"messages", "input", "main cpu", "sound dsp", "gpu feed", "ffb/outputs", "wait", "present", "other"};
				std::fprintf(f, "   phase avg / max ms:");
				for (int k = 0; k < 9; k++) std::fprintf(f, " %s %.2f/%.2f |", nm[k], sum[k] / nn, mx[k]);
				std::fprintf(f, "\n   display frames per game picture: 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7+:%u\n", hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
				std::fprintf(f, "   in the race (%u frames): 1:%u 2:%u 3:%u 4+:%u | game pictures late for lack of host time: %u\n", race, race_h[1], race_h[2], race_h[3], race_h[4], late);
				std::fprintf(f, "   on the display: refreshes per presented frame 0:%u 1:%u 2:%u 3:%u 4+:%u | refreshes per game picture in the race 1:%u 2:%u 3:%u 4:%u 5:%u 6+:%u\n\n",
				             shown[0], shown[1], shown[2], shown[3], shown[4], pic[1], pic[2], pic[3], pic[4], pic[5], pic[6] + pic[7]);
				std::fclose(f);
			}
		if (!csv_path.empty())
			if (FILE *f = std::fopen(csv_path.c_str(), "wb"))
			{
				std::fprintf(f, "frame,dt,msg,input,emu,cpu,dsp,feed,ffb,wait,present,swap,gpu,other,audio_ms,quads,draws,mode,flips,clock_q4,ran,late,refresh,phase,g_poly,g_shadow,g_overlay,g_latch,g_present,g_upload,t_top,t_emu,t_present,t_swap0,t_swap1,t_gpu_done\n");
				for (const FrameRec &r : m_all)
					std::fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%u,%u,%X,%u,%u,%u,%u,%u,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", r.frame, r.dt, r.msg, r.input, r.emu, r.cpu, r.dsp,
					             r.feed, r.ffb, r.wait, r.present, r.swap, r.gpu, r.other, r.audio_ms, r.quads, r.draws, r.mode, r.flips, r.clock_q4, r.ran, r.late, r.refresh, r.phase, r.gpu_ph[0], r.gpu_ph[1], r.gpu_ph[2], r.gpu_ph[3], r.gpu_ph[4], r.gpu_ph[5], r.t_top, r.t_emu, r.t_present, r.t_swap0, r.t_swap1, r.t_gpu_done);
				std::fclose(f);
			}
	}

private:
	// (kept in memory and written by finish(): a file write in the frame loop can itself be the next spike)
	void write_spike(size_t at, double limit)
	{
		if (spike_path.empty() || m_spike_text.size() > (8u << 20)) return;
		char b[400];
		std::snprintf(b, sizeof(b), "spike: frame %u took %.2f ms (limit %.2f, median %.2f)\n", m_all[at].frame, m_all[at].dt, limit, m_median);
		m_spike_text += b;
		m_spike_text += "    frame      dt |   msg input |   emu =  cpu +  dsp + feed |  ffb  wait | present (swap)   gpu | other | audio quads draws mode  flip clk ran\n";
		for (size_t i = at >= 8 ? at - 8 : 0; i <= at + 6 && i < m_all.size(); i++)
		{
			const FrameRec &r = m_all[i];
			std::snprintf(b, sizeof(b), " %c %7u %6.2f | %5.2f %5.2f | %5.2f  %5.2f  %5.2f  %5.2f | %4.2f %5.2f | %6.2f  %5.2f %5.2f | %5.2f | %5.0f %5u %5u %5X %4u %3.2g %3u\n", i == at ? '>' : ' ',
			              r.frame, r.dt, r.msg, r.input, r.emu, r.cpu, r.dsp, r.feed, r.ffb, r.wait, r.present, r.swap, r.gpu, r.other, r.audio_ms, r.quads, r.draws, r.mode & 0xfffff, r.flips,
			              double(r.clock_q4) / 4.0, r.ran);
			m_spike_text += b;
		}
		m_spike_text += "\n";
	}
	std::string m_spike_text;

	std::vector<FrameRec> m_all;
	std::vector<size_t> m_pending;
	size_t m_last_spike = 0, m_spikes = 0;
	float m_median = 0;
};
