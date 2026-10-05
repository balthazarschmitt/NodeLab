#pragma once
// Minimal HTTPS downloads (WinHTTP on Windows), for the AI models (ml/Models.h). Redirects are
// followed, so GitHub release assets and Hugging Face files work as they are linked.
#include <cstdint>
#include <functional>
#include <string>

namespace http {

// Receives the body in pieces; return false to stop (cancel).
using Sink = std::function<bool(const char* data, size_t size)>;

// GETs `url` and streams the body to `sink`. `range` is an HTTP Range value such as
// "bytes=100-199" or "bytes=-4096" (the last 4096 bytes); empty for the whole file, and a ranged
// request fails unless the server honours it. `length` receives the body's Content-Length (0 when
// the server doesn't say). Returns an empty string on success, otherwise what went wrong.
std::string get(const std::string& url, const std::string& range, const Sink& sink, uint64_t* length = nullptr);

}  // namespace http
