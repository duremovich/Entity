#include "entity/ui/ShowControlWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/project/ProjectManager.hpp"

#include <imgui.h>

#include <string>

namespace entity {

ShowControlWindow::ShowControlWindow(Engine* engine)
    : m_engine(engine) {}

void ShowControlWindow::render() {
    auto* pm = m_engine ? m_engine->getProjectManager() : nullptr;
    if (!pm) {
        ImGui::TextDisabled("No active project.");
        return;
    }

    if (ImGui::BeginTabBar("ShowControlTabs")) {
        if (ImGui::BeginTabItem("DMX")) {
            ImGui::TextWrapped(
                "Per-project DMX channel mappings. Edit as JSON below; "
                "the dmx-artnet plugin reloads on the next packet. "
                "Empty -> baked defaults (universe 0 channels 1-8 = "
                "transport + cues 1-4).");
            ImGui::Separator();

            // Bind the text area to the project's mapping JSON string
            // directly. ImGui::InputTextMultiline writes into a fixed
            // buffer; we round-trip through ProjectManager's mutable
            // accessor and copy back on edit.
            std::string& json = pm->dmxMappingsJsonMutable();
            constexpr size_t kBufSize = 16 * 1024;
            char buf[kBufSize];
            const size_t copyLen = std::min(json.size(), kBufSize - 1);
            std::copy_n(json.data(), copyLen, buf);
            buf[copyLen] = '\0';

            ImVec2 sz = ImGui::GetContentRegionAvail();
            sz.y -= 40.0f; // room for the footer hint
            if (sz.y < 100.0f) sz.y = 100.0f;
            if (ImGui::InputTextMultiline("##dmxMappingsJson", buf, kBufSize, sz,
                                           ImGuiInputTextFlags_AllowTabInput)) {
                json.assign(buf);
            }
            ImGui::TextDisabled("Save the project (Ctrl+S) to persist. Format: [{...}, ...]");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace entity
