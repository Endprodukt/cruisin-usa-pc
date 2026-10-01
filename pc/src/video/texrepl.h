// Texture export and replacement.
//
// The arcade's textures are 256 x 256 pages of 8 bit palette indices; the palette is chosen per polygon (pixdata). A "texture" here is
// therefore one page seen through one palette range: its identity is (page base row, palette base, hash of the page's bytes and
// of the 256 palette entries it reaches), written as tex_<hash>.png.
//
//  * export:  every new texture that is drawn is written to the dump folder as a 256 x 256 RGBA PNG (index 0 transparent where the
//             polygon uses transparency).
//  * replace: PNGs with the same names in the replace folder (any square size, a multiple of 256 up to 2048, e.g. an AI-upscaled
//             dump) are drawn instead of the original texture.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TexRepl
{
public:
	struct Config
	{
		std::string dump_dir, repl_dir;
		bool dump = false, replace = false;
		int max_res = 2048;
		bool variants = false;   // export: one file per palette variant (colour hash) instead of one per distinct page data
	};

	void configure(const Config &c) { m_c = c; }
	bool active() const { return m_c.dump || m_layers > 0; }
	// load the replacement folder; returns the number of textures found
	int load(std::string &log);
	int resolution() const { return m_res; }
	int layers() const { return m_layers; }
	const std::vector<uint8_t> &layer_rgba(int i) const { return m_pages[size_t(i)]; }

	// For a textured polygon: `ram` = texture RAM (4 MB), `pal` = palette (0x00RRGGBB, 32768 entries), `tex_gen` changes whenever
	// texture RAM is written. Returns the replacement layer + 1, or 0 for the original texture. May write a dump file.
	int lookup(uint32_t base, uint32_t pix, uint32_t mode, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen);

	int dumped() const { return int(m_dumped.size()); }

private:
	struct Entry
	{
		uint64_t tex_gen = ~0ull;
		uint32_t pal[256] = {};
		bool used[256] = {};
		uint64_t hash = 0, dhash = 0;   // colour hash, page data hash
		bool keyed = false;
		int layer = 0;
	};
	static uint64_t colour_hash(const uint8_t *ram, uint32_t base, const uint32_t *pal, uint32_t pix, bool keyed, bool *used, int *colours, uint64_t *data_hash);
	void write_dump(uint32_t base, uint32_t pix, uint64_t hash, const char *prefix, bool keyed, const uint8_t *ram, const uint32_t *pal);

	Config m_c;
	int m_res = 0, m_layers = 0;
	std::vector<std::vector<uint8_t>> m_pages;
	std::unordered_map<uint64_t, int> m_table;     // colour hash -> layer
	std::unordered_map<uint32_t, Entry> m_cache;   // (base, pix) -> last lookup
	std::unordered_set<uint64_t> m_dumped;
};
