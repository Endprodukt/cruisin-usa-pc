#include "rom_patches.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

	const double k = double(pct) / 100.0;
	const uint32_t blk[5] = {c3x_float(15000), c3x_float(5000), c3x_float(80000), c3x_float(15000), c3x_float(45000)};
	long b = find_seq(ram, blk, 5, scan);

	if (pct > 100)
	{
		// no model degrading at all from 200 % on: the instruction that loads the cruder model is turned into a NOP
		if (pct >= 200 && l1 >= 0 && l2 >= 0)
		{
			ram[size_t(l1) + 1] = 0x0C800000;
			ram[size_t(l2) + 1] = 0x0C800000;
			applied += 2;
			log += "  level-of-detail switching disabled\n";
		}

		// scenery sections are activated k times further ahead. Only the activation distance changes: the deactivation distances
		// (sections behind the camera) stay, otherwise the 20-entry section table fills up with sections nobody sees and the game
		// runs out of road ahead (it restarts). A CPU hook additionally holds new sections back while the table is nearly full.
		if (b >= 0)
		{
			ram[size_t(b) + 2] = c3x_float(80000.0 * k);
			applied++;
			log += "  scenery activation distance x" + std::to_string(pct) + "%\n";
		}
		const uint32_t act[2] = {75000u, 80000u};   // ACTIVEHI1 / ACTIVEHI of the object lists
		long a2 = find_seq(ram, act, 2, scan);
		if (a2 >= 0)
		{
			const double ka = std::min(k, 1.5);   // the active object window: more than 1.5x made the game lock up (too many live objects)
			ram[size_t(a2)] = uint32_t(75000 * ka);
			ram[size_t(a2) + 1] = uint32_t(80000 * ka);
			applied++;
		}

		// The engine projects with a 1/z table of 5000 entries (z / 16, up to 80000). A longer table is built in a free stretch of the
		// ROM image and the table pointer, the table limit (CMPI/LDIGT 4999 in the projection code) and the far clip are moved.
		const uint32_t hs[3] = {c3x_float(256.0), c3x_float(200.0), 80000u};
		long h = find_seq(ram, hs, 3, 0x1000);
		if (h >= 1)
		{
			const uint32_t old_ptr = ram[size_t(h) - 1];
			const size_t entries = size_t(4999 * k) + 2;
			// the part of the ROM window above the last graphics ROM is empty (all ones) and not part of the RAM boot copy
			size_t start = 0x300000;
			for (size_t i = 0; i < entries + 80 && start; i++)
				if (start + i >= ram.size() || ram[start + i] != 0xffffffffu) start = 0;
			if (start && old_ptr >= 80 && old_ptr + 5000 < ram.size())
			{
				// 80 entries in front of the label serve negative indices (points just behind the camera)
				for (size_t i = 0; i < 80; i++) ram[start + i] = ram[old_ptr - 80 + i];
				for (size_t i = 0; i < entries; i++)
					ram[start + 80 + i] = i < 5000 ? ram[old_ptr + i] : c3x_float(512.0 / (16.0 * double(i)));
				ram[size_t(h) - 1] = 0xC00000u + uint32_t(start + 80);
				ram[size_t(h) + 2] = uint32_t(80000 * k);
				const uint32_t lim = uint32_t(4999 * k);
				int n = 0;
				for (size_t i = 0; i < 0xC8F4; i++)
					if ((ram[i] & 0xffff) == 0x1387 && (ram[i] & 0x00600000) == 0x00600000) { ram[i] = (ram[i] & 0xffff0000u) | lim; n++; }
				applied++;
				log += "  longer 1/z table (" + std::to_string(entries) + " entries), far clip " + std::to_string(uint32_t(80000 * k)) + ", " + std::to_string(n) + " table limits\n";
			}
			else
				log += "  no free ROM space for the longer 1/z table\n";
		}
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

	// 2. polygons that reach beyond +-1023 pixels were sent to the clipper (CLIP), which splits them and drops every piece that lies
	//    wholly off the arcade's 512 x 400 picture. With margins that removes geometry that should be visible, and the hardware
	//    (and our GPU) can draw large polygons directly anyway: only coordinates beyond +-16383 (16 bit DMA fields) are still clipped.
	//      CLIPCK: OR R1,R5 / LSH -10,R5 / RETSEQ / LDI 0,R5
	for (size_t i = 0; i + 3 <= std::min(scan, ram.size()); i++)
		if (ram[i] == 0x09E5FFF6 && ram[i + 1] == 0x78850000 && ram[i + 2] == 0x08650000)
		{
			ram[i] = 0x09E5FFF2;
			applied++;
			log += "  large polygons are no longer split by the clipper\n";
			break;
		}

	// 3. polygon clip (CLIP): "all X > 511" tests
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

// Rubber banding (RACER.ASM GETPOWER): the opponents' engine power is 1 + correction * RELATIVITY, the correction growing with the
// distance to the player (+20..40 % when behind, -10..40 % when ahead). RELATIVITY is a per-racer factor in the race table
// (1.0 for most racers, 0.35 / 0.30 for two of them); scaling the ten factors scales the whole effect and keeps the racers' ratios.
void rubberband(std::vector<uint32_t> &ram, int pct, int &applied, std::string &log)
{
	if (pct == 100) return;
	pct = std::clamp(pct, 0, 300);
	const double accel[3] = {0.90, 0.87, 0.84};
	for (size_t i = 0; i + 14 < 0x20000; i++)
		if (ram[i] == c3x_float(accel[0]) && ram[i + 6] == c3x_float(accel[1]) && ram[i + 12] == c3x_float(accel[2]) && ram[i + 1] == 0)
		{
			static const double rel[10] = {1.0, 1.0, 1.0, 1.0, 1.0, 0.35, 1.0, 0.30, 1.0, 1.0};
			for (int k = 0; k < 10; k++)
			{
				double v = rel[k] * double(pct) / 100.0;
				ram[i + 1 + size_t(k) * 6] = v == 0.0 ? 0x80000000u : c3x_float(v);
			}
			applied++;
			log += "  opponent rubber banding " + std::to_string(pct) + "%\n";
			return;
		}
}

// Frame governor (CUSA.ASM MAINLOOP): a new game frame starts only when at least FRAMRATE vblanks have passed. Races and the
// head-to-head logo set it to 2 (INTRO.ASM INIT_GAMELEG), i.e. at most 28.5 frames per second. Everything that moves is scaled by
// NFRAMES (vblanks since the last frame), so with 1 the game simply takes smaller steps more often, provided the CPU finishes a
// frame within one vblank (the host raises the CPU clock for that).
void smooth_frames(std::vector<uint32_t> &ram, int &applied, std::string &log)
{
	const size_t scan = 0x20000;
	// MWAIT0: LDI (INFRAMES),R0 / CMPI (FRAMRATE),R0 / BLT MWAIT0
	long best = -1;
	for (size_t i = 0; i + 3 < scan && best < 0; i++)
		if ((ram[i] & 0xffff0000u) == 0x08200000u && (ram[i + 1] & 0xffff0000u) == 0x04A00000u && ram[i + 2] == 0x6A07FFFDu)
			best = long(ram[i + 1] & 0xffff);
	if (best < 0) { log += "  frame governor not found\n"; return; }
	// FRAMRATE is "minimum vblanks - 1": the wait for it comes before the page swap request, which itself waits for the next vblank.
	// 0 (the attract mode's value) allows a frame per vblank. Every "LDI 1/2,R0 / STI R0,(FRAMRATE)" becomes 0.
	int n = 0;
	for (size_t i = 0; i + 1 < scan; i++)
		if ((ram[i] == 0x08600001u || ram[i] == 0x08600002u) && ram[i + 1] == (0x15200000u | uint32_t(best))) { ram[i] = 0x08600000u; n++; }
	applied += n;
	log += "  frame governor -> 0 (" + std::to_string(n) + " sites)\n";
}

} // namespace

int apply_rom_patches(std::vector<uint32_t> &ram, const RomPatchOptions &opt, std::string &log)
{
	int applied = 0;
	draw_distance(ram, opt.draw_distance_pct, applied, log);
	rubberband(ram, opt.rubberband_pct, applied, log);
	widescreen(ram, opt.wide_margin, applied, log);
	if (opt.smooth_frames) smooth_frames(ram, applied, log);
	return applied;
}
