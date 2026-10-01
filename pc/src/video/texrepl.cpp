#include "texrepl.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
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

// Export: PNG encoding (zlib) takes a few milliseconds per block, too long for the emulation thread (a hitch whenever new textures
// appear). The pixels are converted on the emulation thread and handed to one persistent writer thread.
struct TexRepl::DumpWriter
{
	struct Job { std::string path; std::vector<uint8_t> rgba; };
	std::mutex mx;
	std::condition_variable cv;
	std::deque<Job> jobs;
	bool quit = false;
	std::thread thread;

	DumpWriter()
	{
		thread = std::thread([this] {
			for (;;)
			{
				Job j;
				{
					std::unique_lock<std::mutex> lk(mx);
					cv.wait(lk, [&] { return quit || !jobs.empty(); });
					if (jobs.empty()) return;   // quit, and everything is written
					j = std::move(jobs.front());
					jobs.pop_front();
				}
				png_write_rgba(j.path, 256, 256, j.rgba.data());
			}
		});
	}
	~DumpWriter()
	{
		{ std::lock_guard<std::mutex> lk(mx); quit = true; }
		cv.notify_one();
		thread.join();
	}
	void post(std::string path, std::vector<uint8_t> rgba)
	{
		{ std::lock_guard<std::mutex> lk(mx); jobs.push_back({std::move(path), std::move(rgba)}); }
		cv.notify_one();
	}
};

TexRepl::TexRepl() = default;
TexRepl::~TexRepl() = default;

namespace {
// run fn(i) for i in [0, n) on a few threads (startup work only; the threads end with the call)
template <class F> void parallel_for(size_t n, F fn)
{
	const size_t hw = std::max(1u, std::thread::hardware_concurrency());
	const size_t nt = std::min<size_t>({n, hw, 8});
	if (nt <= 1) { for (size_t i = 0; i < n; i++) fn(i); return; }
	std::atomic<size_t> next{0};
	std::vector<std::thread> ts;
	for (size_t t = 0; t + 1 < nt; t++)
		ts.emplace_back([&] { for (size_t i; (i = next.fetch_add(1)) < n;) fn(i); });
	for (size_t i; (i = next.fetch_add(1)) < n;) fn(i);
	for (auto &t : ts) t.join();
}
}

// Identity of a block = hash of the colours it shows (bytes seen through the palette) and hash of its bytes. `used` marks the palette
// entries it reads.
uint64_t TexRepl::colour_hash(const uint8_t *ram, uint32_t block, const uint32_t *pal, uint32_t pix, bool *used, int *colours, uint64_t *data_hash)
{
	uint64_t h = 0x9E3779B97F4A7C15ull, dh = 0x7654321ull;
	const uint8_t *p = ram + size_t(block) * 65536;
	std::memset(used, 0, 256);
	uint32_t first = 0xffffffffu;
	int distinct = 0;
	for (size_t i = 0; i < 65536; i++)
	{
		uint8_t t = p[i];
		used[t] = true;
		uint32_t c = t == 0 ? 0xff000000u : (pal[(pix + t) & 0x7fff] & 0xffffffu);
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

	struct Item { std::string path, name, err; uint64_t hash64 = 0; int w = 0, h = 0; bool ok = false; std::vector<uint8_t> rgba; };
	std::vector<Item> found;
	for (auto &de : fs::directory_iterator(m_c.repl_dir, ec))
	{
		if (!de.is_regular_file()) continue;
		std::string name = de.path().filename().string();
		unsigned long long hh;
		bool is_idx = false;
		if (std::sscanf(name.c_str(), "tex_%llx.png", &hh) != 1) { if (std::sscanf(name.c_str(), "idx_%llx.png", &hh) != 1) continue; is_idx = true; }
		Item it;
		it.path = de.path().string();
		it.name = name;
		it.hash64 = is_idx ? (hh | (1ull << 63)) : (hh & ~(1ull << 63));
		found.push_back(std::move(it));
	}
	// decoding is independent per file: spread it over the cores (an upscaled pack can be hundreds of large PNGs)
	parallel_for(found.size(), [&](size_t i) {
		Item &it = found[i];
		it.ok = png_read_rgba(it.path, it.w, it.h, it.rgba, &it.err) && it.w >= 16 && it.h >= 16;
	});
	std::vector<Item> items;
	int maxres = 256;
	for (Item &it : found)
	{
		if (!it.ok) { if (!it.err.empty()) log += it.name + ": " + it.err + "\n"; continue; }
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
	m_pages.resize(items.size());
	parallel_for(items.size(), [&](size_t i) {
		m_pages[i] = resample(items[i].rgba, items[i].w, items[i].h, res);
		items[i].rgba.clear(); items[i].rgba.shrink_to_fit();
	});
	for (size_t i = 0; i < items.size(); i++) m_table[items[i].hash64] = int(i);
	m_res = res;
	m_layers = int(m_pages.size());
	log += "loaded " + std::to_string(m_layers) + " replacement textures at " + std::to_string(res) + " px\n";
	return m_layers;
}

TexRepl::Entry &TexRepl::block_entry(uint32_t block, uint32_t pix, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen)
{
	Entry &e = m_cache[(block << 16) | (pix & 0xffff)];
	bool same = e.tex_gen == tex_gen;
	if (same)
		for (int i = 0; i < 256 && same; i++) same = !e.used[i] || e.pal[i] == (pal[(pix + uint32_t(i)) & 0x7fff] & 0xffffffu);
	if (!same)
	{
		e.tex_gen = tex_gen;
		int varied = 0;
		e.hash = colour_hash(ram, block, pal, pix, e.used, &varied, &e.dhash);
		e.hash &= ~(1ull << 63); e.dhash |= (1ull << 63);
		e.varied = varied != 0;
		for (int i = 0; i < 256; i++) e.pal[i] = pal[(pix + uint32_t(i)) & 0x7fff] & 0xffffffu;
		auto it = m_table.find(e.hash);
		if (it == m_table.end()) it = m_table.find(e.dhash);
		e.layer = it == m_table.end() ? 0 : it->second + 1;
	}
	return e;
}

// paint the texels [u0..u1] x [v0..v1] of a block (block coordinates) into its export picture, coloured as the polygon draws them:
// through its palette, or all in one colour when the texture only serves as a mask (`solid` >= 0). Texels that are already painted
// keep their colour (merged mode: the first palette wins).
void TexRepl::paint(Picture &pic, uint32_t block, int u0, int v0, int u1, int v1, uint32_t pix, int solid, bool keyed, const uint8_t *ram, const uint32_t *pal)
{
	if (pic.rgba.empty()) pic.rgba.assign(256 * 256 * 4, 0);
	const uint32_t key = uint32_t(u0) | (uint32_t(v0) << 8) | (uint32_t(u1) << 16) | (uint32_t(v1) << 24);
	pic.block = block; pic.pix = pix;
	if (!pic.rects.insert(key).second) return;
	const uint8_t *p = ram + size_t(block) * 65536;
	for (int v = v0; v <= v1; v++)
		for (int u = u0; u <= u1; u++)
		{
			uint8_t *d = &pic.rgba[(size_t(v) * 256 + size_t(u)) * 4];
			if (d[3]) continue;
			const uint8_t t = p[v * 256 + u];
			const uint32_t col = pal[(pix + uint32_t(solid >= 0 && t ? solid : t)) & 0x7fff];
			d[0] = uint8_t(col >> 16); d[1] = uint8_t(col >> 8); d[2] = uint8_t(col);
			d[3] = (keyed && t == 0) ? 0 : 255;
		}
	pic.painted++;
	pic.age = 0;
	m_dirty = true;
}

void TexRepl::flush_pending(const uint8_t *ram, const uint32_t *pal, bool all)
{
	constexpr int kStableFrames = 45;
	bool open = false;
	for (auto it = m_pics.begin(); it != m_pics.end();)
	{
		Picture &pic = it->second;
		if (pic.painted == pic.written) { ++it; continue; }
		if (!all && ++pic.age < kStableFrames) { open = true; ++it; continue; }
		// still the same block (and, for a palette variant, the same colours)? A state that was only passed through (texture
		// still loading, palette fading) is dropped.
		bool used[256];
		uint64_t dh = 0;
		uint64_t h = colour_hash(ram, pic.block, pal, pic.pix, used, nullptr, &dh);
		h &= ~(1ull << 63); dh |= (1ull << 63);
		if ((m_c.variants ? h : dh) != it->first) { it = m_pics.erase(it); continue; }
		std::error_code ec;
		fs::create_directories(m_c.dump_dir, ec);
		char name[64];
		std::snprintf(name, sizeof name, "%s_%016llX.png", m_c.variants ? "tex" : "idx", (unsigned long long)it->first);
		if (!m_writer) m_writer = std::make_unique<DumpWriter>();
		m_writer->post((fs::path(m_c.dump_dir) / name).string(), pic.rgba);   // a copy: the picture may still grow
		if (pic.written == 0) m_written++;
		pic.written = pic.painted;
		++it;
	}
	m_dirty = open;
}

uint32_t TexRepl::lookup(const uint16_t *dma, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen)
{
	if (!m_c.dump && m_layers == 0) return 0;
	const uint32_t base = dma[14], pix = dma[1], mode = dma[0] & 0xc00;
	if (base >= 0x4000) return 0;   // outside texture RAM (the hardware reads zeros)
	const uint32_t b0 = base >> 8;
	const bool two = (base & 255) != 0 && b0 + 1 < 64;
	Entry &e0 = block_entry(b0, pix, ram, pal, tex_gen);
	uint32_t r = uint32_t(e0.layer);
	const uint64_t name0 = m_c.variants ? e0.hash : e0.dhash;
	const bool varied0 = e0.varied;
	uint64_t name1 = 0; bool varied1 = false;
	if (two)
	{
		Entry &e1 = block_entry(b0 + 1, pix, ram, pal, tex_gen);   // (may rehash the map: e0 is not used after this)
		r |= uint32_t(e1.layer) << 16;
		name1 = m_c.variants ? e1.hash : e1.dhash;
		varied1 = e1.varied;
	}
	if (m_c.dump)
	{
		// the polygon's texture rectangle (one texel more on every side for the filtered edge), as rows of the texture strip
		int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
		for (int i = 0; i < 4; i++)
		{
			const int u = dma[10 + i] & 0xff, v = dma[10 + i] >> 8;
			u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
		}
		u0 = std::max(0, u0 - 1); u1 = std::min(255, u1 + 1); v0 = std::max(0, v0 - 1); v1 = std::min(255, v1 + 1);
		const int solid = mode == 0xc00 ? int(dma[0] & 0xff) : -1;
		const bool keyed = mode != 0;
		const int row0 = int(base) + v0, row1 = int(base) + v1;   // strip rows
		const int split = int(b0 + 1) * 256;
		if (varied0 && row0 < split)
			paint(m_pics[name0], b0, u0, row0 - int(b0) * 256, u1, std::min(row1, split - 1) - int(b0) * 256, pix, solid, keyed, ram, pal);
		if (two && varied1 && row1 >= split)
			paint(m_pics[name1], b0 + 1, u0, std::max(row0, split) - split, u1, row1 - split, pix, solid, keyed, ram, pal);
	}
	return r;
}
