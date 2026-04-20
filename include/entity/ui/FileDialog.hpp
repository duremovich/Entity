#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace entity::ui {

// Native Windows file dialog (IFileOpenDialog / IFileSaveDialog).
//
// HWND is hidden behind void* so this header doesn't pull <windows.h> into
// anything that includes WindowManager. The implementation casts back.

struct FileDialogFilter {
    std::wstring name;     // User-visible label: L"Entity Project Files"
    std::wstring pattern;  // Pattern list: L"*.entity;*.ent"
};

// Open-file dialog. Returns an empty path on cancel or error.
std::filesystem::path openFileDialog(
    void* parentHwnd,
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters);

// Save-file dialog. Applies defaultExtension (no leading dot, e.g. L"entity")
// when the user types a filename without one. If suggestedPath is non-empty,
// pre-populates the filename field and initial folder.
std::filesystem::path saveFileDialog(
    void* parentHwnd,
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters,
    const std::wstring& defaultExtension,
    const std::filesystem::path& suggestedPath = {});

}  // namespace entity::ui
