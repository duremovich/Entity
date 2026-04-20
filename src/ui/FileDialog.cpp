#include "entity/ui/FileDialog.hpp"

#ifdef _WIN32

#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace entity::ui {
namespace {

// RAII wrapper around CoInitializeEx. Calls CoUninitialize on scope exit
// regardless of whether our init returned S_OK (new init) or S_FALSE
// (already init on this thread — decrement matches our increment).
class ComGuard {
public:
    ComGuard() {
        m_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    ~ComGuard() {
        if (SUCCEEDED(m_hr)) CoUninitialize();
    }
    bool ok() const { return SUCCEEDED(m_hr); }

    ComGuard(const ComGuard&) = delete;
    ComGuard& operator=(const ComGuard&) = delete;

private:
    HRESULT m_hr{E_FAIL};
};

std::vector<COMDLG_FILTERSPEC> toComFilters(const std::vector<FileDialogFilter>& filters) {
    std::vector<COMDLG_FILTERSPEC> out;
    out.reserve(filters.size());
    for (const auto& f : filters) {
        out.push_back({f.name.c_str(), f.pattern.c_str()});
    }
    return out;
}

template <typename DialogT>
std::filesystem::path runDialog(
    DialogT* dialog,
    void* parentHwnd)
{
    HRESULT hr = dialog->Show(reinterpret_cast<HWND>(parentHwnd));
    if (FAILED(hr)) {
        // HRESULT_FROM_WIN32(ERROR_CANCELLED) is the normal cancel path — not
        // an error worth logging.
        return {};
    }

    ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) return {};

    PWSTR rawPath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    if (FAILED(hr) || !rawPath) return {};

    std::filesystem::path result{rawPath};
    CoTaskMemFree(rawPath);
    return result;
}

}  // namespace

std::filesystem::path openFileDialog(
    void* parentHwnd,
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters)
{
    ComGuard com;
    if (!com.ok()) {
        std::cerr << "[FileDialog] CoInitializeEx failed" << std::endl;
        return {};
    }

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return {};

    auto comFilters = toComFilters(filters);
    if (!comFilters.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(comFilters.size()), comFilters.data());
        dialog->SetFileTypeIndex(1);
    }
    dialog->SetTitle(title.c_str());

    FILEOPENDIALOGOPTIONS opts = 0;
    dialog->GetOptions(&opts);
    dialog->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);

    return runDialog(dialog.Get(), parentHwnd);
}

std::filesystem::path saveFileDialog(
    void* parentHwnd,
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters,
    const std::wstring& defaultExtension,
    const std::filesystem::path& suggestedPath)
{
    ComGuard com;
    if (!com.ok()) {
        std::cerr << "[FileDialog] CoInitializeEx failed" << std::endl;
        return {};
    }

    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return {};

    auto comFilters = toComFilters(filters);
    if (!comFilters.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(comFilters.size()), comFilters.data());
        dialog->SetFileTypeIndex(1);
    }
    dialog->SetTitle(title.c_str());
    if (!defaultExtension.empty()) {
        dialog->SetDefaultExtension(defaultExtension.c_str());
    }

    if (!suggestedPath.empty()) {
        dialog->SetFileName(suggestedPath.filename().wstring().c_str());

        auto parent = suggestedPath.parent_path();
        if (!parent.empty()) {
            ComPtr<IShellItem> folderItem;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    parent.wstring().c_str(), nullptr, IID_PPV_ARGS(&folderItem)))) {
                dialog->SetFolder(folderItem.Get());
            }
        }
    }

    FILEOPENDIALOGOPTIONS opts = 0;
    dialog->GetOptions(&opts);
    dialog->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);

    return runDialog(dialog.Get(), parentHwnd);
}

}  // namespace entity::ui

#else  // !_WIN32

namespace entity::ui {

std::filesystem::path openFileDialog(
    void*, const std::wstring&, const std::vector<FileDialogFilter>&) {
    return {};
}

std::filesystem::path saveFileDialog(
    void*, const std::wstring&, const std::vector<FileDialogFilter>&,
    const std::wstring&, const std::filesystem::path&) {
    return {};
}

}  // namespace entity::ui

#endif
