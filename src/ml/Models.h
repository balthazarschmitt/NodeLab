#pragma once
// AI models for auto masks (Select Subject, Select Sky), like Lightroom's and darktable's: they
// aren't part of NodeLab.exe but downloaded on request into %APPDATA%\NodeLab\models, together
// with the ONNX Runtime that runs them (ml/Onnx.h). Every file is pinned by size and SHA-256.
#include <cstdint>
#include <string>
#include <vector>

namespace ml {

struct FileSpec {
    const char* name;      // file name in the models folder
    const char* url;       // where it is downloaded from
    const char* zipEntry;  // the file inside a zip at `url` (a NuGet package), or null
    uint64_t size;         // its size once installed
    uint64_t transfer;     // bytes downloaded for it (compressed in a zip)
    const char* sha256;    // of the installed file, lowercase hex
};

struct ModelSpec {
    const char* id;       // "runtime", "subject", "sky"
    const char* title;    // shown in the UI
    const char* source;   // model and authors
    const char* license;
    std::vector<FileSpec> files;
};

constexpr const char* kRuntime = "runtime";

const std::vector<ModelSpec>& catalogue();
const ModelSpec* findModel(const std::string& id);

// The models folder (%APPDATA%\NodeLab\models), created on demand. Tests point it elsewhere.
std::string folder();
void setFolder(const std::string& dirU8);
std::string filePath(const FileSpec& f);

// Every file of the model is there at its pinned size (the hash is checked when downloading).
bool installed(const std::string& id);
// The model and the runtime can run.
inline bool available(const std::string& id) { return installed(kRuntime) && installed(id); }
// Bytes still to download for the model and, if missing, the runtime.
uint64_t downloadSize(const std::string& id);

// One download at a time, on a background thread: the model and, if missing, the runtime.
struct InstallState {
    std::string id;     // model being (or last) installed
    bool running = false;
    uint64_t done = 0, total = 0;
    std::string error;  // why the last install failed (empty when it worked)
};
void startInstall(const std::string& id);
void cancelInstall();
InstallState installState();
// The same, blocking, for the command line (--install-model). progress(done, total) may be null.
std::string installNow(const std::string& id, void (*progress)(uint64_t, uint64_t) = nullptr);
// Deletes the model's files (the runtime's too for kRuntime).
void remove(const std::string& id);

// Bumped whenever models are installed or removed: nodes fold it into their cache key so they
// re-run, and the app re-evaluates when it changes.
int generation();

// SHA-256 of a byte stream, lowercase hex (exposed for tests).
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(const void* data, size_t size);
    std::string hex();

private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
};

}  // namespace ml
