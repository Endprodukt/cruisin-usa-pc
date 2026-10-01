// Texture export and replacement.
//
// Texture RAM is a strip 256 texels wide and 16384 rows high (8 bit palette indices); a polygon reads a 256 x 256 window that starts
// at any row (texbase) and picks its palette per polygon (pixdata). Windows at neighbouring rows overlap, so dumping one file per window
// produced the same artwork many times, shifted. The strip is therefore cut into fixed blocks of 256 rows (row 0, 256, 512, ...):
// a window covers at most two blocks, and every block is one file, named after the hash of its bytes (idx_<hash>.png) or, in
// variants mode, of its colours (tex_<hash>.png).
//
//  * export:  every new block that is drawn is written to the dump folder as a 256 x 256 RGBA PNG (index 0 has alpha 0; it is only
//             transparent on polygons that use transparency).
//  * replace: PNGs with the same names in the replace folder (any square size, a multiple of 256 up to 2048, e.g. an AI-upscaled
//             dump) are drawn instead of the original texture.
#pragma once

#include <cstdint>
#include <memory>
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

	TexRepl();
	~TexRepl();   // waits for the dump files still being written
	TexRepl(const TexRepl &) = delete;
	TexRepl &operator=(const TexRepl &) = delete;

	void configure(const Config &c) { m_c = c; }
	bool active() const { return m_c.dump || m_layers > 0; }
	// load the replacement folder; returns the number of textures found
	int load(std::string &log);
	int resolution() const { return m_res; }
	int layers() const { return m_layers; }
	const std::vector<uint8_t> &layer_rgba(int i) const { return m_pages[size_t(i)]; }

	// For a textured polygon: `ram` = texture RAM (4 MB), `pal` = palette (0x00RRGGBB, 32768 entries), `tex_gen` changes whenever
	// texture RAM is written. Returns (layer + 1) of the block holding row `base` in the low 16 bits and of the following block in the
	// high 16 bits (0 = original texture). May write dump files.
	uint32_t lookup(uint32_t base, uint32_t pix, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen);

	// once per frame: writes the blocks that were drawn and have not changed since (the game streams textures in over several
	// frames; a block caught half loaded would otherwise become a file of its own)
	void tick(const uint8_t *ram, const uint32_t *pal) { if (!m_pending.empty()) flush_pending(ram, pal, false); }
	void flush(const uint8_t *ram, const uint32_t *pal) { flush_pending(ram, pal, true); }

	int dumped() const { return int(m_written); }

private:
	struct Entry
	{
		uint64_t tex_gen = ~0ull;
		uint32_t pal[256] = {};
		bool used[256] = {};
		uint64_t hash = 0, dhash = 0;   // colour hash, page data hash
		int layer = 0;
	};
	static uint64_t colour_hash(const uint8_t *ram, uint32_t block, const uint32_t *pal, uint32_t pix, bool *used, int *colours, uint64_t *data_hash);
	void write_dump(uint32_t block, uint32_t pix, uint64_t hash, const char *prefix, const uint8_t *ram, const uint32_t *pal);
	int block_layer(uint32_t block, uint32_t pix, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen);
	void flush_pending(const uint8_t *ram, const uint32_t *pal, bool all);

	struct DumpWriter;                       // one background thread that encodes and writes the PNG files
	std::unique_ptr<DumpWriter> m_writer;
	struct Pending { uint32_t block, pix; uint64_t name; int age; };
	std::vector<Pending> m_pending;
	int m_written = 0;

	Config m_c;
	int m_res = 0, m_layers = 0;
	std::vector<std::vector<uint8_t>> m_pages;
	std::unordered_map<uint64_t, int> m_table;     // colour hash -> layer
	std::unordered_map<uint32_t, Entry> m_cache;   // (block, pix) -> last lookup
	std::unordered_set<uint64_t> m_dumped;
};
