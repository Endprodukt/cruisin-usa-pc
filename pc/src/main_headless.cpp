// Headless test driver: runs N frames and writes a PNG screenshot.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include "machine/midvunit.h"
#include "../third_party/miniz/miniz.h"

int main(int argc, char **argv)
{
	std::string rom = "D:/Mame/roms/crusnusa.zip", shot = "shot.png", ver = "4.5";
	int frames = 300;
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
		else if (!strcmp(argv[i], "--ver") && i + 1 < argc) ver = argv[++i];
	}
	MidVUnit m;
	std::string err;
	if (!m.load_roms(rom, ver, err)) { fprintf(stderr, "ROM error: %s\n", err.c_str()); return 1; }
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
