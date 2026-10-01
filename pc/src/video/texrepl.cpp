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

// hash of the page bytes and of the palette entries the page actually uses (`used` is filled in)
uint32_t TexRepl::page_hash(const uint8_t *ram, uint32_t base, const uint32_t *pal, uint32_t pix, bool *used)
{
	uint64_t h = 0x9E3779B97F4A7C15ull;
	size_t off = size_t(base) * 256 & 0x3fffff, n = std::min<size_t>(65536, 0x400000 - off);
	const uint8_t *p = ram + off;
	std::memset(used, 0, 256);
	for (size_t i = 0; i < n; i++) used[p[i]] = true;
	for (size_t i = 0; i + 8 <= n; i += 8)
	{
		uint64_t w;
		std::memcpy(&w, p + i, 8);
		h = (h ^ w) * 0xff51afd7ed558ccdull;
		h ^= h >> 32;
	}
	for (int i = 0; i < 256; i++)
		if (used[i])
		{
			h = (h ^ (pal[(pix + uint32_t(i)) & 0x7fff] & 0xffffffu) ^ (uint64_t(i) << 40)) * 0xc4ceb9fe1a85ec53ull;
			h ^= h >> 29;
		}
	return uint32_t(h ^ (h >> 32));
}

int TexRepl::load(std::string &log)
{
	m_table.clear(); m_pages.clear(); m_cache.clear();
	m_res = 0; m_layers = 0;
	if (!m_c.replace || m_c.repl_dir.empty()) return 0;
	std::error_code ec;
	if (!fs::is_directory(m_c.repl_dir, ec)) { log += "replacement folder not found: " + m_c.repl_dir + "\n"; return 0; }

	struct Item { uint32_t base, pix, hash; std::string path; int w = 0, h = 0; std::vector<uint8_t> rgba; };
	std::vector<Item> items;
	int maxres = 256;
	for (auto &de : fs::directory_iterator(m_c.repl_dir, ec))
	{
		if (!de.is_regular_file()) continue;
		std::string name = de.path().filename().string();
		unsigned b, p, hh;
		if (std::sscanf(name.c_str(), "tex_%x_%x_%x.png", &b, &p, &hh) != 3) continue;
		Item it{b, p, hh, de.path().string()};
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
		m_table[key3(it.base, it.pix, it.hash)] = int(m_pages.size());
		m_pages.push_back(resample(it.rgba, it.w, it.h, res));
		it.rgba.clear(); it.rgba.shrink_to_fit();
	}
	m_res = res;
	m_layers = int(m_pages.size());
	log += "loaded " + std::to_string(m_layers) + " replacement textures at " + std::to_string(res) + " px\n";
	return m_layers;
}

void TexRepl::write_dump(uint32_t base, uint32_t pix, uint32_t hash, uint32_t mode, const uint8_t *ram, const uint32_t *pal)
{
	if (!m_dumped.insert(key3(base, pix, hash)).second) return;
	std::error_code ec;
	fs::create_directories(m_c.dump_dir, ec);
	std::vector<uint8_t> rgba(256 * 256 * 4);
	size_t off = size_t(base) * 256 & 0x3fffff;
	const bool keyed = (mode == 0x800 || mode == 0xc00);
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
	std::snprintf(name, sizeof name, "tex_%04X_%04X_%08X.png", base & 0xffff, pix & 0xffff, hash);
	png_write_rgba((fs::path(m_c.dump_dir) / name).string(), 256, 256, rgba.data());
}

int TexRepl::lookup(uint32_t base, uint32_t pix, uint32_t mode, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen)
{
	if (!m_c.dump && m_layers == 0) return 0;
	Entry &e = m_cache[(base << 16) | (pix & 0xffff)];
	bool same = e.tex_gen == tex_gen;
	if (same)
		for (int i = 0; i < 256 && same; i++) same = !e.used[i] || e.pal[i] == (pal[(pix + uint32_t(i)) & 0x7fff] & 0xffffffu);
	if (!same)
	{
		e.tex_gen = tex_gen;
		e.hash = page_hash(ram, base, pal, pix, e.used);
		for (int i = 0; i < 256; i++) e.pal[i] = pal[(pix + uint32_t(i)) & 0x7fff] & 0xffffffu;
		auto it = m_table.find(key3(base, pix, e.hash));
		e.layer = it == m_table.end() ? 0 : it->second + 1;
		if (m_c.dump) write_dump(base, pix, e.hash, mode, ram, pal);
	}
	return e.layer;
}
