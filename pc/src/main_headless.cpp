// Headless test driver: runs N frames and writes a PNG screenshot.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <sstream>
#include "machine/midvunit.h"
#include "../third_party/miniz/miniz.h"

int main(int argc, char **argv)
{
	std::string rom = "D:/Mame/roms/crusnusa.zip", shot = "shot.png", ver = "4.5";
	int frames = 300;
	std::string wav;
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
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
		else if (!strcmp(argv[i], "--ver") && i + 1 < argc) ver = argv[++i];
	}
	MidVUnit m;
	std::string err;
	if (!m.load_roms(rom, ver, err)) { fprintf(stderr, "ROM error: %s\n", err.c_str()); return 1; }
	std::vector<int16_t> pcm; double rate = 0; int blocks = 0;
	m.on_audio = [&](const int16_t *b, int n, double r) { pcm.insert(pcm.end(), b, b + n); if (rate == 0) rate = r; blocks++; };
	m.reset();
	for (int f = 0; f < frames; f++)
	{
		for (auto &a : axes) if (a.at == f) { if (a.which == 'w') m.inputs.wheel = a.val; else if (a.which == 'a') m.inputs.accel = a.val; else m.inputs.brake = a.val; }
		m.inputs.in0 = 0xffff;
		for (auto &e : evs) if (f >= e.at && f < e.at + e.dur) m.inputs.in0 &= ~e.bit;
		m.run_frame();
		if (f % 60 == 0) fprintf(stderr, "frame %d pc=%06X quads=%llu vis=%dx%d\n", f, m.cpu_pc(), (unsigned long long)m.quads_last_frame, m.screen_w(), m.screen_h());
	}
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
