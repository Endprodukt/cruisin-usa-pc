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

void TexRepl::write_dump(uint32_t block, uint32_t pix, uint64_t hash, const char *prefix, const uint8_t *ram, const uint32_t *pal)
{
	m_written++;
	std::error_code ec;
	fs::create_directories(m_c.dump_dir, ec);
	std::vector<uint8_t> rgba(256 * 256 * 4);
	const uint8_t *p = ram + size_t(block) * 65536;
	for (int i = 0; i < 65536; i++)
	{
		uint8_t t = p[i];
		uint32_t c = pal[(pix + t) & 0x7fff];
		rgba[size_t(i) * 4 + 0] = uint8_t(c >> 16);
		rgba[size_t(i) * 4 + 1] = uint8_t(c >> 8);
		rgba[size_t(i) * 4 + 2] = uint8_t(c);
		rgba[size_t(i) * 4 + 3] = t == 0 ? 0 : 255;
	}
	char name[64];
	std::snprintf(name, sizeof name, "%s_%016llX.png", prefix, (unsigned long long)hash);
	if (!m_writer) m_writer = std::make_unique<DumpWriter>();
	m_writer->post((fs::path(m_c.dump_dir) / name).string(), std::move(rgba));
}

int TexRepl::block_layer(uint32_t block, uint32_t pix, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen)
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
		for (int i = 0; i < 256; i++) e.pal[i] = pal[(pix + uint32_t(i)) & 0x7fff] & 0xffffffu;
		auto it = m_table.find(e.hash);
		if (it == m_table.end()) it = m_table.find(e.dhash);
		e.layer = it == m_table.end() ? 0 : it->second + 1;
		// a block of one flat colour is not worth a file; the others are written once they have stayed unchanged for a while
		const uint64_t name = m_c.variants ? e.hash : e.dhash;
		if (m_c.dump && varied && m_dumped.insert(name).second)
			m_pending.push_back({block, pix, name, 0});
	}
	return e.layer;
}

void TexRepl::flush_pending(const uint8_t *ram, const uint32_t *pal, bool all)
{
	constexpr int kStableFrames = 45;
	for (size_t i = 0; i < m_pending.size();)
	{
		Pending &p = m_pending[i];
		bool used[256];
		uint64_t dh = 0;
		uint64_t h = colour_hash(ram, p.block, pal, p.pix, used, nullptr, &dh);
		h &= ~(1ull << 63); dh |= (1ull << 63);
		if ((m_c.variants ? h : dh) != p.name)
		{
			// changed since it was drawn: this state is dropped (the next state gets its own entry when it is drawn); it may come
			// back later, so it is not remembered as written
			m_dumped.erase(p.name);
			m_pending[i] = m_pending.back();
			m_pending.pop_back();
			continue;
		}
		if (all || ++p.age >= kStableFrames)
		{
			write_dump(p.block, p.pix, p.name, m_c.variants ? "tex" : "idx", ram, pal);
			m_pending[i] = m_pending.back();
			m_pending.pop_back();
			continue;
		}
		i++;
	}
}

uint32_t TexRepl::lookup(uint32_t base, uint32_t pix, const uint8_t *ram, const uint32_t *pal, uint64_t tex_gen)
{
	if (!m_c.dump && m_layers == 0) return 0;
	if (base >= 0x4000) return 0;   // outside texture RAM (the hardware reads zeros)
	const uint32_t b0 = base >> 8;
	uint32_t r = uint32_t(block_layer(b0, pix, ram, pal, tex_gen));
	if ((base & 255) != 0 && b0 + 1 < 64) r |= uint32_t(block_layer(b0 + 1, pix, ram, pal, tex_gen)) << 16;
	return r;
}
