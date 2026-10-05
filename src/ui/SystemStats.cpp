#include "ui/SystemStats.h"
#include <algorithm>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#ifdef _WIN32
namespace {
uint64_t ticks(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
}  // namespace
#endif

void SystemStats::update() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (lastSample_ >= 0 && now - lastSample_ < 1.0) return;
    lastSample_ = now;
    cores = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    // The K32 name is in kernel32 itself, so no psapi.lib.
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        privateBytes = pmc.PrivateUsage;
        workingSet = pmc.WorkingSetSize;
        peakWorkingSet = pmc.PeakWorkingSetSize;
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        ramTotal = ms.ullTotalPhys;
        ramUsed = ms.ullTotalPhys - ms.ullAvailPhys;
    }
    // CPU: the time used since the last sample, of the time all cores had.
    FILETIME create, exit, kernel, user, idle, sysKernel, sysUser, wall;
    GetSystemTimeAsFileTime(&wall);
    const bool proc = GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
    const bool sys = GetSystemTimes(&idle, &sysKernel, &sysUser);
    const uint64_t procT = proc ? ticks(kernel) + ticks(user) : 0;
    if (lastWall_ && proc && ticks(wall) > lastWall_)
        cpu = std::clamp(float(double(procT - lastProc_) / (double(ticks(wall) - lastWall_) * cores)), 0.0f, 1.0f);
    if (sys) {
        // System kernel time includes the idle time.
        const uint64_t k = ticks(sysKernel), u = ticks(sysUser), i = ticks(idle);
        const uint64_t total = (k - lastKernel_) + (u - lastUser_);
        if (lastKernel_ && total > 0)
            systemCpu = std::clamp(1.0f - float(double(i - lastIdle_) / double(total)), 0.0f, 1.0f);
        lastKernel_ = k, lastUser_ = u, lastIdle_ = i;
    }
    lastProc_ = procT;
    lastWall_ = ticks(wall);
#endif
}
