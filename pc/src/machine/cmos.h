// The game's battery-backed CMOS (8K x 8 on the top byte lane of a 32-bit bus): adjustments first, then audits.
//
// Every entry is four words; the top byte of each word carries one byte of the value, most significant
// first (see _rd_cw / _wr_cw in CMOS.ASM). The adjustments are guarded by a checksum entry (ADJ_CHECKSUM):
// the sum of adjustments 0..45. A file with a wrong checksum makes the game reset its adjustments.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cmos {

constexpr int NUM_ADJUSTMENTS = 46;
constexpr int ADJ_CHECKSUM = 53;
constexpr size_t WORDS = 0x2000;

enum Adj : int
{
	ADJ_COINMODE = 0, ADJ_FREE_PLAY = 25, ADJ_FREEGAME = 26, ADJ_DIFFICULTY = 27, ADJ_TIME_TO_START = 28,
	ADJ_CHECKPOINT_BONUS = 29, ADJ_ATTRACT_MODE_SOUND = 30, ADJ_HIGH_SCORE_ENTRY = 31, ADJ_MIN_VOL_LEVEL = 32,
	ADJ_MPHORKPM = 34, ADJ_ROADKILL = 35, ADJ_CLINTON = 36, ADJ_GIRLS = 37, ADJ_STEERING_SENSITIVITY = 38,
	ADJ_HIGHSCORE_RESET = 39, ADJ_MAX_CREDITS = 45,
};

uint32_t get(const std::vector<uint32_t> &nv, int index);
void set(std::vector<uint32_t> &nv, int index, uint32_t value);     // writes the four words like the game and fixes the checksum
void fix_checksum(std::vector<uint32_t> &nv);
bool checksum_ok(const std::vector<uint32_t> &nv);

// the operator settings the game's service menu offers (Adjustments menu), with the menu's own wording
struct AdjInfo
{
	int index;
	const char *label;
	const char *help;
	enum Kind { Number, OnOff, StartTime, Speed, Credits } kind;
	int min, max, step, def;
};
const std::vector<AdjInfo> &adjustments();

// load a save file; false if it is missing/short. The vector is sized to WORDS on success.
bool load_file(const std::string &path, std::vector<uint32_t> &nv);
bool save_file(const std::string &path, const std::vector<uint32_t> &nv);

} // namespace cmos
