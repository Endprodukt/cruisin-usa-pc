// Optional modifications of the game program (applied to the RAM copy made at boot; the ROM image stays untouched).
// Every patch is located by the instruction/constant pattern it modifies, so a patch that does not match this ROM
// version is skipped instead of corrupting it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RomPatchOptions
{
	int draw_distance_pct = 100;   // 100 = original; <100 culls far objects, >100 pushes level-of-detail switches and traffic further out
	int rubberband_pct = 100;      // strength of the opponents' catch-up boost relative to the original (0 = none)
	int wide_margin = 0;
	bool smooth_frames = false;   // lift the race frame governor (FRAMRATE 2 -> 1): up to 57 game frames per second           // extra arcade pixels of 3D view on each side of the 512 px wide image (widescreen)
};

// patches `ram` (the first words of the program, as copied to RAM at boot); returns the number of patches applied
int apply_rom_patches(std::vector<uint32_t> &ram, const RomPatchOptions &opt, std::string &log);

// C3x short-float helpers (exposed for tests)
uint32_t c3x_float(double v);
