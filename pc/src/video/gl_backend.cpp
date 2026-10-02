// OpenGL 4.5 core backend
#include "video_backend.h"

#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "shader_src.h"

#pragma comment(lib, "opengl32.lib")
#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x0C11
#endif

namespace {

// ---- minimal GL loader ----------------------------------------------------------------------
using GLchar_ = char;
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ARRAY_BUFFER 0x8892
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE0 0x84C0
#define GL_RGBA8 0x8058
#define GL_R8UI 0x8232
#define GL_R16UI 0x8234
#define GL_RED_INTEGER 0x8D94
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TRIANGLE_STRIP 0x0005
#define GL_MAX_TEXTURE_SIZE_ 0x0D33
#define GL_R8 0x8229
#define GL_TEXTURE_2D_ARRAY 0x8C1A

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

#define GL_FUNCS(X)                                                                                     \
	X(GLuint, glCreateShader, (GLenum))                                                                 \
	X(void, glShaderSource, (GLuint, GLsizei, const GLchar_ *const *, const GLint *))                   \
	X(void, glCompileShader, (GLuint))                                                                  \
	X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                                   \
	X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar_ *))                                \
	X(void, glDeleteShader, (GLuint))                                                                   \
	X(GLuint, glCreateProgram, ())                                                                      \
	X(void, glAttachShader, (GLuint, GLuint))                                                           \
	X(void, glLinkProgram, (GLuint))                                                                    \
	X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                                  \
	X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar_ *))                               \
	X(void, glUseProgram, (GLuint))                                                                     \
	X(void, glGenVertexArrays, (GLsizei, GLuint *))                                                     \
	X(void, glBindVertexArray, (GLuint))                                                                \
	X(void, glGenBuffers, (GLsizei, GLuint *))                                                          \
	X(void, glBindBuffer, (GLenum, GLuint))                                                             \
	X(void, glBufferData, (GLenum, ptrdiff_t, const void *, GLenum))                                    \
	X(void, glBufferSubData, (GLenum, ptrdiff_t, ptrdiff_t, const void *))                             \
	X(void, glBindBufferBase, (GLenum, GLuint, GLuint))                                                 \
	X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))           \
	X(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *))                     \
	X(void, glVertexAttribDivisor, (GLuint, GLuint))                                                    \
	X(void, glEnableVertexAttribArray, (GLuint))                                                        \
	X(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei))                                   \
	X(void, glGenFramebuffers, (GLsizei, GLuint *))                                                     \
	X(void, glBindFramebuffer, (GLenum, GLuint))                                                        \
	X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                            \
	X(GLenum, glCheckFramebufferStatus, (GLenum))                                                       \
	X(void, glActiveTexture, (GLenum))                                                                  \
	X(void, glTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                                \
	X(void, glTexStorage3D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei))                       \
	X(void, glTexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
	X(void, glGenQueries, (GLsizei, GLuint *))                                                          \
	X(void, glBeginQuery, (GLenum, GLuint))                                                             \
	X(void, glEndQuery, (GLenum))                                                                       \
	X(void, glGetQueryObjectiv, (GLuint, GLenum, GLint *))                                              \
	X(void, glGetQueryObjectui64v, (GLuint, GLenum, unsigned long long *))                              \
	X(void, glCopyImageSubData, (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei))

#define X(ret, name, args) using PFN_##name = ret(APIENTRY *) args;
GL_FUNCS(X)
#undef X
#define X(ret, name, args) PFN_##name name = nullptr;
GL_FUNCS(X)
#undef X

using PFNWGLCREATECONTEXTATTRIBSARB = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
using PFNWGLSWAPINTERVALEXT = BOOL(WINAPI *)(int);

bool load_gl_functions()
{
#define X(ret, name, args)                                                     \
	name = reinterpret_cast<PFN_##name>(wglGetProcAddress(#name));             \
	if (!name) { std::fprintf(stderr, "GL: missing %s\n", #name); return false; }
	GL_FUNCS(X)
#undef X
	return true;
}

struct Params { float a[4]; float b[4]; float c[4]; };

class GlBackend final : public IVideoBackend
{
public:
	const char *name() const override { return "OpenGL 4.5"; }

	bool init(HWND hwnd, const VideoOptions &opt, std::string &err) override
	{
		m_hwnd = hwnd;
		m_opt = opt;
		m_opt.scale = std::clamp(m_opt.scale, 1, 8);
		m_dc = GetDC(hwnd);

		PIXELFORMATDESCRIPTOR pfd{};
		pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
		pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
		int pf = ChoosePixelFormat(m_dc, &pfd);
		if (!pf || !SetPixelFormat(m_dc, pf, &pfd)) { err = "OpenGL: pixel format failed"; return false; }

		HGLRC tmp = wglCreateContext(m_dc);
		wglMakeCurrent(m_dc, tmp);
		auto create_attribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARB>(wglGetProcAddress("wglCreateContextAttribsARB"));
		if (!create_attribs) { err = "OpenGL: wglCreateContextAttribsARB unavailable"; return false; }
		const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 5,
		                       WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
		m_rc = create_attribs(m_dc, nullptr, attribs);
		wglMakeCurrent(nullptr, nullptr);
		wglDeleteContext(tmp);
		if (!m_rc) { err = "OpenGL 4.5 core context creation failed"; return false; }
		wglMakeCurrent(m_dc, m_rc);
		if (!load_gl_functions()) { err = "OpenGL: missing functions"; return false; }
		m_swap_interval = reinterpret_cast<PFNWGLSWAPINTERVALEXT>(wglGetProcAddress("wglSwapIntervalEXT"));
		if (m_swap_interval) m_swap_interval(m_opt.vsync ? 1 : 0);

		GLint maxtex = 0;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE_, &maxtex);
		if (maxtex < 16384) { err = "OpenGL: GL_MAX_TEXTURE_SIZE < 16384"; return false; }

		if (!build_programs(err)) return false;
		create_resources();
		RECT r; GetClientRect(hwnd, &r);
		m_win_w = std::max<int>(r.right, 1); m_win_h = std::max<int>(r.bottom, 1);
		return true;
	}

	void shutdown() override
	{
		if (m_rc) { wglMakeCurrent(m_dc, m_rc); wglMakeCurrent(nullptr, nullptr); wglDeleteContext(m_rc); m_rc = nullptr; }
		if (m_dc) { ReleaseDC(m_hwnd, m_dc); m_dc = nullptr; }
	}

	void resize(int w, int h) override { m_win_w = std::max(w, 1); m_win_h = std::max(h, 1); }

	void set_options(const VideoOptions &o) override
	{
		bool rescale = o.scale != m_opt.scale || o.wide_margin != m_opt.wide_margin || mask_scale(o) != mask_scale(m_opt);
		m_opt = o;
		m_opt.scale = std::clamp(m_opt.scale, 1, 8);
		if (m_swap_interval) m_swap_interval(m_opt.vsync ? 1 : 0);
		if (rescale) create_pages();
		apply_page_filter();
	}

	void upload_palette(const uint32_t *argb, int first, int last) override
	{
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, m_tex_pal);
		int r0 = first >> 8, r1 = last >> 8;
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r0, 256, r1 - r0 + 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb + r0 * 256);
	}

	bool init_replacements(int res, int layers) override
	{
		if (m_tex_repl) glDeleteTextures(1, &m_tex_repl);
		m_tex_repl = 0;
		glGenTextures(1, &m_tex_repl);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_tex_repl);
		glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, res, res, layers);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
		m_repl_res = res;
		return glGetError() == 0;
	}

	void upload_replacement(int layer, const uint8_t *rgba) override
	{
		if (!m_tex_repl || m_repl_res <= 0) return;
		glActiveTexture(GL_TEXTURE0 + 2);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_tex_repl);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, m_repl_res, m_repl_res, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	}

	void upload_texture_rows(const uint8_t *ram, int first, int last) override
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_tex_ram);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, first, 256, last - first + 1, GL_RED_INTEGER, GL_UNSIGNED_BYTE, ram + size_t(first) * 256);
	}

	void upload_overlay(int page, const uint16_t *layer, int first, int last) override
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_tex_ovl);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, first, 512, last - first + 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, layer + size_t(first) * 512);

		bind_page(page);
		glUseProgram(m_prog_ovl);
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, m_tex_pal);
		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(m_vao_empty);
		// three strips of the 512 px layer, each placed with its own offset (HUD placement)
		// (only in the top and bottom bands; the middle band is placed as a whole, so that text there is not cut apart)
		static const int cut[4] = {0, 171, 341, 512};
		static const int band[4] = {0, kHudBandTop, kHudBandBottom, 512};
		const float W = page_w_px();
		for (int b = 0; b < 3; b++)
			for (int k = 0; k < 3; k++)
			{
				const int off = b == 1 ? m_ovl_off[1] : m_ovl_off[k];
				float x0 = float(cut[k] + off), x1 = float(cut[k + 1] + off);
				float y0 = -1 + 2 * float(band[b]) / 512.0f, y1 = -1 + 2 * float(band[b + 1]) / 512.0f;
				set_params({-1 + 2 * x0 / W, y0, -1 + 2 * x1 / W, y1}, {float(cut[k]) / 512.0f, float(band[b]) / 512.0f, float(cut[k + 1]) / 512.0f, float(band[b + 1]) / 512.0f});
				glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
			}
	}

	void clear_margins(int page) override
	{
		if (m_opt.wide_margin <= 0) return;
		bind_page(page);
		const int m = m_opt.wide_margin * m_opt.scale;
		glEnable(GL_SCISSOR_TEST);
		glClearColor(0, 0, 0, 1);
		glScissor(0, 0, m, page_h());
		glClear(GL_COLOR_BUFFER_BIT);
		glScissor(page_w() - m, 0, m, page_h());
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
	}

	void set_overlay_offsets(int l, int c, int r) override { m_ovl_off[0] = l; m_ovl_off[1] = c; m_ovl_off[2] = r; }
	void set_pillarbox(bool on) override { m_pillar = on; }
	bool m_pillar = false;

	void draw(int page, const GpuQuad *q, int count) override
	{
		if (count <= 0) return;
		bind_page(page);
		glUseProgram(m_prog_quad);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_tex_ram);
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, m_tex_pal);
		glActiveTexture(GL_TEXTURE0 + 2);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_tex_repl);
		glActiveTexture(GL_TEXTURE0);
		set_params({m_opt.filter_textures ? 1.0f : 0.0f, page_w_px(), m_opt.scale > 1 ? 1.0f : 0.0f, 0}, {0, 0, 0, 0});
		glBindVertexArray(m_vao_quad);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_inst);
		for (int done = 0; done < count;)
		{
			int n = std::min(count - done, MAX_BATCH);
			glBufferData(GL_ARRAY_BUFFER, ptrdiff_t(n) * sizeof(GpuQuad), q + done, GL_DYNAMIC_DRAW);
			glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
			done += n;
		}
	}

	void draw_shadows(int page, const GpuQuad *q, int count) override
	{
		if (count <= 0) return;
		// Soft shadows in three small steps instead of one large one. Before, every pixel of the batch's rectangle on the page
		// read the mask 49 times: at 6x and with a few cars near the camera that alone took longer than a display refresh
		// (15-20 ms on the GPU, the cause of the hitches in traffic). Now the mask is drawn at no more than twice the arcade
		// resolution (a soft shadow has no finer detail), blurred there into a second mask, and the page reads that one once
		// per pixel. Each step only touches the batch's rectangle; the first reaches a radius further than the second, so
		// that the blur never reads what an earlier shadow left in the mask.
		const int sz = page_h(), pw = page_w();
		const int ms = mask_scale(m_opt), mw = mask_w(), mh = mask_h();
		const float sc = float(m_opt.scale), radius = std::max(0.5f, m_opt.shadow_soft);   // in arcade pixels
		float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
		for (int i = 0; i < count; i++)
			for (int k = 0; k < 4; k++)
			{
				minx = std::min(minx, q[i].p[k * 2]); maxx = std::max(maxx, q[i].p[k * 2]);
				miny = std::min(miny, q[i].p[k * 2 + 1]); maxy = std::max(maxy, q[i].p[k * 2 + 1]);
			}
		// the batch's rectangle in arcade pixels, grown by `g` radii, in pixels of a target that is `k` times the arcade size
		auto rect = [&](float g, float k, int w, int h, int r[4]) {
			r[0] = std::clamp(int(std::floor((minx - radius * g - 1) * k)), 0, w); r[2] = std::clamp(int(std::ceil((maxx + radius * g + 1) * k)), 0, w);
			r[1] = std::clamp(int(std::floor((miny - radius * g - 1) * k)), 0, h); r[3] = std::clamp(int(std::ceil((maxy + radius * g + 1) * k)), 0, h);
			return r[2] > r[0] && r[3] > r[1];
		};
		int rm[4], rb[4], rp[4];
		if (!rect(2, float(ms), mw, mh, rm) || !rect(1, float(ms), mw, mh, rb) || !rect(1, sc, pw, sz, rp)) return;

		// 1. hard coverage of the shadow quads into the mask
		glBindFramebuffer(GL_FRAMEBUFFER, m_mask_fbo);
		glViewport(0, 0, mw, mh);
		glEnable(GL_SCISSOR_TEST);
		glScissor(rm[0], rm[1], rm[2] - rm[0], rm[3] - rm[1]);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(m_prog_smask);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_tex_ram);
		set_params({0, page_w_px(), 0, 0}, {0, 0, 0, 0});
		glBindVertexArray(m_vao_quad);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_inst);
		for (int done = 0; done < count;)
		{
			int n = std::min(count - done, MAX_BATCH);
			glBufferData(GL_ARRAY_BUFFER, ptrdiff_t(n) * sizeof(GpuQuad), q + done, GL_DYNAMIC_DRAW);
			glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
			done += n;
		}

		// 2. blurred into the second mask
		glBindFramebuffer(GL_FRAMEBUFFER, m_mask2_fbo);
		glScissor(rb[0], rb[1], rb[2] - rb[0], rb[3] - rb[1]);
		glUseProgram(m_prog_sblur);
		glBindTexture(GL_TEXTURE_2D, m_mask_tex);
		set_params({-1, -1, 1, 1}, {0, 0, 1, 1}, {0, radius * float(ms), 1.0f / float(mw), 1.0f / float(mh)});
		glBindVertexArray(m_vao_empty);
		glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);

		// 3. the blurred mask darkens the page
		bind_page(page);
		glScissor(rp[0], rp[1], rp[2] - rp[0], rp[3] - rp[1]);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glUseProgram(m_prog_scomp);
		glBindTexture(GL_TEXTURE_2D, m_mask2_tex);
		set_params({-1, -1, 1, 1}, {0, 0, 1, 1}, {m_opt.shadow_strength, 0, 0, 0});
		glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
		glDisable(GL_BLEND);
		glDisable(GL_SCISSOR_TEST);
	}

	void latch(int page, int visible_rows) override
	{
		int rows = std::clamp(visible_rows, 1, 512) * m_opt.scale;
		glCopyImageSubData(m_page_tex[page & 1], GL_TEXTURE_2D, 0, 0, 0, 0, m_disp_tex, GL_TEXTURE_2D, 0, 0, 0, 0, page_w(), rows, 1);
	}

	void present(int vis_w, int vis_h) override
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, m_win_w, m_win_h);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);

		// letterbox rect in pixels
		float dw = float(m_win_w), dh = float(m_win_h);
		float tw = dw, th = dh;
		const bool pillar = m_pillar && m_opt.wide_margin > 0 && m_opt.keep_aspect;
		if (m_opt.keep_aspect)
		{
			float a = pillar ? 4.0f / 3.0f : m_opt.aspect;
			if (dw / dh > a) { th = dh; tw = dh * a; } else { tw = dw; th = dw / a; }
			if (m_opt.integer_scale)
			{
				float k = std::max(1.0f, std::floor(th / float(vis_h)));
				th = k * vis_h; tw = th * a;
				if (tw > dw) { tw = dw; th = tw / a; }
			}
		}
		float x0 = (dw - tw) * 0.5f, y0 = (dh - th) * 0.5f;
		// GL NDC: +y is up; image row 0 (top) must appear at the top
		Params p{};
		float nx0 = x0 / dw * 2 - 1, nx1 = (x0 + tw) / dw * 2 - 1;
		float ny_top = 1 - y0 / dh * 2, ny_bot = 1 - (y0 + th) / dh * 2;
		const float u0 = pillar ? float(m_opt.wide_margin) / page_w_px() : 0.0f, u1 = float(vis_w + (pillar ? 1 : 2) * m_opt.wide_margin) / page_w_px();
		set_params({nx0, ny_top, nx1, ny_bot}, {u0, 0, u1, float(vis_h) / 512.0f}, {float(m_opt.aa), 0, 0, 0});
		(void)p;

		glUseProgram(m_prog_present);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_disp_tex);
		glBindVertexArray(m_vao_empty);
		glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
		if (!m_prof) { SwapBuffers(m_dc); return; }

		// profiling: one timer query spans all GL work of a frame (swap to swap); its result is read a few frames later,
		// when it is available, so that reading never waits for the GPU
		if (m_q_open) { glEndQuery(0x88BF); m_q_open = false; m_q_head = (m_q_head + 1) & 3; m_q_used = std::min(m_q_used + 1, 4); }
		while (m_q_used > 1)
		{
			const int oldest = (m_q_head - m_q_used) & 3;
			GLint ready = 0;
			glGetQueryObjectiv(m_query[oldest], 0x8867, &ready);
			if (!ready) break;
			unsigned long long ns = 0;
			glGetQueryObjectui64v(m_query[oldest], 0x8866, &ns);
			m_gpu_ms = double(ns) / 1e6;
			m_q_used--;
		}
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t0);
		SwapBuffers(m_dc);
		QueryPerformanceCounter(&t1);
		m_swap_ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
		if (m_q_used < 4)
		{
			if (!m_query[m_q_head]) glGenQueries(1, &m_query[m_q_head]);
			glBeginQuery(0x88BF, m_query[m_q_head]);
			m_q_open = true;
		}
	}

	void set_profiling(bool on) override { m_prof = on; }
	double last_swap_ms() const override { return m_swap_ms; }
	double last_gpu_ms() const override { return m_gpu_ms; }
	bool m_prof = false, m_q_open = false;
	GLuint m_query[4] = {0, 0, 0, 0};
	int m_q_head = 0, m_q_used = 0;
	double m_swap_ms = -1.0, m_gpu_ms = -1.0;

	bool read_display(std::vector<uint32_t> &out, int &w, int &h) override
	{
		w = page_w(); h = page_h();
		out.resize(size_t(w) * h);
		glBindTexture(GL_TEXTURE_2D, m_disp_tex);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
		return true;
	}

private:
	static constexpr int MAX_BATCH = 8192;

	GLuint compile(GLenum type, const char *body, std::string &err)
	{
		const char *srcs[2] = {shader_src::kCommon, body};
		GLuint s = glCreateShader(type);
		glShaderSource(s, 2, srcs, nullptr);
		glCompileShader(s);
		GLint ok = 0;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			char log[2048];
			glGetShaderInfoLog(s, sizeof(log), nullptr, log);
			err = std::string("GLSL compile error: ") + log;
			return 0;
		}
		return s;
	}

	GLuint link(const char *vs, const char *fs, std::string &err)
	{
		GLuint v = compile(GL_VERTEX_SHADER, vs, err);
		if (!v) return 0;
		GLuint f = compile(GL_FRAGMENT_SHADER, fs, err);
		if (!f) return 0;
		GLuint p = glCreateProgram();
		glAttachShader(p, v);
		glAttachShader(p, f);
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			char log[2048];
			glGetProgramInfoLog(p, sizeof(log), nullptr, log);
			err = std::string("GLSL link error: ") + log;
			return 0;
		}
		glDeleteShader(v);
		glDeleteShader(f);
		return p;
	}

	bool build_programs(std::string &err)
	{
		m_prog_quad = link(shader_src::kQuadVert, shader_src::kQuadFrag, err);
		m_prog_present = link(shader_src::kRectVert, shader_src::kPresentFrag, err);
		m_prog_ovl = link(shader_src::kRectVert, shader_src::kOverlayFrag, err);
		m_prog_smask = link(shader_src::kQuadVert, shader_src::kShadowMaskFrag, err);
		m_prog_scomp = link(shader_src::kRectVert, shader_src::kShadowCompFrag, err);
		m_prog_sblur = link(shader_src::kRectVert, shader_src::kShadowBlurFrag, err);
		return m_prog_quad && m_prog_present && m_prog_ovl && m_prog_smask && m_prog_scomp && m_prog_sblur;
	}

	void new_tex(GLuint &t, GLenum fmt, int w, int h)
	{
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	void create_resources()
	{
		new_tex(m_tex_ram, GL_R8UI, 256, 16384);
		new_tex(m_tex_pal, GL_RGBA8, 256, 128);
		new_tex(m_tex_ovl, GL_R16UI, 512, 512);
		{
			// a one-layer placeholder so that the array sampler is always backed by a texture
			glGenTextures(1, &m_tex_repl);
			glBindTexture(GL_TEXTURE_2D_ARRAY, m_tex_repl);
			glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 1);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		}

		glGenVertexArrays(1, &m_vao_empty);
		glGenVertexArrays(1, &m_vao_quad);
		glGenBuffers(1, &m_vbo_inst);
		glGenBuffers(1, &m_ubo);
		glBindVertexArray(m_vao_quad);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_inst);
		glBufferData(GL_ARRAY_BUFFER, ptrdiff_t(MAX_BATCH) * sizeof(GpuQuad), nullptr, GL_DYNAMIC_DRAW);
		for (int i = 0; i < 4; i++)
		{
			glEnableVertexAttribArray(i);
			glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(GpuQuad), reinterpret_cast<const void *>(size_t(i) * 16));
			glVertexAttribDivisor(i, 1);
		}
		glEnableVertexAttribArray(4);
		glVertexAttribIPointer(4, 4, GL_UNSIGNED_INT, sizeof(GpuQuad), reinterpret_cast<const void *>(64));
		glVertexAttribDivisor(4, 1);
		glBindVertexArray(0);

		glBindBuffer(GL_UNIFORM_BUFFER, m_ubo);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(Params), nullptr, GL_DYNAMIC_DRAW);
		glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_ubo);

		create_pages();
	}

	void create_pages()
	{
		for (int i = 0; i < 2; i++)
		{
			if (m_page_tex[i]) { glDeleteTextures(1, &m_page_tex[i]); m_page_tex[i] = 0; }
			if (i == 0 && m_disp_tex) { glDeleteTextures(1, &m_disp_tex); m_disp_tex = 0; }
			if (m_fbo[i]) { /* FBOs are re-attached below; reuse the object */ }
		}
		int sz = page_h(), pw = page_w();
		for (int i = 0; i < 2; i++)
		{
			new_tex(m_page_tex[i], GL_RGBA8, pw, sz);
			if (!m_fbo[i]) glGenFramebuffers(1, &m_fbo[i]);
			glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_page_tex[i], 0);
			glViewport(0, 0, pw, sz);
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		new_tex(m_disp_tex, GL_RGBA8, pw, sz);
		// the two shadow masks (see draw_shadows)
		for (int i = 0; i < 2; i++)
		{
			GLuint &tex = i ? m_mask2_tex : m_mask_tex, &fbo = i ? m_mask2_fbo : m_mask_fbo;
			if (tex) { glDeleteTextures(1, &tex); tex = 0; }
			new_tex(tex, GL_R8, mask_w(), mask_h());
			if (!fbo) glGenFramebuffers(1, &fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
			glViewport(0, 0, mask_w(), mask_h());
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		apply_page_filter();
	}

	void apply_page_filter()
	{
		GLint f = (m_opt.smooth_output || m_opt.aa > 0) ? GL_LINEAR : GL_NEAREST;
		for (int i = 0; i < 3; i++)
		{
			glBindTexture(GL_TEXTURE_2D, i == 2 ? m_disp_tex : m_page_tex[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
		}
	}

	void bind_page(int page)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[page & 1]);
		glViewport(0, 0, page_w(), page_h());
	}

	int m_ovl_off[3] = {0, 0, 0};
	GLuint m_tex_repl = 0;
	int m_repl_res = 0;
	// resolution of the shadow masks, as a multiple of the arcade's: a soft shadow needs no more than twice, a hard one the page's
	static int mask_scale(const VideoOptions &o) { const int sc = std::clamp(o.scale, 1, 8); return o.shadow_soft >= 1.0f ? std::min(sc, 2) : sc; }
	int mask_w() const { return (512 + 2 * m_opt.wide_margin) * mask_scale(m_opt); }
	int mask_h() const { return 512 * mask_scale(m_opt); }
	int page_w() const { return (512 + 2 * m_opt.wide_margin) * m_opt.scale; }
	int page_h() const { return 512 * m_opt.scale; }
	float page_w_px() const { return float(512 + 2 * m_opt.wide_margin); }

	struct V4 { float x, y, z, w; };
	void set_params(V4 a, V4 b, V4 c = {0, 0, 0, 0})
	{
		Params p{{a.x, a.y, a.z, a.w}, {b.x, b.y, b.z, b.w}, {c.x, c.y, c.z, c.w}};
		glBindBuffer(GL_UNIFORM_BUFFER, m_ubo);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(p), &p);
		glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_ubo);
	}

	HWND m_hwnd = nullptr;
	HDC m_dc = nullptr;
	HGLRC m_rc = nullptr;
	PFNWGLSWAPINTERVALEXT m_swap_interval = nullptr;
	VideoOptions m_opt;
	int m_win_w = 1, m_win_h = 1;

	GLuint m_prog_quad = 0, m_prog_present = 0, m_prog_ovl = 0, m_prog_smask = 0, m_prog_scomp = 0;
	GLuint m_mask_tex = 0, m_mask_fbo = 0, m_mask2_tex = 0, m_mask2_fbo = 0, m_prog_sblur = 0;
	GLuint m_vao_empty = 0, m_vao_quad = 0, m_vbo_inst = 0, m_ubo = 0;
	GLuint m_tex_ram = 0, m_tex_pal = 0, m_tex_ovl = 0;
	GLuint m_page_tex[2] = {0, 0}, m_fbo[2] = {0, 0}, m_disp_tex = 0;
};

} // namespace

std::unique_ptr<IVideoBackend> create_gl_backend() { return std::make_unique<GlBackend>(); }
