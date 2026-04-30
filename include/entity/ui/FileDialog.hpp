#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace entity::ui {

// Native Windows file dialog (IFileOpenDialog / IFileSaveDialog).
//
// All dialogs run on a worker thread so the main render loop keeps pumping
// messages and stays closeable even when a shell extension wedges the
// picker (see Issue #21). Callers fire-and-poll: launch a task, check
// isDone() each frame, consume the result.
//
// No parent HWND is passed to IFileDialog::Show() — the dialog floats as
// a top-level window with no Win32 modality on the main app. Callers gate
// duplicate launches via their own UI state (typically a single
// m_pendingDialog member with BeginDisabled).

struct FileDialogFilter {
    std::wstring name;     // User-visible label: L"Entity Project Files"
    std::wstring pattern;  // Pattern list: L"*.entity;*.ent"
};

class FileDialogTask {
public:
    FileDialogTask() = default;
    ~FileDialogTask();

    FileDialogTask(const FileDialogTask&)            = delete;
    FileDialogTask& operator=(const FileDialogTask&) = delete;
    FileDialogTask(FileDialogTask&&)                 = delete;
    FileDialogTask& operator=(FileDialogTask&&)      = delete;

    bool isDone() const noexcept { return m_done.load(std::memory_order_acquire); }

    // Empty path = cancelled, errored, or task not yet done. Callers should
    // gate on isDone() first.
    std::filesystem::path result() const { return m_result; }

private:
    friend std::unique_ptr<FileDialogTask> openFileDialogAsync(
        const std::wstring&, const std::vector<FileDialogFilter>&);
    friend std::unique_ptr<FileDialogTask> saveFileDialogAsync(
        const std::wstring&, const std::vector<FileDialogFilter>&,
        const std::wstring&, const std::filesystem::path&);
    friend std::unique_ptr<FileDialogTask> pickFolderDialogAsync(
        const std::wstring&, const std::filesystem::path&);

    template <typename F>
    void launch(F&& work) {
        m_thread = std::thread([this, work = std::forward<F>(work)]() mutable {
            std::filesystem::path picked = work();
            m_result = std::move(picked);
            m_done.store(true, std::memory_order_release);
        });
    }

    std::thread             m_thread;
    std::atomic<bool>       m_done{false};
    std::filesystem::path   m_result;  // written before m_done is set
};

// Open-file dialog. Returns a task; consume result() once isDone() is true.
std::unique_ptr<FileDialogTask> openFileDialogAsync(
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters);

// Save-file dialog. defaultExtension is applied (no leading dot, e.g.
// L"entity") when the user types a filename without one. If suggestedPath
// is non-empty, pre-populates the filename field and initial folder.
std::unique_ptr<FileDialogTask> saveFileDialogAsync(
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters,
    const std::wstring& defaultExtension = {},
    const std::filesystem::path& suggestedPath = {});

// Folder-picker dialog (IFileOpenDialog + FOS_PICKFOLDERS).
std::unique_ptr<FileDialogTask> pickFolderDialogAsync(
    const std::wstring& title,
    const std::filesystem::path& initialFolder = {});

}  // namespace entity::ui
