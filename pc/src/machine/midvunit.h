// Midway V-Unit (Cruis'n USA) machine: TMS320C31 + video DMA + inputs.
// Hardware behaviour follows MAME's midvunit driver (BSD-3-Clause,
// copyright-holders: Aaron Giles); this is a standalone re-implementation.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../audio/dcs.h"
#include "../video/video_backend.h"
#include "../cpu/tms320c3x/tms320c3x.h"

// Everything the host feeds into the machine. Bits are active-low like the real boards.
struct MachineInputs
{
	uint16_t in0  = 0xffff;   // coins/start/test/service/volume/gear (active low)
	uint16_t in1  = 0x007f;   // radio + 3 view buttons (active low), motion bits (active high)
	uint16_t dsw  = 0xf9fe;   // dip switches
	uint8_t  wheel = 0x80;    // ADC ch1, 0x10..0xf0, 0x80 = centre
	uint8_t  accel = 0x00;    // ADC ch2
	uint8_t  brake = 0x00;    // ADC ch3
	int      gear  = 0;       // 0 = neutral/none, 1..4  (H-pattern emulation on IN0)
};

// IN0 bit assignments
namespace in0bit {
constexpr uint16_t COIN1 = 0x0001, COIN2 = 0x0002, START = 0x0004, TILT = 0x0008, TEST = 0x0010,
                   SERVICE = 0x0040, COIN3 = 0x0080, VOLDN = 0x0100, VOLUP = 0x0200,
                   GEAR4 = 0x0400, GEAR3 = 0x0800, GEAR2 = 0x1000, GEAR1 = 0x2000, COIN4 = 0x4000;
}
namespace in1bit {
constexpr uint16_t RADIO = 0x0002, VIEW1 = 0x0010, VIEW2 = 0x0020, VIEW3 = 0x0040;
}

// One textured/flat quad as submitted by the game through the DMA queue.
struct VQuad
{
	uint16_t dma[16];      // raw queue words
	uint8_t  page;         // destination video page (0/1)
};

class MidVUnit
{
public:
	MidVUnit();
	~MidVUnit();

	bool load_roms(const std::string &zip_path, const std::string &version, std::string &err);
	void reset();

	// emulate one full video frame; returns true if the visible image changed
	bool run_frame();

	MachineInputs inputs;

	// ---- video output (what the "monitor" shows) ---------------------------------------
	int screen_w() const { return m_vis_w; }
	int screen_h() const { return m_vis_h; }
	const uint32_t *frame_rgba() const { return m_frame.data(); }   // stride = 512
	static constexpr int FRAME_STRIDE = 512;
	double refresh_hz() const { return m_refresh_hz; }

	// ---- GPU video path -------------------------------------------------------------------------
	// With a backend attached, quads are rendered on the GPU (soft rasterizer disabled).
	void attach_video_backend(IVideoBackend *gpu);
	void present_gpu();                       // flush pending draws and present the display page
	int  display_page() const { return m_present_page; }

	// ---- outputs ------------------------------------------------------------------------
	uint8_t wheel_motor = 0;     // WHLCTLZ latch (force feedback command)
	uint8_t lamps[8] = {};       // DRVCTLZ optional drivers

	// audio hooks (DCS)
	std::function<void(uint8_t)> on_sound_data;
	std::function<void(int)> on_dcs_reset;
	std::function<void(const int16_t *, int, double)> on_audio;   // mono PCM blocks + rate
	std::function<void(bool)> on_audio_enable;

	// ---- persistent state ---------------------------------------------------------------
	void load_default_nvram();                 // embedded, pre-calibrated CMOS image
	bool load_nvram(const std::string &path);
	bool save_nvram(const std::string &path) const;

	// ---- debug ---------------------------------------------------------------------------
	uint32_t cpu_pc() const { return m_cpu->pc(); }
	uint64_t total_cycles() const { return m_cycles_total; }
	uint64_t frames() const { return m_frame_count; }
	uint64_t quads_last_frame = 0;
	uint64_t stat_vram_reads = 0, stat_vram_writes = 0, stat_pal_writes = 0, stat_tex_writes = 0;
	Dcs1 *dcs() { return m_dcs.get(); }
	const uint8_t *texture_ram() const { return m_textureram.data(); }
	const uint16_t *video_ram() const { return m_videoram.data(); }

private:
	friend struct MachineBusThunks;

	// bus
	uint32_t bus_read(offs_t addr);
	void     bus_write(offs_t addr, uint32_t data);
	static uint32_t read_thunk(void *ctx, offs_t addr) { return static_cast<MidVUnit *>(ctx)->bus_read(addr); }
	static void     write_thunk(void *ctx, offs_t addr, uint32_t d) { static_cast<MidVUnit *>(ctx)->bus_write(addr, d); }

	uint32_t io_read(offs_t addr);
	void     io_write(offs_t addr, uint32_t data);

	uint64_t now_cycles() const;

	// video
	void dma_queue_write(uint32_t data);
	void dma_trigger();
	void video_control_write(int reg, uint32_t data);
	void page_control_write(uint32_t data);
	void update_screen_rows(int from, int to, int page);   // inclusive rows
	void draw_quad(const VQuad &q);
	void configure_screen();

	// wheel board / galil
	void wheel_board_write(uint32_t data);
	void galil_set_input(const char *s);

	// ADC
	void adc_write(uint8_t v);
	uint8_t adc_read();
	void adc_conversion_done();

	// members ---------------------------------------------------------------------------
	std::unique_ptr<tms320c3x_device::bus_t> m_bus;
	std::unique_ptr<tms320c3x_device> m_cpu;
	std::unique_ptr<Dcs1> m_dcs;
	int run_cpu(int cycles);
	void sync_dcs();

	// GPU feed
	IVideoBackend *m_gpu = nullptr;
	std::vector<GpuQuad> m_gq;
	int m_gq_page = 0;
	int m_present_page = 0;
	int m_pal_lo = 0x7fffffff, m_pal_hi = -1;
	int m_tex_lo = 0x7fffffff, m_tex_hi = -1;
	int m_ovl_lo[2] = {0x7fffffff, 0x7fffffff}, m_ovl_hi[2] = {-1, -1};
	std::vector<uint16_t> m_cpu_layer;     // CPU-written video RAM pixels, bit 15 = valid
	void gpu_flush_quads();
	void gpu_sync_state();
	void gpu_add_quad(const VQuad &q);
	void dcs_write(uint8_t d);
	uint64_t m_dcs_synced = 0;
	double   m_dcs_ahead = 0;

	std::vector<uint32_t> m_ram0, m_ram1;   // 0x000000 / 0x400000, 128K words each
	std::vector<uint32_t> m_rom;            // maindata, mapped at 0xc00000 (4M words)
	std::vector<uint32_t> m_nvram;          // 0x2000 words
	std::vector<uint32_t> m_paletteram;     // 0x8000 words
	std::vector<uint16_t> m_videoram;       // 0x80000 entries, two 512x512 pages
	std::vector<uint8_t>  m_textureram;     // 4 MB
	std::vector<uint32_t> m_palette_rgb;    // 32768 entries, 0x00RRGGBB
	uint32_t m_iram[0x800] = {};            // TMS320C31 internal RAM at 0x809800
	uint32_t m_ctrl[0x80] = {};             // TMS320C31 peripheral registers at 0x808000

public:
	std::vector<uint8_t> sound_rom;         // 16 MB DCS data region (16-bit LE words, high byte 0xff)

private:
	// control
	uint16_t m_control_data = 0;
	uint8_t  m_cmos_protected = 0;
	double   m_timer_rate = 10000000.0;
	uint64_t m_timer_start[2] = {0, 0};

	// video state
	uint16_t m_video_regs[16] = {};
	uint16_t m_dma_data[16] = {};
	uint8_t  m_dma_data_index = 0;
	uint16_t m_page_control = 0;
	int      m_vtotal = 432, m_htotal = 666;
	int      m_vis_w = 512, m_vis_h = 400;
	double   m_refresh_hz = 33333333.0 / 2.0 / (666.0 * 432.0);
	int      m_vpos = 0;
	int      m_irq_line = -1;               // scanline (+1) at which IRQ0 is raised, -1 = none
	std::vector<uint32_t> m_frame;          // 512 x 512 RGBA-ish (0x00RRGGBB)
	int      m_partial_next_row = 0;
	bool     m_video_changed = true;

	// scheduling
	uint64_t m_cycles_total = 0;   // cycles completed before the current slice
	int      m_slice_len = 0;
	uint64_t m_frame_count = 0;
	double   m_cycle_frac = 0;
	uint64_t m_adc_event = ~0ull;

	// ADC
	uint8_t  m_adc_channel = 0x0f;
	uint8_t  m_adc_result = 0xff;
	bool     m_adc_intr = false;
	uint8_t  m_adc_shift = 24;

	// wheel board / galil
	uint8_t  m_wheel_board_output = 0;
	uint32_t m_wheel_board_last = 0;
	uint32_t m_wheel_board_u8_latch = 0;
	uint8_t  m_galil_input_index = 0, m_galil_input_length = 0;
	const char *m_galil_input = "";
	uint16_t m_galil_output_index = 0;
	char     m_galil_output[450] = {};

	// comm (link) stub
	uint8_t  m_comm_flags = 0;
	uint16_t m_comm_data = 0;
};
