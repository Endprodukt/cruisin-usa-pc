// Midway V-Unit (Cruis'n USA) machine -- see midvunit.h
#include "midvunit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <cstdlib>

#include "../../third_party/miniz/miniz.h"

namespace {

constexpr uint64_t CPU_HZ = 25000000;          // 50 MHz / 2 clocks per instruction cycle
constexpr double   VIDEO_PIXCLK = 33333333.0 / 2.0;

inline uint8_t pal5(uint32_t v) { v &= 31; return uint8_t((v << 3) | (v >> 2)); }

// ---- ROM loading -----------------------------------------------------------------------

struct ZipFile
{
	mz_zip_archive z{};
	bool open = false;
	~ZipFile() { if (open) mz_zip_reader_end(&z); }

	bool init(const std::string &path)
	{
		if (!mz_zip_reader_init_file(&z, path.c_str(), 0))
			return false;
		open = true;
		return true;
	}

	// find an entry: prefix must match exactly (directory), name must end with 'suffix'
	int find(const std::string &dir, const std::string &suffix) const
	{
		int n = int(mz_zip_reader_get_num_files(const_cast<mz_zip_archive *>(&z)));
		for (int i = 0; i < n; i++)
		{
			char name[512];
			mz_zip_reader_get_filename(const_cast<mz_zip_archive *>(&z), i, name, sizeof(name));
			std::string s(name);
			std::string d;
			auto slash = s.rfind('/');
			if (slash != std::string::npos)
				d = s.substr(0, slash + 1);
			if (d != dir)
				continue;
			if (s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
				return i;
		}
		return -1;
	}

	bool extract(int idx, std::vector<uint8_t> &out)
	{
		mz_zip_archive_file_stat st;
		if (!mz_zip_reader_file_stat(&z, idx, &st))
			return false;
		out.resize(size_t(st.m_uncomp_size));
		return mz_zip_reader_extract_to_mem(&z, idx, out.data(), out.size(), 0) != 0;
	}
};

const std::map<std::string, std::string> &version_dirs()
{
	static const std::map<std::string, std::string> m = {
		{"4.5", ""}, {"4.4", "crusnusa44/"}, {"4.1", "crusnusa41/"}, {"4.0", "crusnusa40/"},
		{"2.1", "crusnusa21/"}, {"2.0", "crusnusa20/"}, {"1.1", "crusnusa11/"},
	};
	return m;
}

} // namespace

// ---------------------------------------------------------------------------------------

MidVUnit::MidVUnit()
{
	m_bus = std::make_unique<tms320c3x_device::bus_t>();
	m_bus->ctx = this;
	m_bus->slow_read = &MidVUnit::read_thunk;
	m_bus->slow_write = &MidVUnit::write_thunk;

	m_ram0.assign(0x20000, 0);
	m_ram1.assign(0x20000, 0);
	m_rom.assign(0x400000, 0);
	m_nvram.assign(0x2000, 0xffffffffu);   // NVRAM default: all ones
	m_paletteram.assign(0x8000, 0);
	m_palette_rgb.assign(0x8000, 0);
	m_videoram.assign(0x80000, 0);
	m_textureram.assign(0x400000, 0);
	m_frame.assign(FRAME_STRIDE * 512, 0);
	sound_rom.assign(0x1000000, 0xff);

	// fast paths: RAM at 0x000000 and 0x400000, ROM at 0xc00000
	for (int p = 0; p < 0x20000 / tms320c3x_device::PAGE_WORDS; p++)
	{
		m_bus->rpage[p] = &m_ram0[p * tms320c3x_device::PAGE_WORDS];
		m_bus->wpage[p] = &m_ram0[p * tms320c3x_device::PAGE_WORDS];
		int q = (0x400000 >> tms320c3x_device::PAGE_SHIFT) + p;
		m_bus->rpage[q] = &m_ram1[p * tms320c3x_device::PAGE_WORDS];
		m_bus->wpage[q] = &m_ram1[p * tms320c3x_device::PAGE_WORDS];
	}
	for (int p = 0; p < 0x400000 / tms320c3x_device::PAGE_WORDS; p++)
		m_bus->rpage[(0xc00000 >> tms320c3x_device::PAGE_SHIFT) + p] = &m_rom[p * tms320c3x_device::PAGE_WORDS];

	m_cpu = std::make_unique<tms320c3x_device>(m_bus.get(), tms320c3x_device::CHIP_TYPE_TMS320C31, 2);
	m_cpu->on_xf0 = [](int) {};
	m_cpu->on_xf1 = [](int) {};
}

MidVUnit::~MidVUnit() = default;

bool MidVUnit::load_roms(const std::string &zip_path, const std::string &version, std::string &err)
{
	ZipFile zf;
	if (!zf.init(zip_path))
	{
		err = "cannot open ROM zip: " + zip_path;
		return false;
	}

	auto vd = version_dirs().find(version);
	if (vd == version_dirs().end())
	{
		err = "unknown version '" + version + "'";
		return false;
	}
	const std::string &dir = vd->second;

	std::fill(m_rom.begin(), m_rom.end(), 0xffffffffu);

	auto load_game = [&](const std::string &d, int unit, uint32_t byteoff, int lane) -> bool {
		char suffix[16];
		std::snprintf(suffix, sizeof(suffix), ".u%d", unit);
		int idx = zf.find(d, suffix);
		if (idx < 0)
		{
			err = "missing ROM u" + std::to_string(unit) + " in '" + d + "'";
			return false;
		}
		std::vector<uint8_t> data;
		if (!zf.extract(idx, data))
		{
			err = "extract failed for u" + std::to_string(unit);
			return false;
		}
		// ROM_LOAD32_BYTE: file byte i goes to byte 'lane' of 32-bit word (byteoff/4 + i)
		uint8_t *rom8 = reinterpret_cast<uint8_t *>(m_rom.data());
		for (size_t i = 0; i < data.size(); i++)
		{
			size_t o = byteoff + i * 4 + lane;
			if (o < m_rom.size() * 4)
				rom8[o] = data[i];
		}
		return true;
	};

	// u10..u13 : version specific, at 0x000000
	for (int u = 10; u <= 13; u++)
		if (!load_game(dir, u, 0x000000, u - 10))
			return false;
	// u14..u29 : shared graphics/data, root of the zip, four groups of four
	for (int g = 0; g < 4; g++)
		for (int l = 0; l < 4; l++)
			if (!load_game("", 14 + g * 4 + l, 0x200000 + g * 0x200000, l))
				return false;

	// sound ROMs u2..u9 : ROM_LOAD16_BYTE at i * 0x200000 (low byte lane)
	std::fill(sound_rom.begin(), sound_rom.end(), 0xff);
	for (int i = 0; i < 8; i++)
	{
		char suffix[16];
		std::snprintf(suffix, sizeof(suffix), ".u%d", 2 + i);
		int idx = zf.find("", suffix);
		if (idx < 0)
		{
			err = "missing sound ROM u" + std::to_string(2 + i);
			return false;
		}
		std::vector<uint8_t> data;
		if (!zf.extract(idx, data))
		{
			err = "extract failed for sound ROM";
			return false;
		}
		for (size_t k = 0; k < data.size(); k++)
			sound_rom[size_t(i) * 0x200000 + k * 2] = data[k];
	}

	m_dcs = std::make_unique<Dcs1>(reinterpret_cast<const uint16_t *>(sound_rom.data()), sound_rom.size() / 2);
	m_dcs->on_audio = [this](const int16_t *b, int n, double r) { if (on_audio) on_audio(b, n, r); };
	m_dcs->on_audio_enable = [this](bool e) { if (on_audio_enable) on_audio_enable(e); };
	on_sound_data = [this](uint8_t d) { m_dcs->data_w(d); };
	on_dcs_reset = [this](int st) { m_dcs->reset_w(st); };
	return true;
}

void MidVUnit::reset()
{
	// boot: the first 128K words of the ROM are copied to RAM at 0
	std::copy(m_rom.begin(), m_rom.begin() + 0x20000, m_ram0.begin());

	m_control_data = 0;
	m_cmos_protected = 0;
	m_page_control = 0;
	m_dma_data_index = 0;
	std::memset(m_video_regs, 0, sizeof(m_video_regs));
	m_irq_line = -1;
	m_cycles_total = 0;
	m_cycle_frac = 0;
	m_adc_event = ~0ull;
	m_adc_intr = false;
	m_timer_start[0] = m_timer_start[1] = 0;
	m_timer_rate = 10000000.0;
	m_htotal = 666; m_vtotal = 432; m_vis_w = 512; m_vis_h = 400;
	m_refresh_hz = VIDEO_PIXCLK / (m_htotal * m_vtotal);
	m_cpu->reset();
	if (m_dcs) { m_dcs->reset_w(0); m_dcs->reset_w(1); }
	m_wheel_board_output = 0;
	m_wheel_board_last = 0;
	galil_set_input(":");
}

int MidVUnit::run_cpu(int cycles)
{
	int used = m_cpu->run(cycles);
	if (m_dcs)
		m_dcs->advance(used * (Dcs1::ADSP_CLOCK / double(CPU_HZ)));
	return used;
}

uint64_t MidVUnit::now_cycles() const
{
	return m_cycles_total + uint64_t(m_slice_len - std::max(0, m_cpu->icount()));
}

// ---------------------------------------------------------------------------------------
// bus
// ---------------------------------------------------------------------------------------

uint32_t MidVUnit::bus_read(offs_t addr)
{
	if (addr >= 0x900000 && addr < 0x980000)
		return m_videoram[addr - 0x900000];
	if (addr >= 0xa00000 && addr < 0xc00000)
	{
		size_t o = size_t(addr - 0xa00000) * 2;
		return (uint32_t(m_textureram[o + 1]) << 8) | m_textureram[o];
	}
	if (addr >= 0x9c0000 && addr < 0x9c2000)
		return m_nvram[addr - 0x9c0000];
	if (addr >= 0x9e0000 && addr < 0x9e8000)
		return m_paletteram[addr - 0x9e0000];
	if (addr >= 0x808000 && addr < 0x808080)
	{
		uint32_t off = addr - 0x808000;
		if (off == 0x24 || off == 0x34)
		{
			int which = (off >> 4) & 1;
			double elapsed = double(now_cycles() - m_timer_start[which]) / double(CPU_HZ);
			return uint32_t(int32_t(elapsed * m_timer_rate));
		}
		return m_ctrl[off];
	}
	if (addr >= 0x809800 && addr < 0x80a000)
		return m_iram[addr - 0x809800];
	return io_read(addr);
}

void MidVUnit::bus_write(offs_t addr, uint32_t data)
{
	if (addr == 0x600000)
	{
		dma_queue_write(data);
		return;
	}
	if (addr >= 0x900000 && addr < 0x980000)
	{
		m_videoram[addr - 0x900000] = uint16_t(data);
		return;
	}
	if (addr >= 0xa00000 && addr < 0xc00000)
	{
		size_t o = size_t(addr - 0xa00000) * 2;
		m_textureram[o] = uint8_t(data);
		m_textureram[o + 1] = uint8_t(data >> 8);
		return;
	}
	if (addr >= 0x9c0000 && addr < 0x9c2000)
	{
		if (!m_cmos_protected)
			m_nvram[addr - 0x9c0000] = data;
		return;
	}
	if (addr >= 0x9e0000 && addr < 0x9e8000)
	{
		m_paletteram[addr - 0x9e0000] = data;
		m_palette_rgb[addr - 0x9e0000] =
			(uint32_t(pal5(data >> 10)) << 16) | (uint32_t(pal5(data >> 5)) << 8) | pal5(data);
		return;
	}
	if (addr >= 0x808000 && addr < 0x808080)
	{
		uint32_t off = addr - 0x808000;
		m_ctrl[off] = data;
		if (off == 0x20 || off == 0x30)
		{
			int which = (off >> 4) & 1;
			if (data & 0x40)
				m_timer_start[which] = now_cycles();
			m_timer_rate = (data & 0x200) ? double(CPU_HZ) : 10000000.0;
		}
		return;
	}
	if (addr >= 0x809800 && addr < 0x80a000)
	{
		m_iram[addr - 0x809800] = data;
		return;
	}
	io_write(addr, data);
}

uint32_t MidVUnit::io_read(offs_t addr)
{
	switch (addr)
	{
	case 0x980000: return 0;                        // dma queue entries: always 0
	case 0x980020: return uint32_t(m_vpos);         // scanline
	case 0x980040: return m_page_control;
	case 0x980083: dma_trigger(); return 0;
	case 0x990000: return 4;                        // INTCS
	case 0x991030: return inputs.in1;
	case 0x991060:
	{
		uint16_t val = inputs.in0;
		// H-pattern shifter emulation: gear buttons become mutually exclusive positions
		static const uint16_t gearbits[5] = {0, in0bit::GEAR1, in0bit::GEAR2, in0bit::GEAR3, in0bit::GEAR4};
		val = (val | 0x3c00) ^ gearbits[std::min(std::max(inputs.gear, 0), 4)];
		return (uint32_t(val) << 16) | val;
	}
	case 0x992000: return inputs.dsw;
	case 0x993000: return (m_control_data & 0x40) ? 0xffffffffu : (uint32_t(adc_read()) << m_adc_shift);
	case 0x995000: return uint32_t(m_wheel_board_output) << 8;
	case 0x997000:
	{
		uint16_t data = 0;
		uint16_t mask = 0;
		if (m_comm_flags & 0x20) mask |= (m_comm_data >> 4) & 0xf00;
		if (m_comm_flags & 0x40) mask |= 0xff;
		data = m_comm_data & mask;
		if (m_comm_flags & 0x20) data &= ~((m_comm_data >> 4) & 0xf00);
		if (m_comm_flags & 0x40) data &= ~0xff;
		return uint32_t(data) << 16;
	}
	default: return 0;
	}
}

void MidVUnit::io_write(offs_t addr, uint32_t data)
{
	if (addr >= 0x980020 && addr < 0x98002c)
	{
		video_control_write(int(addr - 0x980020), data);
		return;
	}
	switch (addr)
	{
	case 0x980040: page_control_write(data); break;
	case 0x993000:
		if (!(m_control_data & 0x20))
			adc_write(uint8_t(data >> m_adc_shift));
		break;
	case 0x994000:
		m_control_data = uint16_t(data);
		// bit 7 LED, bit 3 watchdog, bit 1 DCS reset (handled by audio layer)
		if (on_dcs_reset) on_dcs_reset((m_control_data >> 1) & 1);
		break;
	case 0x995000: wheel_board_write(data); break;
	case 0x995020: m_cmos_protected = ((data & 0xc00) != 0xc00); break;
	case 0x997000: m_comm_data = uint16_t(data >> 16); break;
	case 0x997001: m_comm_flags = uint8_t((data >> 24) & 0xe0); break;
	case 0x9a0000:
		if (on_sound_data) on_sound_data(uint8_t(data));
		break;
	default: break;
	}
}

// ---------------------------------------------------------------------------------------
// ADC0844
// ---------------------------------------------------------------------------------------

void MidVUnit::adc_write(uint8_t v)
{
	if (m_adc_intr) { m_adc_intr = false; m_cpu->set_input(3, CLEAR_LINE); }
	m_adc_channel = v & 0x0f;
	m_adc_event = now_cycles() + 1000;     // 40 us
}

uint8_t MidVUnit::adc_read()
{
	if (m_adc_intr) { m_adc_intr = false; m_cpu->set_input(3, CLEAR_LINE); }
	return m_adc_result;
}

void MidVUnit::adc_conversion_done()
{
	auto clamp = [](int v) { return uint8_t(std::min(255, std::max(0, v))); };
	int ch1 = inputs.wheel, ch2 = inputs.accel, ch3 = inputs.brake, ch4 = 0xff;
	switch (m_adc_channel)
	{
	case 0x00: case 0x08: m_adc_result = clamp(0xff - (ch2 - ch1)); break;
	case 0x01: case 0x09: m_adc_result = clamp(0xff - (ch1 - ch2)); break;
	case 0x02: case 0x0a: m_adc_result = clamp(0xff - (ch4 - ch3)); break;
	case 0x03: case 0x0b: m_adc_result = clamp(0xff - (ch3 - ch4)); break;
	case 0x04: m_adc_result = uint8_t(ch1); break;
	case 0x05: m_adc_result = uint8_t(ch2); break;
	case 0x06: m_adc_result = uint8_t(ch3); break;
	case 0x07: m_adc_result = uint8_t(ch4); break;
	case 0x0c: m_adc_result = clamp(0xff - (ch4 - ch1)); break;
	case 0x0d: m_adc_result = clamp(0xff - (ch4 - ch2)); break;
	case 0x0e: m_adc_result = clamp(0xff - (ch4 - ch3)); break;
	case 0x0f: m_adc_result = 0x00; break;
	}
	m_adc_intr = true;
	m_cpu->set_input(3, ASSERT_LINE);
}

// ---------------------------------------------------------------------------------------
// wheel board / Galil motion controller stub
// ---------------------------------------------------------------------------------------

void MidVUnit::galil_set_input(const char *s)
{
	m_galil_input = s;
	m_galil_input_index = 0;
	m_galil_input_length = uint8_t(std::strlen(s));
}

void MidVUnit::wheel_board_write(uint32_t data)
{
	if (BIT(data, 11) && !BIT(m_wheel_board_last, 11))
	{
		m_wheel_board_u8_latch = 0;
		m_wheel_board_u8_latch |= BIT(data, 0) << 6;
		m_wheel_board_u8_latch |= BIT(data, 1) << 5;
		m_wheel_board_u8_latch |= BIT(data, 2) << 4;
		m_wheel_board_u8_latch |= BIT(data, 3) << 3;
	}

	if (!BIT(data, 9))
	{
		// U13 74HC245 (DCS): ignored
	}
	else if (!BIT(data, 10))
	{
		uint8_t arg = data & 0xff;
		uint8_t wa = BIT(m_wheel_board_u8_latch, 6) | (BIT(m_wheel_board_u8_latch, 5) << 1) | (BIT(m_wheel_board_u8_latch, 4) << 2);
		if (BIT(m_wheel_board_u8_latch, 3))
		{
			switch (wa)
			{
			case 0:
				m_wheel_board_output = m_galil_input[m_galil_input_index++];
				break;
			case 1:
				if (arg != 0xd)
				{
					m_galil_output[m_galil_output_index] = char(arg);
					if (m_galil_output_index < 450)
						m_galil_output_index++;
				}
				else
				{
					if (std::strstr(m_galil_output, "MG \"V\" IBO {$2.0}")) galil_set_input("V$00");
					else if (std::strstr(m_galil_output, "MG \"X\", _TSX {$2.0}")) galil_set_input("X$00");
					else if (std::strstr(m_galil_output, "MG \"Y\", _TSY {$2.0}")) galil_set_input("Y$00");
					else if (std::strstr(m_galil_output, "MG \"Z\", _TSZ {$2.0}")) galil_set_input("Z$00");
					else galil_set_input(":");
					std::memset(m_galil_output, 0, m_galil_output_index);
					m_galil_output_index = 0;
				}
				break;
			case 2:
				m_wheel_board_output = (m_galil_input_index < m_galil_input_length) ? 0x80 : 0x0;
				break;
			case 3: break;
			case 4:
				galil_set_input(":");
				m_galil_output_index = 0;
				std::memset(m_galil_output, 0, 450);
				break;
			}
		}
		else
		{
			switch (wa)
			{
			case 4: wheel_motor = arg; break;                                    // WHLCTLZ
			case 5: for (int b = 0; b < 8; b++) lamps[b] = BIT(data, b); break;  // DRVCTLZ
			default: break;
			}
		}
	}
	m_wheel_board_last = data;
}

// ---------------------------------------------------------------------------------------
// video
// ---------------------------------------------------------------------------------------

void MidVUnit::dma_queue_write(uint32_t data)
{
	if (m_dma_data_index < 16)
		m_dma_data[m_dma_data_index++] = uint16_t(data);
}

void MidVUnit::dma_trigger()
{
	VQuad q;
	std::memcpy(q.dma, m_dma_data, sizeof(q.dma));
	q.page = (m_page_control & 4) ? 1 : 0;
	if (const char *qd = std::getenv("QDUMP"))
		if (m_frame_count == uint64_t(std::atoi(qd)))
		{
			for (int i = 0; i < 16; i++)
				std::fprintf(stderr, "%04X ", q.dma[i]);
			std::fprintf(stderr, "\n");
		}
	draw_quad(q);
	quads_last_frame++;
	m_dma_data_index = 0;
}

void MidVUnit::configure_screen()
{
	int r2 = m_video_regs[2], r5 = m_video_regs[5], r6 = m_video_regs[6];
	int r7 = m_video_regs[7], r10 = m_video_regs[10], r11 = m_video_regs[11];
	if (r6 == 0 || r11 == 0)
		return;
	auto pmod = [](int a, int n) { return ((a % n) + n) % n; };
	int max_x = pmod(r6 + r2 - r5, r6);
	int max_y = pmod(r11 + r7 - r10, r11);
	m_htotal = r6;
	m_vtotal = r11;
	m_vis_w = std::min(max_x + 1, 512);
	m_vis_h = std::min(max_y + 1, 512);
	m_refresh_hz = VIDEO_PIXCLK / (double(m_htotal) * double(m_vtotal));
}

void MidVUnit::video_control_write(int reg, uint32_t data)
{
	uint16_t old = m_video_regs[reg];
	m_video_regs[reg] = uint16_t(data);
	if (reg == 0)
		m_irq_line = int(data & 0x1ff) + 1;
	if (old != m_video_regs[reg] && m_video_regs[6] != 0 && m_video_regs[11] != 0)
		configure_screen();
}

// copy rows [from,to] of the given page into the frame (as palette indices, high bit marks "index")
void MidVUnit::update_screen_rows(int from, int to, int page)
{
	from = std::max(from, 0);
	to = std::min(to, m_vis_h - 1);
	const uint16_t *base = &m_videoram[page ? 0x40000 : 0];
	for (int y = from; y <= to; y++)
	{
		uint32_t *dst = &m_frame[size_t(y) * FRAME_STRIDE];
		const uint16_t *src = base + size_t(y) * 512;
		for (int x = 0; x < m_vis_w; x++)
			dst[x] = src[x] & 0x7fff;
	}
	m_video_changed = true;
}

void MidVUnit::page_control_write(uint32_t data)
{
	if ((m_page_control ^ data) & 1)
	{
		// the visible page flips: everything up to the current beam position was drawn from the old page
		int upto = m_vpos - 1;
		if (upto >= m_partial_next_row)
		{
			update_screen_rows(m_partial_next_row, upto, m_page_control & 1);
			m_partial_next_row = std::min(upto + 1, m_vis_h);
		}
	}
	m_page_control = uint16_t(data);
}

// ---- reference software rasterizer (bit-compatible with MAME's poly_manager usage) --------

namespace {

struct Vert { float x, y; float p[2]; };

inline int32_t round_coordinate(float value)
{
	float ipart = std::floor(value);
	float fpart = value - ipart;
	return int32_t(ipart) + ((fpart > 0.5f) ? 1 : 0);
}

struct Extent { int32_t startx, stopx; float pstart[2]; float dpdx[2]; };

template <int PARAMS, typename Fn>
void raster_polygon(const Vert *v, int clip_r, int clip_b, Fn &&scan)
{
	constexpr int NV = 4;
	float minx = v[0].x, maxx = v[0].x;
	int minv = 0, maxv = 0;
	for (int i = 1; i < NV; i++)
	{
		if (v[i].y < v[minv].y) minv = i;
		else if (v[i].y > v[maxv].y) maxv = i;
		minx = std::min(minx, v[i].x);
		maxx = std::max(maxx, v[i].x);
	}
	int32_t miny = round_coordinate(v[minv].y);
	int32_t maxy = round_coordinate(v[maxv].y);
	int32_t minyclip = std::max(miny, 0);
	int32_t maxyclip = std::min(maxy, clip_b + 1);
	if (maxyclip - minyclip <= 0)
		return;

	struct Edge { const Vert *v1, *v2; float dxdy; float dpdy[2]; };
	Edge fedge[NV - 1] = {}, bedge[NV - 1] = {};

	Edge *ep = &fedge[0];
	for (int cur = minv; cur != maxv; cur = (cur == NV - 1) ? 0 : cur + 1)
	{
		ep->v1 = &v[cur];
		ep->v2 = &v[(cur == NV - 1) ? 0 : cur + 1];
		if (ep->v1->y == ep->v2->y)
			continue;
		float ooy = 1.0f / (ep->v2->y - ep->v1->y);
		ep->dxdy = (ep->v2->x - ep->v1->x) * ooy;
		for (int k = 0; k < PARAMS; k++)
			ep->dpdy[k] = (ep->v2->p[k] - ep->v1->p[k]) * ooy;
		++ep;
	}
	ep = &bedge[0];
	for (int cur = minv; cur != maxv; cur = (cur == 0) ? NV - 1 : cur - 1)
	{
		ep->v1 = &v[cur];
		ep->v2 = &v[(cur == 0) ? NV - 1 : cur - 1];
		if (ep->v1->y == ep->v2->y)
			continue;
		float ooy = 1.0f / (ep->v2->y - ep->v1->y);
		ep->dxdy = (ep->v2->x - ep->v1->x) * ooy;
		for (int k = 0; k < PARAMS; k++)
			ep->dpdy[k] = (ep->v2->p[k] - ep->v1->p[k]) * ooy;
		++ep;
	}

	const Edge *ledge, *redge;
	if ((fedge[0].v1 == bedge[0].v1 && fedge[0].dxdy < bedge[0].dxdy) ||
	    (fedge[0].v1 != bedge[0].v1 && fedge[0].v1->x < bedge[0].v1->x))
	{
		ledge = fedge; redge = bedge;
	}
	else
	{
		ledge = bedge; redge = fedge;
	}

	for (int32_t scan_y = minyclip; scan_y < maxyclip; scan_y++)
	{
		float fully = float(scan_y) + 0.5f;
		while (fully > ledge->v2->y && fully < v[maxv].y) ++ledge;
		while (fully > redge->v2->y && fully < v[maxv].y) ++redge;
		float startx = ledge->v1->x + (fully - ledge->v1->y) * ledge->dxdy;
		float stopx = redge->v1->x + (fully - redge->v1->y) * redge->dxdy;
		int32_t istartx = round_coordinate(startx);
		int32_t istopx = round_coordinate(stopx);
		if (istartx > istopx) std::swap(istartx, istopx);
		istartx = std::max(istartx, 0);
		istopx = std::min(istopx, clip_r + 1);

		Extent e{};
		if (PARAMS > 0)
		{
			float ldy = fully - ledge->v1->y;
			float rdy = fully - redge->v1->y;
			float oox = 1.0f / (stopx - startx);
			for (int k = 0; k < PARAMS; k++)
			{
				float lp = ledge->v1->p[k] + ldy * ledge->dpdy[k];
				float rp = redge->v1->p[k] + rdy * redge->dpdy[k];
				float dpdx = (rp - lp) * oox;
				e.pstart[k] = lp + (float(istartx) + 0.5f - startx) * dpdx;
				e.dpdx[k] = dpdx;
			}
		}
		if (istartx >= istopx)
			istartx = istopx = 0;
		e.startx = istartx;
		e.stopx = istopx;
		scan(scan_y, e);
	}
}

} // namespace

void MidVUnit::draw_quad(const VQuad &q)
{
	const uint16_t *d = q.dma;
	Vert vert[4];
	for (int i = 0; i < 4; i++)
	{
		vert[i].x = float(int16_t(d[2 + i * 2])) + 0.5f;
		vert[i].y = float(int16_t(d[3 + i * 2])) + 0.5f;
		vert[i].p[0] = vert[i].p[1] = 0;
	}

	// make vertices inclusive of right/bottom points (clockwise assumed)
	{
		uint8_t rmask = 0, bmask = 0, eqmask = 0;
		for (int vn = 0; vn < 4; vn++)
		{
			Vert *c = &vert[vn], *n = &vert[(vn + 1) & 3];
			if (n->y == c->y && n->x == c->x) eqmask |= 1 << vn;
			if (n->y > c->y || (n->y == c->y && n->x < c->x)) rmask |= 1 << vn;
			if (n->x < c->x || (n->x == c->x && n->y < c->y)) bmask |= 1 << vn;
		}
		if (eqmask != 0x0f)
		{
			for (int vn = 0; vn < 4; vn++)
			{
				int eff = vn;
				while (eqmask & (1 << eff)) eff = (eff + 1) & 3;
				if (rmask & (1 << eff)) vert[vn].x += 0.001f;
				if (bmask & (1 << eff)) vert[vn].y += 0.001f;
			}
		}
	}

	uint16_t pixdata = d[1];
	const bool textured = ((d[0] & 0x300) == 0x100);
	uint16_t *dest = &m_videoram[q.page ? 0x40000 : 0];
	const int dither = (d[0] & 0x2000) ? 1 : 0;
	const int xstep = dither + 1;
	const int clip_r = m_vis_w - 1, clip_b = m_vis_h - 1;

	if (!textured)
	{
		pixdata += (d[0] & 0x00ff);
		raster_polygon<0>(vert, clip_r, clip_b, [&](int32_t y, const Extent &e) {
			uint16_t *row = dest + size_t(y) * 512;
			int startx = e.startx;
			startx += (y ^ startx) & dither;
			if (pixdata == 0 && xstep == 1)
				std::memset(&row[startx], 0, 2 * size_t(e.stopx - startx + 1));   // (sic) one extra pixel, as MAME
			else
				for (int x = startx; x < e.stopx; x += xstep)
					row[x] = pixdata;
		});
		return;
	}

	for (int i = 0; i < 4; i++)
	{
		vert[i].p[0] = float(d[10 + i] & 0xff) * 65536.0f + 32768.0f;
		vert[i].p[1] = float(d[10 + i] >> 8) * 65536.0f + 32768.0f;
	}
	const uint8_t *texbase = m_textureram.data() + (size_t(d[14]) * 256 & 0x3fffff);
	const uint32_t tmax = 0x3fffff - uint32_t(texbase - m_textureram.data());
	const int mode = (d[0] & 0xc00);   // 0x000 opaque, 0x800 transparent, 0xc00 masked transparent
	if (mode == 0x400)
	{
		// invalid combination: MAME falls back to a flat fill
		pixdata += (d[0] & 0x00ff);
		raster_polygon<0>(vert, clip_r, clip_b, [&](int32_t y, const Extent &e) {
			uint16_t *row = dest + size_t(y) * 512;
			int startx = e.startx;
			startx += (y ^ startx) & dither;
			if (pixdata == 0 && xstep == 1)
				std::memset(&row[startx], 0, 2 * size_t(e.stopx - startx + 1));
			else
				for (int x = startx; x < e.stopx; x += xstep)
					row[x] = pixdata;
		});
		return;
	}
	if (mode == 0xc00)
		pixdata += (d[0] & 0x00ff);

	raster_polygon<2>(vert, clip_r, clip_b, [&](int32_t y, const Extent &e) {
		uint16_t *row = dest + size_t(y) * 512;
		int startx = e.startx;
		const int stopx = e.stopx;
		int32_t u = int32_t(e.pstart[0]);
		int32_t v = int32_t(e.pstart[1]);
		int32_t dudx = int32_t(e.dpdx[0]);
		int32_t dvdx = int32_t(e.dpdx[1]);
		if (xstep == 2)
		{
			if ((y ^ startx) & 1)
			{
				startx++;
				u += dudx;
				v += dvdx;
			}
			dudx *= 2;
			dvdx *= 2;
		}
		for (int x = startx; x < stopx; x += xstep)
		{
			uint32_t ti = uint32_t(((v >> 8) & 0xff00) + (u >> 16));
			uint8_t pix = ti <= tmax ? texbase[ti] : 0;
			if (mode == 0x000)
				row[x] = pixdata + pix;
			else if (mode == 0x800)
			{
				if (pix != 0) row[x] = pixdata + pix;
			}
			else
			{
				if (pix != 0) row[x] = pixdata;
			}
			u += dudx;
			v += dvdx;
		}
	});
}

// ---------------------------------------------------------------------------------------
// frame execution
// ---------------------------------------------------------------------------------------

bool MidVUnit::run_frame()
{
	quads_last_frame = 0;
	m_video_changed = false;
	m_partial_next_row = 0;

	const double cycles_per_frame = double(CPU_HZ) / m_refresh_hz;
	const double cycles_per_line = cycles_per_frame / double(m_vtotal);

	for (m_vpos = 0; m_vpos < m_vtotal; m_vpos++)
	{
		// visible area ends here: draw the remainder of the visible page (vblank start)
		if (m_vpos == m_vis_h)
		{
			if (m_partial_next_row < m_vis_h)
				update_screen_rows(m_partial_next_row, m_vis_h - 1, m_page_control & 1);
			m_partial_next_row = m_vis_h;
		}

		// scanline interrupt (level triggered, released ~40 ns later)
		if (m_vpos == m_irq_line)
		{
			m_cpu->set_input(0, ASSERT_LINE);
			m_slice_len = 2;
			run_cpu(2);
			m_cycles_total += 2;
			m_cpu->set_input(0, CLEAR_LINE);
			m_cycle_frac -= 2;
		}

		m_cycle_frac += cycles_per_line;
		int line_cycles = int(m_cycle_frac);
		m_cycle_frac -= line_cycles;

		while (line_cycles > 0)
		{
			int slice = line_cycles;
			if (m_adc_event != ~0ull)
			{
				int64_t until = int64_t(m_adc_event) - int64_t(m_cycles_total);
				if (until <= 0)
				{
					m_adc_event = ~0ull;
					adc_conversion_done();
					continue;
				}
				if (until < slice)
					slice = int(until);
			}
			m_slice_len = slice;
			int used = run_cpu(slice);
			m_cycles_total += uint64_t(used);
			line_cycles -= used;
			if (m_adc_event != ~0ull && m_cycles_total >= m_adc_event)
			{
				m_adc_event = ~0ull;
				adc_conversion_done();
			}
		}
	}
	m_frame_count++;

	// convert the indexed frame to RGB with the palette as it is at end of frame
	for (int y = 0; y < m_vis_h; y++)
	{
		uint32_t *row = &m_frame[size_t(y) * FRAME_STRIDE];
		for (int x = 0; x < m_vis_w; x++)
			row[x] = m_palette_rgb[row[x] & 0x7fff];
	}
	return m_video_changed;
}

// ---------------------------------------------------------------------------------------
// NVRAM
// ---------------------------------------------------------------------------------------

bool MidVUnit::load_nvram(const std::string &path)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	size_t n = std::fread(m_nvram.data(), 4, m_nvram.size(), f);
	std::fclose(f);
	return n == m_nvram.size();
}

bool MidVUnit::save_nvram(const std::string &path) const
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	size_t n = std::fwrite(m_nvram.data(), 4, m_nvram.size(), f);
	std::fclose(f);
	return n == m_nvram.size();
}
