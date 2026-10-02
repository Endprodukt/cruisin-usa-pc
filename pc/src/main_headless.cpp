// Headless test driver: runs N frames and writes a PNG screenshot.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "machine/autosetup.h"
#include "machine/cmos.h"
#include <algorithm>
#include <chrono>
#include <thread>
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
	if (const char *sc = getenv("STEADY")) m.steady_cadence = atoi(sc);   // 0 machine, 1 due frames only (default), 2 before every vblank
	m.quad_stats = getenv("QUADSTAT") != nullptr || getenv("POLYLOG") != nullptr;   // polygon statistics; POLYLOG=<csv>: one line per game picture
	if (const char *pl = getenv("POLYLOG")) m.poly_log = fopen(pl, "w");
	m.steady_budget_ms = getenv("STEADY_MS") ? atof(getenv("STEADY_MS")) : 0.0;   // headless runs are not paced: no host time limit
	if (const char *dd = getenv("DD")) m.rom_patches.draw_distance_pct = atoi(dd);
	if (getenv("PCHIST")) m.m_pchist_on = true;
	if (const char *wm = getenv("WM")) m.rom_patches.wide_margin = atoi(wm);
	if (getenv("NORASTER")) m.skip_raster = true;
	if (const char *ex = getenv("EXPORT")) { TexRepl::Config tc; tc.dump = true; tc.dump_dir = ex; tc.variants = getenv("EXPORTVAR") != nullptr; m.texrepl.configure(tc); }
	if (const char *rp = getenv("REPLACE"))
	{   // load timing of a replacement pack
		TexRepl::Config tc; tc.replace = true; tc.repl_dir = rp; m.texrepl.configure(tc);
		std::string log; const auto t0 = std::chrono::steady_clock::now();
		m.texrepl.load(log);
		fprintf(stderr, "REPLACE load %.0f ms: %s", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), log.c_str());
	}
	if (const char *rb = getenv("RB")) m.rom_patches.rubberband_pct = atoi(rb);
	if (const char *oc = getenv("OC")) m.cpu_overclock = atoi(oc);
	if (getenv("ROUTINES")) m.debug_routines = true;
	if (const char *ao = getenv("AUTOOC")) m.auto_overclock = atoi(ao) != 0;
	if (getenv("SMOOTH")) m.rom_patches.smooth_frames = true;
	if (const char *dt = getenv("DCSTHREAD")) m.dcs_thread = atoi(dt);
	m.reset();
	if (getenv("DEFAULT_NV")) m.load_default_nvram();
	if (const char *adj = getenv("ADJ")) { int idx, val, n = 0; const char *q = adj; while (sscanf(q, "%d=%d%n", &idx, &val, &n) == 2) { cmos::set(m.nvram(), idx, uint32_t(val)); q += n; if (*q == ',') q++; } }
	if (!makenv.empty()) { run_auto_setup(m); m.save_nvram(makenv); fprintf(stderr, "saved %s\n", makenv.c_str()); return 0; }
	const auto wall0 = std::chrono::steady_clock::now();
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
		// TRACK=n: start race n (CHOSEN_RACE is forced until the car is rolling)
		if (const char *tr = getenv("TRACK"))
		{
			static bool rolling = false;
			Telemetry t;
			if (!rolling)
			{
				m.ram_poke(0xE664, uint32_t(atoi(tr)));
				if (getenv("USA")) m.ram_poke(0xE666, 1);   // RACE_MODE = RM_USA: the legs follow each other
				if (m.read_telemetry(t) && t.speed > 30) rolling = true;
			}
		}
		// AUTOPILOT=1 / -1 (steering sign): keeps the car near the middle of the road at full throttle, time limit frozen
		if (const char *ap = getenv("AUTOPILOT"))
		{
			static float prev = 0, dfilt = 0; static int pilot_frames = 0;
			Telemetry t;
			if (m.read_telemetry(t))
			{
				const float d = t.dist_to_center - (getenv("AP_OFFSET") ? float(atof(getenv("AP_OFFSET"))) : 0.0f);   // lane offset
				if (d != prev) { dfilt = dfilt * 0.6f + (d - prev) * 0.4f; prev = d; }
				const float kp = getenv("AP_KP") ? float(atof(getenv("AP_KP"))) : 0.10f, kd = getenv("AP_KD") ? float(atof(getenv("AP_KD"))) : 1.2f;
				const float u = std::clamp(kp * d + kd * dfilt, -100.0f, 100.0f) * float(atoi(ap));
				m.inputs.wheel = uint8_t(std::clamp(128.0f + u, 16.0f, 240.0f));
				const int thr = getenv("AP_THROTTLE") ? atoi(getenv("AP_THROTTLE")) : 255;
				m.inputs.accel = uint8_t(std::fabs(d) > 700.0f ? std::min(120, thr) : thr);
				if (m.ram_peek(0xE634) < 40) m.ram_poke(0xE634, 60);   // _countdown
				if (getenv("AP_LOG") && ++pilot_frames % atoi(getenv("AP_LOG")) == 0)
					fprintf(stderr, "AP f%d spd=%.0f dist=%.0f wheel=%d onroad=%X mode=%X race=%u%c", f, t.speed, d, m.inputs.wheel, t.onroad, m.ram_word(0xC8F5), m.ram_word(0xE664), 10);
			}
			else if (f > 2600 && (f / 6) % 30 == 0) m.inputs.in0 &= ~in0bit::START;   // between races: press start now and then to move on
		}
		if (const char *od = getenv("OBJDUMP"))
		{   // OBJDUMP=frame: the large objects on the active list at that frame (id, section group, distance, radius, position)
			if (f == atoi(od))
			{
				fprintf(stderr, "OBJDUMP frame %d mode %X groups %u%c", f, m.ram_word(0xC8F5), m.ram_word(0xE49C), 10);
				int n = 0;
				for (uint32_t o = m.ram_peek(m.ram_word(0x40)); o && o != 0xDEADBEEFu && n < 6000; o = m.ram_peek(o), n++)
				{
					const uint32_t rad = m.ram_peek(o + 0x1D);
					if (int32_t(rad) < 2500) continue;
					fprintf(stderr, "  obj %06X id %04X group %06X dist %d rad %u pos %.0f %.0f %.0f flags %08X%c", o, m.ram_peek(o + 0xF), m.ram_peek(o + 0x1F), int32_t(m.ram_peek(o + 0x1C)), rad,
					        c3x_to_double(m.ram_peek(o + 1)), c3x_to_double(m.ram_peek(o + 2)), c3x_to_double(m.ram_peek(o + 3)), m.ram_peek(o + 0xE), 10);
				}
				fprintf(stderr, "  (%d objects on the active list)%c", n, 10);
				// the loaded sections (index, anchor distance from the camera) and the scenery objects near the camera
				const uint32_t cam = m.ram_word(m.ram_word(0x401D) & 0xffff), tab = m.ram_word(0x3F7A), groups = m.ram_word(0xE49C);
				auto romw = [&](uint32_t a) { return a >= 0xC00000 ? m.rom_word(a - 0xC00000) : m.ram_peek(a); };
				const double cx = c3x_to_double(m.ram_peek(cam)), cz = c3x_to_double(m.ram_peek(cam + 2));
				for (uint32_t g = 0; g < groups && g < 20; g++)
				{
					const uint32_t bin = m.ram_peek(tab + g * 5 + 1);
					const uint32_t w = romw(bin);
					fprintf(stderr, "  section %u: anchor %.0f from the camera (entry %06X flags %08X)%c", m.ram_peek(tab + g * 5 + 4),
					        std::hypot(c3x_to_double(romw(bin + 1)) - cx, c3x_to_double(romw(bin + 3)) - cz), bin, w, 10);
				}
				for (uint32_t list = 0x40; list <= 0x41; list++)
					for (uint32_t o = m.ram_peek(m.ram_word(list)), k = 0; o && o != 0xDEADBEEFu && k < 6000; o = m.ram_peek(o), k++)
					{
						const uint32_t id = m.ram_peek(o + 0xF);
						const double d = std::hypot(c3x_to_double(m.ram_peek(o + 1)) - cx, c3x_to_double(m.ram_peek(o + 3)) - cz);
						if (d < 30000 && (id & 0xF00) != 0x300 && int32_t(m.ram_peek(o + 0x1D)) > 1500)
							fprintf(stderr, "  near: obj %06X id %04X section %u flags %08X rad %u, %.0f from the camera, y %.0f, list %s%c", o, id, m.ram_peek(o + 0x1F) >> 8, m.ram_peek(o + 0xE), m.ram_peek(o + 0x1D), d, c3x_to_double(m.ram_peek(o + 2)), list == 0x40 ? "active" : "idle", 10);
					}
			}
		}
		if (getenv("PALLOG"))
		{   // palettes being loaded / released (RAWLOCS: palette id -> slot, 0 = not loaded), with the frame
			static std::vector<uint32_t> prev(512, 0);
			const uint32_t tab = m.ram_word(0x9EA9);
			std::string on, off;
			char b[16];
			for (uint32_t id = 1; id < 256 && tab; id++)
			{
				const uint32_t v = m.ram_peek(tab + id);
				if ((v != 0) != (prev[id] != 0)) { std::snprintf(b, sizeof b, " %X", id); (v ? on : off) += b; }
				prev[id] = v;
			}
			if (f > 2400 && (!on.empty() || !off.empty())) fprintf(stderr, "PAL f%d mode=%X +[%s] -[%s]%c", f, m.ram_word(0xC8F5), on.c_str(), off.c_str(), 10);
		}
		if (getenv("TRACE") && !m.cpu_debug().trace_jumps) m.cpu_debug().trace_jumps = true;
		if (getenv("SPWATCH") && !m.cpu_debug().trace_step)
		{   // report the instruction after which the stack pointer leaves the on-chip stack area
			m.cpu_debug().trace_jumps = true;
			tms320c3x_device *c = &m.cpu_debug();
			MidVUnit *mp = &m;
			m.cpu_debug().trace_step = [c, mp]() {
				static bool done = false; static uint32_t lastsp = 0x809C00, lastpc = 0;
				const uint32_t sp = c->reg(20);
				if (!done && lastsp >= 0x809C00 && lastsp < 0x809E00 && (sp < 0x809C00 || sp >= 0x809E00) && mp->ram_word(0xC8F5) != 0xFFFFFFFFu && (mp->ram_word(0xC8F5) & 0xf) != 1)
				{
					done = true;
					fprintf(stderr, "SPWATCH: SP %06X -> %06X by the instruction at %06X (mode %X)%c", lastsp, sp, lastpc, mp->ram_word(0xC8F5), 10);
					c->trace_dump(80);
				}
				static bool done2 = false; static uint32_t vec[12] = {};
				if (!done2 && (mp->ram_word(0xC8F5) & 0xf) != 1 && mp->ram_word(0xC8F5) != 0xFFFFFFFFu)
					for (uint32_t k = 0; k < 12; k++)
					{
						const uint32_t v = mp->ram_word(k);
						if (vec[k] && v != vec[k])
						{
							done2 = true;
							fprintf(stderr, "VECWATCH: RAM[%X] %08X -> %08X by the instruction at %06X (frame mode %X)%c", k, vec[k], v, lastpc, mp->ram_word(0xC8F5), 10);
							c->trace_dump(60);
							break;
						}
						vec[k] = v;
					}
				lastsp = sp; lastpc = c->pc();
			};
		}
		{
			static uint64_t wd = 0;
			if (m.watchdog_resets != wd) { wd = m.watchdog_resets; fprintf(stderr, "RESET: watchdog fired at frame %d (mode %X)%c", f, m.ram_word(0xC8F5), 10); }
			static uint64_t pq = 0;
			if (m.palette_queue_overflows != pq) { pq = m.palette_queue_overflows; fprintf(stderr, "PALQ: palette queue full, flushed (frame %d, mode %X)%c", f, m.ram_word(0xC8F5), 10); }
		}
		if (getenv("PACE"))
		{   // like the app: one emulated frame per 17.27 ms; report frames whose emulation took long
			static auto next = std::chrono::steady_clock::now();
			std::this_thread::sleep_until(next);
			next += std::chrono::microseconds(17270);
			const auto t0 = std::chrono::steady_clock::now();
			m.run_frame();
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			static double worst = 0; static int slow = 0;
			worst = std::max(worst, ms); if (ms > 6.0) slow++;
			if (f % 600 == 599) { fprintf(stderr, "PACE f%d worst %.2f ms, %d frames > 6 ms%c", f, worst, slow, 10); worst = 0; slow = 0; }
		}
		else
			m.run_frame();
		if (const char *se = getenv("SHOTEVERY"))
		{   // SHOTEVERY=n SHOTFROM=f SHOTDIR=dir: a picture every n frames (survey of a whole drive)
			const int every = atoi(se), from = getenv("SHOTFROM") ? atoi(getenv("SHOTFROM")) : 0;
			if (every > 0 && f >= from && (f - from) % every == 0 && getenv("SHOTDIR"))
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
				char name[300]; std::snprintf(name, sizeof(name), "%s/f%06d.png", getenv("SHOTDIR"), f);
				if (FILE *fp = fopen(name, "wb")) { fwrite(png, 1, len, fp); fclose(fp); }
				mz_free(png);
			}
		}
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
		if (getenv("SWAPLOG"))
		{   // share of vblanks in which a new picture was drawn (all screens), per 120 frames
			static uint64_t last = 0;
			if (f % 120 == 119) { fprintf(stderr, "SWAP f%d mode=%X gov=%u groups=%u flips %d/120%c", f, m.ram_word(0xC8F5), m.ram_word(0xC961), m.ram_word(0xE49C), int(m.page_flips - last), 10); last = m.page_flips; }
		}
		if (getenv("NFLOG"))
		{   // histogram of NFRAMES (vblanks per game frame) in races, printed every 600 frames
			static uint32_t nf_addr = 0; static int hist[5] = {}; static uint32_t last_inf = 0;
			if (!nf_addr)
				for (uint32_t i = 2; i + 1 < 0x20000 && !nf_addr; i++)
				{
					uint32_t a = m.ram_word(i), b = m.ram_word(i + 1), c = m.ram_word(i - 2);
					if ((a & 0xffff0000u) == 0x15210000u && (b & 0xffff0000u) == 0x15200000u && c == (0x08200000u | (a & 0xffff))) nf_addr = b & 0xffff;
				}
			Telemetry t;
			if (nf_addr && m.read_telemetry(t))
			{
				// count each game frame once: the per-frame step of a moving car changes when a new frame ran
				static float lastspd = -1; static int same = 0;
				uint32_t nf = m.ram_word(nf_addr);
				if (t.speed != lastspd) { hist[std::min<uint32_t>(nf, 4)]++; lastspd = t.speed; }
				(void)same; (void)last_inf;
			}
			if (f % 600 == 599 && nf_addr && getenv("NFLOG")) fprintf(stderr, "CATCHUP %llu finished, %llu late%c", (unsigned long long)m.catchups, (unsigned long long)m.catchup_fails, 10);
		if (f % 600 == 599 && nf_addr) { fprintf(stderr, "FRAMRATE=%u ", m.ram_word(0xC961)); fprintf(stderr, "NFRAMES hist 1:%d 2:%d 3:%d 4+:%d%c", hist[1], hist[2], hist[3], hist[4], 10); for (int &h : hist) h = 0; }
		}
		if (getenv("COLLOG"))
		{   // object hits and spins, with the modern force feedback's output over the frame
			static Telemetry pt; static FfbModern fx; Telemetry t;
			bool ok = m.read_telemetry(t);
			fx.frame(ok ? t : Telemetry{}, 0.0f, (float(m.inputs.wheel) - 128.0f) / 112.0f);
			float mn = 1, mx = -1;
			for (int k = 0; k < 4; k++) { float o = fx.step(1.0 / 232.0); mn = std::min(mn, o); mx = std::max(mx, o); }
			if (ok)
			{
				bool ch = t.spin != pt.spin || t.hits_light != pt.hits_light || t.hits_object != pt.hits_object || t.hits_wall != pt.hits_wall;
				static int show = 0;
				if (ch) show = 40;
				if (show > 0) { show--; fprintf(stderr, "C %d spd=%.1f hits=%u/%u/%u spin=%d drot=%+.4f yrot=%+.3f vrot=%+.3f ffb=%+.2f..%+.2f%c", f, t.speed, t.hits_light, t.hits_object, t.hits_wall, t.spin, t.d_rot, t.y_rot, t.v_rot, mn, mx, 10); }
				pt = t;
			}
		}
		if (const char *sl = getenv("STATELOG"))
		{   // STATELOG=<file>: the machine's state every STATEEVERY (default 250) frames, to compare runs: emulated cycles,
			// instructions executed, vblanks, game pictures, timers, random number, the player's car, and hashes of the RAM
			static FILE *sf = nullptr; static uint32_t rand_addr = 0, gtime_addr = 0; static int every = 250;
			if (!sf)
			{
				sf = fopen(sl, "w");
				if (getenv("STATEEVERY")) every = std::max(1, atoi(getenv("STATEEVERY")));
				for (uint32_t i = 0; i + 3 < 0x20000 && (!rand_addr || !gtime_addr); i++)
				{
					// RANDOM: LDI @RAND,R0 / LDI R0,R1 / LSH 1,R0     and the interrupt: LDF @FLOAT_TIK,R0 / ADDF @GAME_TIMER,R0 / STF R0,@GAME_TIMER
					if (!rand_addr && (m.ram_word(i) & 0xffff0000u) == 0x08200000u && m.ram_word(i + 1) == 0x08010000u && m.ram_word(i + 2) == 0x09E00001u) rand_addr = m.ram_word(i) & 0xffff;
					if (!gtime_addr && (m.ram_word(i) & 0xffff0000u) == 0x07200000u && (m.ram_word(i + 1) & 0xffff0000u) == 0x01A00000u &&
					    m.ram_word(i + 2) == (0x14200000u | (m.ram_word(i + 1) & 0xffff))) gtime_addr = m.ram_word(i + 1) & 0xffff;
				}
				if (sf) fprintf(sf, "frame,cycles,instr,instr_extra,flips,mode,inframes,nframes,rand,game_timer,countdown,car_x,car_y,car_z,speed,dist,hash_bss,hash_ram1,hash_cars%c", 10);
			}
			if (sf && f % every == 0)
			{
				auto fnv = [&](uint32_t from, uint32_t to) { uint64_t h = 1469598103934665603ull; for (uint32_t a = from; a < to; a++) { h ^= m.ram_peek(a); h *= 1099511628211ull; } return h; };
				const uint32_t blk = m.ram_word(0xE8A8);
				auto cf = [&](int o) { return blk ? c3x_to_double(m.ram_peek(blk + uint32_t(o))) : 0.0; };
				fprintf(sf, "%d,%llu,%llu,%llu,%llu,%X,%u,%u,%08X,%.6f,%d,%.3f,%.3f,%.3f,%.4f,%.2f,%016llX,%016llX,%016llX%c", f, (unsigned long long)m.cycles_total(),
				        (unsigned long long)m.instr_total, (unsigned long long)m.instr_extra, (unsigned long long)m.page_flips, m.ram_word(0xC8F5), m.ram_word(0xC960), m.ram_word(0xC95F),
				        rand_addr ? m.ram_word(rand_addr) : 0, gtime_addr ? c3x_to_double(m.ram_word(gtime_addr)) : 0.0, int(m.ram_word(0xE634)), cf(0), cf(1), cf(2), cf(38), cf(39),
				        (unsigned long long)fnv(0xC8F0, 0x20000), (unsigned long long)fnv(0x400000, 0x420000), (unsigned long long)fnv(0xE8A8, 0xF400), 10);
				fflush(sf);
			}
		}
		if (getenv("SURFLOG"))
		{   // what the car drives on: every change of CAR_ONROAD (object id of the piece under the car), with the frictions and bumps
			static int last = -1, lastbump = 0; Telemetry t;
			if (m.read_telemetry(t))
			{
				if (t.onroad != last) { fprintf(stderr, "S %d onroad=%X spd=%.0f d2c=%.0f rdfr=%.3f offr=%.3f trac=%.3f%c", f, t.onroad, t.speed, t.dist_to_center, t.road_friction, t.offroad_friction, t.traction, 10); last = t.onroad; }
				if (t.bump != lastbump) { if (t.bump) fprintf(stderr, "B %d bump=%d onroad=%X spd=%.0f%c", f, t.bump, t.onroad, t.speed, 10); lastbump = t.bump; }
			}
		}
		if (const char *tl = getenv("TELEMLOG")) { Telemetry t; if (m.read_telemetry(t) && f % atoi(tl) == 0) fprintf(stderr, "T %d spd=%.2f skid=%.2f thr=%.2f brk=%.2f turn=%.3f trac=%.2f rpm=%.1f yv=%.3f xm=%.3f zm=%.3f xl=%.3f zl=%.3f d2c=%.1f road=%d onroad=%d bump=%d spin=%d air=%d/%d gear=%d yv0=%.2f dy0=%.2f poly=%d\n", f, t.speed, t.skid, t.throttle, t.brake, t.turn, t.traction, t.rpm, t.y_vel, t.x_mom, t.z_mom, t.x_lean, t.z_lean, t.dist_to_center, (int)t.road_friction, t.onroad, t.bump, t.spin, t.air_front, t.air_rear, t.gear, t.susp_yv[0], t.susp_dy[0], t.road_poly[0]); }
		if (f % 60 == 0) fprintf(stderr, "frame %d free=%d mode=%X pc=%06X quads=%llu vis=%dx%d\n", f, m.free_objects(), m.ram_word(0xC8F5) | (m.ram_word(0xE49C) << 16), m.cpu_pc(), (unsigned long long)m.quads_last_frame, m.screen_w(), m.screen_h());
	}
	if (const uint64_t *h = m.cpu_hits()) { std::vector<std::pair<uint64_t, int>> v; uint64_t tot = 0; for (int i = 0; i < 2048; i++) { v.push_back({h[i], i}); tot += h[i]; } std::sort(v.rbegin(), v.rend()); fprintf(stderr, "OPS total %llu%c", (unsigned long long)tot, 10); for (int i = 0; i < 25; i++) fprintf(stderr, "OP %03X %llu (%.1f%%)%c", v[i].second, (unsigned long long)v[i].first, 100.0 * v[i].first / double(tot), 10); }
	if (m.quad_stats && m.poly_race_frames)
	{
		const MidVUnit::PolyFrame &p = m.poly_race;
		const double n = double(m.poly_race_frames), wm = double(m.rom_patches.wide_margin);
		static const char *const nm[MidVUnit::kShapes] = {"rectangle", "triangle", "convex quad", "concave quad", "bowtie", "degenerate"};
		fprintf(stderr, "QUADSTAT %llu race pictures, %.0f polygons per picture%c", (unsigned long long)m.poly_race_frames, double(p.quads) / n, 10);
		for (int k = 0; k < MidVUnit::kShapes; k++)
			fprintf(stderr, "QUADSTAT   %-13s %5.1f %% of the polygons, %5.1f %% of the rectangles' pixels, fills %5.1f %% of its rectangles%c", nm[k], 100.0 * p.shape[k] / std::max(1u, p.quads),
			        100.0 * p.shape_box[k] / std::max(1.0, p.box[0]), 100.0 * p.shape_cover[k] / std::max(1.0, p.shape_box[k]), 10);
		std::vector<float> e = m.poly_eff;
		std::sort(e.begin(), e.end());
		for (int si = 0; si < MidVUnit::kStatScales; si++)
		{
			const int s = MidVUnit::kStatScale[si];
			const double screen = (512.0 + 2.0 * wm) * 400.0 * s * s;
			fprintf(stderr, "QUADSTAT   %dx: per picture %.2f Mpx of rectangles (%.2f screens), %.2f Mpx of polygons, %.2f Mpx discarded; polygons / rectangles %.1f %%%c", s,
			        p.box[si] / n / 1e6, p.box[si] / n / screen, p.cover[si] / n / 1e6, (p.box[si] - p.cover[si]) / n / 1e6, 100.0 * p.cover[si] / std::max(1.0, p.box[si]), 10);
		}
		fprintf(stderr, "QUADSTAT   efficiency per picture at 6x: min %.1f %%, 5th percentile %.1f %%, median %.1f %%, max %.1f %%%c", 100.0 * e.front(), 100.0 * e[e.size() / 20], 100.0 * e[e.size() / 2], 100.0 * e.back(), 10);
	}
	if (m.poly_log) fclose(m.poly_log);
	if (getenv("PROFILE")) prof.stop_and_report();
	if (getenv("CPUTIME")) fprintf(stderr, "CPUTIME main cpu %.1f ms, dsp %.1f ms over %d frames (%.3f ms/frame)%c", m.perf_cpu_ms, m.perf_dcs_ms, frames, m.perf_cpu_ms / frames, 10);
	if (getenv("RAMUSE")) m.debug_ram_usage();
	if (getenv("CPUTIME"))
	{
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();
		fprintf(stderr, "WALL %.1f ms over %d frames (%.3f ms/frame)%c", ms, frames, ms / frames, 10);
	}
	if (getenv("EXPORT")) m.texrepl.flush(m.texture_ram(), m.palette_rgb());
	if (getenv("EXPORT")) fprintf(stderr, "exported %d textures%c", m.texrepl.dumped(), 10);
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
