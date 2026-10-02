// Midway V-Unit (Cruis'n USA) machine: TMS320C31 + video DMA + inputs.
// Hardware behaviour follows MAME's midvunit driver (BSD-3-Clause,
// copyright-holders: Aaron Giles); this is a standalone re-implementation.
#pragma once
#include <map>
#include <unordered_map>
#include "rom_patches.h"
#include "../video/texrepl.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../audio/dcs.h"
#include "dcs_worker.h"
#include "../video/video_backend.h"
#include "../cpu/tms320c3x/tms320c3x.h"

// Everything the host feeds into the machine. Bits are active-low like the real boards.
struct MachineInputs
{
	uint16_t in0  = 0xffff;   // coins/start/test/service/volume/gear (active low)
	uint16_t in1  = 0x007f;   // radio + 3 view buttons (active low), motion bits (active high)
	uint16_t dsw  = 0xf9bf;   // dip switches (link master, motion off)
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
	// diagnostics for the headless tool
	tms320c3x_device &cpu_debug() { return *m_cpu; }
	void ram_poke(uint32_t a, uint32_t v) { if (a < m_ram0.size()) m_ram0[a] = v; else if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) m_ram1[a - 0x400000] = v; }
	uint32_t ram_peek(uint32_t a) const { return (a >= 0x809000 && a < 0x80A000) ? m_iram_page[a - 0x809000] : a < m_ram0.size() ? m_ram0[a] : (a >= 0x400000 && a - 0x400000 < m_ram1.size()) ? m_ram1[a - 0x400000] : 0xDEADBEEFu; }
	bool in_attract() const { return (m_ram0[0xC8F5] & 0xf) == 2; }
	// The 3D world fills the picture: a race, its finish (bonus), the attract mode's demo race, or the garage of the car
	// selection (a 3D room: the car on the outer lift reaches past the 4:3 picture). Everything else is a menu or a 2D screen
	// made for the arcade's 4:3 picture (boot, race and transmission selection, initials, continue, high scores, logos).
	// At the end of a race the road map unfolds over the picture; once it is open the game loads other palettes (the trophy
	// girls') and sometimes another backdrop behind it, which shows in the margins. From then on: 4:3 with black bars.
	bool world_shown() const { const uint32_t m = m_ram0[0xC8F5]; return (m & 0xf) == 4 || ((m & 0xf) == 5 && !m_map_full) || ((m & 0xf) == 2 && (m & 0x200)) || m_garage; }
	uint32_t m_activehi_addr = 0;          // ACTIVEHI1 / ACTIVEHI in RAM (the object lists' active window), see run_frame
	bool m_map_full = false;               // the road map of the bonus screen is fully unfolded
	bool m_garage = false;                 // the car selection is on screen (updated once per frame)   // _MODE == MATTR: the attract mode (nobody is playing)
	uint64_t palette_queue_overflows = 0;  // times the palette transfer queue was full and was flushed by the host
	uint32_t rom_word(uint32_t i) const { return i < m_rom.size() ? m_rom[i] : 0; }
	uint64_t watchdog_resets = 0;          // times the game's watchdog fired (the main loop hung)
	bool debug_routines = false;           // print the track's section routines as they are called (headless)
	bool auto_overclock = true;            // raise the CPU clock while a longer draw distance needs it (see update_clock)
	int cpu_overclock = 1;                 // 1..4: more main CPU instructions per emulated time (the game's heavy frames finish sooner)
	bool skip_raster = false;              // benchmarking: do not rasterise quads on the CPU
	int clock_q4() const { return m_oc_q4; }
	// 0: the machine as it is. 1: a game frame that would miss the vblank it is due at gets the instructions it still needs
	// (catch_up); frames that are in time run exactly as with 0. 2: the same before every vblank (the first version, kept for
	// the comparison in docs/PERFORMANCE.md; it changes where in the frame the interrupts fall).
	int steady_cadence = 1;
	double steady_budget_ms = 0.0;         // optional host time limit per display frame for catch_up (0 = none: the emulation stays deterministic)
	uint64_t instr_total = 0, instr_extra = 0;   // instructions given to the main CPU / of which by catch_up
	uint64_t cycles_total() const { return m_cycles_total; }
	uint64_t catchups = 0, catchup_fails = 0;   // due game frames finished that way / frames that were late even so
	void catch_up();
	uint32_t m_mwait0_pc = 0;              // MWAIT0 in the main loop (the frame's work is done, the governor holds it)
	double m_frame_cpu0 = 0;               // interpreter time (without the time spent in the video backend) at the start of run_frame   // emulated CPU clock, in quarters of the original
	bool quad_stats = false;               // diagnostics: screen area of the quads' bounding boxes and of the quads themselves
	double stat_bbox = 0, stat_poly = 0; uint64_t stat_quads = 0;
	uint64_t page_flips = 0;               // displayed page changes = new pictures shown (game frame rate)
	int dcs_thread = 0;                    // sound DSP on its own thread: -1 auto (4+ hardware threads), 0 off, 1 on
	bool idle_skip = true;                 // fast-forward the game's wait loops (see setup_idle_hooks)
	void debug_ram_usage() const;
	int free_objects() const;
	uint32_t ram_word(uint32_t a) const { return a < m_ram0.size() ? m_ram0[a] : 0; }
	// performance counters (read and cleared by the front end)
	double perf_cpu_ms = 0, perf_dcs_ms = 0;   // main CPU interpreter / sound DSP time
	double perf_feed_ms = 0;               // time spent handing quads / state to the video backend
	uint64_t perf_draws = 0, perf_quads = 0;
	const uint64_t *cpu_hits() const;
	bool m_pchist_on = false;
	std::map<uint32_t, uint32_t> m_pchist;
	bool read_telemetry(struct Telemetry &t) const;   // player car state for motion / force feedback
	RomPatchOptions rom_patches;           // applied to the RAM copy at reset

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
	void set_shadow_mode(int m) { m_shadow_mode = m; }
	void set_wide_margin(int m) { m_wide = m; }
	TexRepl texrepl;                         // texture export / replacement (configure + load before attach_video_backend)
	void set_hud_spread(float f) { m_hud_spread = f; }   // 0 = HUD in the 4:3 centre, 1 = pushed to the screen edges
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
	std::vector<uint32_t> &nvram() { return m_nvram; }
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
	const uint32_t *palette_rgb() const { return m_palette_rgb.data(); }
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
	void setup_idle_hooks();
	uint32_t m_campos_ptr_addr = 0, m_dgroups_ptr_addr = 0;   // CAMERAPOSI / DGROUPSI (pointers in the code's constant area)
	bool m_finish_loaded = false;          // the section with this leg's finish line is active (see the section activation hook)
	bool m_tower_armed = false;            // the bridge tower palette is still the first bridge's (see setup_idle_hooks, (f))
	int m_oc_q4 = 4;                       // effective CPU clock factor in quarters (4 = original speed)
	uint32_t m_framrate_addr = 0;          // FRAMRATE, the game's frame governor (minimum vblanks per frame - 1)
	void update_clock();
	uint32_t m_idle_flag_addr = 0, m_idle_sync_addr = 0, m_dgroup_count_addr = 0, m_ofree_addr = 0;
	uint32_t m_car_hits = 0; float m_car_hit[4] = {};   // other cars hitting the player's car (see Telemetry)
	uint32_t m_wreck_addr = 0;             // WRECKFLG
	uint32_t m_obj_hits[5] = {};   // player hits: light (bushes, debris), road objects, walls, animals, immobile objects
	uint32_t m_hit_obj[8] = {}; uint64_t m_hit_frame[8] = {}; int m_hit_next = 0;   // objects hit lately (one count per contact)
	bool m_zsort_first = true;
	void sync_dcs();

	// GPU feed
	IVideoBackend *m_gpu = nullptr;
	std::vector<GpuQuad> m_gq;
	std::vector<GpuQuad> m_gs;             // shadow quads (modern shadow mode), same page as m_gq_page
	float m_hud_spread = 0;
	std::unordered_map<uint32_t, uint8_t> m_pc2d;   // writer PC -> is the 2D (rdma) routine
	bool writer_is_2d();
	bool in_race() const;
	bool hud_shown() const;
	void text_begin();
	struct TextSeen { uint32_t str = 0, posx = 0; int y = 0; uint64_t frame = 0; bool moving = false; int place = 0; };
	TextSeen m_text_seen[64];
	int m_text_next = 0;
	bool m_text_on = false;                // a string of the text list is being drawn
	float m_text_shift = 0;                // ...and this is where its letters go (page pixels added to the game's x)
	static constexpr int kHudTop = 110, kHudBottom = 290;   // rows of the HUD's top and bottom bands (the middle stays centred)
	uint64_t m_tex_gen = 1;                 // bumped on every texture RAM write
	bool m_repl_pending = false;
	int m_wide = 0;                        // widescreen: extra page pixels on each side (quads are shifted by this)
	uint32_t m_cpu_page_writes[2] = {};    // video RAM words the CPU wrote per page since the last vblank (CPU-drawn screens)
	int m_shadow_mode = 0;                 // 0 original, 1 modern, 2 off
	int m_gq_page = 0;
	int m_present_page = 0;
	int m_pal_lo = 0x7fffffff, m_pal_hi = -1;
	int m_tex_lo = 0x7fffffff, m_tex_hi = -1;
	int m_ovl_lo[2] = {0x7fffffff, 0x7fffffff}, m_ovl_hi[2] = {-1, -1};
	std::vector<uint16_t> m_cpu_layer;     // CPU-written video RAM pixels, bit 15 = valid
	void gpu_flush_quads();
	void gpu_flush_shadows();
	void gpu_sync_state();
	void gpu_add_quad(const VQuad &q);
	void dcs_write(uint8_t d);
	uint64_t m_dcs_synced = 0;
	double   m_dcs_ahead = 0;
	std::unique_ptr<DcsWorker> m_dcs_worker;
	void dcs_direct() { if (m_dcs_worker) m_dcs_worker->drain(); }   // before touching m_dcs on the main thread
	void dcs_end_frame();
	// with the worker the DSP's sound output is collected and handed to on_audio at the end of the frame (on the main thread)
	struct AudioEvent { int count; double rate; int enable; };   // enable: -1 = `count` samples, 0/1 = on_audio_enable
	std::vector<AudioEvent> m_aud_ev;
	std::vector<int16_t> m_aud_pcm;

	std::vector<uint32_t> m_code_orig;      // pristine first 128K words of the ROM (patches are applied to m_rom at reset)
	std::vector<uint32_t> m_ram0, m_ram1;   // 0x000000 / 0x400000, 128K words each
	std::vector<uint32_t> m_rom;            // maindata, mapped at 0xc00000 (4M words)
	std::vector<uint32_t> m_nvram;          // 0x2000 words
	std::vector<uint32_t> m_paletteram;     // 0x8000 words
	std::vector<uint16_t> m_videoram;       // 0x80000 entries, two 512x512 pages
	std::vector<uint8_t>  m_textureram;     // 4 MB
	std::vector<uint32_t> m_palette_rgb;    // 32768 entries, 0x00RRGGBB
	uint32_t m_iram_page[0x1000] = {};      // page 0x809000: the TMS320C31 internal RAM sits in its upper half (mapped as a fast page)
	uint32_t *const m_iram = m_iram_page + 0x800;   // 0x809800
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
