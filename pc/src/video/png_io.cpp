#include "png_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../third_party/miniz/miniz.h"

namespace {
bool fail(std::string *err, const char *m) { if (err) *err = m; return false; }
uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
	return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}
}

bool png_read_rgba(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba, std::string *err)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return fail(err, "cannot open file");
	std::vector<uint8_t> file;
	{
		std::fseek(f, 0, SEEK_END);
		long n = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (n < 33) { std::fclose(f); return fail(err, "file too small"); }
		file.resize(size_t(n));
		size_t got = std::fread(file.data(), 1, file.size(), f);
		std::fclose(f);
		if (got != file.size()) return fail(err, "read error");
	}
	static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	if (std::memcmp(file.data(), sig, 8) != 0) return fail(err, "not a PNG");

	int ctype = -1, depth = 0, interlace = 0;
	w = h = 0;
	std::vector<uint8_t> idat, plte, trns;
	size_t pos = 8;
	while (pos + 12 <= file.size())
	{
		uint32_t len = be32(&file[pos]);
		const uint8_t *type = &file[pos + 4];
		const uint8_t *data = &file[pos + 8];
		if (pos + 12 + len > file.size()) return fail(err, "truncated chunk");
		if (!std::memcmp(type, "IHDR", 4) && len >= 13)
		{
			w = int(be32(data)); h = int(be32(data + 4)); depth = data[8]; ctype = data[9]; interlace = data[12];
		}
		else if (!std::memcmp(type, "PLTE", 4)) plte.assign(data, data + len);
		else if (!std::memcmp(type, "tRNS", 4)) trns.assign(data, data + len);
		else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), data, data + len);
		else if (!std::memcmp(type, "IEND", 4)) break;
		pos += 12 + len;
	}
	if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return fail(err, "bad size");
	if (depth != 8) return fail(err, "only 8 bit PNGs are supported");
	if (interlace) return fail(err, "interlaced PNGs are not supported");
	int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
	if (!ch) return fail(err, "unsupported colour type");

	size_t rawlen = 0;
	uint8_t *raw = static_cast<uint8_t *>(tinfl_decompress_mem_to_heap(idat.data(), idat.size(), &rawlen, TINFL_FLAG_PARSE_ZLIB_HEADER));
	if (!raw) return fail(err, "inflate failed");
	const size_t stride = size_t(w) * ch;
	if (rawlen < (stride + 1) * size_t(h)) { mz_free(raw); return fail(err, "short image data"); }

	std::vector<uint8_t> cur(stride), prev(stride, 0);
	rgba.assign(size_t(w) * h * 4, 255);
	for (int y = 0; y < h; y++)
	{
		const uint8_t *line = raw + (stride + 1) * size_t(y);
		int filter = line[0];
		for (size_t i = 0; i < stride; i++)
		{
			int a = i >= size_t(ch) ? cur[i - ch] : 0, b = prev[i], c = i >= size_t(ch) ? prev[i - ch] : 0, x = line[1 + i], v;
			switch (filter)
			{
			case 0: v = x; break;
			case 1: v = x + a; break;
			case 2: v = x + b; break;
			case 3: v = x + ((a + b) >> 1); break;
			case 4: v = x + paeth(a, b, c); break;
			default: mz_free(raw); return fail(err, "bad filter");
			}
			cur[i] = uint8_t(v);
		}
		uint8_t *o = &rgba[size_t(y) * w * 4];
		for (int x = 0; x < w; x++)
		{
			const uint8_t *s = &cur[size_t(x) * ch];
			switch (ctype)
			{
			case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
			case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
			case 3: {
				size_t idx = s[0];
				o[0] = idx * 3 + 2 < plte.size() ? plte[idx * 3] : 0; o[1] = idx * 3 + 2 < plte.size() ? plte[idx * 3 + 1] : 0;
				o[2] = idx * 3 + 2 < plte.size() ? plte[idx * 3 + 2] : 0; o[3] = idx < trns.size() ? trns[idx] : 255;
				break; }
			case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
			case 6: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
			}
			o += 4;
		}
		std::swap(cur, prev);
	}
	mz_free(raw);
	return true;
}

bool png_write_rgba(const std::string &path, int w, int h, const uint8_t *rgba)
{
	size_t len = 0;
	void *png = tdefl_write_image_to_png_file_in_memory(rgba, w, h, 4, &len);
	if (!png) return false;
	FILE *f = std::fopen(path.c_str(), "wb");
	bool ok = f && std::fwrite(png, 1, len, f) == len;
	if (f) std::fclose(f);
	mz_free(png);
	return ok;
}
