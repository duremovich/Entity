#include "entity/ui/CueListWindow.hpp"
#include "entity/ui/MathInput.hpp"
#include "entity/core/Engine.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/timeline/CueTag.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace entity {

namespace {

// SMPTE HH:MM:SS:FF rendering against the timeline's frame rate.
std::string formatSmpte(Timecode microseconds, double frameRate) {
    if (frameRate <= 0.0) frameRate = 30.0;
    double seconds = microseconds / 1000000.0;
    int totalSeconds = static_cast<int>(seconds);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs = totalSeconds % 60;
    int frames = static_cast<int>((seconds - totalSeconds) * frameRate);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
    return std::string(buf);
}

} // namespace

CueListWindow::CueListWindow(Engine* engine)
    : m_engine(engine) {
}

void CueListWindow::render() {
    if (!m_engine) return;
    Timeline* timeline = m_engine->getTimeline();
    if (!timeline) return;
    CommandDispatcher* dispatcher = m_engine->getCommandDispatcher();

    // Toolbar
    if (ImGui::Button("+ Add Cue")) {
        m_isEditMode = false;
        m_editOldNumber = 0.0;
        const auto& cues = timeline->getCueTags();
        m_editNumber = cues.empty() ? 1.0 : (cues.back().number + 1.0);
        m_editFrame = timeline->getCurrentFrame();
        m_editLabelBuf[0] = '\0';
        m_openEditModal = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(double-click row to fire)");

    ImGui::Separator();

    const double frameRate = timeline->getFrameRate();
    const auto& cues = timeline->getCueTags();
    auto selectedCueOpt = timeline->getSelectedCueNumber();

    // Track right-click target so the popup body knows which cue was clicked
    // (BeginPopup runs after the table loop has ended).
    static double rightClickedNumber = 0.0;
    static bool   rightClickedValid  = false;

    if (ImGui::BeginTable("##CueTable", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Label",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();

        for (const auto& cue : cues) {
            ImGui::TableNextRow();

            ImGui::PushID(static_cast<int>(cue.number * 100));  // 2-decimal id

            ImGui::TableSetColumnIndex(0);
            char numBuf[24];
            std::snprintf(numBuf, sizeof(numBuf), "%.2f", cue.number);
            const bool isSelected = selectedCueOpt.has_value() && *selectedCueOpt == cue.number;
            ImGuiSelectableFlags selFlags = ImGuiSelectableFlags_AllowDoubleClick;
            if (ImGui::Selectable(numBuf, isSelected, selFlags)) {
                timeline->setSelectedCueNumber(cue.number);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && dispatcher) {
                    dispatcher->enqueue(std::make_unique<FireCueCommand>(cue.number));
                }
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                rightClickedNumber = cue.number;
                rightClickedValid = true;
                ImGui::OpenPopup("CueRowContextMenu");
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cue.label.c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                rightClickedNumber = cue.number;
                rightClickedValid = true;
                ImGui::OpenPopup("CueRowContextMenu");
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(
                formatSmpte(timeline->frameToTime(cue.frame), frameRate).c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                rightClickedNumber = cue.number;
                rightClickedValid = true;
                ImGui::OpenPopup("CueRowContextMenu");
            }

            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("Fire")) {
                if (dispatcher) {
                    dispatcher->enqueue(std::make_unique<FireCueCommand>(cue.number));
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit")) {
                m_isEditMode = true;
                m_editOldNumber = cue.number;
                m_editNumber = cue.number;
                m_editFrame = cue.frame;
                std::strncpy(m_editLabelBuf, cue.label.c_str(), sizeof(m_editLabelBuf) - 1);
                m_editLabelBuf[sizeof(m_editLabelBuf) - 1] = '\0';
                m_openEditModal = true;
            }

            ImGui::PopID();
        }

        // Row context menu
        if (ImGui::BeginPopup("CueRowContextMenu")) {
            if (rightClickedValid) {
                const CueTag* live = timeline->findCueTag(rightClickedNumber);
                if (live) {
                    ImGui::TextDisabled("Cue %.2f", live->number);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Fire Cue") && dispatcher) {
                        dispatcher->enqueue(std::make_unique<FireCueCommand>(live->number));
                    }
                    if (ImGui::MenuItem("Edit Cue...")) {
                        m_isEditMode = true;
                        m_editOldNumber = live->number;
                        m_editNumber = live->number;
                        m_editFrame = live->frame;
                        std::strncpy(m_editLabelBuf, live->label.c_str(),
                                     sizeof(m_editLabelBuf) - 1);
                        m_editLabelBuf[sizeof(m_editLabelBuf) - 1] = '\0';
                        m_openEditModal = true;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete Cue") && dispatcher) {
                        dispatcher->enqueue(std::make_unique<RemoveCueCommand>(live->number));
                    }
                }
            }
            ImGui::EndPopup();
        }

        ImGui::EndTable();
    }

    // Add/Edit modal
    if (m_openEditModal) {
        ImGui::OpenPopup("CueEditModal");
        m_openEditModal = false;
    }
    if (ImGui::BeginPopupModal("CueEditModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(m_isEditMode ? "Edit Cue" : "Add Cue");
        ImGui::Separator();

        entity::ui::InputDouble("Number", &m_editNumber, 0.1, 1.0, "%.2f");
        ImGui::InputText("Label", m_editLabelBuf, sizeof(m_editLabelBuf));

        // Cue position is an integer timeline frame; bind directly.
        long long frameLL = static_cast<long long>(m_editFrame);
        if (entity::ui::InputScalar("Frame", ImGuiDataType_S64, &frameLL)) {
            if (frameLL < 0) frameLL = 0;
            m_editFrame = static_cast<FrameNumber>(frameLL);
        }
        ImGui::TextDisabled("%s",
            formatSmpte(timeline->frameToTime(m_editFrame), frameRate).c_str());

        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            if (dispatcher) {
                if (m_isEditMode) {
                    auto cmd = std::make_unique<EditCueCommand>(
                        m_editOldNumber, m_editNumber, m_editFrame,
                        std::string(m_editLabelBuf));
                    if (const CueTag* live = timeline->findCueTag(m_editOldNumber)) {
                        cmd->setPreviousState(*live);
                    }
                    dispatcher->enqueue(std::move(cmd));
                } else {
                    dispatcher->enqueue(std::make_unique<AddCueAtCommand>(
                        m_editNumber, m_editFrame, std::string(m_editLabelBuf)));
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace entity
