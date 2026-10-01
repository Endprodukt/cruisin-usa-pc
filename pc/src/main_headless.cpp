// Headless test driver: runs N frames and writes a PNG screenshot.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "machine/autosetup.h"
#include "machine/cmos.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include "profiler.h"
#include "machine/midvunit.h"
#include "machine/telemetry.h"
#include "input/ffb_modern.h"
#include "../third_party/miniz/miniz.h"

int main(int argc, char **argv)
{
	std::string rom = "D:/Mame/roms/crusnusa.zip", shot = "shot.png", ver = "4.5";
	int frames = 300;
	std::string wav, makenv;
	bool autoplay = false; int seq = 0;
	struct Ev { int at, dur; uint16_t bit; };
	std::vector<Ev> evs;
	struct Ax { int at; char which; int val; };
	std::vector<Ax> axes;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
		else if (!strcmp(argv[i], "--press") && i + 1 < argc)
		{   // name@frame+dur  e.g. test@2000+10
			std::string a = argv[++i]; char n[32]; int at, dur;
			if (sscanf(a.c_str(), "%31[a-z0-9]@%d+%d", n, &at, &dur) == 3)
			{
				std::string nm = n; uint16_t b = 0;
				if (nm == "test") b = in0bit::TEST; else if (nm == "coin") b = in0bit::COIN1;
				else if (nm == "start") b = in0bit::START; else if (nm == "voldn") b = in0bit::VOLDN; else if (nm == "volup") b = in0bit::VOLUP; else if (nm == "service") b = in0bit::SERVICE;
				evs.push_back({at, dur, b});
			}
		}
		else if (!strcmp(argv[i], "--axis") && i + 1 < argc)
		{   // w|a|b@frame=value
			char c; int at, v; std::string a = argv[++i];
			if (sscanf(a.c_str(), "%c@%d=%d", &c, &at, &v) == 3) axes.push_back({at, c, v});
		}
		else if (!strcmp(argv[i], "--make-nv") && i + 1 < argc) makenv = argv[++i];
		else if (!strcmp(argv[i], "--autoplay")) autoplay = true;
		else if (!strcmp(argv[i], "--seq") && i + 1 < argc) seq = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
		else if (!strcmp(argv[i], "--ver") && i + 1 < argc) ver = argv[++i];
	}
	MidVUnit m;
	std::string err;
	if (!m.load_roms(rom, ver, err)) { fprintf(stderr, "ROM error: %s\n", err.c_str()); return 1; }
	std::vector<int16_t> pcm; double rate = 0; int blocks = 0;
	m.on_audio = [&](const int16_t *b, int n, double r) { pcm.insert(pcm.end(), b, b + n); if (rate == 0) rate = r; blocks++; };
	if (const char *dd = getenv("DD")) m.rom_patches.draw_distance_pct = atoi(dd);
	if (getenv("PCHIST")) m.m_pchist_on = true;
	if (const char *wm = getenv("WM")) m.rom_patches.wide_margin = atoi(wm);
	if (getenv("NORASTER")) m.skip_raster = true;
	if (const char *rb = getenv("RB")) m.rom_patches.rubberband_pct = atoi(rb);
	m.reset();
	if (getenv("DEFAULT_NV")) m.load_default_nvram();
	if (const char *adj = getenv("ADJ")) { int idx, val, n = 0; const char *q = adj; while (sscanf(q, "%d=%d%n", &idx, &val, &n) == 2) { cmos::set(m.nvram(), idx, uint32_t(val)); q += n; if (*q == ',') q++; } }
	if (!makenv.empty()) { run_auto_setup(m); m.save_nvram(makenv); fprintf(stderr, "saved %s\n", makenv.c_str()); return 0; }
	SampleProfiler prof;
	if (getenv("PROFILE")) prof.start();
	for (int f = 0; f < frames; f++)
	{
		for (auto &a : axes) if (a.at == f) { if (a.which == 'w') m.inputs.wheel = a.val; else if (a.which == 'a') m.inputs.accel = a.val; else m.inputs.brake = a.val; }
		m.inputs.in0 = 0xffff;
		for (auto &e : evs) if (f >= e.at && f < e.at + e.dur) m.inputs.in0 &= ~e.bit;
		if (autoplay)
		{
			int af = f - 1355;
			auto hit = [&](int at) { return af >= at && af < at + 6; };
			if (hit(60) || hit(80) || hit(100)) m.inputs.in0 &= ~in0bit::COIN1;
			if (hit(140) || hit(320) || hit(500) || hit(680) || hit(860) || hit(1040)) m.inputs.in0 &= ~in0bit::START;
			if (af > 1300) m.inputs.accel = 255;
		}
		m.run_frame();
		if (seq > 0 && f >= frames - seq)
		{
			int w = m.screen_w(), h = m.screen_h();
			std::vector<uint8_t> rgb(size_t(w) * h * 3);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
				{
					uint32_t c = m.frame_rgba()[y * MidVUnit::FRAME_STRIDE + x];
					uint8_t *d = &rgb[(size_t(y) * w + x) * 3];
					d[0] = c >> 16; d[1] = c >> 8; d[2] = c;
				}
			size_t len = 0; void *png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &len);
			char name[256]; std::snprintf(name, sizeof(name), "%s_%03d.png", shot.substr(0, shot.rfind('.')).c_str(), f - (frames - seq));
			FILE *fp = fopen(name, "wb"); fwrite(png, 1, len, fp); fclose(fp);
		}
		{ static bool once = false; if (!once && m.quads_last_frame > 50) { once = true; fprintf(stderr, "first 3D frame: %d\n", f); } }
		if (getenv("VSTAT") && f % 500 == 0) { fprintf(stderr, "f%d vram r/w %llu/%llu pal %llu tex %llu quads %llu\n", f, (unsigned long long)m.stat_vram_reads, (unsigned long long)m.stat_vram_writes, (unsigned long long)m.stat_pal_writes, (unsigned long long)m.stat_tex_writes, (unsigned long long)m.quads_last_frame); }
		if (getenv("ROTLOG")) { Telemetry t; if (m.read_telemetry(t) && f >= atoi(getenv("ROTLOG"))) fprintf(stderr, "R %d wheel=%d spd=%.1f turn=%.3f yrot=%.4f vrot=%.4f drot=%.4f rot=%.4f xl=%.3f zl=%.3f\n", f, m.inputs.wheel, t.speed, t.turn, t.y_rot, t.v_rot, t.d_rot, t.over_rot, t.x_lean, t.z_lean); }
		if (getenv("FFBTEST"))
		{
			static FfbModern fx; static float mx = 0, sum2 = 0, sum1 = 0; static int n = 0, kicks = 0;
			Telemetry t; bool ok = m.read_telemetry(t);
			fx.frame(ok ? t : Telemetry{}, 0.0f, (float(m.inputs.wheel) - 128.0f) / 112.0f);
			for (int k = 0; k < 4; k++) { float o = fx.step(1.0 / 232.0); mx = std::max(mx, std::fabs(o)); sum2 += o * o; sum1 += o; n++; }
			if (f % atoi(getenv("FFBTEST")) == 0 && ok) { fprintf(stderr, "FFB f%d wheel=%d spd=%.0f slip=%.3f onroad=%X mean=%+.2f max=%.2f rms=%.3f vib=%.2f\n", f, m.inputs.wheel, t.speed, fx.slip(), t.onroad, sum1 / std::max(1, n), mx, std::sqrt(sum2 / std::max(1, n)), fx.vibration()); mx = 0; sum2 = 0; sum1 = 0; n = 0; }
			(void)kicks;
		}
		if (const char *tl = getenv("TELEMLOG")) { Telemetry t; if (m.read_telemetry(t) && f % atoi(tl) == 0) fprintf(stderr, "T %d spd=%.2f skid=%.2f thr=%.2f brk=%.2f turn=%.3f trac=%.2f rpm=%.1f yv=%.3f xm=%.3f zm=%.3f xl=%.3f zl=%.3f d2c=%.1f road=%d onroad=%d bump=%d spin=%d air=%d/%d gear=%d yv0=%.2f dy0=%.2f col=%d\n", f, t.speed, t.skid, t.throttle, t.brake, t.turn, t.traction, t.rpm, t.y_vel, t.x_mom, t.z_mom, t.x_lean, t.z_lean, t.dist_to_center, (int)t.road_friction, t.onroad, t.bump, t.spin, t.air_front, t.air_rear, t.gear, t.susp_yv[0], t.susp_dy[0], t.collided[0]); }
		if (f % 60 == 0) fprintf(stderr, "frame %d pc=%06X quads=%llu vis=%dx%d\n", f, m.cpu_pc(), (unsigned long long)m.quads_last_frame, m.screen_w(), m.screen_h());
	}
	if (const uint64_t *h = m.cpu_hits()) { std::vector<std::pair<uint64_t, int>> v; uint64_t tot = 0; for (int i = 0; i < 2048; i++) { v.push_back({h[i], i}); tot += h[i]; } std::sort(v.rbegin(), v.rend()); fprintf(stderr, "OPS total %llu%c", (unsigned long long)tot, 10); for (int i = 0; i < 25; i++) fprintf(stderr, "OP %03X %llu (%.1f%%)%c", v[i].second, (unsigned long long)v[i].first, 100.0 * v[i].first / double(tot), 10); }
	if (getenv("PROFILE")) prof.stop_and_report();
	if (getenv("CPUTIME")) fprintf(stderr, "CPUTIME main cpu %.1f ms, dsp %.1f ms over %d frames (%.3f ms/frame)%c", m.perf_cpu_ms, m.perf_dcs_ms, frames, m.perf_cpu_ms / frames, 10);
	if (getenv("RAMUSE")) m.debug_ram_usage();
	if (const char *tr = getenv("TEXRAW")) { FILE *tf = fopen(tr, "wb"); fwrite(m.texture_ram(), 1, 0x400000, tf); fclose(tf); }
	if (const char *td = getenv("TEXDUMP"))
	{   // TEXDUMP=firstpage  -> 8x8 grid of 256x256 pages as grayscale
		int first = atoi(td); int W = 2048, H = 2048; std::vector<uint8_t> g(size_t(W) * H);
		for (int pg = 0; pg < 64; pg++)
			for (int y = 0; y < 256; y++)
				for (int x = 0; x < 256; x++)
					g[size_t((pg / 8) * 256 + y) * W + (pg % 8) * 256 + x] = m.texture_ram()[size_t(first + pg) * 65536 + y * 256 + x];
		size_t l = 0; void *pn = tdefl_write_image_to_png_file_in_memory(g.data(), W, H, 1, &l);
		FILE *fp = fopen("texdump.png", "wb"); fwrite(pn, 1, l, fp); fclose(fp);
	}
	if (!wav.empty() && !pcm.empty())
	{
		FILE *wf = fopen(wav.c_str(), "wb");
		uint32_t sr = uint32_t(rate + 0.5), dl = uint32_t(pcm.size() * 2), br = sr * 2, rl = 36 + dl;
		uint16_t fmt = 1, ch = 1, ba = 2, bps = 16; uint32_t fl = 16;
		fwrite("RIFF", 1, 4, wf); fwrite(&rl, 4, 1, wf); fwrite("WAVEfmt ", 1, 8, wf); fwrite(&fl, 4, 1, wf);
		fwrite(&fmt, 2, 1, wf); fwrite(&ch, 2, 1, wf); fwrite(&sr, 4, 1, wf); fwrite(&br, 4, 1, wf);
		fwrite(&ba, 2, 1, wf); fwrite(&bps, 2, 1, wf); fwrite("data", 1, 4, wf); fwrite(&dl, 4, 1, wf);
		fwrite(pcm.data(), 2, pcm.size(), wf); fclose(wf);
		int peak = 0; for (auto v : pcm) peak = std::max(peak, std::abs(int(v)));
		fprintf(stderr, "audio: %zu samples @ %.1f Hz, %d blocks, peak %d\n", pcm.size(), rate, blocks, peak);
	}
	else fprintf(stderr, "audio: none (%zu samples)\n", pcm.size());
	if (m.dcs()) fprintf(stderr, "dcs: writes %llu overwrites(lost) %llu reads %llu\n", (unsigned long long)m.dcs()->stat_writes, (unsigned long long)m.dcs()->stat_overwrites, (unsigned long long)m.dcs()->stat_reads);
	int w = m.screen_w(), h = m.screen_h();
	std::vector<uint8_t> rgb(size_t(w) * h * 3);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			uint32_t c = m.frame_rgba()[y * MidVUnit::FRAME_STRIDE + x];
			uint8_t *d = &rgb[(size_t(y) * w + x) * 3];
			d[0] = c >> 16; d[1] = c >> 8; d[2] = c;
		}
	size_t len = 0;
	void *png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &len);
	FILE *fp = fopen(shot.c_str(), "wb"); fwrite(png, 1, len, fp); fclose(fp);
	fprintf(stderr, "wrote %s (%dx%d)\n", shot.c_str(), w, h);
	return 0;
}
