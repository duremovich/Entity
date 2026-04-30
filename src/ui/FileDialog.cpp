#include "entity/ui/FileDialog.hpp"

#include <iostream>

#ifdef _WIN32

#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace entity::ui {
namespace {

// RAII wrapper around CoInitializeEx. Each worker thread initializes its
// own STA apartment (required for IFileOpenDialog).
class ComGuard {
public:
    ComGuard() {
        m_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    ~ComGuard() {
        if (SUCCEEDED(m_hr)) CoUninitialize();
    }
    bool ok() const { return SUCCEEDED(m_hr); }

    ComGuard(const ComGuard&)            = delete;
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
std::filesystem::path runDialog(DialogT* dialog) {
    // No parent HWND — see FileDialog.hpp + Issue #21. The dialog floats as
    // a top-level window so a wedged shell extension can never pin the main
    // app into an unkillable state.
    HRESULT hr = dialog->Show(nullptr);
    if (FAILED(hr)) {
        // HRESULT_FROM_WIN32(ERROR_CANCELLED) is the normal cancel path —
        // not an error worth logging.
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

std::filesystem::path runOpenFileDialog(
    std::wstring title,
    std::vector<FileDialogFilter> filters)
{
    ComGuard com;
    if (!com.ok()) {
        std::cerr << "[FileDialog] CoInitializeEx failed (open)" << std::endl;
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

    return runDialog(dialog.Get());
}

std::filesystem::path runSaveFileDialog(
    std::wstring title,
    std::vector<FileDialogFilter> filters,
    std::wstring defaultExtension,
    std::filesystem::path suggestedPath)
{
    ComGuard com;
    if (!com.ok()) {
        std::cerr << "[FileDialog] CoInitializeEx failed (save)" << std::endl;
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

    return runDialog(dialog.Get());
}

std::filesystem::path runPickFolderDialog(
    std::wstring title,
    std::filesystem::path initialFolder)
{
    ComGuard com;
    if (!com.ok()) {
        std::cerr << "[FileDialog] CoInitializeEx failed (folder)" << std::endl;
        return {};
    }

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return {};

    dialog->SetTitle(title.c_str());

    FILEOPENDIALOGOPTIONS opts = 0;
    dialog->GetOptions(&opts);
    dialog->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    if (!initialFolder.empty()) {
        ComPtr<IShellItem> folderItem;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                initialFolder.wstring().c_str(), nullptr, IID_PPV_ARGS(&folderItem)))) {
            dialog->SetFolder(folderItem.Get());
        }
    }

    return runDialog(dialog.Get());
}

}  // namespace

FileDialogTask::~FileDialogTask() {
    if (m_thread.joinable()) {
        if (!m_done.load(std::memory_order_acquire)) {
            // Worker is still inside IFileOpenDialog::Show(); we cannot
            // safely interrupt it. Detach so the destructor doesn't
            // std::terminate, and let the OS reclaim the thread on
            // process exit.
            std::cerr << "[FileDialog] Worker thread still running at task "
                         "shutdown — detaching" << std::endl;
            m_thread.detach();
        } else {
            m_thread.join();
        }
    }
}

std::unique_ptr<FileDialogTask> openFileDialogAsync(
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters)
{
    auto task = std::make_unique<FileDialogTask>();
    task->launch([title, filters]() {
        return runOpenFileDialog(title, filters);
    });
    return task;
}

std::unique_ptr<FileDialogTask> saveFileDialogAsync(
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters,
    const std::wstring& defaultExtension,
    const std::filesystem::path& suggestedPath)
{
    auto task = std::make_unique<FileDialogTask>();
    task->launch([title, filters, defaultExtension, suggestedPath]() {
        return runSaveFileDialog(title, filters, defaultExtension, suggestedPath);
    });
    return task;
}

std::unique_ptr<FileDialogTask> pickFolderDialogAsync(
    const std::wstring& title,
    const std::filesystem::path& initialFolder)
{
    auto task = std::make_unique<FileDialogTask>();
    task->launch([title, initialFolder]() {
        return runPickFolderDialog(title, initialFolder);
    });
    return task;
}

}  // namespace entity::ui

#else  // !_WIN32

namespace entity::ui {

FileDialogTask::~FileDialogTask() {
    if (m_thread.joinable()) m_thread.join();
}

std::unique_ptr<FileDialogTask> openFileDialogAsync(
    const std::wstring&, const std::vector<FileDialogFilter>&) {
    auto task = std::make_unique<FileDialogTask>();
    task->launch([]() { return std::filesystem::path{}; });
    return task;
}

std::unique_ptr<FileDialogTask> saveFileDialogAsync(
    const std::wstring&, const std::vector<FileDialogFilter>&,
    const std::wstring&, const std::filesystem::path&) {
    auto task = std::make_unique<FileDialogTask>();
    task->launch([]() { return std::filesystem::path{}; });
    return task;
}

std::unique_ptr<FileDialogTask> pickFolderDialogAsync(
    const std::wstring&, const std::filesystem::path&) {
    auto task = std::make_unique<FileDialogTask>();
    task->launch([]() { return std::filesystem::path{}; });
    return task;
}

}  // namespace entity::ui

#endif
