// Midway V-Unit (Cruis'n USA) machine -- see midvunit.h
#include "midvunit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <cstdlib>

#include "../../third_party/miniz/miniz.h"
#include "default_nvram.h"
#include "cmos.h"

#include <windows.h>
namespace {
struct PerfTimer
{
	double &acc;
	LARGE_INTEGER t0;
	explicit PerfTimer(double &a) : acc(a) { QueryPerformanceCounter(&t0); }
	~PerfTimer()
	{
		static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
		LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
		acc += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
	}
};
}
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
	// RAM1 is 128K words at 0x400000; the 128K words after it (0x420000) are not used by the game. They are mapped as extra RAM
	// for the enlarged object pool of the long draw distance (see setup_idle_hooks).
	m_ram1.assign(0x40000, 0);
	m_rom.assign(0x400000, 0);
	m_nvram.assign(0x2000, 0xffffffffu);   // NVRAM default: all ones
	m_paletteram.assign(0x8000, 0);
	m_palette_rgb.assign(0x8000, 0);
	m_videoram.assign(0x80000, 0);
	m_textureram.assign(0x400000, 0);
	m_frame.assign(FRAME_STRIDE * 512, 0);
	sound_rom.assign(0x1000000, 0xff);
	m_cpu_layer.assign(0x80000, 0);

	// fast paths: RAM at 0x000000 and 0x400000, ROM at 0xc00000
	for (int p = 0; p < 0x20000 / tms320c3x_device::PAGE_WORDS; p++)
	{
		m_bus->rpage[p] = &m_ram0[p * tms320c3x_device::PAGE_WORDS];
		m_bus->wpage[p] = &m_ram0[p * tms320c3x_device::PAGE_WORDS];
	}
	for (int p = 0; p < int(m_ram1.size()) / tms320c3x_device::PAGE_WORDS; p++)
	{
		int q = (0x400000 >> tms320c3x_device::PAGE_SHIFT) + p;
		m_bus->rpage[q] = &m_ram1[p * tms320c3x_device::PAGE_WORDS];
		m_bus->wpage[q] = &m_ram1[p * tms320c3x_device::PAGE_WORDS];
	}
	// internal RAM (game code runs from it): fast page instead of the slow bus path
	m_bus->rpage[0x809000 >> tms320c3x_device::PAGE_SHIFT] = m_iram_page;
	m_bus->wpage[0x809000 >> tms320c3x_device::PAGE_SHIFT] = m_iram_page;
	for (int p = 0; p < 0x400000 / tms320c3x_device::PAGE_WORDS; p++)
		m_bus->rpage[(0xc00000 >> tms320c3x_device::PAGE_SHIFT) + p] = &m_rom[p * tms320c3x_device::PAGE_WORDS];

	m_cpu = std::make_unique<tms320c3x_device>(m_bus.get(), tms320c3x_device::CHIP_TYPE_TMS320C31, 2);
	m_cpu->on_xf0 = [](int) {};
	m_cpu->on_xf1 = [](int) {};
}

MidVUnit::~MidVUnit()
{
	m_dcs_worker.reset();   // first: its last slices still deliver sound into members declared after it
}

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

	m_code_orig.clear();
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
	m_dcs->on_audio = [this](const int16_t *b, int n, double r) {
		if (!m_dcs_worker) { if (on_audio) on_audio(b, n, r); return; }
		m_aud_pcm.insert(m_aud_pcm.end(), b, b + n);   // worker thread (or the main thread while the worker is drained)
		m_aud_ev.push_back({n, r, -1});
	};
	m_dcs->on_audio_enable = [this](bool e) {
		if (!m_dcs_worker) { if (on_audio_enable) on_audio_enable(e); return; }
		m_aud_ev.push_back({0, 0.0, e ? 1 : 0});
	};
	on_sound_data = [this](uint8_t d) { dcs_write(d); };
	on_dcs_reset = [this](int st) { dcs_direct(); m_dcs->reset_w(st); };
	return true;
}

void MidVUnit::reset()
{
	// Optional program patches go into the in-memory ROM image (the game re-copies its code from the ROM while it runs,
	// so patching only the RAM copy would be undone). The image is restored from the pristine copy first.
	if (m_code_orig.empty()) m_code_orig.assign(m_rom.begin(), m_rom.begin() + 0x20000);
	std::copy(m_code_orig.begin(), m_code_orig.end(), m_rom.begin());
	{
		std::string plog;
		int n = apply_rom_patches(m_rom, rom_patches, plog);
		if (n && std::getenv("ROMPATCH_LOG")) std::fprintf(stderr, "ROM patches: %d\n%s", n, plog.c_str());
	}
	// boot: the first 128K words of the ROM are copied to RAM at 0
	std::copy(m_rom.begin(), m_rom.begin() + 0x20000, m_ram0.begin());
	setup_idle_hooks();

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
	if (m_dcs)
	{
		dcs_direct();
		m_dcs->reset_w(0);
		m_dcs->reset_w(1);
		const bool want = dcs_thread > 0 || (dcs_thread < 0 && DcsWorker::worthwhile());
		if (want && !m_dcs_worker) m_dcs_worker = std::make_unique<DcsWorker>(*m_dcs);
		if (!want && m_dcs_worker) { dcs_end_frame(); m_dcs_worker.reset(); }
	}
	m_wheel_board_output = 0;
	m_wheel_board_last = 0;
	galil_set_input(":");
}

// The emulated CPU's clock factor for the coming frame.
//
// A longer draw distance keeps several times more scenery alive, and the arcade CPU then needs three or four vblanks per game
// frame instead of two. The game counts its sequences (ready - set - go, the flag girl, logos, everything that SLEEPs) in game
// frames, so they would run at half speed, and the picture would stutter. The game itself limits the frame rate with its governor
// (FRAMRATE: at least FRAMRATE + 1 vblanks per frame, 1 in races and in the attract drive), so giving the CPU more clock there
// only lets it keep the governor's pace again: measured with 400 % and 21:9, three times the clock gives exactly the original
// cadence in races (2 vblanks), at the end of a race and in the attract drive (3 vblanks).
// Where the game sets no governor (the first selection screen, logos) the original pace is given by the CPU alone; there the
// clock is raised only moderately, in proportion to the draw distance, and not at all while no scenery is loaded.
void MidVUnit::update_clock()
{
	int q = std::max(1, cpu_overclock) * 4;
	const int dd = rom_patches.draw_distance_pct;
	if (auto_overclock && dd > 100 && m_dgroup_count_addr)
	{
		const uint32_t groups = m_ram0[m_dgroup_count_addr];
		if (groups > 0 && groups <= 20)
		{
			const bool governed = m_framrate_addr && m_ram0[m_framrate_addr] >= 1 && m_ram0[m_framrate_addr] <= 4;
			const int want = governed ? (dd >= 300 ? 12 : 8) : 4 + (dd - 100) / 100;
			q = std::max(q, want);
		}
	}
	m_oc_q4 = q;
}

int MidVUnit::run_cpu(int cycles)
{
	int used;
	{
		PerfTimer pt(perf_cpu_ms);
		// overclock: the CPU gets m_oc_q4 / 4 times the instructions in the same emulated time
		const int ran = m_cpu->run(cycles * m_oc_q4 / 4);
		used = std::max(ran * 4 / m_oc_q4, std::min(cycles, ran));
	}
	if (m_pchist_on) m_pchist[m_cpu->pc() >> 2]++;
	sync_dcs();
	return used;
}

// bring the DCS up to the main CPU's current time
void MidVUnit::sync_dcs()
{
	if (!m_dcs)
		return;
	uint64_t now = now_cycles();
	if (now <= m_dcs_synced)
		return;
	double delta = double(now - m_dcs_synced) * (Dcs1::ADSP_CLOCK / double(CPU_HZ));
	m_dcs_synced = now;
	double use = std::max(0.0, delta - m_dcs_ahead);
	m_dcs_ahead = std::max(0.0, m_dcs_ahead - delta);
	if (use <= 0)
		return;
	if (m_dcs_worker)
	{
		m_dcs_worker->advance(use);
		return;
	}
	PerfTimer pt(perf_dcs_ms);
	m_dcs->advance(use);
}

// end of an emulated frame: let the worker catch up and pass its sound output on, in the order it was produced
void MidVUnit::dcs_end_frame()
{
	if (!m_dcs_worker)
		return;
	m_dcs_worker->drain();
	perf_dcs_ms += m_dcs_worker->take_busy_ms();
	size_t off = 0;
	for (const AudioEvent &e : m_aud_ev)
	{
		if (e.enable < 0)
		{
			if (on_audio) on_audio(m_aud_pcm.data() + off, e.count, e.rate);
			off += size_t(e.count);
		}
		else if (on_audio_enable)
			on_audio_enable(e.enable != 0);
	}
	m_aud_ev.clear();
	m_aud_pcm.clear();
}

// host -> DCS command byte: make sure the previous byte was consumed first (real hardware
// has a far tighter host/DSP interleave than our per-scanline slices)
void MidVUnit::dcs_write(uint8_t d)
{
	if (!m_dcs)
		return;
	sync_dcs();
	dcs_direct();   // the latch state below must include every slice queued so far
	for (int guard = 0; m_dcs->input_full() && guard < 100; guard++)
	{
		m_dcs->advance(100);
		m_dcs_ahead += 100;
	}
	m_dcs->data_w(d);
}

uint64_t MidVUnit::now_cycles() const
{
	return m_cycles_total + uint64_t(std::max(0, m_slice_len * m_oc_q4 / 4 - std::max(0, m_cpu->icount())) * 4 / m_oc_q4);
}

// ---------------------------------------------------------------------------------------
// bus
// ---------------------------------------------------------------------------------------

uint32_t MidVUnit::bus_read(offs_t addr)
{
	if (addr >= 0x900000 && addr < 0x980000)
	{
		stat_vram_reads++;
		return m_videoram[addr - 0x900000];
	}
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
		stat_vram_writes++;
		m_videoram[addr - 0x900000] = uint16_t(data);
		if (m_gpu)
		{
			m_cpu_page_writes[(addr - 0x900000) >> 18]++;
			if (!m_gq.empty() || !m_gs.empty()) gpu_flush_quads();
			uint32_t off = addr - 0x900000;
			int pg = (off >> 18) & 1, row = (off >> 9) & 511;
			m_cpu_layer[off] = uint16_t((data & 0x7fff) | 0x8000);
			m_ovl_lo[pg] = std::min(m_ovl_lo[pg], row);
			m_ovl_hi[pg] = std::max(m_ovl_hi[pg], row);
		}
		return;
	}
	if (addr >= 0xa00000 && addr < 0xc00000)
	{
		stat_tex_writes++;
		size_t o = size_t(addr - 0xa00000) * 2;
		if (m_gpu)
		{
			if (!m_gq.empty() || !m_gs.empty()) gpu_flush_quads();
			int row = int(o >> 8);
			m_tex_lo = std::min(m_tex_lo, row);
			m_tex_hi = std::max(m_tex_hi, row);
		}
		m_textureram[o] = uint8_t(data);
		m_textureram[o + 1] = uint8_t(data >> 8);
		m_tex_gen++;
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
		stat_pal_writes++;
		if (m_gpu)
		{
			if (!m_gq.empty() || !m_gs.empty()) gpu_flush_quads();
			m_pal_lo = std::min(m_pal_lo, int(addr - 0x9e0000));
			m_pal_hi = std::max(m_pal_hi, int(addr - 0x9e0000));
		}
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
	if (m_gpu)
	{
		gpu_add_quad(q);
		quads_last_frame++;
		m_dma_data_index = 0;
		return;
	}
	if (texrepl.active() && (q.dma[0] & 0x300) == 0x100 && (q.dma[0] & 0xc00) != 0x400)   // texture export without a GPU (headless)
		texrepl.lookup(q.dma[14], q.dma[1], m_textureram.data(), m_palette_rgb.data(), m_tex_gen);
	if (!skip_raster)
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
	if ((m_page_control ^ data) & 1) page_flips++;
	if (((m_page_control ^ data) & 1) && !m_gpu)
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
	update_clock();
	m_video_changed = false;
	m_partial_next_row = 0;

	const double cycles_per_frame = double(CPU_HZ) / m_refresh_hz;
	const double cycles_per_line = cycles_per_frame / double(m_vtotal);

	for (m_vpos = 0; m_vpos < m_vtotal; m_vpos++)
	{
		// visible area ends here: draw the remainder of the visible page (vblank start)
		if (m_vpos == m_vis_h && m_gpu)
		{
			// vblank: freeze what the monitor shows (the game already starts drawing the next frame)
			m_present_page = m_page_control & 1;
			gpu_flush_quads();
			gpu_sync_state();
			// The game clears a page either with a full-screen fill (stretched over the margins in gpu_add_quad) or, for boot and
			// test screens, by writing the video RAM itself. Only the latter leaves stale 3D in the widescreen margins, so they are
			// cleared when the CPU rewrote most of a page since the last vblank. (Not "no polygons this frame": the game also holds a
			// finished 3D picture for many vblanks while it loads, and the margins must stay.)
			for (int pg = 0; pg < 2; pg++)
			{
				if (m_wide && m_cpu_page_writes[pg] > 512u * 400u / 2) m_gpu->clear_margins(pg);
				m_cpu_page_writes[pg] = 0;
			}
			m_gpu->latch(m_present_page, m_vis_h);
		}
		if (m_vpos == m_vis_h && texrepl.active()) texrepl.tick(m_textureram.data(), m_palette_rgb.data());
		if (m_vpos == m_vis_h && !m_gpu)
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
	dcs_end_frame();

	// convert the indexed frame to RGB with the palette as it is at end of frame
	for (int y = 0; y < (m_gpu ? 0 : m_vis_h); y++)
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

void MidVUnit::load_default_nvram()
{
	std::fill(m_nvram.begin(), m_nvram.end(), 0xffffffffu);
	for (const NvPair &p : kDefaultNvram)
		if (p.index < m_nvram.size())
			m_nvram[p.index] = p.value;
	cmos::set(m_nvram, cmos::ADJ_FREE_PLAY, 1);   // free play by default: a PC has no coin slot
}

bool MidVUnit::load_nvram(const std::string &path)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::vector<uint32_t> tmp(m_nvram.size());
	size_t n = std::fread(tmp.data(), 4, tmp.size(), f);
	std::fclose(f);
	if (n != tmp.size())
		return false;

	// The control calibration (CMOS words 8..31) is fixed by design: wheel 0x10..0xf0 with centre 0x80 and
	// pedals 0..0xff, exactly what the input layer produces. A file carrying other calibration values
	// (e.g. from an older build or a service-menu recalibration) is rejected so the wheel stays centred.
	for (const NvPair &p : kDefaultNvram)
		if (p.index >= 8 && p.index < 32 && tmp[p.index] != p.value)
			return false;

	m_nvram = tmp;
	return true;
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


// ---------------------------------------------------------------------------------------
// GPU feed
// ---------------------------------------------------------------------------------------

void MidVUnit::attach_video_backend(IVideoBackend *gpu)
{
	m_gpu = gpu;
	m_gq.clear();
	m_gq.reserve(4096);
	if (!gpu)
		return;
	// everything the GPU knows is stale: push full palette / texture state on the next sync
	m_pal_lo = 0; m_pal_hi = 0x7fff;
	m_tex_lo = 0; m_tex_hi = 16383;
	m_repl_pending = texrepl.layers() > 0;
	for (int pg = 0; pg < 2; pg++)
	{
		m_ovl_lo[pg] = 0; m_ovl_hi[pg] = 511;
		// replay whatever the CPU drew so far as overlay content
		for (size_t i = 0; i < 0x40000; i++)
			m_cpu_layer[size_t(pg) * 0x40000 + i] = uint16_t((m_videoram[size_t(pg) * 0x40000 + i] & 0x7fff) | 0x8000);
	}
}

void MidVUnit::gpu_sync_state()
{
	if (!m_gpu)
		return;
	if (m_repl_pending)
	{
		m_repl_pending = false;
		if (m_gpu->init_replacements(texrepl.resolution(), texrepl.layers()))
			for (int i = 0; i < texrepl.layers(); i++) m_gpu->upload_replacement(i, texrepl.layer_rgba(i).data());
	}
	if (m_pal_hi >= 0)
	{
		m_gpu->upload_palette(m_palette_rgb.data(), m_pal_lo, m_pal_hi);
		m_pal_lo = 0x7fffffff; m_pal_hi = -1;
	}
	if (m_tex_hi >= 0)
	{
		m_gpu->upload_texture_rows(m_textureram.data(), m_tex_lo, m_tex_hi);
		m_tex_lo = 0x7fffffff; m_tex_hi = -1;
	}
	{
		// the CPU-drawn layer (HUD text, gauges) follows the same placement as the 2D quads
		int l = m_wide, c = m_wide, r = m_wide;
		if (m_hud_spread > 0 && in_race())
		{
			l = int(std::lround(m_wide * (1.0f - m_hud_spread)));
			r = int(std::lround(m_wide * (1.0f + m_hud_spread)));
		}
		m_gpu->set_overlay_offsets(l, c, r);
	}
	for (int pg = 0; pg < 2; pg++)
	{
		if (m_ovl_hi[pg] < 0)
			continue;
		uint16_t *layer = m_cpu_layer.data() + size_t(pg) * 0x40000;
		m_gpu->upload_overlay(pg, layer, m_ovl_lo[pg], m_ovl_hi[pg]);
		// pixels are consumed: clear the valid bits of the uploaded rows
		for (int y = m_ovl_lo[pg]; y <= m_ovl_hi[pg]; y++)
			for (int x = 0; x < 512; x++)
				layer[size_t(y) * 512 + x] &= 0x7fff;
		m_ovl_lo[pg] = 0x7fffffff; m_ovl_hi[pg] = -1;
	}
}

void MidVUnit::gpu_flush_shadows()
{
	if (!m_gpu || m_gs.empty())
		return;
	PerfTimer pt(perf_feed_ms);
	perf_draws++;
	gpu_sync_state();
	m_gpu->draw_shadows(m_gq_page, m_gs.data(), int(m_gs.size()));
	m_gs.clear();
}

void MidVUnit::gpu_flush_quads()
{
	if (!m_gpu)
		return;
	gpu_flush_shadows();
	if (m_gq.empty())
		return;
	PerfTimer pt(perf_feed_ms);
	perf_draws++;
	perf_quads += m_gq.size();
	gpu_sync_state();
	m_gpu->draw(m_gq_page, m_gq.data(), int(m_gq.size()));
	m_gq.clear();
}

void MidVUnit::gpu_add_quad(const VQuad &q)
{
	// the game's shadows are flat dithered quads; in modern mode they become a soft blended pass
	const bool is2d = writer_is_2d();
	const bool is_shadow = (q.dma[0] & 0x2000) != 0 && !is2d;   // dithered boxes of the 2D routine are HUD panels, not shadows
	if (is_shadow && m_shadow_mode == 2)
		return;
	const bool modern = is_shadow && m_shadow_mode == 1;
	if ((!m_gq.empty() || !m_gs.empty()) && q.page != m_gq_page)
		gpu_flush_quads();
	if (modern && !m_gq.empty())
		gpu_flush_quads();
	else if (!modern && !m_gs.empty())
		gpu_flush_shadows();
	m_gq_page = q.page;

	const uint16_t *d = q.dma;
	GpuQuad g{};
	float vx[4], vy[4];
	for (int i = 0; i < 4; i++)
	{
		vx[i] = float(int16_t(d[2 + i * 2]));
		vy[i] = float(int16_t(d[3 + i * 2]));
		g.p[i * 2] = vx[i];
		g.p[i * 2 + 1] = vy[i];
		g.t[i * 2] = float(d[10 + i] & 0xff) + 0.5f;
		g.t[i * 2 + 1] = float(d[10 + i] >> 8) + 0.5f;
	}

	// hardware model: vertices are pixel centres; "right"/"bottom" points get a 0.001 nudge so that
	// their edge pixels are included (same rule as the reference rasterizer)
	for (int i = 0; i < 4; i++) { g.p[i * 2] += 0.5f; g.p[i * 2 + 1] += 0.5f; }
	uint8_t rmask = 0, bmask = 0, eqmask = 0;
	for (int vn = 0; vn < 4; vn++)
	{
		int nx = (vn + 1) & 3;
		if (vy[nx] == vy[vn] && vx[nx] == vx[vn]) eqmask |= 1 << vn;
		if (vy[nx] > vy[vn] || (vy[nx] == vy[vn] && vx[nx] < vx[vn])) rmask |= 1 << vn;
		if (vx[nx] < vx[vn] || (vx[nx] == vx[vn] && vy[nx] < vy[vn])) bmask |= 1 << vn;
	}
	if (eqmask != 0x0f)
		for (int vn = 0; vn < 4; vn++)
		{
			int eff = vn;
			while (eqmask & (1 << eff)) eff = (eff + 1) & 3;
			if (rmask & (1 << eff)) g.p[vn * 2] += 0.001f;
			if (bmask & (1 << eff)) g.p[vn * 2 + 1] += 0.001f;
		}
	if (m_wide)
	{
		// the page is wider than the arcade's 512 px: game x 0 sits m_wide pixels in. A flat full-screen fill (the per-frame clear)
		// is stretched over the whole page instead so that the margins are cleared too.
		float minx = std::min(std::min(vx[0], vx[1]), std::min(vx[2], vx[3])), maxx = std::max(std::max(vx[0], vx[1]), std::max(vx[2], vx[3]));
		float miny = std::min(std::min(vy[0], vy[1]), std::min(vy[2], vy[3])), maxy = std::max(std::max(vy[0], vy[1]), std::max(vy[2], vy[3]));
		bool flat = (d[0] & 0x300) != 0x100 || (d[0] & 0xc00) == 0x400;
		bool full = flat && minx <= 0 && maxx >= 511 && miny <= 0 && maxy >= 399;
		// HUD placement: 2D elements of a race are moved towards the screen edges by their third of the picture
		float shift = float(m_wide);
		if (is2d && m_hud_spread > 0 && in_race() && !full)
		{
			float cx = (minx + maxx) * 0.5f;
			if (cx < 171) shift = float(m_wide) * (1.0f - m_hud_spread);
			else if (cx > 341) shift = float(m_wide) * (1.0f + m_hud_spread);
		}
		for (int i = 0; i < 4; i++)
		{
			float &x = g.p[i * 2];
			if (full) x = vx[i] <= 0 ? 0.5f : x + float(2 * m_wide);
			else x += shift;
		}
	}
	g.edge = 0;
	if (texrepl.active() && (d[0] & 0x300) == 0x100 && (d[0] & 0xc00) != 0x400)
		g.edge = texrepl.lookup(d[14], d[1], m_textureram.data(), m_palette_rgb.data(), m_tex_gen);
	g.flags = d[0];
	g.pixdata = d[1];
	g.texbase = d[14];
	std::vector<GpuQuad> &dst = modern ? m_gs : m_gq;
	dst.push_back(g);
	if (dst.size() >= 4096)
		gpu_flush_quads();
}

void MidVUnit::present_gpu()
{
	if (!m_gpu)
		return;
	PerfTimer pt(perf_feed_ms);
	m_gpu->present(m_vis_w, m_vis_h);
}

void MidVUnit::debug_ram_usage() const
{
	std::fprintf(stderr, "words ram0[BF]=%08X ram0[C3]=%08X ram1[BF]=%08X rom[BF]=%08X ram0[55]=%08X ram1[55]=%08X\n", m_ram0[0xBF], m_ram0[0xC3], m_ram1[0xBF], m_rom[0xBF], m_ram0[0x55], m_ram1[0x55]);
	for (auto &kv : m_pchist) if (kv.second > 40) std::fprintf(stderr, "PCHIST %06X00 %u\n", kv.first, kv.second);
	{
		const uint32_t pat[2] = {0x04E21F40u, 0x04E23A98u};
		auto scan = [&](const char *name, const std::vector<uint32_t> &v) {
			for (size_t i = 0; i + 4 <= v.size(); i++)
				if (v[i] == pat[0] || v[i] == pat[1]) std::fprintf(stderr, "pattern %08X in %s at 0x%zX\n", v[i], name, i);
		};
		scan("ram0", m_ram0); scan("ram1", m_ram1); scan("rom", m_rom);
		std::vector<uint32_t> ir(m_iram, m_iram + 0x800);
		scan("iram", ir);
	}
	for (int b = 0; b < 2; b++)
	{
		const std::vector<uint32_t> &r = b ? m_ram1 : m_ram0;
		size_t hi = 0, nz = 0;
		for (size_t i = 0; i < r.size(); i++) if (r[i]) { hi = i; nz++; }
		std::fprintf(stderr, "RAM%d: highest nonzero word 0x%zX, %zu nonzero of %zu\n", b, hi, nz, r.size());
	}
}

bool MidVUnit::in_race() const
{
	return (m_ram0[0xC8F5] & 0xf) == 4;   // _MODE == MGAME
}

// The 2D draw routine (rdma / _stuff_fpga in TOTALA.ASM) is the only writer that starts with
//   STI RS,($xxxx) / STI RE,($xxxx)
// A trigger comes from inside it; recognise it by searching back for that pair.
bool MidVUnit::writer_is_2d()
{
	uint32_t pc = m_cpu->pc();
	auto it = m_pc2d.find(pc);
	if (it != m_pc2d.end()) return it->second != 0;
	bool found = false;
	if (pc < m_ram0.size())
		for (uint32_t k = 1; k < 96 && k <= pc && !found; k++)
		{
			uint32_t a = pc - k;
			if ((m_ram0[a] & 0xffff0000u) == 0x15390000u && (m_ram0[a + 1] & 0xffff0000u) == 0x153A0000u) found = true;
		}
	m_pc2d[pc] = found ? 1 : 0;
	return found;
}

// ---------------------------------------------------------------------------------------
// Idle loop fast-forward.
//
// The game finishes its frame early and then spins until the video interrupt arrives:
//  (a) a plain wait on a frame counter:   LDI (X),R0 / CMPI (X),R0 / BEQ -2 / RETS
//  (b) ZSORTWT (OBJ.ASM): bubble-sorts the object list again and again until CLEARRDY is cleared by the interrupt.
// Once the list is sorted a pass changes nothing, so spinning on until the next time slice has no effect on the game state; the
// interpreter spent about 40% of its time there. Both loops are recognised by their instruction patterns, so a ROM version
// that does not contain them simply runs unchanged. Interrupts are only raised between time slices, which is where the skip ends.
// ---------------------------------------------------------------------------------------
void MidVUnit::setup_idle_hooks()
{
	for (uint32_t &h : m_cpu->hook_pc) h = ~0u;
	m_cpu->on_hook = nullptr;
	const bool idle = idle_skip && !std::getenv("NOIDLESKIP");
	const std::vector<uint32_t> &c = m_ram0;
	uint32_t sync_pc = ~0u, sort_entry = ~0u, sort_top = ~0u, dact_pc = ~0u, dact_skip = ~0u, snd_pc = ~0u, objinit_pc = ~0u, ofreecnt = 0, debris_ptr = 0;
	m_ofree_addr = 0;
	for (uint32_t i = 0; i + 8 < 0x20000; i++)
	{
		// (a)
		if (idle && (c[i] & 0xffff0000u) == 0x08200000u && (c[i + 1] & 0xffff0000u) == 0x04A00000u && (c[i] & 0xffff) == (c[i + 1] & 0xffff) &&
		    c[i + 2] == 0x6A05FFFEu && c[i + 3] == 0x78800000u && sync_pc == ~0u)
		{
			sync_pc = i + 1;
			m_idle_sync_addr = c[i] & 0xffff;
		}
		// (b)  LDI 1,R0 / STI R0,(CLEARRDY) / SUBI R6,R6 / LDI (OACTIVEI),AR0 / LDI *AR0,AR1
		if (idle && c[i] == 0x08600001u && (c[i + 1] & 0xffff0000u) == 0x15200000u && c[i + 2] == 0x18060006u &&
		    (c[i + 3] & 0xffff0000u) == 0x08280000u && c[i + 4] == 0x0849C000u && sort_entry == ~0u)
		{
			sort_entry = i;
			sort_top = i + 2;
			m_idle_flag_addr = c[i + 1] & 0xffff;
		}
		// MWAIT0 in the main loop: LDI (INFRAMES),R0 / CMPI (FRAMRATE),R0 / BLT MWAIT0  -> the frame governor's address
		if ((c[i] & 0xffff0000u) == 0x08200000u && (c[i + 1] & 0xffff0000u) == 0x04A00000u && c[i + 2] == 0x6A07FFFDu && m_framrate_addr == 0)
			m_framrate_addr = c[i + 1] & 0xffff;
		// (c) BGD_WATCHER "activate the next scenery section":  CMPF (DACT_DIST),R0 / BGT NOACT / LDI AR0,AR2 / CALL activate /
		//     LDI (DGROUP_COUNT),AR1 / MPYI 5,AR1. With a longer view distance more sections are wanted than the 20-entry section table
		//     holds, so the activation is held back while the table is nearly full.
		if (rom_patches.draw_distance_pct > 100 && (c[i] & 0xffff0000u) == 0x04200000u && (c[i + 1] & 0xffff0000u) == 0x6A090000u &&
		    c[i + 2] == 0x080A0008u && (c[i + 3] & 0xff000000u) == 0x62000000u && (c[i + 4] & 0xffff0000u) == 0x08290000u &&
		    c[i + 5] == 0x0AE90005u && dact_pc == ~0u)
		{
			dact_pc = i + 1;   // the branch: taken = no activation
			dact_skip = i + 2 + (c[i + 1] & 0xffff);
			m_dgroup_count_addr = c[i + 4] & 0xffff;
		}
		// (d) SNDFX, the start of a sound effect: ONESNDFX: LDI 255,R0 / SNDFX: PUSH R1 / PUSH R2 / PUSH R3 / PUSH AR0 / PUSH R0 /
		//     LDI (_MODE),R1. The sound index in AR2 tells which object the player's car just hit (force feedback).
		if (c[i] == 0x086000FFu && c[i + 1] == 0x0F210000u && c[i + 2] == 0x0F220000u && c[i + 3] == 0x0F230000u &&
		    c[i + 4] == 0x0F280000u && c[i + 5] == 0x0F200000u && (c[i + 6] & 0xffff0000u) == 0x08210000u && snd_pc == ~0u)
			snd_pc = i + 1;
		// (e) end of OBJ_INIT: LDI 1099,RC / RPTB / STI AR1,*AR0 / LDI AR1,AR0 / ADDI OBJSIZ,AR1 / LDI 0,R0 / STI R0,*AR0.
		//     * Bug fix of the original: OBJ_INIT empties every object list except ROAD_DEBRIS, and several screen changes call it
		//       without INIT_RDDEBRIS. Debris objects (barrels, barriers) that were active then stay in that list although they are
		//       free; once they are handed out again the collision scan over the list never ends (watchdog reset). The list is
		//       emptied here as well.
		//     * With a longer draw distance the 1100 objects run out (scenery sections are held back, or the game locks up), so more
		//       objects in the unused RAM at 0x420000 are linked onto the end of the free list.
		if (c[i] == 0x087B044Bu && (c[i + 1] & 0xffff0000u) == 0x64000000u && c[i + 2] == 0x1549C000u &&
		    c[i + 3] == 0x08080009u && c[i + 4] == 0x02690022u && c[i + 5] == 0x08600000u && c[i + 6] == 0x1540C000u && objinit_pc == ~0u)
		{
			objinit_pc = i + 5;
			for (uint32_t j = i; j > 8 && j + 30 > i; j--)   // OFREECNT: LDI 1100,R0 / STI R0,(OFREECNT) at the start of OBJ_INIT
				if (c[j] == 0x0860044Cu && (c[j + 1] & 0xffff0000u) == 0x15200000u) { ofreecnt = c[j + 1] & 0xffff; break; }
		}
		// DRONE_VS_DEBRIS / DRONE_VS_SIGN: LDI (ROAD_DEBRISI),AR1 / BU +1 / LDI (SIGN_LISTI),AR1 / LDI (CAR_LIST),R0 / LDI R0,AR0 / RETSEQ
		if ((c[i] & 0xffff0000u) == 0x08290000u && c[i + 1] == 0x6A000001u && (c[i + 2] & 0xffff0000u) == 0x08290000u &&
		    (c[i + 3] & 0xffff0000u) == 0x08200000u && c[i + 4] == 0x08080000u && c[i + 5] == 0x78850000u && debris_ptr == 0)
			debris_ptr = c[i] & 0xffff;
		// OBJ_FREE: PUSH R0 / LDI (OFREE),R0 / STI R0,*AR2 / STI AR2,(OFREE)  -> head of the free object list
		if (c[i] == 0x0F200000u && (c[i + 1] & 0xffff0000u) == 0x08200000u && c[i + 2] == 0x1540C200u &&
		    (c[i + 3] & 0xffff0000u) == 0x152A0000u && (c[i + 1] & 0xffff) == (c[i + 3] & 0xffff) && m_ofree_addr == 0)
			m_ofree_addr = c[i + 1] & 0xffff;
	}
	if (sync_pc == ~0u && sort_top == ~0u && dact_pc == ~0u && snd_pc == ~0u && objinit_pc == ~0u)
		return;
	m_cpu->hook_pc[0] = sync_pc;
	m_cpu->hook_pc[1] = sort_entry;
	m_cpu->hook_pc[2] = sort_top;
	m_cpu->hook_pc[3] = dact_pc;
	m_cpu->hook_pc[4] = snd_pc;
	m_cpu->hook_pc[5] = objinit_pc;
	m_cpu->refresh_hooks();
	m_zsort_first = true;
	m_cpu->on_hook = [this, sync_pc, sort_entry, sort_top, dact_pc, dact_skip, snd_pc, objinit_pc, ofreecnt, debris_ptr]() -> bool {
		uint32_t pc = m_cpu->pc();
		if (pc == objinit_pc)
		{
			auto wr = [&](uint32_t a, uint32_t v) {
				if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) m_ram1[a - 0x400000] = v;
				else if (a < m_ram0.size()) m_ram0[a] = v;
			};
			if (debris_ptr) wr(m_ram0[debris_ptr], 0);   // ROAD_DEBRIS = empty: its objects no longer exist
			if (rom_patches.draw_distance_pct <= 100) return false;
			// AR0 = last object of the array; the next instruction ends the list at AR0. Link the extra objects behind it and
			// let the game end the list at the last extra one instead.
			constexpr uint32_t OBJSIZ = 0x22, BASE = 0x420000;
			const uint32_t n = uint32_t(m_ram1.size() - 0x20000) / OBJSIZ;
			uint32_t last = m_cpu->reg(8);
			for (uint32_t k = 0; k < n; k++)
			{
				const uint32_t a = BASE + k * OBJSIZ;
				wr(last, a);
				last = a;
			}
			m_cpu->set_reg(8, last);
			if (ofreecnt) m_ram0[ofreecnt] += n;
			return false;
		}
		if (pc == snd_pc)
		{
			// sound indices (SNDTAB.EQU) that only the player's car triggers (the drones use DRONESND): COLLA.ASM RUNOVER / FLYCOLL
			// and PLYR.ASM CURBCOLP
			switch (m_cpu->reg(10))   // AR2
			{
			case 531: case 534: case 537: case 540: m_obj_hits[0]++; break;   // SAGESND..3: sage brush
			case 507: case 522: case 525: case 528: m_obj_hits[1]++; break;   // DRUMSND, SIGNSND, LAMPSND, DONGSND: barrels, signs, posts
			case 498: case 501: case 504: m_obj_hits[2]++; break;             // WALLHITA..C: the car is thrown back off the edge of the world
			default: break;
			}
			return false;
		}
		if (pc == dact_pc)
		{
			// hold the next section back while the section table is nearly full or the object pool runs low
			// (a section brings up to a few hundred objects; running out makes the game reset)
			bool hold = m_ram0[m_dgroup_count_addr] >= 16;
			if (!hold && m_ofree_addr)
			{
				int n = 0;
				uint32_t o = m_ram0[m_ofree_addr];
				while (o && n < 600)
				{
					uint32_t next;
					if (o < m_ram0.size()) next = m_ram0[o];
					else if (o >= 0x400000 && o - 0x400000 < m_ram1.size()) next = m_ram1[o - 0x400000];
					else break;
					o = next;
					n++;
				}
				hold = n < 500;
			}
			if (hold) { m_cpu->set_pc(dact_skip); return true; }
			return false;
		}
		if (pc == sync_pc)
		{
			// R0 still equals the polled word: nothing changes until the next interrupt
			if (m_ram0[m_idle_sync_addr] == m_cpu->reg(0))
			{
				m_cpu->skip_rest_of_slice();
				return true;
			}
			return false;
		}
		if (pc == sort_entry)
		{
			m_zsort_first = true;   // a new wait begins: the first pass must really run
			return false;
		}
		// loop top of the sorting wait: R6 is the "something was swapped" flag of the pass that just finished
		if (m_zsort_first)
		{
			m_zsort_first = false;
			return false;
		}
		if (m_cpu->reg(6) == 0 && m_ram0[m_idle_flag_addr] != 0)
		{
			m_cpu->skip_rest_of_slice();
			return true;
		}
		return false;
	};
}

#ifdef C3X_PROFILE
const uint64_t *MidVUnit::cpu_hits() const { return m_cpu->m_hits; }
#else
const uint64_t *MidVUnit::cpu_hits() const { return nullptr; }
#endif

int MidVUnit::free_objects() const
{
	if (!m_ofree_addr) return -1;
	int n = 0;
	uint32_t o = m_ram0[m_ofree_addr];
	while (o && n < 5000)
	{
		if (o < m_ram0.size()) o = m_ram0[o];
		else if (o >= 0x400000 && o - 0x400000 < m_ram1.size()) o = m_ram1[o - 0x400000];
		else break;
		n++;
	}
	return n;
}
