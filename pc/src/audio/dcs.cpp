// DCS 2K audio board -- see dcs.h
#include "dcs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr uint16_t LCTRL_OUTPUT_EMPTY = 0x400;
constexpr uint16_t LCTRL_INPUT_EMPTY  = 0x800;

enum
{
	IDMA_CONTROL_REG = 0,
	PROG_FLAG_DATA_REG = 5,
	PROG_FLAG_CONTROL_REG = 6,
	S1_AUTOBUF_REG = 15,
	S1_RFSDIV_REG,
	S1_SCLKDIV_REG,
	S1_CONTROL_REG,
	S0_AUTOBUF_REG,
	S0_RFSDIV_REG,
	S0_SCLKDIV_REG,
	S0_CONTROL_REG,
	S0_MCTXLO_REG,
	S0_MCTXHI_REG,
	S0_MCRXLO_REG,
	S0_MCRXHI_REG,
	TIMER_SCALE_REG,
	TIMER_COUNT_REG,
	TIMER_PERIOD_REG,
	WAITSTATES_REG,
	SYSCONTROL_REG
};
} // namespace

Dcs1::Dcs1(const uint16_t *rom, size_t rom_words) : m_rom(rom), m_rom_words(rom_words), m_banks(rom_words / 0x1000)
{
	m_bus = std::make_unique<adsp21xx_device::bus_t>();
	m_bus->ctx = this;
	m_bus->pgm_read = &Dcs1::pgm_read_thunk;
	m_bus->pgm_write = &Dcs1::pgm_write_thunk;
	m_bus->data_read = &Dcs1::data_read_thunk;
	m_bus->data_write = &Dcs1::data_write_thunk;

	// program: internal RAM 0x0000-0x03ff, external RAM (2K words) mirrored at 0x0800/0x1000/0x1800
	for (int p = 0; p < 4; p++)
		m_bus->ppage[p] = &m_internal_program_ram[p * 256];
	for (int p = 8; p < 32; p++)
		m_bus->ppage[p] = &m_external_program_ram[((p - 8) % 8) * 256];

	// data: internal RAM 0x3800-0x39ff
	for (int p = 0; p < 2; p++)
	{
		m_bus->drpage[0x38 + p] = &m_iram[p * 256];
		m_bus->dwpage[0x38 + p] = &m_iram[p * 256];
	}

	m_cpu = std::make_unique<adsp21xx_device>(m_bus.get(), adsp21xx_device::CHIP_TYPE_ADSP2105);
	m_cpu->on_sport_tx = [this](int port, uint32_t d) { sound_tx(port, d); };
	m_cpu->on_timer_enable = [this](int s) { timer_enable(s); };

	update_bank_pages();
}

Dcs1::~Dcs1() = default;

// ---- bus ------------------------------------------------------------------------------

uint32_t Dcs1::pgm_read(uint32_t) { return 0; }
void Dcs1::pgm_write(uint32_t, uint32_t) {}

void Dcs1::update_bank_pages()
{
	size_t base = size_t(m_sounddata_bank % m_banks) * 0x1000;
	for (int p = 0; p < 16; p++)
		m_bus->drpage[0x20 + p] = const_cast<uint16_t *>(m_rom) + base + p * 256;
}

uint16_t Dcs1::data_read(uint32_t a)
{
	a &= 0x3fff;
	if (a < 0x2000)                       // 0x0000-0x07ff mirrored (0x1800): external program RAM, upper 16 bits
		return (a < 0x800 || true) ? uint16_t(m_external_program_ram[a & 0x7ff] >> 8) : 0;
	if (a >= 0x3400 && a < 0x3800)
		return input_latch_r();
	if (a >= 0x3fe0)
		return adsp_control_r(a - 0x3fe0);
	return 0;
}

void Dcs1::data_write(uint32_t a, uint16_t d)
{
	a &= 0x3fff;
	if (a < 0x2000)
	{
		uint32_t &w = m_external_program_ram[a & 0x7ff];
		w = (uint32_t(d) << 8) | (w & 0xff);
	}
	else if (a >= 0x3000 && a < 0x3400)
	{
		m_sounddata_bank = d & 0x7ff;
		update_bank_pages();
	}
	else if (a >= 0x3400 && a < 0x3800)
		output_latch_w(d);
	else if (a >= 0x3fe0)
		adsp_control_w(a - 0x3fe0, d);
}

// ---- latches --------------------------------------------------------------------------

uint16_t Dcs1::input_latch_r()
{
	input_latch_ack();      // non-RAM boards ack automatically
	return m_input_data;
}

void Dcs1::input_latch_ack()
{
	m_latch_control |= LCTRL_INPUT_EMPTY;
	m_cpu->set_input(ADSP2105_IRQ2, CLEAR_LINE);
}

void Dcs1::output_latch_w(uint16_t d)
{
	m_latch_control &= ~LCTRL_OUTPUT_EMPTY;
	m_output_data = d >> 8;
}

void Dcs1::data_w(uint8_t data)
{
	if (m_halted)
		return;
	m_cpu->set_input(ADSP2105_IRQ2, ASSERT_LINE);
	m_latch_control &= ~LCTRL_INPUT_EMPTY;
	m_input_data = data;
}

// ---- reset / boot -----------------------------------------------------------------------

void Dcs1::boot()
{
	const uint16_t *base = &m_rom[(size_t(m_sounddata_bank) * 0x1000) % m_rom_words];
	uint8_t buffer[0x1000];
	for (int i = 0; i < 0x1000; i++)
		buffer[i] = uint8_t(base[i]);
	m_cpu->load_boot_data(buffer, m_internal_program_ram);
}

void Dcs1::dcs_reset()
{
	m_sounddata_bank = 0;
	update_bank_pages();
	m_ireg = m_incs = m_size = 0;
	std::memset(m_control_regs, 0, sizeof(m_control_regs));
	m_cpu->set_input(ADSP2105_IRQ0, CLEAR_LINE);
	m_cpu->set_input(ADSP2105_IRQ1, CLEAR_LINE);
	m_cpu->set_input(ADSP2105_IRQ2, CLEAR_LINE);
	m_latch_control |= LCTRL_INPUT_EMPTY | LCTRL_OUTPUT_EMPTY;
	boot();
	m_timer_ignore = false;
	m_timer_enable = false;
	m_timer_scale = 1;
	m_int_active = false;
	m_reg_active = false;
	if (m_dac_enabled) { m_dac_enabled = false; if (on_audio_enable) on_audio_enable(false); }
	m_cpu->reset();
}

void Dcs1::reset_w(int state)
{
	if (!state)
	{
		dcs_reset();
		m_halted = true;
	}
	else
		m_halted = false;
}

// ---- execution --------------------------------------------------------------------------

void Dcs1::advance(double adsp_cycles)
{
	if (m_halted)
		return;
	uint64_t target = m_cyc + uint64_t(std::max(0.0, adsp_cycles));
	while (m_cyc < target)
	{
		uint64_t next = target;
		if (m_int_active && m_int_next > 0 && uint64_t(m_int_next) < next) next = uint64_t(m_int_next);
		if (m_reg_active && uint64_t(m_reg_next) < next) next = uint64_t(m_reg_next);
		int len = int(next > m_cyc ? next - m_cyc : 1);
		m_chunk = len;
		int used = m_cpu->run(len);
		m_cyc += uint64_t(std::max(used, 1));
		m_chunk = 0;

		if (m_int_active && int64_t(m_cyc) >= m_int_next)
			internal_timer_fire();
		if (m_reg_active)
		{
			int guard = 0;
			while (m_reg_active && double(m_cyc) >= m_reg_next && guard++ < 8)
			{
				m_reg_next += m_reg_period;
				dcs_irq();
			}
		}
	}
}

// ---- ADSP internal timer ---------------------------------------------------------------

void Dcs1::update_timer_count()
{
	if (!m_timer_enable)
		return;
	uint64_t elapsed_cycles = total_cycles() - m_timer_start_cycles;
	uint64_t elapsed_clocks = elapsed_cycles / m_timer_scale;
	if (elapsed_clocks < m_timer_start_count + 1)
	{
		m_timer_start_count -= uint32_t(elapsed_clocks);
		m_control_regs[TIMER_COUNT_REG - 0] = uint16_t(m_timer_start_count);
	}
	else
	{
		elapsed_clocks -= m_timer_start_count + 1;
		uint64_t periods_since_start = elapsed_clocks / (m_timer_period + 1);
		elapsed_clocks -= periods_since_start * (m_timer_period + 1);
		m_timer_start_count = uint32_t(m_timer_period - elapsed_clocks);
		m_control_regs[TIMER_COUNT_REG] = uint16_t(m_timer_start_count);
	}
}

void Dcs1::reset_timer()
{
	if (!m_timer_enable)
		return;
	m_timer_start_cycles = total_cycles();
	m_timers_fired = 0;

	if (!m_timer_ignore)
	{
		// if the interrupt routine is only the DRAM refresh stub, don't bother firing
		if (m_internal_program_ram[0x18] == 0x0c0030 && m_internal_program_ram[0x19] == 0x804828 &&
		    m_internal_program_ram[0x1a] == 0x904828 && m_internal_program_ram[0x1b] == 0x0c0020 &&
		    m_internal_program_ram[0x1c] == 0x0a001f)
			m_timer_ignore = true;
	}

	if (!m_timer_ignore)
	{
		m_int_active = true;
		m_int_next = int64_t(m_timer_start_cycles + uint64_t(m_timer_scale) * (m_timer_start_count + 1));
	}
	else
		m_int_active = false;
}

void Dcs1::timer_enable(int state)
{
	if (state)
	{
		m_timer_enable = true;
		reset_timer();
	}
	else
	{
		update_timer_count();
		m_timer_enable = false;
		m_int_active = false;
	}
}

void Dcs1::internal_timer_fire()
{
	m_timers_fired++;
	int64_t target = int64_t(m_timer_start_cycles + uint64_t(m_timer_scale) * (m_timer_start_count + 1 + m_timers_fired * uint64_t(m_timer_period + 1)));
	if (!m_timer_ignore && (m_timer_period > 10 || m_timer_scale > 1))
		m_int_next = std::max<int64_t>(target, int64_t(m_cyc) + 1);
	else
		m_int_active = false;
	m_cpu->set_input(ADSP2105_TIMER, ASSERT_LINE);
	m_cpu->set_input(ADSP2105_TIMER, CLEAR_LINE);
}

// ---- control registers ------------------------------------------------------------------

uint16_t Dcs1::adsp_control_r(uint32_t off)
{
	switch (off)
	{
	case PROG_FLAG_DATA_REG:
	{
		uint16_t r = (m_control_regs[PROG_FLAG_CONTROL_REG] & m_control_regs[PROG_FLAG_DATA_REG]) |
		             (m_progflags & ~m_control_regs[PROG_FLAG_CONTROL_REG]);
		m_progflags ^= 0x6;
		return r;
	}
	case IDMA_CONTROL_REG:
		return 0xffff;
	case TIMER_COUNT_REG:
		update_timer_count();
		return m_control_regs[off];
	default:
		return m_control_regs[off];
	}
}

void Dcs1::adsp_control_w(uint32_t off, uint16_t data)
{
	m_control_regs[off] = data;
	switch (off)
	{
	case SYSCONTROL_REG:
		if (data & 0x0200)
		{
			// software reboot: reset the DSP and boot again
			m_cpu->reset();
			boot();
			m_control_regs[SYSCONTROL_REG] = 0;
		}
		if ((data & 0x0800) == 0)
		{
			if (m_dac_enabled) { m_dac_enabled = false; if (on_audio_enable) on_audio_enable(false); }
			m_reg_active = false;
		}
		break;

	case S1_AUTOBUF_REG:
		if ((data & 0x0002) == 0)
		{
			if (m_dac_enabled) { m_dac_enabled = false; if (on_audio_enable) on_audio_enable(false); }
			m_reg_active = false;
		}
		break;

	case TIMER_SCALE_REG:
	{
		uint32_t s = (data & 0xff) + 1;
		if (s != m_timer_scale)
		{
			update_timer_count();
			m_timer_scale = s;
			reset_timer();
		}
		break;
	}
	case TIMER_COUNT_REG:
		m_timer_start_count = data;
		reset_timer();
		break;
	case TIMER_PERIOD_REG:
		if (data != m_timer_period)
		{
			update_timer_count();
			m_timer_period = data;
			reset_timer();
		}
		break;
	}
}

// ---- SPORT1 autobuffer -> PCM ----------------------------------------------------------------

void Dcs1::sound_tx(int port, uint32_t)
{
	if (port != 1)
		return;

	if ((m_control_regs[SYSCONTROL_REG] & 0x0800) && (m_control_regs[S1_AUTOBUF_REG] & 0x0002))
	{
		m_ireg = (m_control_regs[S1_AUTOBUF_REG] >> 9) & 7;
		int mreg = (m_control_regs[S1_AUTOBUF_REG] >> 7) & 3;
		mreg |= m_ireg & 0x04;
		int lreg = m_ireg;

		uint16_t source = uint16_t(m_cpu->ireg(m_ireg));
		m_incs = m_cpu->mreg(mreg);
		m_size = int(m_cpu->lreg(lreg));

		source &= ~0xf;
		m_cpu->set_ireg(m_ireg, source);
		m_ireg_base = source;
		recompute_sample_rate();
		return;
	}

	if (m_dac_enabled) { m_dac_enabled = false; if (on_audio_enable) on_audio_enable(false); }
	m_reg_active = false;
}

void Dcs1::recompute_sample_rate()
{
	double sample_period;   // seconds per output sample
	if (m_control_regs[S1_CONTROL_REG] & 0x4000)
		sample_period = (1.0 / ADSP_CLOCK) * (2.0 * (m_control_regs[S1_SCLKDIV_REG] + 1)) * 16.0;
	else
		sample_period = 1.0 / 31250.0;

	m_sample_rate = 1.0 / sample_period;
	if (!m_dac_enabled)
	{
		m_dac_enabled = true;
		if (on_audio_enable) on_audio_enable(true);
	}

	if (m_incs)
	{
		double period = (sample_period * m_size) / (2.0 * 1 * m_incs);
		m_reg_period = period * ADSP_CLOCK;
		m_reg_next = double(total_cycles()) + m_reg_period;
		m_reg_active = m_reg_period > 0;
	}
}

void Dcs1::dcs_irq()
{
	int reg = int(m_cpu->ireg(m_ireg));
	int count = m_size / (2 * (m_incs ? m_incs : 1));
	int16_t buffer[0x800];
	count = std::min(count, 0x800);
	for (int i = 0; i < count; i++)
	{
		buffer[i] = int16_t(data_read(uint32_t(reg)));
		reg += m_incs;
	}
	if (on_audio && count > 0)
		on_audio(buffer, count, m_sample_rate);

	m_ireg_base = m_cpu->get_ibase(m_ireg);
	if (reg >= int(m_ireg_base) + m_size)
	{
		reg = int(m_ireg_base);
		m_cpu->set_input(ADSP2105_IRQ1, ASSERT_LINE);
		m_cpu->set_input(ADSP2105_IRQ1, CLEAR_LINE);
	}
	m_cpu->set_ireg(m_ireg, uint32_t(reg));
}
