#pragma once
// Runs the AI models (ml/Models.h) with ONNX Runtime, loaded at run time from the models folder:
// Refractory.exe is linked statically and must start without it. The DirectML provider runs models
// on the GPU, and the CPU takes over when it can't.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ml {

struct Tensor {
    std::vector<int64_t> shape;  // e.g. {1, 3, 1024, 1024}
    std::vector<float> data;
};

// The model's first input shape (-1 for dynamic dimensions); empty if it can't be loaded.
std::vector<int64_t> inputShape(const std::string& modelId, std::string& err);
// Runs the model on one float input and returns its first output. The session is created on first
// use; on the CPU it is unloaded when the run ends. `cancel` stops a run part-way (the result is then false with "Cancelled").
bool run(const std::string& modelId, const Tensor& input, Tensor& output, std::string& err,
         const std::atomic<bool>* cancel = nullptr);

// GPU (DirectML), off by default (Preferences > AI Masks). A model whose GPU run fails falls back to
// the CPU. Changing it drops the sessions.
void setUseGpu(bool on);
bool useGpu();
// What run() is doing, for progress displays: loading the model (seconds on the CPU) or running it.
enum class Stage { Idle, Loading, Running };
Stage stage();
// True when the last session was created on the GPU.
bool ranOnGpu();
// Frees the loaded models (before deleting their files, or to save memory).
void releaseSessions();

}  // namespace ml
