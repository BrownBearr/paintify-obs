#include "filedialog.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>

#include <vector>

namespace filedialog {
namespace {

bool g_init = false;

std::wstring widen(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// The COMDLG_FILTERSPEC array points at these, so they have to outlive the
// dialog call -- hence held by the caller rather than returned by value.
struct FilterStore {
    std::vector<std::wstring> names, patterns;
    std::vector<COMDLG_FILTERSPEC> specs;
};

void buildFilters(const std::vector<Filter>& in, FilterStore* out) {
    out->names.reserve(in.size() + 1);
    out->patterns.reserve(in.size() + 1);
    for (const Filter& f : in) {
        out->names.push_back(widen(f.name));
        out->patterns.push_back(widen(f.patterns));
    }
    out->names.push_back(L"All files");
    out->patterns.push_back(L"*.*");
    for (size_t i = 0; i < out->names.size(); ++i)
        out->specs.push_back({out->names[i].c_str(), out->patterns[i].c_str()});
}

std::string pathOf(IShellItem* item) {
    if (!item) return {};
    PWSTR w = nullptr;
    std::string result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w)) && w) {
        result = narrow(w);
        CoTaskMemFree(w);
    }
    return result;
}

// Shared setup for the three open variants.
std::vector<std::string> runOpen(const char* title, const std::vector<Filter>& filters,
                                 bool multi, bool folders) {
    init();
    std::vector<std::string> out;

    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return out;

    DWORD flags = 0;
    dlg->GetOptions(&flags);
    flags |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (multi)   flags |= FOS_ALLOWMULTISELECT;
    if (folders) flags |= FOS_PICKFOLDERS;
    dlg->SetOptions(flags);

    const std::wstring wtitle = widen(title);
    if (!wtitle.empty()) dlg->SetTitle(wtitle.c_str());

    FilterStore store;
    if (!folders && !filters.empty()) {
        buildFilters(filters, &store);
        dlg->SetFileTypes(UINT(store.specs.size()), store.specs.data());
        dlg->SetFileTypeIndex(1);
    }

    if (SUCCEEDED(dlg->Show(nullptr))) {
        if (multi) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dlg->GetResults(&items)) && items) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* it = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                        std::string p = pathOf(it);
                        if (!p.empty()) out.push_back(std::move(p));
                        it->Release();
                    }
                }
                items->Release();
            }
        } else {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                std::string p = pathOf(item);
                if (!p.empty()) out.push_back(std::move(p));
                item->Release();
            }
        }
    }
    dlg->Release();
    return out;
}

} // namespace

void init() {
    if (g_init) return;
    // APARTMENTTHREADED is what the shell dialogs expect. A failure here means
    // COM was already initialised in another mode, which is fine -- the
    // dialogs still work, so this is not treated as an error.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_init = true;
}

std::string openFile(const char* title, const std::vector<Filter>& filters) {
    std::vector<std::string> r = runOpen(title, filters, /*multi=*/false, /*folders=*/false);
    return r.empty() ? std::string() : r.front();
}

std::vector<std::string> openFiles(const char* title, const std::vector<Filter>& filters) {
    return runOpen(title, filters, /*multi=*/true, /*folders=*/false);
}

std::string pickFolder(const char* title) {
    std::vector<std::string> r = runOpen(title, {}, /*multi=*/false, /*folders=*/true);
    return r.empty() ? std::string() : r.front();
}

std::string saveFile(const char* title, const std::vector<Filter>& filters,
                     const char* defaultName, const char* defaultExt) {
    init();
    std::string out;

    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return out;

    DWORD flags = 0;
    dlg->GetOptions(&flags);
    dlg->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR);

    const std::wstring wtitle = widen(title);
    if (!wtitle.empty()) dlg->SetTitle(wtitle.c_str());
    const std::wstring wname = widen(defaultName);
    if (!wname.empty()) dlg->SetFileName(wname.c_str());
    const std::wstring wext = widen(defaultExt);
    if (!wext.empty()) dlg->SetDefaultExtension(wext.c_str());

    FilterStore store;
    if (!filters.empty()) {
        buildFilters(filters, &store);
        dlg->SetFileTypes(UINT(store.specs.size()), store.specs.data());
        dlg->SetFileTypeIndex(1);
    }

    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            out = pathOf(item);
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

} // namespace filedialog

#else   // not Windows: no native dialogs, callers fall back to the CLI.

namespace filedialog {
void init() {}
std::string openFile(const char*, const std::vector<Filter>&) { return {}; }
std::vector<std::string> openFiles(const char*, const std::vector<Filter>&) { return {}; }
std::string pickFolder(const char*) { return {}; }
std::string saveFile(const char*, const std::vector<Filter>&, const char*, const char*) { return {}; }
} // namespace filedialog

#endif
