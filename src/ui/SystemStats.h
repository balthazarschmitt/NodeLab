#pragma once
// The computer's load for the status bar, like Blender's Scene Statistics and System Memory:
// Refractory's own memory and CPU, and the whole system's. Sampled at most once a second.
#include <cstdint>

struct SystemStats {
    uint64_t privateBytes = 0;  // Refractory's committed memory (Task Manager's "Memory")
    uint64_t workingSet = 0;    // of it, in RAM now
    uint64_t peakWorkingSet = 0;
    uint64_t ramTotal = 0, ramUsed = 0;  // the whole system's
    float cpu = 0;        // Refractory's share of all cores, 0..1
    float systemCpu = 0;  // the whole system's, 0..1
    int cores = 1;

    // Samples again when a second has passed since the last time.
    void update();

private:
    double lastSample_ = -1;
    uint64_t lastProc_ = 0, lastIdle_ = 0, lastKernel_ = 0, lastUser_ = 0, lastWall_ = 0;
};
