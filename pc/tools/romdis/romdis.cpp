// Disassemble the Cruis'n USA main CPU ROM (for locating patch sites):  romdis <rom.zip> <version-dir|-> <startword> <count>
#include "emu.h"
#include "../../src/cpu/tms320c3x/tms320c3x_dasm.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <string>
#include "../../third_party/miniz/miniz.h"

struct Buf : util::disasm_interface::data_buffer
{
	const std::vector<uint32_t> *w;
	uint32_t r32(offs_t pc) const override { return pc < w->size() ? (*w)[pc] : 0; }
};

int main(int argc, char **argv)
{
	if (argc < 5) { std::fprintf(stderr, "romdis <zip> <dir or -> <start hex> <count>\n"); return 1; }
	mz_zip_archive z{};
	if (!mz_zip_reader_init_file(&z, argv[1], 0)) return 2;
	std::string dir = std::string(argv[2]) == "-" ? "" : argv[2];
	std::vector<uint32_t> words;
	for (int u = 10; u <= 13; u++)
	{
		char suf[16]; std::snprintf(suf, sizeof suf, ".u%d", u);
		std::vector<uint8_t> data;
		for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++)
		{
			char name[512]; mz_zip_reader_get_filename(&z, i, name, sizeof name);
			std::string n = name;
			bool rootfile = n.find('/') == std::string::npos;
			if (n.size() > 4 && n.compare(n.size() - 4, 4, suf) == 0 && (dir.empty() ? rootfile : n.rfind(dir, 0) == 0))
			{
				size_t sz; void *p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
				data.assign((uint8_t *)p, (uint8_t *)p + sz);
			}
		}
		if (words.size() < data.size()) words.resize(data.size());
		for (size_t i = 0; i < data.size(); i++) words[i] |= uint32_t(data[i]) << (8 * (u - 10));
	}
	tms320c3x_disassembler d;
	Buf b; b.w = &words;
	uint32_t pc = std::strtoul(argv[3], nullptr, 16), n = std::strtoul(argv[4], nullptr, 0);
	for (uint32_t i = 0; i < n; i++, pc++)
	{
		std::ostringstream os;
		d.disassemble(os, pc, b, b);
		std::printf("%05X  %08X  %s\n", pc, words[pc], os.str().c_str());
	}
}
