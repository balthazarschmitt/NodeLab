#include "ui/FileDialog.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

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

std::vector<std::string> openFilesDialog(const char* title, const char* filter) {
    std::vector<wchar_t> buf(1 << 20, L'\0');  // room for a few thousand names
    std::wstring wfilter = makeFilter(filter);
    std::wstring wtitle = widen(title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return {};
    // One file: its full path. Several: the folder, then each name, NUL-separated, ending in two NULs.
    std::vector<std::wstring> parts;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) parts.emplace_back(p);
    std::vector<std::string> out;
    if (parts.size() == 1) out.push_back(narrow(parts[0].c_str()));
    else
        for (size_t i = 1; i < parts.size(); ++i) out.push_back(narrow((parts[0] + L"\\" + parts[i]).c_str()));
    return out;
}

std::optional<std::string> folderDialog(const char* title) {
    // The old SHBrowseForFolder tree is awkward; the Vista dialog in folder mode is what Explorer uses.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::optional<std::string> result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        std::wstring wtitle = widen(title);
        dlg->SetTitle(wtitle.c_str());
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->Show(GetActiveWindow())) && SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = narrow(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

#else
std::optional<std::string> openFileDialog(const char*, const char*) { return std::nullopt; }
std::optional<std::string> saveFileDialog(const char*, const char*, const char*) { return std::nullopt; }
std::vector<std::string> openFilesDialog(const char*, const char*) { return {}; }
std::optional<std::string> folderDialog(const char*) { return std::nullopt; }
#endif
