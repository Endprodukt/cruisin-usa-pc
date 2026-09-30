// GLSL sources shared by the OpenGL and Vulkan backends (compiled with -DVULKAN for SPIR-V).
#pragma once

namespace shader_src {

static const char *const kCommon = R"GLSL(
#version 450
#ifdef VULKAN
#define BIND(b) set = 0, binding = b
#define VID gl_VertexIndex
#else
#define BIND(b) binding = b
#define VID gl_VertexID
#endif
)GLSL";

// ---- quad pass: one instance per hardware quad --------------------------------------------
static const char *const kQuadVert = R"GLSL(
layout(location = 0) in vec4 a_p01;
layout(location = 1) in vec4 a_p23;
layout(location = 2) in vec4 a_t01;
layout(location = 3) in vec4 a_t23;
layout(location = 4) in uvec4 a_misc;

layout(location = 0) out vec2 v_pos;
layout(location = 1) flat out vec4 f_p01;
layout(location = 2) flat out vec4 f_p23;
layout(location = 3) flat out vec4 f_t01;
layout(location = 4) flat out vec4 f_t23;
layout(location = 5) flat out uvec4 f_misc;

void main()
{
	vec2 P[4] = vec2[4](a_p01.xy, a_p01.zw, a_p23.xy, a_p23.zw);
	vec2 lo = vec2(1e9), hi = vec2(-1e9);
	for (int i = 0; i < 4; i++)
	{
		lo = min(lo, P[i]);
		hi = max(hi, P[i]);
	}
	lo = clamp(floor(lo), vec2(0.0), vec2(512.0));
	hi = clamp(ceil(hi), vec2(0.0), vec2(512.0));
	vec2 corner = vec2(float(VID & 1), float(VID >> 1));
	vec2 pos = mix(lo, hi, corner);
	gl_Position = vec4(pos / 512.0 * 2.0 - 1.0, 0.0, 1.0);
	v_pos = pos;
	f_p01 = a_p01; f_p23 = a_p23; f_t01 = a_t01; f_t23 = a_t23; f_misc = a_misc;
}
)GLSL";

static const char *const kQuadFrag = R"GLSL(
layout(location = 0) in vec2 v_pos;
layout(location = 1) flat in vec4 f_p01;
layout(location = 2) flat in vec4 f_p23;
layout(location = 3) flat in vec4 f_t01;
layout(location = 4) flat in vec4 f_t23;
layout(location = 5) flat in uvec4 f_misc;

layout(BIND(0)) uniform usampler2D u_tex;    // 256 x 16384, R8UI texture RAM
layout(BIND(1)) uniform sampler2D u_pal;     // 256 x 128, RGBA8 palette (32768 entries)
layout(std140, BIND(3)) uniform Params { vec4 pa; vec4 pb; vec4 pc; } u_par;

layout(location = 0) out vec4 o_color;

// evaluate the two boundary edges of the quad at height y; returns false if fewer than 2 cross.
// clampsel: choose edges using y clamped into the vertical extent, but evaluate at the real y
// (extrapolation, like the hardware's scanline walker).
bool scan(vec2 P[4], vec2 T[4], float y, bool clampsel, out float xl, out float xr, out vec2 tl, out vec2 tr)
{
	float ymin = min(min(P[0].y, P[1].y), min(P[2].y, P[3].y));
	float ymax = max(max(P[0].y, P[1].y), max(P[2].y, P[3].y));
	float ys = clampsel ? clamp(y, ymin, ymax - 1e-4) : y;
	xl = 1e9; xr = -1e9; tl = vec2(0.0); tr = vec2(0.0);
	int cnt = 0;
	for (int i = 0; i < 4; i++)
	{
		int j = (i + 1) & 3;
		float ya = P[i].y, yb = P[j].y;
		if ((ya <= ys && ys < yb) || (yb <= ys && ys < ya))
		{
			float t = (y - ya) / (yb - ya);
			float x = mix(P[i].x, P[j].x, t);
			vec2 tc = mix(T[i], T[j], t);
			if (x < xl) { xl = x; tl = tc; }
			if (x > xr) { xr = x; tr = tc; }
			cnt++;
		}
	}
	return cnt >= 2;
}

vec3 palette(uint idx)
{
	idx &= 0x7fffu;
	return texelFetch(u_pal, ivec2(int(idx & 255u), int(idx >> 8)), 0).rgb;
}

uint texel(uint base, int col, int row)
{
	int lin = int(base) * 256 + (row & 255) * 256 + col;
	if (lin < 0 || lin >= 4194304) return 0u;
	return texelFetch(u_tex, ivec2(lin & 255, lin >> 8), 0).r;
}

void main()
{
	uint flags = f_misc.x, pix = f_misc.y, base = f_misc.z;
	vec2 P[4] = vec2[4](f_p01.xy, f_p01.zw, f_p23.xy, f_p23.zw);
	vec2 T[4] = vec2[4](f_t01.xy, f_t01.zw, f_t23.xy, f_t23.zw);

	// Hardware model: vertices sit at pixel centres (x + 0.5, plus 0.001 on "right/bottom" points);
	// a pixel is covered when its centre lies between the two boundary edges of the scanline and
	// the texture coordinate is interpolated along the edges and then across the scanline.
	// the page border strip (half an arcade pixel, only visible when upscaled) follows the first/last pixel row/column
	vec2 vp = clamp(v_pos, vec2(0.5), vec2(511.5));
	float xl, xr; vec2 tl, tr;
	if (!scan(P, T, vp.y, false, xl, xr, tl, tr)) discard;
	if (vp.x < xl || vp.x >= xr) discard;
	vec2 uv = mix(tl, tr, (vp.x - xl) / max(xr - xl, 1e-6));

	if ((flags & 0x2000u) != 0u)
	{
		ivec2 pp = ivec2(floor(v_pos));
		if (((pp.x ^ pp.y) & 1) != 0) discard;
	}

	bool textured = (flags & 0x300u) == 0x100u;
	uint mode = flags & 0xc00u;
	if (!textured || mode == 0x400u)
	{
		o_color = vec4(palette(pix + (flags & 0xffu)), 1.0);
		return;
	}

	bool filt = u_par.pa.x > 0.5;
	if (!filt)
	{
		ivec2 ti = ivec2(floor(uv));
		uint t = texel(base, ti.x, ti.y);
		if (mode != 0u && t == 0u) discard;
		uint idx = (mode == 0xc00u) ? (pix + (flags & 0xffu)) : (pix + t);
		o_color = vec4(palette(idx), 1.0);
		return;
	}

	// optional bilinear filtering on the resolved colours
	vec2 st = uv - 0.5;
	ivec2 i0 = ivec2(floor(st));
	vec2 f = st - vec2(i0);
	vec3 acc = vec3(0.0);
	float aw = 0.0;
	for (int k = 0; k < 4; k++)
	{
		int dx = k & 1, dy = k >> 1;
		float w = (dx == 1 ? f.x : 1.0 - f.x) * (dy == 1 ? f.y : 1.0 - f.y);
		uint t = texel(base, i0.x + dx, i0.y + dy);
		float a = (mode != 0u && t == 0u) ? 0.0 : 1.0;
		uint idx = (mode == 0xc00u) ? (pix + (flags & 0xffu)) : (pix + t);
		acc += palette(idx) * (w * a);
		aw += w * a;
	}
	if (aw < 0.5) discard;
	o_color = vec4(acc / aw, 1.0);
}
)GLSL";

// ---- fullscreen-rect pass (present + CPU overlay) ------------------------------------------------
// Params.pa = NDC rect (x0, y0, x1, y1) for corner (0,0) -> (1,1); Params.pb = uv rect (u0, v0, u1, v1)
static const char *const kRectVert = R"GLSL(
layout(std140, BIND(3)) uniform Params { vec4 pa; vec4 pb; vec4 pc; } u_par;
layout(location = 0) out vec2 v_uv;
void main()
{
	vec2 c = vec2(float(VID & 1), float(VID >> 1));
	vec2 ndc = mix(u_par.pa.xy, u_par.pa.zw, c);
	v_uv = mix(u_par.pb.xy, u_par.pb.zw, c);
	gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

static const char *const kPresentFrag = R"GLSL(
layout(location = 0) in vec2 v_uv;
layout(BIND(0)) uniform sampler2D u_page;
layout(std140, BIND(3)) uniform Params { vec4 pa; vec4 pb; vec4 pc; } u_par;
layout(location = 0) out vec4 o_color;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// FXAA (Lottes' console variant) on the scaled output; pc.x selects the strength: 1 light, 2 normal, 3 strong
void main()
{
	vec3 rgbM = texture(u_page, v_uv).rgb;
	if (u_par.pc.x < 0.5) { o_color = vec4(rgbM, 1.0); return; }
	vec2 px = vec2(abs(dFdx(v_uv.x)) + abs(dFdy(v_uv.x)), abs(dFdx(v_uv.y)) + abs(dFdy(v_uv.y)));
	float lvl = u_par.pc.x;
	float thr = lvl < 1.5 ? 0.125 : (lvl < 2.5 ? 0.0625 : 0.03125);
	float span = lvl < 1.5 ? 4.0 : (lvl < 2.5 ? 8.0 : 12.0);
	vec3 rgbNW = texture(u_page, v_uv + vec2(-1.0, -1.0) * px).rgb;
	vec3 rgbNE = texture(u_page, v_uv + vec2(1.0, -1.0) * px).rgb;
	vec3 rgbSW = texture(u_page, v_uv + vec2(-1.0, 1.0) * px).rgb;
	vec3 rgbSE = texture(u_page, v_uv + vec2(1.0, 1.0) * px).rgb;
	float lM = luma(rgbM), lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE);
	float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
	float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
	if (lMax - lMin < max(0.0312, lMax * thr)) { o_color = vec4(rgbM, 1.0); return; }
	vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
	float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
	float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
	dir = clamp(dir * rcpDirMin, vec2(-span), vec2(span)) * px;
	vec3 rgbA = 0.5 * (texture(u_page, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(u_page, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
	vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(u_page, v_uv + dir * -0.5).rgb + texture(u_page, v_uv + dir * 0.5).rgb);
	float lB = luma(rgbB);
	o_color = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}
)GLSL";

static const char *const kOverlayFrag = R"GLSL(
layout(location = 0) in vec2 v_uv;
layout(BIND(0)) uniform usampler2D u_ovl;    // 512 x 512 R16UI, bit 15 = valid
layout(BIND(1)) uniform sampler2D u_pal;
layout(location = 0) out vec4 o_color;
void main()
{
	ivec2 p = ivec2(floor(v_uv * 512.0));
	uint v = texelFetch(u_ovl, p, 0).r;
	if ((v & 0x8000u) == 0u) discard;
	v &= 0x7fffu;
	o_color = vec4(texelFetch(u_pal, ivec2(int(v & 255u), int(v >> 8)), 0).rgb, 1.0);
}
)GLSL";

// ---- shadow mask: hard coverage of the game's shadow quads (union of the triangles), no dither -----------
static const char *const kShadowMaskFrag = R"GLSL(
layout(location = 0) in vec2 v_pos;
layout(location = 1) flat in vec4 f_p01;
layout(location = 2) flat in vec4 f_p23;
layout(location = 3) flat in vec4 f_t01;
layout(location = 4) flat in vec4 f_t23;
layout(location = 5) flat in uvec4 f_misc;
layout(BIND(0)) uniform usampler2D u_tex;
layout(location = 0) out vec4 o_color;

uint texel(uint base, int col, int row)
{
	int lin = int(base) * 256 + (row & 255) * 256 + col;
	if (lin < 0 || lin >= 4194304) return 0u;
	return texelFetch(u_tex, ivec2(lin & 255, lin >> 8), 0).r;
}

// evaluate the two boundary edges of the quad at height y; returns false if fewer than 2 cross.
// clampsel: choose edges using y clamped into the vertical extent, but evaluate at the real y
// (extrapolation, like the hardware's scanline walker).
bool scan(vec2 P[4], vec2 T[4], float y, bool clampsel, out float xl, out float xr, out vec2 tl, out vec2 tr)
{
	float ymin = min(min(P[0].y, P[1].y), min(P[2].y, P[3].y));
	float ymax = max(max(P[0].y, P[1].y), max(P[2].y, P[3].y));
	float ys = clampsel ? clamp(y, ymin, ymax - 1e-4) : y;
	xl = 1e9; xr = -1e9; tl = vec2(0.0); tr = vec2(0.0);
	int cnt = 0;
	for (int i = 0; i < 4; i++)
	{
		int j = (i + 1) & 3;
		float ya = P[i].y, yb = P[j].y;
		if ((ya <= ys && ys < yb) || (yb <= ys && ys < ya))
		{
			float t = (y - ya) / (yb - ya);
			float x = mix(P[i].x, P[j].x, t);
			vec2 tc = mix(T[i], T[j], t);
			if (x < xl) { xl = x; tl = tc; }
			if (x > xr) { xr = x; tr = tc; }
			cnt++;
		}
	}
	return cnt >= 2;
}


void main()
{
	vec2 P[4] = vec2[4](f_p01.xy, f_p01.zw, f_p23.xy, f_p23.zw);
	vec2 T[4] = vec2[4](f_t01.xy, f_t01.zw, f_t23.xy, f_t23.zw);
	float xl, xr; vec2 tl, tr;
	if (!scan(P, T, v_pos.y, false, xl, xr, tl, tr)) discard;
	if (v_pos.x < xl || v_pos.x >= xr) discard;
	uint flags = f_misc.x, base = f_misc.z;
	uint mode = flags & 0xc00u;
	if ((flags & 0x300u) == 0x100u && (mode == 0x800u || mode == 0xc00u))
	{
		vec2 uv = mix(tl, tr, (v_pos.x - xl) / max(xr - xl, 1e-6));
		ivec2 ti = ivec2(floor(uv));
		if (texel(base, ti.x, ti.y) == 0u) discard;
	}
	o_color = vec4(1.0);
}
)GLSL";

// ---- shadow composite: blurred mask darkens the page -----------------------------------------------------
// Params.pc = (strength, radius in target pixels, 1/width, 1/height); pa/pb place the rect (whole page, uv 0..1)
static const char *const kShadowCompFrag = R"GLSL(
layout(location = 0) in vec2 v_uv;
layout(BIND(0)) uniform sampler2D u_mask;
layout(std140, BIND(3)) uniform Params { vec4 pa; vec4 pb; vec4 pc; } u_par;
layout(location = 0) out vec4 o_color;
void main()
{
	float radius = max(u_par.pc.y, 0.5);
	float sigma = radius * 0.5;
	vec2 texel = u_par.pc.zw;
	float step_px = max(1.0, radius / 3.0);
	float acc = 0.0, wsum = 0.0;
	for (int j = -3; j <= 3; j++)
		for (int i = -3; i <= 3; i++)
		{
			vec2 d = vec2(float(i), float(j)) * step_px;
			float w = exp(-dot(d, d) / (2.0 * sigma * sigma));
			acc += w * texture(u_mask, v_uv + d * texel).r;
			wsum += w;
		}
	float a = acc / wsum * u_par.pc.x;
	if (a <= 0.002) discard;
	o_color = vec4(0.0, 0.0, 0.0, a);
}
)GLSL";

} // namespace shader_src
