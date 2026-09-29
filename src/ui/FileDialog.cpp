#include "ui/FileDialog.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>

#include <vector>

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// "A|*.a|B|*.b" -> L"A\0*.a\0B\0*.b\0\0"
std::wstring makeFilter(const char* filter) {
    std::wstring f = widen(filter);
    for (auto& c : f)
        if (c == L'|') c = L'\0';
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

std::optional<std::string> run(bool save, const char* title, const char* filter, const char* defaultExt) {
    std::vector<wchar_t> buf(32768, L'\0');
    std::wstring wfilter = makeFilter(filter);
    std::wstring wtitle = widen(title);
    std::wstring wext = defaultExt ? widen(defaultExt) : std::wstring();

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    ofn.lpstrTitle = wtitle.c_str();
    ofn.lpstrDefExt = wext.empty() ? nullptr : wext.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER;
    ofn.Flags |= save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);

    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return std::nullopt;
    return narrow(buf.data());
}

}  // namespace

std::optional<std::string> openFileDialog(const char* title, const char* filter) {
    return run(false, title, filter, nullptr);
}

std::optional<std::string> saveFileDialog(const char* title, const char* filter, const char* defaultExt) {
    return run(true, title, filter, defaultExt);
}

#else
std::optional<std::string> openFileDialog(const char*, const char*) { return std::nullopt; }
std::optional<std::string> saveFileDialog(const char*, const char*, const char*) { return std::nullopt; }
#endif
