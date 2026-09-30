#include "rom_patches.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

uint32_t c3x_float(double v)
{
	if (v == 0.0) return 0x80000000u;
	int e;
	double m = std::frexp(v, &e);           // v = m * 2^e, 0.5 <= |m| < 1
	e -= 1;
	double f = v / std::ldexp(1.0, e) - 1.0; // 0 <= f < 1 for positive v
	return (uint32_t(e & 0xff) << 24) | uint32_t(std::lround(f * double(1 << 23)));
}

namespace {

// position of the first occurrence of `w` in ram[0, limit)
long find_word(const std::vector<uint32_t> &ram, uint32_t w, size_t limit)
{
	for (size_t i = 0; i < std::min(limit, ram.size()); i++)
		if (ram[i] == w) return long(i);
	return -1;
}

long find_seq(const std::vector<uint32_t> &ram, const uint32_t *seq, size_t n, size_t limit)
{
	for (size_t i = 0; i + n <= std::min(limit, ram.size()); i++)
	{
		bool ok = true;
		for (size_t k = 0; k < n && ok; k++) ok = ram[i + k] == seq[k];
		if (ok) return long(i);
	}
	return -1;
}

constexpr uint32_t OP_CMPI_IMM_R2 = 0x04E20000;   // CMPI imm16,R2

void draw_distance(std::vector<uint32_t> &ram, int pct, int &applied, std::string &log)
{
	pct = std::clamp(pct, 10, 400);
	if (pct == 100) return;
	const size_t scan = 0x20000;

	// level-of-detail switch distances (model degrade): CMPI 8000,R2 and CMPI 15000,R2 in the object loop
	long l1 = find_word(ram, OP_CMPI_IMM_R2 | 8000, scan), l2 = find_word(ram, OP_CMPI_IMM_R2 | 15000, scan);
	if (l1 >= 0 && l2 >= 0)
	{
		uint32_t d1 = uint32_t(std::clamp(8000 * pct / 100, 500, 32767));
		uint32_t d2 = uint32_t(std::clamp(std::max(15000 * pct / 100, int(d1) + 1), 600, 32767));
		ram[size_t(l1)] = OP_CMPI_IMM_R2 | d1;
		ram[size_t(l2)] = OP_CMPI_IMM_R2 | d2;
		applied += 2;
		log += "  level-of-detail distances " + std::to_string(d1) + " / " + std::to_string(d2) + "\n";
	}

	// scenery activation block: ATTRACT_ACTIVATE 15000, ACTIVATE 5000, DACT 80000, DDACT 15000, ATTR_DDACT 45000 (floats)
	const uint32_t blk[5] = {c3x_float(15000), c3x_float(5000), c3x_float(80000), c3x_float(15000), c3x_float(45000)};
	long b = find_seq(ram, blk, 5, scan);
	if (b >= 0 && pct > 100)
	{
		// only the deactivation distances of dynamic objects (traffic, aircraft, trains) are pushed out; 80000 is the engine's limit
		ram[size_t(b) + 3] = c3x_float(std::min(15000.0 * pct / 100.0, 80000.0));
		ram[size_t(b) + 4] = c3x_float(std::min(45000.0 * pct / 100.0, 80000.0));
		applied += 2;
		log += "  dynamic object deactivation distances scaled\n";
	}

	// far clip: objects whose nearest point is beyond HIGH_CLIP_LEV8 (80000) are dropped. Only shortened here
	// (beyond 80000 the engine's 1/z table ends). It sits right after SCRNHXI/SCRNHYI = 256.0, 200.0.
	const uint32_t hs[3] = {c3x_float(256.0), c3x_float(200.0), 80000u};
	long h = find_seq(ram, hs, 3, 0x1000);
	if (h >= 0 && pct < 100)
	{
		ram[size_t(h) + 2] = uint32_t(std::max(4000, 80000 * pct / 100));
		applied++;
		log += "  far clip " + std::to_string(ram[size_t(h) + 2]) + "\n";
	}
}

} // namespace

int apply_rom_patches(std::vector<uint32_t> &ram, const RomPatchOptions &opt, std::string &log)
{
	int applied = 0;
	draw_distance(ram, opt.draw_distance_pct, applied, log);
	(void)opt.wide_margin;
	return applied;
}
