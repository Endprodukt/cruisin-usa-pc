// Midway DCS audio board, "2K" variant (ADSP-2105 @ 10 MHz + 2K shared RAM, ROM-based).
// Behaviour follows MAME's dcs.cpp (BSD-3-Clause, copyright-holders: Aaron Giles).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "../cpu/adsp2100/adsp2100.h"

class Dcs1
{
public:
	// rom: 16-bit LE words of the "dcs" region (data in the low byte, high byte 0xff)
	Dcs1(const uint16_t *rom, size_t rom_words);
	~Dcs1();

	static constexpr double ADSP_CLOCK = 10000000.0;

	void reset_w(int state);           // host-controlled reset line (0 = held in reset)
	void data_w(uint8_t data);         // host -> DCS command byte
	void advance(double adsp_cycles);  // run the DSP for this many ADSP cycles

	// PCM output: mono int16 blocks at 'rate' Hz, in emulated-time order
	std::function<void(const int16_t *, int, double)> on_audio;
	std::function<void(bool)> on_audio_enable;

	bool halted() const { return m_halted; }
	// diagnostics
	uint64_t stat_writes = 0, stat_overwrites = 0, stat_reads = 0;
	bool input_full() const { return !(m_latch_control & 0x800); }
	uint64_t total_cycles() const { return m_cyc + uint64_t(m_chunk - std::max(0, m_cpu->icount())); }

private:
	void dcs_reset();
	void boot();

	// bus
	static uint32_t pgm_read_thunk(void *c, uint32_t a) { return static_cast<Dcs1 *>(c)->pgm_read(a); }
	static void     pgm_write_thunk(void *c, uint32_t a, uint32_t d) { static_cast<Dcs1 *>(c)->pgm_write(a, d); }
	static uint16_t data_read_thunk(void *c, uint32_t a) { return static_cast<Dcs1 *>(c)->data_read(a); }
	static void     data_write_thunk(void *c, uint32_t a, uint16_t d) { static_cast<Dcs1 *>(c)->data_write(a, d); }
	uint32_t pgm_read(uint32_t a);
	void     pgm_write(uint32_t a, uint32_t d);
	uint16_t data_read(uint32_t a);
	void     data_write(uint32_t a, uint16_t d);
	void     update_bank_pages();

	// latches
	uint16_t input_latch_r();
	void     input_latch_ack();
	void     output_latch_w(uint16_t d);

	// control registers 0x3fe0..0x3fff
	uint16_t adsp_control_r(uint32_t off);
	void     adsp_control_w(uint32_t off, uint16_t data);

	// timers
	void update_timer_count();
	void reset_timer();
	void timer_enable(int state);
	void internal_timer_fire();
	void sound_tx(int port, uint32_t data);
	void recompute_sample_rate();
	void dcs_irq();

	std::unique_ptr<adsp21xx_device::bus_t> m_bus;
	std::unique_ptr<adsp21xx_device> m_cpu;

	const uint16_t *m_rom;
	size_t m_rom_words;
	size_t m_banks;

	uint32_t m_internal_program_ram[0x400] = {};
	uint32_t m_external_program_ram[0x800] = {};
	uint16_t m_iram[0x200] = {};
	uint16_t m_control_regs[32] = {};

	// state
	double   m_carry = 0;
	bool     m_halted = true;
	uint32_t m_sounddata_bank = 0;
	uint16_t m_latch_control = 0;
	uint16_t m_input_data = 0, m_output_data = 0;
	uint16_t m_progflags = 0;

	// timers (absolute ADSP cycle counts)
	uint64_t m_cyc = 0;
	int      m_chunk = 0;
	bool     m_timer_enable = false, m_timer_ignore = false;
	uint64_t m_timer_start_cycles = 0;
	uint32_t m_timer_start_count = 0, m_timer_scale = 1, m_timer_period = 0;
	uint64_t m_timers_fired = 0;
	bool     m_int_active = false;
	int64_t  m_int_next = 0;

	bool     m_reg_active = false;
	double   m_reg_next = 0, m_reg_period = 0;
	int      m_ireg = 0, m_incs = 0, m_size = 0;
	uint32_t m_ireg_base = 0;
	double   m_sample_rate = 31250.0;
	bool     m_dac_enabled = false;
};
