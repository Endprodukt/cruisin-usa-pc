// Minimal stand-ins for the MAME disassembler interface (used only by the romdis tool)
#pragma once
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <sstream>
#include <cstdarg>

using offs_t = uint32_t;
using u32 = uint32_t;
namespace util {
struct disasm_interface
{
	static constexpr u32 STEP_OVER = 0x10000000, STEP_OUT = 0x20000000, STEP_COND = 0x40000000, SUPPORTED = 0x80000000;
	struct data_buffer { virtual uint32_t r32(offs_t pc) const = 0; };
	virtual ~disasm_interface() = default;
	virtual u32 opcode_alignment() const = 0;
	virtual offs_t disassemble(std::ostream &stream, offs_t pc, const data_buffer &opcodes, const data_buffer &params) = 0;
	static constexpr u32 step_over_extra(int n) { return u32(n) << 20; }
};
inline std::string string_format(const char *fmt, ...)
{
	char buf[256];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	return buf;
}
inline void stream_format(std::ostream &s, const char *fmt, ...)
{
	char buf[256];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	s << buf;
}
}
