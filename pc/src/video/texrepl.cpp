#include "texrepl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "png_io.h"

namespace fs = std::filesystem;

namespace {
uint64_t key3(uint32_t base, uint32_t pix, uint32_t hash) { return (uint64_t(base & 0xffff) << 48) | (uint64_t(pix & 0xffff) << 32) | hash; }

// resample an RGBA image to n x n (bilinear when growing, box when shrinking by an integer factor is good enough with bilinear too)
std::vector<uint8_t> resample(const std::vector<uint8_t> &src, int sw, int sh, int n)
{
	if (sw == n && sh == n) return src;
	std::vector<uint8_t> out(size_t(n) * n * 4);
	for (int y = 0; y < n; y++)
	{
		float fy = (float(y) + 0.5f) * float(sh) / float(n) - 0.5f;
		int y0 = std::clamp(int(std::floor(fy)), 0, sh - 1), y1 = std::min(y0 + 1, sh - 1);
		float ty = std::clamp(fy - float(y0), 0.0f, 1.0f);
		for (int x = 0; x < n; x++)
		{
			float fx = (float(x) + 0.5f) * float(sw) / float(n) - 0.5f;
			int x0 = std::clamp(int(std::floor(fx)), 0, sw - 1), x1 = std::min(x0 + 1, sw - 1);
			float tx = std::clamp(fx - float(x0), 0.0f, 1.0f);
			for (int c = 0; c < 4; c++)
			{
				float a = src[(size_t(y0) * sw + x0) * 4 + c] * (1 - tx) + src[(size_t(y0) * sw + x1) * 4 + c] * tx;
				float b = src[(size_t(y1) * sw + x0) * 4 + c] * (1 - tx) + src[(size_t(y1) * sw + x1) * 4 + c] * tx;
				out[(size_t(y) * n + x) * 4 + c] = uint8_t(a * (1 - ty) + b * ty + 0.5f);
			}
		}
	}
	return out;
}
}

// Identity of a texture = hash of the colours it actually shows (page bytes seen through the palette), so pages that look the same
// share one file no matter where in texture RAM they sit or which palette range they use. `used` marks the palette entries it reads.
uint64_t TexRepl::colour_hash(const uint8_t *ram, uint32_t base, const uint32_t *pal, uint32_t pix, bool keyed, bool *used, int *colours, uint64_t *data_hash)
{
	uint64_t h = 0x9E3779B97F4A7C15ull;
	size_t off = size_t(base) * 256 & 0x3fffff, n = std::min<size_t>(65536, 0x400000 - off);
	const uint8_t *p = ram + off;
	std::memset(used, 0, 256);
	uint32_t first = 0xffffffffu;
	int distinct = 0;
	uint64_t dh = keyed ? 0x1234567ull : 0x7654321ull;
	for (size_t i = 0; i < 65536; i++)
	{
		uint8_t t = i < n ? p[i] : 0;
		used[t] = true;
		uint32_t c = (keyed && t == 0) ? 0xff000000u : (pal[(pix + t) & 0x7fff] & 0xffffffu);
		if (c != first) { if (first == 0xffffffffu) first = c; else distinct = 1; }
		h = (h ^ c) * 0xff51afd7ed558ccdull;
		h ^= h >> 29;
		dh = (dh ^ t) * 0xc4ceb9fe1a85ec53ull;
		dh ^= dh >> 31;
	}
	if (data_hash) *data_hash = dh ? dh : 1;
	if (colours) *colours = distinct;
	return h ? h : 1;
}

int TexRepl::load(std::string &log)
{
	m_table.clear(); m_pages.clear(); m_cache.clear();
	m_res = 0; m_layers = 0;
	if (!m_c.replace || m_c.repl_dir.empty()) return 0;
	std::error_code ec;
	if (!fs::is_directory(m_c.repl_dir, ec)) { log += "replacement folder not found: " + m_c.repl_dir + "\n"; return 0; }

	struct Item { uint32_t base, pix, hash; std::string path; uint64_t hash64 = 0; int w = 0, h = 0; std::vector<uint8_t> rgba; };
	std::vector<Item> items;
	int maxres = 256;
	for (auto &de : fs::directory_iterator(m_c.repl_dir, ec))
	{
		if (!de.is_regular_file()) continue;
		std::string name = de.path().filename().string();
		unsigned long long hh;
		bool is_idx = false;
		if (std::sscanf(name.c_str(), "tex_%llx.png", &hh) != 1) { if (std::sscanf(name.c_str(), "idx_%llx.png", &hh) != 1) continue; is_idx = true; }
		Item it{0, 0, 0, de.path().string()};
		it.hash64 = is_idx ? (hh | (1ull << 63)) : (hh & ~(1ull << 63));
		std::string err;
		if (!png_read_rgba(it.path, it.w, it.h, it.rgba, &err)) { log += name + ": " + err + "\n"; continue; }
		if (it.w < 16 || it.h < 16) continue;
		maxres = std::max(maxres, std::max(it.w, it.h));
		items.push_back(std::move(it));
	}
	if (items.empty()) return 0;

	int res = 256;
	while (res < maxres && res < m_c.max_res) res *= 2;
	// memory budget for the layers on the GPU
	const size_t budget = size_t(384) << 20;
	size_t max_layers = std::max<size_t>(1, budget / (size_t(res) * res * 4));
	max_layers = std::min<size_t>(max_layers, 512);
	if (items.size() > max_layers)
	{
		log += "replacement pack has " + std::to_string(items.size()) + " textures; only the first " + std::to_string(max_layers) + " fit at " + std::to_string(res) + " px\n";
		items.resize(max_layers);
	}
	for (auto &it : items)
	{
		m_table[it.hash64] = int(m_pages.size());
		m_pages.push_back(resample(it.rgba, it.w, it.h, res));
		it.rgba.clear(); it.rgba.shrink_to_fit();
	}
	m_res = res;
	m_layers = int(m_pages.size());
	log += "loaded " + std::to_string(m_layers) + " replacement textures at " + std::to_string(res) + " px\n";
	return m_layers;
}

void TexRepl::write_dump(uint32_t base, uint32_t pix, uint64_t hash, const char *prefix, bool keyed, const uint8_t *ram, const uint32_t *pal)
{
	if (!m_dumped.insert(hash).second) return;
	std::error_code ec;
	fs::create_directories(m_c.dump_dir, ec);
	std::vector<uint8_t> rgba(256 * 256 * 4);
	size_t off = size_t(base) * 256 & 0x3fffff;
	for (int i = 0; i < 65536; i++)
	{
		uint8_t t = off + size_t(i) < 0x400000 ? ram[off + size_t(i)] : 0;
		uint32_t c = pal[(pix + t) & 0x7fff];
		rgba[size_t(i) * 4 + 0] = uint8_t(c >> 16);
		rgba[size_t(i) * 4 + 1] = uint8_t(c >> 8);
		rgba[size_t(i) * 4 + 2] = uint8_t(c);
		rgba[size_t(i) * 4 + 3] = (keyed && t == 0) ? 0 : 255;
	}
	char name[64];
	std::snprintf(name, sizeof name, "%s_%016llX.png", prefix, (unsigned long long)hash);
	png_write_rgba((fs::path(m_c.dump_dir) / name).string(), 256, 256, rgba.data());
}

int TexRepl::lookup(uint32_t base, uint32_t pix, uint32_t mode, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen)
{
	if (!m_c.dump && m_layers == 0) return 0;
	const bool keyed = (mode == 0x800 || mode == 0xc00);
	Entry &e = m_cache[(base << 16) | (pix & 0xffff)];
	bool same = e.tex_gen == tex_gen && e.keyed == keyed;
	if (same)
		for (int i = 0; i < 256 && same; i++) same = !e.used[i] || e.pal[i] == (pal[(pix + uint32_t(i)) & 0x7fff] & 0xffffffu);
	if (!same)
	{
		e.tex_gen = tex_gen;
		e.keyed = keyed;
		int varied = 0;
		e.hash = colour_hash(ram, base, pal, pix, keyed, e.used, &varied, &e.dhash);
		e.hash &= ~(1ull << 63); e.dhash |= (1ull << 63);
		for (int i = 0; i < 256; i++) e.pal[i] = pal[(pix + uint32_t(i)) & 0x7fff] & 0xffffffu;
		auto it = m_table.find(e.hash);
		if (it == m_table.end()) it = m_table.find(e.dhash);
		e.layer = it == m_table.end() ? 0 : it->second + 1;
		if (m_c.dump && varied)
		{
			if (m_c.variants) write_dump(base, pix, e.hash, "tex", keyed, ram, pal);
			else write_dump(base, pix, e.dhash, "idx", keyed, ram, pal);
		}   // a page of one flat colour is not worth a file
	}
	return e.layer;
}
