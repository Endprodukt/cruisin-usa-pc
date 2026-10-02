#include "splash.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../video/png_io.h"

namespace {

struct Picture
{
	int w = 0, h = 0;
	std::vector<uint8_t> bgra;
	std::string caption;
};
Picture g_pic;

void paint(HWND wnd, HDC dc)
{
	RECT cr;
	GetClientRect(wnd, &cr);
	FillRect(dc, &cr, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
	if (g_pic.w > 0 && cr.right > 0 && cr.bottom > 0)
	{
		// cover the window: crop the picture's longer side, never stretch it
		const double wa = double(cr.right) / cr.bottom, pa = double(g_pic.w) / g_pic.h;
		int sx = 0, sy = 0, sw = g_pic.w, sh = g_pic.h;
		if (wa > pa) { sh = int(g_pic.w / wa); sy = (g_pic.h - sh) / 2; }
		else { sw = int(g_pic.h * wa); sx = (g_pic.w - sw) / 2; }
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = g_pic.w;
		bi.bmiHeader.biHeight = -g_pic.h;   // top-down
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		SetStretchBltMode(dc, HALFTONE);
		SetBrushOrgEx(dc, 0, 0, nullptr);
		StretchDIBits(dc, 0, 0, cr.right, cr.bottom, sx, sy, sw, sh, g_pic.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
	}
	if (!g_pic.caption.empty())
	{
		const int size = std::max(18, int(cr.bottom) / 28);
		HFONT font = CreateFontA(-size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
		HGDIOBJ old = SelectObject(dc, font);
		SetBkMode(dc, TRANSPARENT);
		RECT r = cr;
		r.bottom -= size;
		r.top = r.bottom - size * 2;
		RECT shadow = r;
		OffsetRect(&shadow, std::max(1, size / 12), std::max(1, size / 12));
		SetTextColor(dc, RGB(0, 0, 0));
		DrawTextA(dc, g_pic.caption.c_str(), -1, &shadow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SetTextColor(dc, RGB(255, 255, 255));
		DrawTextA(dc, g_pic.caption.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, old);
		DeleteObject(font);
	}
}

LRESULT CALLBACK splash_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		paint(h, dc);
		EndPaint(h, &ps);
		return 0;
	}
	case WM_ERASEBKGND: return 1;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	}
	return DefWindowProcA(h, msg, wp, lp);
}

} // namespace

void Splash::show(HWND game, const char *caption)
{
	hide();
	HINSTANCE hi = GetModuleHandleA(nullptr);
	if (g_pic.w == 0)
	{
		if (HRSRC res = FindResourceA(nullptr, "BACKGROUND", MAKEINTRESOURCEA(10)))   // RT_RCDATA
		{
			HGLOBAL mem = LoadResource(nullptr, res);
			const uint8_t *data = mem ? static_cast<const uint8_t *>(LockResource(mem)) : nullptr;
			std::vector<uint8_t> rgba;
			if (data && png_decode_rgba(data, SizeofResource(nullptr, res), g_pic.w, g_pic.h, rgba))
			{
				g_pic.bgra.resize(rgba.size());
				for (size_t i = 0; i + 3 < rgba.size(); i += 4)
				{
					g_pic.bgra[i] = rgba[i + 2]; g_pic.bgra[i + 1] = rgba[i + 1]; g_pic.bgra[i + 2] = rgba[i]; g_pic.bgra[i + 3] = 255;
				}
			}
			else g_pic.w = g_pic.h = 0;
		}
	}
	g_pic.caption = caption ? caption : "";

	WNDCLASSA wc{};
	wc.lpfnWndProc = splash_proc;
	wc.hInstance = hi;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "CruisnSplash";
	RegisterClassA(&wc);   // fails harmlessly when registered before
	RECT cr;
	GetClientRect(game, &cr);
	POINT tl{0, 0};
	ClientToScreen(game, &tl);
	// an owned popup: it stays above the game window without being a window of its own in the task bar
	m_wnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "CruisnSplash", "", WS_POPUP, tl.x, tl.y, cr.right, cr.bottom, game, nullptr, hi, nullptr);
	if (!m_wnd) return;
	ShowWindow(m_wnd, SW_SHOWNOACTIVATE);
	UpdateWindow(m_wnd);
}

void Splash::hide()
{
	if (!m_wnd) return;
	DestroyWindow(m_wnd);
	m_wnd = nullptr;
}
