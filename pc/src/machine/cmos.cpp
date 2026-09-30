#include "cmos.h"

#include <cstdio>

namespace cmos {

uint32_t get(const std::vector<uint32_t> &nv, int index)
{
	size_t b = size_t(index) * 4;
	if (b + 3 >= nv.size()) return 0;
	return ((nv[b] >> 24) << 24) | ((nv[b + 1] >> 24) << 16) | ((nv[b + 2] >> 24) << 8) | (nv[b + 3] >> 24);
}

void set(std::vector<uint32_t> &nv, int index, uint32_t value)
{
	size_t b = size_t(index) * 4;
	if (b + 3 >= nv.size()) return;
	// as _wr_cw stores it: the value, then shifted left a byte at a time
	nv[b] = value;
	nv[b + 1] = value << 8;
	nv[b + 2] = value << 16;
	nv[b + 3] = value << 24;
	if (index < NUM_ADJUSTMENTS) fix_checksum(nv);
}

static uint32_t checksum(const std::vector<uint32_t> &nv)
{
	uint32_t sum = 0;
	for (int i = 0; i < NUM_ADJUSTMENTS; i++) sum += get(nv, i);
	return sum;
}

void fix_checksum(std::vector<uint32_t> &nv)
{
	size_t b = size_t(ADJ_CHECKSUM) * 4;
	uint32_t v = checksum(nv);
	nv[b] = v; nv[b + 1] = v << 8; nv[b + 2] = v << 16; nv[b + 3] = v << 24;
}

bool checksum_ok(const std::vector<uint32_t> &nv) { return get(nv, ADJ_CHECKSUM) == checksum(nv); }

const std::vector<AdjInfo> &adjustments()
{
	// index, label, help, kind, min, max, step, default (limits and defaults are the game's own VADJTAB)
	static const std::vector<AdjInfo> t = {
		{ADJ_FREE_PLAY, "Free play", "No coins needed.", AdjInfo::OnOff, 0, 1, 1, 0},
		{ADJ_FREEGAME, "First place awards free game", "", AdjInfo::OnOff, 0, 1, 1, 1},
		{ADJ_MAX_CREDITS, "Max credits", "10 to 50.", AdjInfo::Credits, 10, 50, 1, 30},
		{ADJ_DIFFICULTY, "Game difficulty", "0 (easy) to 9 (hard).", AdjInfo::Number, 0, 9, 1, 5},
		{ADJ_TIME_TO_START, "Start time bonus", "Initial race time: 60 to 90 seconds in steps of 5.", AdjInfo::StartTime, 0, 6, 1, 3},
		{ADJ_CHECKPOINT_BONUS, "Checkpoint bonus time", "10 to 25 seconds per checkpoint.", AdjInfo::Number, 10, 25, 1, 20},
		{ADJ_STEERING_SENSITIVITY, "Steering sensitivity", "0 (adult) to 5 (game player).", AdjInfo::Number, 0, 5, 1, 5},
		{ADJ_MPHORKPM, "Speed units", "MPH or KPH on the speedometer.", AdjInfo::Speed, 0, 1, 1, 0},
		{ADJ_ROADKILL, "Show roadkill", "", AdjInfo::OnOff, 0, 1, 1, 1},
		{ADJ_CLINTON, "Show president", "", AdjInfo::OnOff, 0, 1, 1, 1},
		{ADJ_GIRLS, "Show girls", "", AdjInfo::OnOff, 0, 1, 1, 1},
		{ADJ_ATTRACT_MODE_SOUND, "Attract mode sound", "", AdjInfo::OnOff, 0, 1, 1, 0},
		{ADJ_HIGH_SCORE_ENTRY, "High score entry", "", AdjInfo::OnOff, 0, 1, 1, 1},
		{ADJ_HIGHSCORE_RESET, "Plays to high score reset", "1000 to 25000 in steps of 1000.", AdjInfo::Number, 1000, 25000, 1000, 5000},
		{ADJ_MIN_VOL_LEVEL, "Minimum volume level", "0 to 255.", AdjInfo::Number, 0, 255, 1, 100},
	};
	return t;
}

bool load_file(const std::string &path, std::vector<uint32_t> &nv)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::vector<uint32_t> tmp(WORDS);
	size_t n = std::fread(tmp.data(), 4, tmp.size(), f);
	std::fclose(f);
	if (n != tmp.size()) return false;
	nv = std::move(tmp);
	return true;
}

bool save_file(const std::string &path, const std::vector<uint32_t> &nv)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	size_t n = std::fwrite(nv.data(), 4, nv.size(), f);
	std::fclose(f);
	return n == nv.size();
}

} // namespace cmos
