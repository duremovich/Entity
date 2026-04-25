#include "entity/ui/SettingsWindow.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace entity {

namespace {

constexpr const char* kPopupId = "Preferences##SettingsModal";

// Render a frame-cache slider with MB-granularity. The underlying field is
// uint64_t bytes; we expose it in MiB to keep the spinner sane.
void renderFrameCacheRow(uint64_t& bytes) {
    int mib = static_cast<int>(bytes / (1024ull * 1024ull));
    // Reasonable bounds: 64 MiB minimum (anything less and the cache barely
    // holds a single 4K HAP-Q frame), 16 GiB ceiling (above this you're
    // pre-loading the entire show — different problem, different setting).
    constexpr int kMinMiB = 64;
    constexpr int kMaxMiB = 16 * 1024;
    if (ImGui::SliderInt("Frame Cache (MiB)##frameCache", &mib,
                         kMinMiB, kMaxMiB,
                         "%d MiB",
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        mib = std::clamp(mib, kMinMiB, kMaxMiB);
        bytes = static_cast<uint64_t>(mib) * 1024ull * 1024ull;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "RAM budget for decoded frames. Higher = fewer re-decodes when\n"
            "scrubbing back into already-viewed regions, more RAM in use.\n"
            "Default 512 MiB ≈ 128 frames @ 4K HAP-Q.\n"
            "Bump on a workstation with lots of RAM; drop on a laptop.\n"
            "Wired up by Phase C.10 (FrameCache).");
        ImGui::EndTooltip();
    }
}

} // namespace

void SettingsWindow::open(const Settings& current) {
    m_staged = current;
    m_pendingOpen = true;
}

void SettingsWindow::render() {
    if (m_pendingOpen) {
        ImGui::OpenPopup(kPopupId);
        m_pendingOpen = false;
        m_open = true;
    }

    // Center on the viewport so the modal doesn't end up in the corner.
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(kPopupId, &m_open,
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextUnformatted("Machine-global Entity preferences. Saved to:");
    char pathBuf[512];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s",
                  reinterpret_cast<const char*>(settingsPath().u8string().c_str()));
    ImGui::TextDisabled("%s", pathBuf);
    ImGui::Separator();

    // ----- Playback / Cache -------------------------------------------------
    if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderFrameCacheRow(m_staged.frameCacheBytes);
    }

    ImGui::Separator();

    const float buttonWidth = 100.0f;
    // Right-align: layout = [Cancel] [OK] flush to the right edge.
    const float availableX = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availableX - 2 * buttonWidth - ImGui::GetStyle().ItemSpacing.x);

    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
        m_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
        if (m_apply) m_apply(m_staged);
        m_open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace entity
