#include "ml/Models.h"
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include "io/Paths.h"
#include "ml/Http.h"
#include "ml/Onnx.h"

namespace fs = std::filesystem;

namespace ml {

// ---------------------------------------------------------------- catalogue

const std::vector<ModelSpec>& catalogue() {
    // ONNX Runtime's DirectML build runs on any DirectX 12 GPU and falls back to the CPU. Only the
    // two DLLs are read out of the NuGet packages (their range requests), about 15 MB instead of
    // the packages' 210 MB. The Hugging Face link is pinned to a revision.
    static const std::vector<ModelSpec> list = {
        {kRuntime, "AI runtime", "ONNX Runtime 1.24.4 with DirectML 1.15.4 (Microsoft)", "MIT",
         {{"onnxruntime.dll",
           "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/"
           "microsoft.ml.onnxruntime.directml.1.24.4.nupkg",
           "runtimes/win-x64/native/onnxruntime.dll", 17328152, 6039111,
           "e7eedec6a6f26dc39dc948276a75ef6d2bee3fff944d874ceed0bbd3b97bff40"},
          {"DirectML.dll",
           "https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg",
           "bin/x64-win/DirectML.dll", 18527776, 9332741,
           "9c9e6d822561c6c41b90e6994b3e8857cf1d66dbfb1e0c4c799c7c89b4e92da1"}}},
        {"subject", "Subject model", "BiRefNet general lite (Zheng et al.), ONNX export from rembg", "MIT",
         {{"birefnet-general-lite.onnx",
           "https://github.com/danielgatis/rembg/releases/download/v0.0.0/"
           "BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx",
           nullptr, 224005088, 224005088, "5600024376f572a557870a5eb0afb1e5961636bef4e1e22132025467d0f03333"}}},
        // Select Subject's Light model: 4.6 MB and well under a second on a CPU, for computers
        // where BiRefNet's minute and 4 GB are too much. Coarser edges (Refine Edges helps).
        {"subject-light", "Light subject model", "U\xC2\xB2-Net small (u2netp, Qin et al.), ONNX export from rembg",
         "Apache-2.0",
         {{"u2netp.onnx", "https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2netp.onnx", nullptr,
           4574861, 4574861, "309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8"}}},
        {"sky", "Sky model", "U\xC2\xB2-Net sky segmentation (xiongzhu666)", "MIT",
         {{"skyseg.onnx",
           "https://huggingface.co/JianyuanWang/skyseg/resolve/3ba8c6df1d9ba9ff26f637c7ba9568ac11a9aa7f/skyseg.onnx",
           nullptr, 175997079, 175997079, "ab9c34c64c3d821220a2886a4a06da4642ffa14d5b30e8d5339056a089aa1d39"}}},
    };
    return list;
}

const ModelSpec* findModel(const std::string& id) {
    for (const ModelSpec& m : catalogue())
        if (id == m.id) return &m;
    return nullptr;
}

// ---------------------------------------------------------------- folder and status

namespace {

std::mutex& folderMutex() {
    static std::mutex m;
    return m;
}
std::string& folderOverride() {
    static std::string s;
    return s;
}
std::atomic<int> g_generation{1};

}  // namespace

std::string folder() {
    std::lock_guard<std::mutex> lock(folderMutex());
    fs::path p;
    if (!folderOverride().empty()) {
        p = u8ToPath(folderOverride());
    } else {
#ifdef _WIN32
        if (const wchar_t* appdata = _wgetenv(L"APPDATA")) p = fs::path(appdata) / "NodeLab" / "models";
#endif
        if (p.empty()) p = fs::current_path() / "models";
    }
    std::error_code ec;
    fs::create_directories(p, ec);
    return pathToU8(p);
}

void setFolder(const std::string& dirU8) {
    {
        std::lock_guard<std::mutex> lock(folderMutex());
        folderOverride() = dirU8;
    }
    releaseSessions();
    ++g_generation;
}

std::string filePath(const FileSpec& f) { return pathToU8(u8ToPath(folder()) / f.name); }

namespace {

bool fileInstalled(const FileSpec& f) {
    std::error_code ec;
    return fs::file_size(u8ToPath(filePath(f)), ec) == f.size && !ec;
}

}  // namespace

bool installed(const std::string& id) {
    const ModelSpec* m = findModel(id);
    if (!m) return false;
    for (const FileSpec& f : m->files)
        if (!fileInstalled(f)) return false;
    return true;
}

uint64_t downloadSize(const std::string& id) {
    uint64_t n = 0;
    for (const char* part : {kRuntime, id.c_str()})
        if (const ModelSpec* m = findModel(part))
            for (const FileSpec& f : m->files)
                if (!fileInstalled(f)) n += f.transfer;
    return n;
}

int generation() { return g_generation.load(); }

// ---------------------------------------------------------------- SHA-256 (FIPS 180-4)

namespace {

struct ShaState {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[64] = {};
    size_t used = 0;
    uint64_t bytes = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress(const unsigned char* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    void add(const unsigned char* p, size_t n) {
        bytes += n;
        while (n > 0) {
            const size_t take = std::min(n, sizeof(block) - used);
            std::memcpy(block + used, p, take);
            used += take, p += take, n -= take;
            if (used == sizeof(block)) {
                compress(block);
                used = 0;
            }
        }
    }
};

}  // namespace

Sha256::Sha256() { alg_ = new ShaState; }
Sha256::~Sha256() { delete static_cast<ShaState*>(alg_); }
void Sha256::add(const void* data, size_t size) { static_cast<ShaState*>(alg_)->add(static_cast<const unsigned char*>(data), size); }

std::string Sha256::hex() {
    ShaState& s = *static_cast<ShaState*>(alg_);
    const uint64_t bits = s.bytes * 8;
    const unsigned char pad = 0x80, zero = 0;
    s.add(&pad, 1);
    while (s.used != 56) s.add(&zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; ++i) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    s.add(len, 8);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint32_t v : s.h)
        for (int i = 28; i >= 0; i -= 4) out += digits[(v >> i) & 15];
    return out;
}

// ---------------------------------------------------------------- downloads

namespace {

struct Job {
    std::mutex m;
    InstallState state;
    std::atomic<bool> cancel{false};
};
// Never destroyed: a download thread may still be running while the process exits.
Job& job() {
    static Job* j = new Job;
    return *j;
}

uint32_t le16(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t le32(const unsigned char* p) { return le16(p) | le16(p + 2) << 16; }

std::string fetch(const std::string& url, const std::string& range, std::string& out) {
    out.clear();
    return http::get(url, range, [&](const char* d, size_t n) {
        out.append(d, n);
        return true;
    });
}

// Where one entry's data lies in a zip file on a server, found with range requests: the end of
// central directory record in the last 64 KB, then the central directory, then the local header.
struct ZipEntry {
    uint64_t offset = 0, compSize = 0;
    int method = 0;
};

std::string locateZipEntry(const std::string& url, const std::string& name, ZipEntry& e) {
    std::string tail;
    if (std::string err = fetch(url, "bytes=-65558", tail); !err.empty()) return err;
    const auto* t = reinterpret_cast<const unsigned char*>(tail.data());
    size_t eocd = std::string::npos;
    for (size_t i = tail.size() >= 22 ? tail.size() - 22 + 1 : 0; i-- > 0;)
        if (le32(t + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return "Not a zip file: " + url;
    const uint32_t cdSize = le32(t + eocd + 12), cdOffset = le32(t + eocd + 16);
    if (cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) return "Zip64 packages aren't supported";
    std::string cd;
    if (std::string err = fetch(url, "bytes=" + std::to_string(cdOffset) + "-" + std::to_string(uint64_t(cdOffset) + cdSize - 1), cd);
        !err.empty())
        return err;
    const auto* c = reinterpret_cast<const unsigned char*>(cd.data());
    for (size_t i = 0; i + 46 <= cd.size() && le32(c + i) == 0x02014b50;) {
        const uint32_t nameLen = le16(c + i + 28), extraLen = le16(c + i + 30), commentLen = le16(c + i + 32);
        if (i + 46 + nameLen > cd.size()) break;
        if (cd.compare(i + 46, nameLen, name) == 0) {
            e.method = int(le16(c + i + 10));
            e.compSize = le32(c + i + 20);
            const uint32_t local = le32(c + i + 42);
            std::string lh;
            if (std::string err = fetch(url, "bytes=" + std::to_string(local) + "-" + std::to_string(uint64_t(local) + 29), lh);
                !err.empty())
                return err;
            const auto* l = reinterpret_cast<const unsigned char*>(lh.data());
            if (lh.size() < 30 || le32(l) != 0x04034b50) return "Damaged zip file: " + url;
            e.offset = uint64_t(local) + 30 + le16(l + 26) + le16(l + 28);
            return {};
        }
        i += 46 + nameLen + extraLen + commentLen;
    }
    return name + " is missing from " + url;
}

// Downloads one file into the models folder: to a .part file, checked against the pinned size and
// SHA-256, then renamed, so a failed or cancelled download never looks installed.
std::string downloadFile(const FileSpec& f, uint64_t base, Job& j) {
    const fs::path dst = u8ToPath(filePath(f));
    fs::path part = dst;
    part += ".part";
    Sha256 sha;
    uint64_t written = 0;
    std::string err;
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return "Can't write " + pathToU8(part);
        auto progress = [&](uint64_t transferred) {
            std::lock_guard<std::mutex> lock(j.m);
            j.state.done = base + std::min(transferred, f.transfer);
        };
        auto keep = [&](const char* d, size_t n) {
            sha.add(d, n);
            out.write(d, std::streamsize(n));
            written += n;
            return bool(out) && written <= f.size;
        };
        if (!f.zipEntry) {
            err = http::get(f.url, {}, [&](const char* d, size_t n) {
                if (j.cancel) return false;
                progress(written + n);
                return keep(d, n);
            });
        } else {
            ZipEntry e;
            err = locateZipEntry(f.url, f.zipEntry, e);
            if (err.empty() && e.method != 8 && e.method != 0) err = "Unsupported zip compression";
            if (err.empty()) {
                z_stream zs{};
                const bool deflated = e.method == 8;
                if (deflated && inflateInit2(&zs, -MAX_WBITS) != Z_OK) err = "inflateInit failed";
                uint64_t got = 0;
                std::vector<char> buf(1 << 17);
                bool ended = !deflated;
                if (err.empty())
                    err = http::get(f.url, "bytes=" + std::to_string(e.offset) + "-" + std::to_string(e.offset + e.compSize - 1),
                                    [&](const char* d, size_t n) {
                                        if (j.cancel) return false;
                                        got += n;
                                        progress(got);
                                        if (!deflated) return keep(d, n);
                                        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(d));
                                        zs.avail_in = uInt(n);
                                        while (zs.avail_in > 0 && !ended) {
                                            zs.next_out = reinterpret_cast<Bytef*>(buf.data());
                                            zs.avail_out = uInt(buf.size());
                                            const int r = inflate(&zs, Z_NO_FLUSH);
                                            if (r != Z_OK && r != Z_STREAM_END) return false;
                                            if (!keep(buf.data(), buf.size() - zs.avail_out)) return false;
                                            ended = r == Z_STREAM_END;
                                        }
                                        return true;
                                    });
                if (deflated) inflateEnd(&zs);
                if (err.empty() && !ended) err = "The zip entry ended early";
            }
        }
        if (j.cancel) err = "Cancelled";
    }
    std::error_code ec;
    if (err.empty() && written != f.size)
        err = f.name + std::string(" has the wrong size (") + std::to_string(written) + " bytes)";
    if (err.empty() && sha.hex() != f.sha256) err = f.name + std::string(" doesn't match its checksum");
    if (err.empty()) {
        fs::rename(part, dst, ec);
        if (ec) err = "Can't rename " + pathToU8(part) + ": " + ec.message();
    }
    if (!err.empty()) fs::remove(part, ec);
    return err;
}

std::string runInstall(const std::string& id, Job& j) {
    std::vector<const FileSpec*> todo;
    uint64_t total = 0;
    for (const char* part : {kRuntime, id.c_str()}) {
        const ModelSpec* m = findModel(part);
        if (!m) return "Unknown model: " + std::string(part);
        for (const FileSpec& f : m->files)
            if (!fileInstalled(f)) {
                todo.push_back(&f);
                total += f.transfer;
            }
    }
    {
        std::lock_guard<std::mutex> lock(j.m);
        j.state.total = total;
        j.state.done = 0;
    }
    uint64_t base = 0;
    for (const FileSpec* f : todo) {
        if (std::string err = downloadFile(*f, base, j); !err.empty()) return err;
        base += f->transfer;
        ++g_generation;
    }
    return {};
}

}  // namespace

void startInstall(const std::string& id) {
    Job& j = job();
    if (installState().running) return;
    j.cancel = false;
    {
        std::lock_guard<std::mutex> lock(j.m);
        j.state = InstallState{id, true, 0, downloadSize(id), {}};
    }
    std::thread([id, &j] {
        const std::string err = runInstall(id, j);
        std::lock_guard<std::mutex> lock(j.m);
        j.state.running = false;
        j.state.error = err;
        ++g_generation;
    }).detach();  // exiting mid-download just leaves a .part file
}

void cancelInstall() { job().cancel = true; }

InstallState installState() {
    Job& j = job();
    std::lock_guard<std::mutex> lock(j.m);
    return j.state;
}

std::string installNow(const std::string& id, void (*progress)(uint64_t, uint64_t)) {
    Job& j = job();
    j.cancel = false;
    std::atomic<bool> done{false};
    std::thread watcher;
    if (progress)
        watcher = std::thread([&] {
            while (!done) {
                const InstallState s = installState();
                progress(s.done, s.total);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
    const std::string err = runInstall(id, j);
    done = true;
    if (watcher.joinable()) watcher.join();
    ++g_generation;
    return err;
}

void remove(const std::string& id) {
    const ModelSpec* m = findModel(id);
    if (!m) return;
    releaseSessions();
    std::error_code ec;
    for (const FileSpec& f : m->files) fs::remove(u8ToPath(filePath(f)), ec);
    ++g_generation;
}

}  // namespace ml
