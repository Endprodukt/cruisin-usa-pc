// GPU video backend interface (OpenGL / Vulkan).
//
// The V-Unit video hardware is a list of textured/flat quads drawn into two 512x512 paletted
// pages. The backends render those quads at an N x internal resolution, resolving the
// palette at draw time, and present the display page scaled to the window.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// One hardware quad, exactly as the shader wants it (20 words, used as instance data).
struct GpuQuad
{
	float p[8];              // x0 y0 x1 y1 x2 y2 x3 y3: 1x pixel coordinates (unshifted, integer values)
	float t[8];              // u0 v0 .. u3 v3: texel coordinates at the vertices (texel centre = +0.5)
	uint32_t flags;          // DMA word 0 (mode bits)
	uint32_t pixdata;        // DMA word 1 (palette base)
	uint32_t texbase;        // DMA word 14 (texture base in 256-byte rows)
	uint32_t edge;           // texture replacement: layer + 1 of the block holding texbase (low 16 bits) and of the next block (high 16 bits)
};
static_assert(sizeof(GpuQuad) == 80, "GpuQuad layout");

struct VideoOptions
{
	int  scale = 2;              // internal resolution multiplier, 1..8
	bool vsync = true;
	bool filter_textures = false;   // bilinear texture filtering (off = authentic point sampling)
	int wide_margin = 0;            // extra arcade pixels on each side of the 512 px wide page (widescreen)
	int aa = 0;                     // post anti-aliasing on the scaled output: 0 off, 1..3 FXAA light/normal/strong
	bool smooth_output = true;      // linear filtering when scaling the final image to the window
	bool integer_scale = false;
	bool keep_aspect = true;
	float aspect = 4.0f / 3.0f;     // arcade monitor aspect ratio (display area)
	float shadow_strength = 0.55f;  // darkness of modern shadows (0..1)
	float shadow_soft = 3.0f;       // penumbra radius in arcade (1x) pixels
};

class IVideoBackend
{
public:
	virtual ~IVideoBackend() = default;

	virtual const char *name() const = 0;
	virtual bool init(HWND hwnd, const VideoOptions &opt, std::string &err) = 0;
	virtual void shutdown() = 0;
	virtual void resize(int width, int height) = 0;
	virtual void set_options(const VideoOptions &opt) = 0;   // live changes (filtering, vsync, aspect)

	// state uploads (inclusive first/last indices)
	virtual void upload_palette(const uint32_t *argb32768, int first, int last) = 0;
	// optional texture replacement pack: `layers` RGBA8 pages of res x res, selected per polygon by GpuQuad::edge (layer + 1)
	virtual bool init_replacements(int res, int layers) = 0;
	virtual void upload_replacement(int layer, const uint8_t *rgba) = 0;
	virtual void upload_texture_rows(const uint8_t *ram4mb, int first_row, int last_row) = 0;   // 256-byte rows
	// CPU-drawn pixels (index | 0x8000 marks a valid pixel) composited into a page
	// where the CPU-drawn layer (HUD text and gauges) is placed on the wide page, as extra pixels from the page's left edge for the
	// left / centre / right third of the 512 px picture
	virtual void set_overlay_offsets(int left, int centre, int right) = 0;
	static constexpr int kHudBandTop = 110, kHudBandBottom = 290;   // rows: above / below these the HUD thirds are placed separately
	virtual void upload_overlay(int page, const uint16_t *layer512, int first_row, int last_row) = 0;

	virtual void draw(int page, const GpuQuad *quads, int count) = 0;
	// the game's shadow quads (flat dithered triangles): rendered as a soft blended shadow instead
	virtual void draw_shadows(int page, const GpuQuad *quads, int count) = 0;
	// freeze the given page as the displayed image (what the monitor shows at vblank); draws recorded
	// afterwards (the game already renders the next frame) must not show up
	virtual void latch(int page, int visible_rows) = 0;   // freeze the picture of a page (only the rows the monitor shows are copied)
	virtual void present(int vis_w, int vis_h) = 0;      // draw the latched image to the window

	// grab the latched (displayed) image at internal resolution (RGBA8, top-down)
	// widescreen: paint the parts of a page outside the 512 px picture black (screens the CPU draws, e.g. boot text)
	virtual void clear_margins(int page) { (void)page; }
	// widescreen: show only the arcade's 4:3 picture, black bars beside it (menus and 2D screens)
	virtual void set_pillarbox(bool on) { (void)on; }
	virtual double last_gpu_ms() const { return -1.0; }   // GPU time of the last finished frame (-1 = not measured)
	virtual bool read_display(std::vector<uint32_t> &out, int &w, int &h) = 0;
};

std::unique_ptr<IVideoBackend> create_gl_backend();
std::unique_ptr<IVideoBackend> create_vk_backend();
