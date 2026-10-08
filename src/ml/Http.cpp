#include "ml/Http.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <memory>
#include <vector>
#endif

namespace http {

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string lastError(const char* what) {
    return std::string(what) + " failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET v) : h(v) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

}  // namespace

std::string get(const std::string& url, const std::string& range, const Sink& sink, uint64_t* length) {
    if (length) *length = 0;
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = DWORD(-1);
    uc.dwUrlPathLength = DWORD(-1);
    uc.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return "Bad URL: " + url;
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    // Automatic proxy (Windows 8.1+) finds the system's proxy settings; older systems fall back
    // to the default (WinHTTP's own) configuration.
    Handle session(WinHttpOpen(L"Refractory", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h)
        session.h = WinHttpOpen(L"Refractory", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return lastError("WinHttpOpen");
    // Resolve, connect, send, receive (ms): generous, since a stalled download can be cancelled.
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);
    Handle connection(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connection.h) return lastError("Connecting to the server");
    const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.h) return lastError("WinHttpOpenRequest");
    std::wstring headers;
    if (!range.empty()) headers = L"Range: " + widen(range) + L"\r\n";
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return lastError("Sending the request");
    if (!WinHttpReceiveResponse(request.h, nullptr)) return lastError("Receiving the response");

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    // A server that ignores the Range answers 200 with the whole file, which the caller can't use.
    if (status != (range.empty() ? 200u : 206u)) return "The server answered HTTP " + std::to_string(status);
    wchar_t lenText[32] = {};
    size = sizeof(lenText);
    if (length && WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenText,
                                      &size, WINHTTP_NO_HEADER_INDEX))
        *length = std::wcstoull(lenText, nullptr, 10);

    std::vector<char> buf(1 << 16);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), DWORD(buf.size()), &got)) return lastError("Reading the download");
        if (got == 0) break;
        if (!sink(buf.data(), got)) return "Cancelled";
    }
    return {};
}

#else

std::string get(const std::string&, const std::string&, const Sink&, uint64_t* length) {
    if (length) *length = 0;
    return "Downloads are only supported on Windows";
}

#endif

}  // namespace http
