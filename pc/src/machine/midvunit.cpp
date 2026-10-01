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
#include "telemetry.h"

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
		texrepl.lookup(q.dma, m_textureram.data(), m_palette_rgb.data(), m_tex_gen);
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
		if (m_hud_spread > 0 && hud_shown())
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
		if (is2d && m_text_on && !full) shift = m_text_shift;   // a letter of a text string: the string is placed as a whole (text_begin)
		else if (is2d && m_hud_spread > 0 && hud_shown() && !full)
		{
			// only the HUD's corners move: the top and bottom bands and the course map at the right edge. Whatever is drawn in
			// the middle band (track names, banners, results) stays where it is, in one piece.
			const float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
			const bool band = cy < float(kHudTop) || cy >= float(kHudBottom);
			const bool map = cx > 420 && cy < 215 && maxx - minx < 90;
			if (band && cx < 171) shift = float(m_wide) * (1.0f - m_hud_spread);
			else if ((band || map) && cx > 341) shift = float(m_wide) * (1.0f + m_hud_spread);
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
		g.edge = texrepl.lookup(d, m_textureram.data(), m_palette_rgb.data(), m_tex_gen);
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

// A string of the game's text list is about to be drawn (hook in TEXT_OUTPUT), letter by letter. On a wide picture the
// placement is decided once for the whole string instead of per letter:
//  * A string that moves (the track names at the start and the end of a race, "NEXT RACE:", the banners) scrolls in from
//    outside the arcade's 512 pixels and parks out there again. The wide page shows that outside, so the way is stretched:
//    the string's distance from the middle is scaled so that "just off the arcade screen" becomes "just off the wide one".
//    The same goes for a string that lies outside altogether.
//  * With the HUD at the screen edges, a still string in the left or right third of the top or bottom rows goes to that edge
//    with the rest of the HUD (speed, elapsed time, gear). Per letter that tore apart every string reaching across two thirds.
//  * Everything else stays where the game puts it.
// The game adds the HUD's strings anew every frame, so a string is recognised by its text and row, not by its structure.
void MidVUnit::text_begin()
{
	auto rd = [&](uint32_t a) -> uint32_t {
		if (a < m_ram0.size()) return m_ram0[a];
		if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) return m_ram1[a - 0x400000];
		if (a >= 0xC00000 && a - 0xC00000 < m_rom.size()) return m_rom[a - 0xC00000];
		return 0;
	};
	m_text_on = true;
	m_text_shift = float(m_wide);
	if (!m_wide || !world_shown()) return;   // menus are shown as the arcade's 4:3 picture: nothing to place
	const uint32_t t = m_cpu->reg(12);   // AR4: TEXT_PTR 1, TEXT_POSX 3, TEXT_HEIGHT 9, TEXT_ADDR 10 (font table: 4 words per character)
	const uint32_t str = rd(t + 1), posx = rd(t + 3), font = rd(t + 10);
	const int x0 = int32_t(m_cpu->reg(2)), y0 = int32_t(m_cpu->reg(3)), height = int32_t(rd(t + 9));
	// the same string in the same row, its anchor elsewhere than a frame ago: it is on the move. Once it stands still it is
	// placed like any other string (the HUD's numbers slide in with the HUD at the start and then belong to their corner).
	// (a string and its drop shadow are two entries of the same text in the same row, three pixels apart)
	TextSeen *e = nullptr;
	for (TextSeen &k : m_text_seen) if (k.str == str && k.y == y0 && k.posx == posx && m_frame_count - k.frame <= 8) { e = &k; break; }
	if (e) e->moving = false;
	else
	{
		for (TextSeen &k : m_text_seen) if (k.str == str && k.y == y0 && k.frame != m_frame_count && m_frame_count - k.frame <= 8) { e = &k; break; }
		if (e) e->moving = true;
		else { e = &m_text_seen[m_text_next]; m_text_next = (m_text_next + 1) % int(std::size(m_text_seen)); *e = TextSeen{}; e->str = str; e->y = y0; }
	}
	e->posx = posx; e->frame = m_frame_count;
	// width as STRLEN computes it: characters packed four to a word, low byte first
	int w = 0, last = 8;
	for (int n = 0; n < 80; n++)
	{
		const uint32_t ch = (rd(str + uint32_t(n / 4)) >> (8 * (n % 4))) & 0xff;
		if (ch == 0) break;
		if (ch != ' ')
		{
			const uint32_t ent = font + ((ch == '/' ? uint32_t('@') : ch) - '0') * 4, e0 = rd(ent);
			last = int(rd(ent + 2)) - int(rd(ent + 1)) + int(int16_t(e0 & 0xffff)) + int(e0 >> 16);
			if (last < 0 || last > 64) last = 8;
		}
		w += last;
	}
	const int x1 = x0 + w;
	const float mid = float(x0 + x1) * 0.5f, half = float(w) * 0.5f;
	if (e->moving || x1 <= 0 || x0 >= 512)
		m_text_shift = float(m_wide) + (mid - 256.0f) * float(m_wide) / (256.0f + half);
	else if (m_hud_spread > 0 && hud_shown())
	{
		const float cy = float(y0) + float(height) * 0.5f;
		if (cy >= float(kHudTop) && cy < float(kHudBottom)) return;
		if (x1 <= 256 && mid < 171) m_text_shift = float(m_wide) * (1.0f - m_hud_spread);
		else if (x0 >= 256 && mid > 341) m_text_shift = float(m_wide) * (1.0f + m_hud_spread);
	}
}

// the race HUD is on screen: the race itself and the phases after the finish line (modes 5 and 7), so that the HUD does not jump
// back to the middle (leaving its pieces in the margins) when the race ends
bool MidVUnit::hud_shown() const
{
	const uint32_t m = m_ram0[0xC8F5] & 0xf;
	return m == 4 || m == 5 || m == 7;
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
	uint32_t sync_pc = ~0u, sort_entry = ~0u, sort_top = ~0u, dact_pc = ~0u, dact_skip = ~0u, snd_pc = ~0u, objinit_pc = ~0u, ofreecnt = 0, debris_ptr = 0, routine_pc = ~0u, routine_tab = 0, wdog_pc = ~0u, palq_pc = ~0u, palq_free = 0, palq_active = 0, hit_pc = ~0u, text_pc = ~0u, textend_pc = ~0u, carhit_pc = ~0u;
	m_ofree_addr = 0;
	m_wreck_addr = 0;
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
			// ... LDI (CAMERAPOSI),R2 / CALL GET_XZ_DISTANCE / CMPF ...   and   ... MPYI 5,AR1 / ADDI (DGROUPSI),AR1
			if (i >= 2 && (c[i - 2] & 0xffff0000u) == 0x08220000u) m_campos_ptr_addr = c[i - 2] & 0xffff;
			if ((c[i + 6] & 0xffff0000u) == 0x02290000u) m_dgroups_ptr_addr = c[i + 6] & 0xffff;
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
		// (f) SECTION_ROUTINE (OVERLAY.ASM), called when the car passes a point of the track that has a routine:
		//     CMPI 0,AR0 / RETSEQ / ADDI (ROUTINE_TABLEI),AR0 / LDI *AR0,AR0 / CALLU AR0. The hook sits on the CALLU.
		//     The towers of both San Francisco bridges share one palette: red for the first bridge; routine 12 (TOWER_PAL_LD) repaints
		//     it grey at a point between the bridges, routine 13 (TOWER_PAL_RESTORE) makes it red again. With the original view
		//     distance the second bridge only comes into sight after that point; with a longer one it is seen red first and turns
		//     grey on the way. So the repaint is done as soon as the car leaves the first bridge (routine 17 / 11, BRIDGE_OFF), which
		//     is behind the camera from then on.
		if (c[i] == 0x04E80000u && c[i + 1] == 0x78850000u && (c[i + 2] & 0xffff0000u) == 0x02280000u && c[i + 3] == 0x0848C000u &&
		    c[i + 4] == 0x70000008u && routine_pc == ~0u)
		{
			routine_pc = i + 4;
			routine_tab = c[c[i + 2] & 0xffff];
		}
		// PALXFER_GET (PALL.ASM): PUSH R0 / LDI 1,R0 / STI R0,(AVAILABLE) / LDI (PALXFER_FREE),AR0 / LDI *AR0,R0 / STI R0,(PALXFER_FREE) /
		// LDI (PALXFER_ACTIVE),R0 / STI R0,*AR0 / STI AR0,(PALXFER_ACTIVE).
		// Bug fix of the original: palette changes are queued (128 blocks) and the vblank interrupt carries out 12 per frame. With
		// the queue full, PALXFER_FREE is 0 and the release build takes block 0 without a check (the debug build stops there):
		// PAL_SET then writes its transfer over RAM words 0..3, the reset and interrupt vectors, and the next vblank interrupt
		// jumps into data. Loading a leg's palettes queues many at once; whether the queue overflows depends on how fast the CPU
		// queues compared to the vblank rate. When the queue is full the pending transfers are carried out at once (what the
		// interrupt would do over the next frames) and their blocks freed.
		if (c[i] == 0x0F200000u && c[i + 1] == 0x08600001u && (c[i + 2] & 0xffff0000u) == 0x15200000u && (c[i + 3] & 0xffff0000u) == 0x08280000u &&
		    c[i + 4] == 0x08400000u && c[i + 5] == (0x15200000u | (c[i + 3] & 0xffff)) && (c[i + 6] & 0xffff0000u) == 0x08200000u &&
		    c[i + 7] == 0x15400000u && c[i + 8] == (0x15280000u | (c[i + 6] & 0xffff)) && palq_pc == ~0u)
		{
			palq_pc = i + 3;
			palq_free = c[i + 3] & 0xffff;
			palq_active = c[i + 6] & 0xffff;
		}
		// COLSGCK (COLLA.ASM), "GOT A COLLISION": a car (AR0) touches an object of the sign or debris list (AR1):
		//   NOP / LDI *+AR0(OCARBLK),AR5 / LDI *+AR1(OID),R0 / AND TYPE_M,R0 / CMPI TSC_IGNORE,R0.
		// Every roadside object that can be driven into ends up here (signs, posts, lamps, bushes, barrels, barriers, cones,
		// trees, and the animals of ROADKILL.ASM), for the player and for the other cars. Hits of the player's car are counted
		// for the force feedback.
		if (c[i] == 0x0C800000u && c[i + 1] == 0x084D001Bu && c[i + 2] == 0x0840010Fu && c[i + 3] == 0x02E000F0u && c[i + 4] == 0x04E00060u && hit_pc == ~0u)
			hit_pc = i + 1;
		// TEXT_OUTPUT (TEXT.ASM), TEXT_RET: a string of the text list is about to be drawn, letter by letter:
		//   SUBI R0,R2 / CLRI RS / CMPI -32,RS / BNE +2 / CLRI RS / NOP *AR2++ / LDI *AR2,AR0 / LSH RS,AR0
		// (R2 = left edge, R3 = top, AR4 = the text structure); the routine starts 23 words before with PUSH AR4 .. BUD NXTGRP,
		// and TXTOUT, its end, is two words behind NXTGRP. The HUD placement treats a string as one piece (see text_begin).
		if (i >= 24 && c[i] == 0x18190019u && c[i + 1] == 0x04F9FFE0u && c[i + 2] == 0x6A060002u && c[i + 3] == 0x18190019u && c[i + 4] == 0x0CC02201u &&
		    c[i + 5] == 0x0848C200u && c[i + 6] == 0x09880019u && c[i - 1] == 0x18020000u && (c[i - 19] & 0xffff0000u) == 0x6A200000u && text_pc == ~0u)
		{
			text_pc = i;
			textend_pc = (i - 19) + 3 + (c[i - 19] & 0xffff) + 2;
		}
		// PLYRSPIN (COLLA.ASM SPINROT): another car has hit the player's car.
		//   CMPI (PLYCAR),AR1 / BNE DRONESPIN / CALL BEHINDCK / CMPF 50,R3 / BGT +3
		// At the CALL: AR0 = the other car, AR1 = the player's car, AR5 = its car block, R3 = closing speed, R0 = turn to give.
		if ((c[i] & 0xffff0000u) == 0x04A90000u && (c[i + 1] & 0xffff0000u) == 0x6A060000u && (c[i + 2] & 0xff000000u) == 0x62000000u &&
		    c[i + 3] == 0x04635480u && c[i + 4] == 0x6A090003u && carhit_pc == ~0u)
			carhit_pc = i + 2;
		// WRECKST (RACER.ASM): LDF -60,R0 / STF R0,*+AR4(OVELY) / LDI 1,R0 / STI R0,(WRECKFLG) / LDI 0,R0 / STI R0,*+AR5(CARSHAD)
		if (c[i] == 0x07605900u && c[i + 1] == 0x14400412u && c[i + 2] == 0x08600001u && (c[i + 3] & 0xffff0000u) == 0x15200000u &&
		    c[i + 4] == 0x08600000u && c[i + 5] == 0x15400541u && m_wreck_addr == 0)
			m_wreck_addr = c[i + 3] & 0xffff;
		// the watchdog in the vblank interrupt: ... ADDI 1,R0 / CMPI 300,R0 / BLE ok / BU error. The main loop did not get to
		// its process dispatch for 300 vblanks: it hangs. (Only counted and, with the jump trace on, reported.)
		if (c[i] == 0x02600001u && c[i + 1] == 0x04E0012Cu && (c[i + 2] & 0xffff0000u) == 0x6A080000u && wdog_pc == ~0u)
			wdog_pc = i + 3;
		// DRONE_VS_DEBRIS / DRONE_VS_SIGN: LDI (ROAD_DEBRISI),AR1 / BU +1 / LDI (SIGN_LISTI),AR1 / LDI (CAR_LIST),R0 / LDI R0,AR0 / RETSEQ
		if ((c[i] & 0xffff0000u) == 0x08290000u && c[i + 1] == 0x6A000001u && (c[i + 2] & 0xffff0000u) == 0x08290000u &&
		    (c[i + 3] & 0xffff0000u) == 0x08200000u && c[i + 4] == 0x08080000u && c[i + 5] == 0x78850000u && debris_ptr == 0)
			debris_ptr = c[i] & 0xffff;
		// OBJ_FREE: PUSH R0 / LDI (OFREE),R0 / STI R0,*AR2 / STI AR2,(OFREE)  -> head of the free object list
		if (c[i] == 0x0F200000u && (c[i + 1] & 0xffff0000u) == 0x08200000u && c[i + 2] == 0x1540C200u &&
		    (c[i + 3] & 0xffff0000u) == 0x152A0000u && (c[i + 1] & 0xffff) == (c[i + 3] & 0xffff) && m_ofree_addr == 0)
			m_ofree_addr = c[i + 1] & 0xffff;
	}
	if (routine_tab == 0 || routine_tab + 46 >= c.size()) routine_pc = ~0u;
	if (sync_pc == ~0u && sort_top == ~0u && dact_pc == ~0u && snd_pc == ~0u && objinit_pc == ~0u && routine_pc == ~0u)
		return;
	m_cpu->hook_pc[0] = sync_pc;
	m_cpu->hook_pc[1] = sort_entry;
	m_cpu->hook_pc[2] = sort_top;
	m_cpu->hook_pc[3] = dact_pc;
	m_cpu->hook_pc[4] = snd_pc;
	m_cpu->hook_pc[5] = objinit_pc;
	m_cpu->hook_pc[6] = routine_pc;
	m_cpu->hook_pc[7] = wdog_pc;
	m_cpu->hook_pc[8] = palq_pc;
	m_cpu->hook_pc[9] = hit_pc;
	m_cpu->hook_pc[10] = text_pc;
	m_cpu->hook_pc[11] = textend_pc;
	m_cpu->hook_pc[12] = carhit_pc;
	m_cpu->refresh_hooks();
	m_zsort_first = true;
	m_cpu->on_hook = [this, sync_pc, sort_entry, sort_top, dact_pc, dact_skip, snd_pc, objinit_pc, ofreecnt, debris_ptr, routine_pc, routine_tab, wdog_pc, palq_pc, palq_free, palq_active, hit_pc, text_pc, textend_pc, carhit_pc]() -> bool {
		uint32_t pc = m_cpu->pc();
		if (pc == text_pc) { text_begin(); return false; }
		if (pc == textend_pc) { m_text_on = false; return false; }
		if (pc == carhit_pc)
		{
			auto rd = [&](uint32_t a) -> uint32_t {
				if (a < m_ram0.size()) return m_ram0[a];
				if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) return m_ram1[a - 0x400000];
				return 0;
			};
			// where the other car is, seen from the player's car: the game's velocity is (cos, sin)(rot + pi/2) * speed in x, z
			const uint32_t other = m_cpu->reg(8), me = m_cpu->reg(9), blk = m_cpu->reg(13);
			const double dx = c3x_to_double(rd(other + 1)) - c3x_to_double(rd(me + 1)), dz = c3x_to_double(rd(other + 3)) - c3x_to_double(rd(me + 3));
			const double yrot = c3x_to_double(rd(blk + 44)) + 1.5707963, fx = std::cos(yrot), fz = std::sin(yrot), len = std::max(1.0, std::hypot(dx, dz));
			m_car_hit[0] = float(m_cpu->reg_float(3));
			m_car_hit[1] = float(m_cpu->reg_float(0));
			m_car_hit[2] = float((dx * fx + dz * fz) / len);
			m_car_hit[3] = float(std::fabs(dx * fz - dz * fx) / len);
			m_car_hits++;
			if (debug_routines) std::fprintf(stderr, "CARHIT frame %llu closing %.1f turn %+.3f long %+.2f lat %.2f\n", (unsigned long long)m_frame_count, m_car_hit[0], m_car_hit[1], m_car_hit[2], m_car_hit[3]);
			return false;
		}
		if (pc == hit_pc)
		{
			auto rd = [&](uint32_t a) -> uint32_t {
				if (a < m_ram0.size()) return m_ram0[a];
				if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) return m_ram1[a - 0x400000];
				return 0;
			};
			const uint32_t car = m_cpu->reg(8), obj = m_cpu->reg(9);
			if (rd(car + 0x1B) != m_ram0[0xE8A8] || m_ram0[0xE8A8] == 0) return false;   // not the player's car (OCARBLK != PLYCBLK)
			// an object stays in contact for several frames (a barrel flying along with the car): count it once
			for (int k = 0; k < 8; k++)
				if (m_hit_obj[k] == obj && m_frame_count - m_hit_frame[k] < 40) { m_hit_frame[k] = m_frame_count; return false; }
			m_hit_obj[m_hit_next] = obj; m_hit_frame[m_hit_next] = m_frame_count; m_hit_next = (m_hit_next + 1) & 7;
			const uint32_t id = rd(obj + 0xF), type = id & 0xF0, sub = id & 0xF;
			if (debug_routines) std::fprintf(stderr, "HIT frame %llu obj %06X id %04X\n", (unsigned long long)m_frame_count, obj, id);
			if (type == 0x60) return false;                                     // TSC_IGNORE
			if ((id & 0xFF0) == 0x750) m_obj_hits[sub == 3 ? 0 : 3]++;           // road kill: flying parts are light, the animal itself is heavy
			else if (type == 0x30) m_obj_hits[1]++;                              // TSC_FLYING: barrels, barriers, cones, mail boxes
			else if (type == 0x20) m_obj_hits[sub == 1 ? 0 : 1]++;               // TSC_RUNOVER: sage brush is light; signs, posts, lamps, small trees
			else m_obj_hits[4]++;                                                // immobile / hard: trees, poles, walls of rock
			return false;
		}
		if (pc == palq_pc)
		{
			if (m_ram0[palq_free] != 0) return false;
			palette_queue_overflows++;
			auto rd = [&](uint32_t a) -> uint32_t {
				if (a < m_ram0.size()) return m_ram0[a];
				if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) return m_ram1[a - 0x400000];
				if (a >= 0xC00000 && a - 0xC00000 < m_rom.size()) return m_rom[a - 0xC00000];
				if (a >= 0x809000 && a < 0x80A000) return m_iram_page[a - 0x809000];
				return 0;
			};
			// like PAL_XFER, but all of the queue: newest block first; a negative count marks a packed palette (two colours per word)
			uint32_t blk = m_ram0[palq_active];
			for (int guard = 0; blk && blk < m_ram0.size() - 4 && guard < 200; guard++)
			{
				const uint32_t next = m_ram0[blk];
				uint32_t src = m_ram0[blk + 1], dst = m_ram0[blk + 2];
				const uint32_t count = m_ram0[blk + 3];
				if (count & 0x80000000u)
					for (uint32_t n = (count << 1) >> 2; n > 0 && n <= 256; n--) { const uint32_t w = rd(src++); bus_write(dst++, w); bus_write(dst++, w >> 16); }
				else
					for (uint32_t n = count; n > 0 && n <= 512; n--) bus_write(dst++, rd(src++));
				m_ram0[blk] = m_ram0[palq_free];   // back onto the free list
				m_ram0[palq_free] = blk;
				blk = next;
			}
			m_ram0[palq_active] = 0;
			return false;
		}
		if (pc == wdog_pc)
		{
			watchdog_resets++;
			if (m_cpu->trace_jumps)
			{
				std::fprintf(stderr, "WATCHDOG at frame %llu, mode %X: the main loop hangs. Jumps before the reset:\n", (unsigned long long)m_frame_count, m_ram0[0xC8F5]);
				m_cpu->trace_dump(400);
			}
			return false;
		}
		if (pc == routine_pc)
		{
			const uint32_t target = m_cpu->reg(8);   // AR0 = the routine about to be called
			int idx = -1;
			for (int k = 1; k < 46; k++) if (m_ram0[routine_tab + uint32_t(k)] == target) { idx = k; break; }
			if (debug_routines) std::fprintf(stderr, "ROUTINE %d at frame %llu (mode %X)%s\n", idx, (unsigned long long)m_frame_count, m_ram0[0xC8F5], m_tower_armed ? " tower armed" : "");
			if (idx == 13) m_tower_armed = true;        // towers red (again): the first bridge is ahead
			else if (idx == 12) m_tower_armed = false;  // the game repaints them itself
			else if ((idx == 17 || idx == 11) && m_tower_armed && rom_patches.draw_distance_pct > 100)
			{
				// leaving the first bridge: do BRIDGE_OFF's work here and let the game call TOWER_PAL_LD instead
				m_tower_armed = false;
				const uint32_t boff = m_ram0[routine_tab + 17];
				if ((m_ram0[boff] & 0xffff0000u) == 0x08200000u && (m_ram0[boff + 1] & 0xffff0000u) == 0x03600000u)
				{
					m_ram0[m_ram0[boff] & 0xffff] &= ~(m_ram0[boff + 1] & 0xffff);   // _MODE &= ~MBRIDGE
					m_cpu->set_reg(8, m_ram0[routine_tab + 12]);
				}
			}
			return false;
		}
		if (pc == objinit_pc)
		{
			auto wr = [&](uint32_t a, uint32_t v) {
				if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) m_ram1[a - 0x400000] = v;
				else if (a < m_ram0.size()) m_ram0[a] = v;
			};
			if (debris_ptr) wr(m_ram0[debris_ptr], 0);   // ROAD_DEBRIS = empty: its objects no longer exist
			m_finish_loaded = false;                       // a new scene: no section of it is active yet
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
			// sound index (SNDTAB.EQU) that only the player's car triggers: PLYR.ASM CURBCOLP (objects are counted at the collision
			// routine itself, see hit_pc)
			switch (m_cpu->reg(10))   // AR2
			{
			case 498: case 501: case 504: m_obj_hits[2]++; break;             // WALLHITA..C: the car is thrown back off the edge of the world
			default: break;
			}
			return false;
		}
		if (pc == dact_pc)
		{
			// R0 = distance to the next section to activate, AR0 = its entry in the track table (routine index in bits 16..23).
			// hold the next section back while the section table is nearly full or the object pool runs low
			// (a section brings up to a few hundred objects; running out makes the game reset)
			bool hold = m_ram0[m_dgroup_count_addr] >= 16;
			// The legs of the trip follow each other in the track table, each with its own set of palettes, which is loaded
			// when the leg starts. Sections behind the finish line belong to the next leg: created before its palettes are
			// loaded they stop the game (OBJ_GETE: "CALL PAL_FIND / BC $", then the watchdog resets the board). The original
			// only gets 80000 units past the finish, where the data still uses the current leg's palettes, so behind the finish
			// (routines 32..45 BONUS1..14 and 19 END_OF_GAME) the original activation distance applies.
			{
				const uint32_t entry = m_cpu->reg(8);
				const uint32_t w = entry < m_ram0.size() ? m_ram0[entry] : (entry >= 0xC00000 && entry - 0xC00000 < m_rom.size()) ? m_rom[entry - 0xC00000] : 0;
				const uint32_t routine = (w >> 16) & 0xff;
				const double dist = m_cpu->reg_float(0);
				if (m_finish_loaded && dist > 80000.0) hold = true;
				// The game decides by the straight-line distance, but the tracks are not laid out as one world: US 101 winds
				// round in a loop of eight sections and comes back over its own road, a thousand units higher. Section 206 lies
				// on top of section 200. The original never has both loaded, since only 80000 units are ever there. Loaded
				// four times as far, the ground and the hills of the later section stand on the road being driven. So the
				// distance along the track has to be within the draw distance as well: from the camera to the anchor of the
				// section after the one the car is on, then anchor to anchor up to the new section. On a straight track that
				// is the same as the straight-line distance.
				if (!hold && rom_patches.draw_distance_pct > 100 && m_campos_ptr_addr && m_dgroups_ptr_addr)
				{
					auto rd = [&](uint32_t a) -> uint32_t {
						if (a < m_ram0.size()) return m_ram0[a];
						if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) return m_ram1[a - 0x400000];
						if (a >= 0xC00000 && a - 0xC00000 < m_rom.size()) return m_rom[a - 0xC00000];
						if (a >= 0x809000 && a < 0x80A000) return m_iram_page[a - 0x809000];   // the camera position is in on-chip RAM
						return 0;
					};
					const uint32_t groups = m_ram0[m_dgroup_count_addr], cam = m_ram0[m_campos_ptr_addr], tab = m_ram0[m_dgroups_ptr_addr];
					if (groups >= 1 && groups <= 20)
					{
						const double cx = c3x_to_double(rd(cam)), cz = c3x_to_double(rd(cam + 2));
						double ax[21], az[21];
						for (uint32_t g = 0; g < groups; g++)
						{
							const uint32_t bin = rd(tab + g * 5 + 1);
							ax[g] = c3x_to_double(rd(bin + 1)); az[g] = c3x_to_double(rd(bin + 3));
						}
						ax[groups] = c3x_to_double(rd(entry + 1)); az[groups] = c3x_to_double(rd(entry + 3));
						// the section the car is on: that of the nearest road piece (PLYCBLK -> CARTRAK -> OUSR1 >> 8); without a
						// car (attract mode) the first section whose successor's anchor is still ahead, i.e. the nearest anchor
						int own = -1;
						if ((m_ram0[0xC8F5] & 0xf) == 4)
						{
							const uint32_t blk = m_ram0[0xE8A8], piece = blk ? rd(blk + 55) : 0;
							if (piece)
							{
								const uint32_t idx = rd(piece + 0x1E) >> 8;
								for (uint32_t g = 0; g < groups; g++) if (rd(tab + g * 5 + 4) == idx) { own = int(g); break; }
							}
						}
						if (own < 0)
						{
							double nearest = 1e30;
							for (uint32_t g = 0; g < groups; g++)
							{
								const double d = std::hypot(ax[g] - cx, az[g] - cz);
								if (d < nearest) { nearest = d; own = int(g); }
							}
						}
						double path = std::hypot(ax[own + 1] - cx, az[own + 1] - cz);
						for (uint32_t g = uint32_t(own) + 1; g < groups; g++) path += std::hypot(ax[g + 1] - ax[g], az[g + 1] - az[g]);
						if (path > 80000.0 * double(rom_patches.draw_distance_pct) / 100.0 * 1.25) hold = true;
						// ...and the loop is short (seven sections round), so even within that distance the track is back at the
						// car: a section that is much nearer in a straight line than along the road has curled back towards
						// the camera and waits until the car has gone far enough round to see it from the other side
						if (path > 100000.0 && dist < path * 0.6) hold = true;
					}
				}
				if (!hold && ((routine >= 32 && routine <= 45) || routine == 19)) m_finish_loaded = true;   // this section may load
			}
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
