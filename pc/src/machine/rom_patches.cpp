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

// 16-bit short float immediate of the C3x (4 bit exponent, sign, 11 bit mantissa), positive values only
uint32_t c3x_short(double v)
{
	if (v <= 0.0) return 0x8000;
	int e = int(std::floor(std::log2(v)));
	e = std::clamp(e, -7, 7);
	long m = std::lround((v / std::ldexp(1.0, e) - 1.0) * 2048.0);
	if (m >= 2048) { m = 0; e++; }
	if (m < 0) m = 0;
	return (uint32_t(e & 0xf) << 12) | uint32_t(m);
}

// widescreen: let the 3D engine keep objects and polygons that lie beyond the arcade's 512 px wide picture
void widescreen(std::vector<uint32_t> &ram, int margin, int &applied, std::string &log)
{
	if (margin <= 0) return;
	const size_t scan = 0x20000;

	// 1. object trivial rejection (DIRQ): "CMPF/ADDF SCRNHX" tests against the screen edges with the projected radius in R4.
	//    The Y test's delay slot holds a NOP; enlarging R4 there by `margin` pixels widens the X tests (and, harmlessly, the lower Y test).
	//      ADDF ($0054),R2 / BLTD x / NOP / SUBF R4,R3 / CMPF ($0054),R3
	for (size_t i = 0; i + 5 <= std::min(scan, ram.size()); i++)
		if (ram[i] == 0x01A20054 && (ram[i + 1] & 0xffff0000) == 0x6A270000 && ram[i + 2] == 0x0C800000 && ram[i + 3] == 0x17830004 && ram[i + 4] == 0x04230054)
		{
			ram[i + 2] = 0x01E40000u | c3x_short(double(margin));   // ADDF margin,R4
			applied++;
			log += "  object rejection widened by " + std::to_string(margin) + " px\n";
			break;
		}

	// 2. polygon clip (CLIP): "all X > 511" tests
	const uint32_t seq[5] = {0x086101FF, 0x274201C0, 0x27430140, 0x02820003, 0x086301FF};   // LDI 511,R1 / SUBI3 / SUBI3 / AND / LDI 511,R3
	int n = 0;
	for (size_t i = 0; i + 5 <= std::min(scan, ram.size()); i++)
	{
		bool ok = true;
		for (int k = 0; k < 5 && ok; k++) ok = ram[i + k] == seq[k];
		if (!ok) continue;
		ram[i] = 0x08610000u | uint32_t(511 + margin);
		ram[i + 4] = 0x08630000u | uint32_t(511 + margin);
		n++;
	}
	if (n) { applied += n; log += "  polygon clip right edge moved (" + std::to_string(n) + " sites)\n"; }
}

} // namespace

int apply_rom_patches(std::vector<uint32_t> &ram, const RomPatchOptions &opt, std::string &log)
{
	int applied = 0;
	draw_distance(ram, opt.draw_distance_pct, applied, log);
	widescreen(ram, opt.wide_margin, applied, log);
	return applied;
}
