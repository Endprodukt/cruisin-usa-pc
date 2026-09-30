// Minimal compatibility layer so that CPU/DSP cores derived from MAME
// (BSD-3-Clause, copyright-holders: Aaron Giles et al.) build without the
// MAME device framework.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <bit>
#include <functional>

using offs_t = uint32_t;

enum line_state { CLEAR_LINE = 0, ASSERT_LINE = 1 };

#define BYTE_XOR_BE(x) ((x) ^ 1) // little-endian host

template <typename T, typename U>
constexpr bool BIT(T x, U n) { return (x >> n) & 1; }

template <typename T, typename U, typename V>
constexpr T BIT(T x, U n, V w) { return (x >> n) & ((T(1) << w) - 1); }

namespace util {
constexpr int32_t sext(uint32_t v, unsigned bits)
{
	return int32_t(v << (32 - bits)) >> (32 - bits);
}
} // namespace util

constexpr int64_t mul_32x32(int32_t a, int32_t b) { return int64_t(a) * int64_t(b); }
constexpr uint64_t mulu_32x32(uint32_t a, uint32_t b) { return uint64_t(a) * uint64_t(b); }

[[noreturn]] inline void fatalerror_impl(const char *fmt, ...);
#include <cstdarg>
[[noreturn]] inline void fatalerror_impl(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fflush(stderr);
	std::abort();
}
#define fatalerror(...) fatalerror_impl(__VA_ARGS__)

#ifndef CRUISN_CPU_LOG
#define logerror(...) ((void)0)
#else
#define logerror(...) std::fprintf(stderr, __VA_ARGS__)
#endif
