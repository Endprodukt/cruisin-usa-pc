// Texture export and replacement.
//
// Texture RAM is a strip 256 texels wide and 16384 rows high (8 bit palette indices); a polygon reads a 256 x 256 window that starts
// at any row (texbase) and picks its palette per polygon (pixdata). Windows at neighbouring rows overlap, so dumping one file per window
// produced the same artwork many times, shifted. The strip is therefore cut into fixed blocks of 256 rows (row 0, 256, 512, ...):
// a window covers at most two blocks, and every block is one file, named after the hash of its bytes (idx_<hash>.png) or, in
// variants mode, of its colours (tex_<hash>.png).
//
//  * export:  a block usually holds several pictures, each drawn with its own palette (the palette belongs to the polygon, not to
//             the texture). Colouring a whole block with one polygon's palette gives the other pictures on it wrong colours, so
//             a file only contains what polygons really drew: every polygon paints the part of the block it uses (its texture
//             rectangle) with its palette, the rest stays transparent.
//               - merged (default): one file per block, idx_<hash of the bytes>.png; each part has the palette of the first polygon
//                 that used it. A replacement is used for every palette.
//               - variants: one file per block and palette, tex_<hash of the colours>.png, with only the parts drawn with that
//                 palette (cars in five colours, day / night versions ...).
//             Files are written once nothing new was painted for a while and rewritten when more of the block comes into use.
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

	// For a textured polygon: `dma` = its 16 queue words (flags, palette base, texture coordinates, texture base), `ram` = texture
	// RAM (4 MB), `pal` = palette (0x00RRGGBB, 32768 entries), `tex_gen` changes whenever texture RAM is written. Returns
	// (layer + 1) of the block holding the texture base row in the low 16 bits and of the following block in the high 16 bits
	// (0 = original texture). In export mode it also paints the polygon's part of the block into the export picture.
	uint32_t lookup(const uint16_t *dma, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen);

	// once per frame: writes the export pictures nothing was added to for a while. A picture whose block has changed in the
	// meantime (the game streams textures in over several frames, palettes fade) is dropped instead.
	void tick(const uint8_t *ram, const uint32_t *pal) { if (m_dirty) flush_pending(ram, pal, false); }
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
		bool varied = false;            // more than one colour (a flat block is not exported)
	};
	static uint64_t colour_hash(const uint8_t *ram, uint32_t block, const uint32_t *pal, uint32_t pix, bool *used, int *colours, uint64_t *data_hash);
	Entry &block_entry(uint32_t block, uint32_t pix, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen);
	void flush_pending(const uint8_t *ram, const uint32_t *pal, bool all);

	struct DumpWriter;                       // one background thread that encodes and writes the PNG files
	std::unique_ptr<DumpWriter> m_writer;
	// export picture of one file name
	struct Picture
	{
		std::vector<uint8_t> rgba;                 // 256 x 256, alpha 0 = not drawn by any polygon (or the transparent index)
		std::unordered_set<uint32_t> rects;        // texture rectangles already painted
		uint32_t block = 0, pix = 0;               // where it was last seen (to check that it is still there when it is written)
		uint32_t painted = 0, written = 0;         // rectangles painted / painted when the file was last written
		int age = 0;                               // frames since something was painted
	};
	std::unordered_map<uint64_t, Picture> m_pics;  // file name hash -> picture
	void paint(Picture &pic, uint32_t block, int u0, int v0, int u1, int v1, uint32_t pix, int solid, bool keyed, const uint8_t *ram, const uint32_t *pal);
	bool m_dirty = false;
	int m_written = 0;

	Config m_c;
	int m_res = 0, m_layers = 0;
	std::vector<std::vector<uint8_t>> m_pages;
	std::unordered_map<uint64_t, int> m_table;     // colour hash -> layer
	std::unordered_map<uint32_t, Entry> m_cache;   // (block, pix) -> last lookup
};
