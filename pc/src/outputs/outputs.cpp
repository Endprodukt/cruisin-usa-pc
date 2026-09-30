#include "outputs.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

// The game's lamp test (DIAG.ASM) drives these bits of the lamp latch: 1 START, 2 VIEW 1, 4 VIEW 2, 8 VIEW 3,
// 0x10 LEFT TAIL, 0x20 RIGHT TAIL, 0x40 LEFT FRONT, 0x80 RIGHT FRONT.
const char *const kNames[] = {"START", "VIEW 1", "VIEW 2", "VIEW 3", "LEFT TAIL", "RIGHT TAIL", "LEFT FRONT", "RIGHT FRONT", "WHEEL MOTOR"};
constexpr size_t kCount = sizeof(kNames) / sizeof(kNames[0]);
constexpr size_t kClients = 16;

} // namespace

struct Outputs::Impl
{
	OutputSettings cfg;
	std::thread thread;
	std::atomic<bool> quit{false};
	std::atomic<bool> up{false};
	std::atomic<long> value[kCount];
	std::atomic<bool> started_ok{false};

	// windows protocol
	HWND hwnd = nullptr;
	UINT om_start = 0, om_stop = 0, om_update = 0, om_register = 0, om_unregister = 0, om_getid = 0;
	struct Client { HWND hwnd; LPARAM id; } clients[kClients]{};
	size_t client_count = 0;

	// network protocol
	SOCKET listen_sock = INVALID_SOCKET;
	SOCKET socks[kClients];
	size_t sock_count = 0;

	const char *out_name(LPARAM id) const
	{
		if (id == 0) return cfg.game_name.c_str();
		if (id >= 1 && size_t(id) <= kCount) return kNames[id - 1];
		return "";
	}

	void win_send_id(HWND to, LPARAM id)
	{
		const char *name = out_name(id);
		size_t len = std::strlen(name);
		char buf[4 + 128];
		uint32_t id32 = uint32_t(id);
		if (len > sizeof(buf) - 5) len = sizeof(buf) - 5;
		std::memcpy(buf, &id32, 4);
		std::memcpy(buf + 4, name, len);
		buf[4 + len] = 0;
		COPYDATASTRUCT cd{};
		cd.dwData = 1;              // COPYDATA_MESSAGE_ID_STRING
		cd.cbData = DWORD(4 + len + 1);
		cd.lpData = buf;
		SendMessageTimeoutA(to, WM_COPYDATA, WPARAM(hwnd), LPARAM(&cd), SMTO_ABORTIFHUNG, 200, nullptr);
	}

	static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	static Impl *&self() { static Impl *p = nullptr; return p; }

	bool win_open()
	{
		om_start = RegisterWindowMessageA("MAMEOutputStart");
		om_stop = RegisterWindowMessageA("MAMEOutputStop");
		om_update = RegisterWindowMessageA("MAMEOutputUpdateState");
		om_register = RegisterWindowMessageA("MAMEOutputRegister");
		om_unregister = RegisterWindowMessageA("MAMEOutputUnregister");
		om_getid = RegisterWindowMessageA("MAMEOutputGetIDString");
		WNDCLASSA wc{};
		wc.lpfnWndProc = wndproc;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "MAMEOutput";
		RegisterClassA(&wc);
		self() = this;
		// a plain window that is never shown: clients find it with FindWindow
		hwnd = CreateWindowExA(0, "MAMEOutput", "MAMEOutput", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
		if (!hwnd) return false;
		PostMessageA(HWND_BROADCAST, om_start, WPARAM(hwnd), 0);
		return true;
	}

	void win_close()
	{
		if (!hwnd) return;
		PostMessageA(HWND_BROADCAST, om_stop, WPARAM(hwnd), 0);
		DestroyWindow(hwnd);
		hwnd = nullptr;
	}

	bool net_open()
	{
		WSADATA wd;
		if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
		listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listen_sock == INVALID_SOCKET) return false;
		BOOL yes = TRUE;
		setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(u_short(cfg.port));
		a.sin_addr.s_addr = INADDR_ANY;
		if (bind(listen_sock, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(listen_sock, 4) != 0)
		{
			closesocket(listen_sock);
			listen_sock = INVALID_SOCKET;
			return false;
		}
		u_long nb = 1;
		ioctlsocket(listen_sock, FIONBIO, &nb);
		for (auto &s : socks) s = INVALID_SOCKET;
		return true;
	}

	void net_line(size_t c, const char *name, long v, bool text)
	{
		char line[160];
		int n = text ? std::snprintf(line, sizeof(line), "%s = %s\r", name, cfg.game_name.c_str())
		             : std::snprintf(line, sizeof(line), "%s = %ld\r", name, v);
		if (n <= 0 || socks[c] == INVALID_SOCKET) return;
		if (send(socks[c], line, n, 0) != n) { closesocket(socks[c]); socks[c] = INVALID_SOCKET; }
	}

	void net_poll()
	{
		SOCKET s;
		while ((s = accept(listen_sock, nullptr, nullptr)) != INVALID_SOCKET)
		{
			u_long nb = 0;
			ioctlsocket(s, FIONBIO, &nb);
			size_t c = 0;
			for (; c < sock_count; c++) if (socks[c] == INVALID_SOCKET) break;
			if (c == sock_count && sock_count < kClients) sock_count++;
			if (c >= kClients) { closesocket(s); continue; }
			socks[c] = s;
			net_line(c, "mame_start", 0, true);
			for (size_t i = 0; i < kCount; i++) net_line(c, kNames[i], value[i], false);
		}
	}

	void net_close()
	{
		for (size_t c = 0; c < sock_count; c++)
			if (socks[c] != INVALID_SOCKET) { send(socks[c], "mame_stop = 1\r", 14, 0); closesocket(socks[c]); }
		sock_count = 0;
		if (listen_sock != INVALID_SOCKET) closesocket(listen_sock);
		listen_sock = INVALID_SOCKET;
		WSACleanup();
	}

	void run()
	{
		bool ok = cfg.mode == OutputMode::Windows ? win_open() : net_open();
		started_ok = ok;
		up = true;
		if (!ok) return;
		long sent[kCount];
		for (size_t i = 0; i < kCount; i++) sent[i] = value[i];
		while (!quit)
		{
			MSG m;
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
			while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
			if (cfg.mode == OutputMode::Network) net_poll();
			for (size_t i = 0; i < kCount; i++)
			{
				long v = value[i];
				if (v == sent[i]) continue;
				sent[i] = v;
				if (cfg.mode == OutputMode::Windows)
					for (size_t c = 0; c < client_count; c++) PostMessageA(clients[c].hwnd, om_update, WPARAM(i + 1), LPARAM(v));
				else
					for (size_t c = 0; c < sock_count; c++) net_line(c, kNames[i], v, false);
			}
		}
		if (cfg.mode == OutputMode::Windows) win_close(); else net_close();
	}
};

LRESULT CALLBACK Outputs::Impl::wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	Impl *s = self();
	if (s)
	{
		if (msg == s->om_register && s->om_register)
		{
			size_t i = 0;
			for (; i < s->client_count; i++) if (s->clients[i].hwnd == HWND(wp)) break;
			if (i == s->client_count && s->client_count < kClients) s->client_count++;
			if (i < kClients)
			{
				s->clients[i] = {HWND(wp), lp};
				for (size_t k = 0; k < kCount; k++) PostMessageA(HWND(wp), s->om_update, WPARAM(k + 1), LPARAM(long(s->value[k])));
			}
			return 1;
		}
		if (msg == s->om_unregister && s->om_unregister)
		{
			for (size_t i = 0; i < s->client_count; i++)
				if (s->clients[i].hwnd == HWND(wp)) { s->clients[i] = s->clients[--s->client_count]; break; }
			return 1;
		}
		if (msg == s->om_getid && s->om_getid) { s->win_send_id(HWND(wp), lp); return 1; }
	}
	return DefWindowProcA(h, msg, wp, lp);
}

Outputs::Outputs() : m_impl(new Impl) { for (auto &v : m_impl->value) v = 0; }
Outputs::~Outputs() { stop(); }

const char *const *Outputs::names(size_t &count) { count = kCount; return kNames; }

bool Outputs::start(const OutputSettings &cfg)
{
	stop();
	if (cfg.mode == OutputMode::Off) return false;
	m_impl->cfg = cfg;
	m_impl->quit = false;
	m_impl->up = false;
	m_impl->started_ok = false;
	m_impl->thread = std::thread([this] { m_impl->run(); });
	for (int i = 0; i < 200 && !m_impl->up; i++) Sleep(5);
	return m_impl->started_ok;
}

void Outputs::stop()
{
	if (m_impl->thread.joinable())
	{
		m_impl->quit = true;
		m_impl->thread.join();
	}
	m_impl->up = false;
}

bool Outputs::running() const { return m_impl->thread.joinable() && m_impl->started_ok; }

void Outputs::update(const uint8_t lamps[8], uint8_t wheel_motor)
{
	for (size_t i = 0; i < 8; i++) m_impl->value[i] = lamps[i] ? 1 : 0;
	m_impl->value[8] = long(int8_t(wheel_motor));
}
