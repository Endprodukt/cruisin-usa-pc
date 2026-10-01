// Tiny sampling profiler for development (headless tool): samples the instruction pointer of the calling thread from a helper
// thread and prints the hottest source lines / functions using the PDB. Enable with PROFILE=1 in cruisn_headless.
#pragma once

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

class SampleProfiler
{
public:
	void start()
	{
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
		m_run = true;
		m_thread = std::thread([this] {
			while (m_run)
			{
				Sleep(1);
				if (SuspendThread(m_target) == DWORD(-1)) continue;
				CONTEXT c{};
				c.ContextFlags = CONTEXT_CONTROL;
				if (GetThreadContext(m_target, &c)) m_samples.push_back(c.Rip);
				ResumeThread(m_target);
			}
		});
	}

	void stop_and_report(int top = 40)
	{
		m_run = false;
		if (m_thread.joinable()) m_thread.join();
		HANDLE proc = GetCurrentProcess();
		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(proc, nullptr, TRUE);
		std::map<std::string, size_t> by_line, by_func;
		for (DWORD64 a : m_samples)
		{
			char buf[sizeof(SYMBOL_INFO) + 256];
			SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 255;
			DWORD64 disp = 0;
			std::string fn = SymFromAddr(proc, a, &disp, sym) ? sym->Name : "?";
			by_func[fn]++;
			IMAGEHLP_LINE64 line{};
			line.SizeOfStruct = sizeof(line);
			DWORD ld = 0;
			if (SymGetLineFromAddr64(proc, a, &ld, &line))
			{
				std::string f = line.FileName;
				size_t p = f.find_last_of("\\/");
				by_line[(p == std::string::npos ? f : f.substr(p + 1)) + ":" + std::to_string(line.LineNumber) + "  " + fn]++;
			}
		}
		auto dump = [&](const char *title, std::map<std::string, size_t> &m) {
			std::vector<std::pair<size_t, std::string>> v;
			for (auto &kv : m) v.push_back({kv.second, kv.first});
			std::sort(v.rbegin(), v.rend());
			std::fprintf(stderr, "---- %s (%zu samples)\n", title, m_samples.size());
			for (int i = 0; i < top && i < int(v.size()); i++)
				std::fprintf(stderr, "%6.2f%%  %s\n", 100.0 * double(v[size_t(i)].first) / double(m_samples.size()), v[size_t(i)].second.c_str());
		};
		dump("by function", by_func);
		dump("by source line", by_line);
	}

private:
	HANDLE m_target = nullptr;
	std::atomic<bool> m_run{false};
	std::thread m_thread;
	std::vector<DWORD64> m_samples;
};
