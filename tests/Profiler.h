#pragma once

// A sampling profiler for the benchmark (Windows): another thread stops the audio thread about every
// millisecond, notes where it is, and at the end the places are named from the program's debug symbols
// (a build with /Zi). Inlined code is counted on its own source line, so the hot lines show up even
// inside one big function.
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

class SamplingProfiler
{
public:
    void start()
    {
        DuplicateHandle (GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
        timeBeginPeriod (1);
        running = true;
        worker = std::thread ([this]
        {
            SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            while (running)
            {
                Sleep (1);
                if (! active)
                    continue;
                if (SuspendThread (target) == (DWORD) -1)
                    continue;
                CONTEXT c {};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext (target, &c))
                    samples.push_back (c.Rip);
                ResumeThread (target);
            }
        });
    }

    std::atomic<bool> active { false };

    void stopAndReport (FILE* out, int topLines = 60, int topFunctions = 40)
    {
        running = false;
        worker.join();
        timeEndPeriod (1);
        HANDLE process = GetCurrentProcess();
        SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize (process, nullptr, TRUE);
        std::map<std::string, int> byFunction, byLine;
        for (auto rip : samples)
        {
            char buffer[sizeof (SYMBOL_INFO) + 512] {};
            auto* symbol = (SYMBOL_INFO*) buffer;
            symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            std::string name = SymFromAddr (process, rip, &displacement, symbol) ? symbol->Name : "?";
            ++byFunction[name];
            IMAGEHLP_LINE64 line {};
            line.SizeOfStruct = sizeof (line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64 (process, rip, &lineDisplacement, &line))
            {
                std::string file = line.FileName;
                file = file.substr (file.find_last_of ("\\/") + 1);
                ++byLine[file + ":" + std::to_string (line.LineNumber)];
            }
            else
                ++byLine[name];
        }
        const auto print = [&] (const char* title, const std::map<std::string, int>& m, int top)
        {
            std::vector<std::pair<int, std::string>> v;
            for (auto& [k, n] : m)
                v.push_back ({ n, k });
            std::sort (v.rbegin(), v.rend());
            std::fprintf (out, "== %s (%zu samples)\n", title, samples.size());
            for (int i = 0; i < std::min (top, (int) v.size()); ++i)
                std::fprintf (out, "%6.2f %%  %s\n", 100.0 * v[(size_t) i].first / std::max<size_t> (1, samples.size()), v[(size_t) i].second.c_str());
        };
        print ("functions", byFunction, topFunctions);
        print ("lines", byLine, topLines);
        SymCleanup (process);
        CloseHandle (target);
    }

private:
    HANDLE target = nullptr;
    std::thread worker;
    std::atomic<bool> running { false };
    std::vector<DWORD64> samples;
};
