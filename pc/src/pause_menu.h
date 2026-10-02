// Pause menu: CONTINUE / RETURN TO ATTRACT / EXIT GAME, drawn with the game's own fonts onto the picture that is on display.
//
// The fonts are the ones the game's text routine uses (TEXT.ASM): a table of four words per character from '0' on (leading
// space | trailing space << 16, first and last texel column, first texel row), an image in the texture memory, a height and a
// palette. The font is found by the code that selects it (SET18FONT, the game's standard font; the title uses it at twice the
// size, the larger fonts being digits only), so nothing here depends on addresses of one program version. Its palette is read
// from the program's palette table and put, for as long as the menu is up, into palette rows the picture on display no
// longer needs (it is already drawn); MidVUnit::ui_draw hands the game's palette back when the game runs again.
#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include "machine/midvunit.h"

class PauseMenu
{
public:
	enum Choice { None, Continue, Attract, Exit };

	bool is_open() const { return m_open; }
	void open(MidVUnit &m)
	{
		if (!m_looked) { find_fonts(m); m_looked = true; }
		m_open = true; m_sel = 0;
		draw(m);
	}
	void close() { m_open = false; }

	// one step: the edges of up / down / select; returns what was chosen
	Choice update(MidVUnit &m, bool up, bool down, bool select)
	{
		if (!m_open) return None;
		const int old = m_sel;
		if (up) m_sel = (m_sel + kItems - 1) % kItems;
		if (down) m_sel = (m_sel + 1) % kItems;
		if (m_sel != old) draw(m);
		if (select) return m_sel == 0 ? Continue : m_sel == 1 ? Attract : Exit;
		return None;
	}

private:
	static constexpr int kItems = 3;
	static constexpr uint32_t kPalSmall = 0x7E00, kPalUi = 0x7F00;   // palette rows used while the menu is up
	struct Font
	{
		uint32_t table = 0, img = 0, pal_index = 0;
		int height = 0;
		std::vector<uint32_t> rgb;
		bool ok = false;
	};
	Font m_small;
	bool m_looked = false, m_open = false;
	int m_sel = 0;

	// "LDI h,R0 / STI R0,*+AR0(TEXT_HEIGHT) / LDI @image,R0 / STI R0,*+AR0(TEXT_IMG) / LDI palette,AR2 / CALL PAL_FIND /
	//  STI R0,*+AR0(TEXT_PAL) / LDI @table,R0 / STI R0,*+AR0(TEXT_ADDR) / RETS"
	void find_fonts(const MidVUnit &m)
	{
		uint32_t palrom = 0;
		for (uint32_t i = 0; i + 10 < 0x20000; i++)
		{
			const uint32_t w0 = m.ram_word(i);
			if ((w0 & 0xffffff00u) != 0x08600000u || m.ram_word(i + 1) != 0x15400009u || (m.ram_word(i + 2) & 0xffff0000u) != 0x08200000u ||
			    m.ram_word(i + 3) != 0x1540000Bu || (m.ram_word(i + 5) & 0xff000000u) != 0x62000000u || m.ram_word(i + 6) != 0x1540000Cu ||
			    (m.ram_word(i + 7) & 0xffff0000u) != 0x08200000u || m.ram_word(i + 8) != 0x1540000Au || m.ram_word(i + 9) != 0x78800000u)
				continue;
			const uint32_t w4 = m.ram_word(i + 4);
			uint32_t pal;
			if ((w4 & 0xffff0000u) == 0x082A0000u) pal = m.ram_word(w4 & 0xffff);
			else if ((w4 & 0xffff0000u) == 0x086A0000u) pal = w4 & 0xffff;
			else continue;
			const int h = int(w0 & 0xff);
			Font *f = h == 17 ? &m_small : nullptr;
			if (!f || f->ok) continue;
			f->height = h; f->img = m.ram_word(m.ram_word(i + 2) & 0xffff); f->table = m.ram_word(m.ram_word(i + 7) & 0xffff); f->pal_index = pal;
			if (!palrom)
			{
				// PAL_FIND's neighbour above it reads the palette table: "ADDI @PALROMI,AR2 / LDI *AR2,AR2 / LDI *AR2++,R3"
				const uint32_t pf = m.ram_word(i + 5) & 0xffffffu;
				for (uint32_t k = 1; k < 16 && k < pf; k++)
					if ((m.ram_word(pf - k) & 0xffff0000u) == 0x022A0000u && m.ram_word(pf - k + 1) == 0x084AC200u) { palrom = m.ram_word(m.ram_word(pf - k) & 0xffff); break; }
			}
			if (!palrom) continue;
			// the palette: a count, then the colours; with the count's top bit set, two 15-bit colours per word (low half first)
			const uint32_t p = m.mem_peek(palrom + pal);
			const uint32_t cnt = m.mem_peek(p);
			const bool packed = (cnt >> 31) != 0;
			const uint32_t n = cnt & 0x7fffffffu;
			if (n == 0 || n > 256 || m.mem_peek(f->table + 1) > 255) continue;
			f->rgb.assign(256, 0);
			for (uint32_t k = 0; k < n; k++)
			{
				const uint32_t w = packed ? m.mem_peek(p + 1 + k / 2) >> ((k & 1) * 16) : m.mem_peek(p + 1 + k);
				f->rgb[k] = MidVUnit::colour_rgb(w & 0x7fff);
			}
			f->ok = true;
		}
	}

	static GpuQuad rect(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t flags, uint32_t pal, uint32_t tex)
	{
		// corners at pixel centres, right and bottom edges inclusive (the same rule as the game's polygons)
		GpuQuad g{};
		const float px[4] = {x + 0.5f, x + w + 0.501f, x + w + 0.501f, x + 0.5f}, py[4] = {y + 0.5f, y + 0.5f, y + h + 0.501f, y + h + 0.501f};
		const float tu[4] = {u0, u1, u1, u0}, tv[4] = {v0, v0, v1, v1};
		for (int i = 0; i < 4; i++) { g.p[i * 2] = px[i]; g.p[i * 2 + 1] = py[i]; g.t[i * 2] = tu[i] + 0.5f; g.t[i * 2 + 1] = tv[i] + 0.5f; }
		g.flags = flags; g.pixdata = pal; g.texbase = tex;
		return g;
	}

	// the game's TEXT_OUTPUT for one string; returns the width. `q` null: measure only
	static int text(const MidVUnit &m, const Font &f, std::vector<GpuQuad> *q, int x, int y, const char *s, uint32_t flags, uint32_t pal, int k = 1)
	{
		const int x0 = x;
		int adv = f.height / 2 * k;
		for (; *s; s++)
		{
			int c = *s == '/' ? '@' : *s;
			if (c == ' ') { x += adv; continue; }
			if (c < '0' || c > 'Z') continue;
			const uint32_t e = f.table + uint32_t(c - '0') * 4;
			const uint32_t w0 = m.mem_peek(e);
			const int pre = int16_t(w0 & 0xffff), trail = int(w0 >> 16);
			const int xs = int(m.mem_peek(e + 1)), xe = int(m.mem_peek(e + 2)), ys = int(m.mem_peek(e + 3));
			x += pre * k;
			// (k times the size: the right and bottom edges are inclusive, so k * (w + 1) pixels show w + 1 texels)
			if (q) q->push_back(rect(float(x), float(y), float(k * (xe - xs + 1) - 1), float(k * (f.height + 1) - 1), float(xs), float(ys), float(xe) + 1.0f - 1.0f / float(k),
			                         float(ys + f.height) + 1.0f - 1.0f / float(k), flags, pal, f.img));
			adv = ((xe - xs) + trail) * k;
			x += adv;
		}
		return x - x0;
	}

	void draw(MidVUnit &m)
	{
		static const char *const items[kItems] = {"CONTINUE", "RETURN TO ATTRACT", "EXIT GAME"};
		const uint32_t ui[8] = {0x000000, 0xFFFFFF, 0xFFE000, 0x9098A8, 0xFF3020, 0, 0, 0};
		m.ui_palette(kPalUi, ui, 8);
		if (m_small.ok) m.ui_palette(kPalSmall, m_small.rgb.data(), 256);
		std::vector<GpuQuad> q;
		const float wm = float(m.wide_margin());
		// the picture behind: every second pixel black, as the game does for its own panels
		q.push_back(rect(-wm, 0, 511 + 2 * wm, 511, 0, 0, 0, 0, 0x2000, kPalUi, 0));
		if (!m_small.ok) { m.ui_draw(q.data(), int(q.size())); return; }
		const uint32_t kTex = 0x900, kFlat = 0xD00;   // textured with transparent zero texels / the same in one colour (low byte)
		auto line = [&](int y, const char *s, bool own_colours, uint32_t colour, int k) {
			const int w = text(m, m_small, nullptr, 0, 0, s, 0, 0, k), x = (512 - w) / 2;
			text(m, m_small, &q, x + 2 * k, y + 2 * k, s, kFlat | 0, kPalUi, k);          // shadow
			if (own_colours) text(m, m_small, &q, x, y, s, kTex, kPalSmall, k);
			else text(m, m_small, &q, x, y, s, kFlat | colour, kPalUi, k);
		};
		line(104, "PAUSED", true, 0, 2);
		for (int i = 0; i < kItems; i++) line(190 + i * 30, items[i], i != m_sel, 2, 1);
		m.ui_draw(q.data(), int(q.size()));
	}
};
