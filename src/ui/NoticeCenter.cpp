#include "entity/ui/NoticeCenter.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace entity {

std::uint64_t NoticeCenter::post(Severity severity, Presentation presentation,
                                 std::string title, std::string message,
                                 std::string key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!key.empty()) {
        m_notices.erase(std::remove_if(m_notices.begin(), m_notices.end(),
                                       [&](const Notice& n) { return n.key == key; }),
                        m_notices.end());
    }
    Notice n;
    n.id           = m_nextId++;
    n.severity     = severity;
    n.presentation = presentation;
    n.key          = std::move(key);
    n.title        = std::move(title);
    n.message      = std::move(message);
    m_notices.push_back(std::move(n));
    return m_notices.back().id;
}

void NoticeCenter::dismiss(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_notices.erase(std::remove_if(m_notices.begin(), m_notices.end(),
                                   [&](const Notice& n) { return n.id == id; }),
                    m_notices.end());
}

void NoticeCenter::dismissKey(std::string_view key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_notices.erase(std::remove_if(m_notices.begin(), m_notices.end(),
                                   [&](const Notice& n) { return n.key == key; }),
                    m_notices.end());
}

void NoticeCenter::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_notices.clear();
}

std::vector<NoticeCenter::Notice> NoticeCenter::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_notices;
}

std::size_t NoticeCenter::count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_notices.size();
}

bool NoticeCenter::hasKey(std::string_view key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_notices.begin(), m_notices.end(),
                       [&](const Notice& n) { return n.key == key; });
}

namespace {

ImVec4 severityColor(NoticeCenter::Severity s) {
    switch (s) {
        case NoticeCenter::Severity::Error:   return ImVec4(1.00f, 0.45f, 0.40f, 1.0f);
        case NoticeCenter::Severity::Warning: return ImVec4(1.00f, 0.80f, 0.35f, 1.0f);
        case NoticeCenter::Severity::Info:    break;
    }
    return ImVec4(0.75f, 0.85f, 1.00f, 1.0f);
}

} // namespace

void NoticeCenter::render() {
    const std::vector<Notice> notices = snapshot();
    if (notices.empty()) {
        m_openModalId = 0;
        return;
    }

    // --- Modal: one at a time, oldest first ---------------------------------
    const Notice* modal = nullptr;
    for (const auto& n : notices) {
        if (n.presentation == Presentation::Modal) { modal = &n; break; }
    }
    if (modal) {
        // Fixed popup id so the open/close bookkeeping doesn't depend on the
        // title; the visible title rides in front of "###".
        const std::string popupId = modal->title + "###entity_notice_modal";
        if (m_openModalId != modal->id || !ImGui::IsPopupOpen("###entity_notice_modal")) {
            ImGui::OpenPopup(popupId.c_str());
            m_openModalId = modal->id;
        }
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 0.0f), ImVec2(640.0f, FLT_MAX));
        if (ImGui::BeginPopupModal(popupId.c_str(), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PushStyleColor(ImGuiCol_Text, severityColor(modal->severity));
            ImGui::TextUnformatted(modal->title.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
            ImGui::TextUnformatted(modal->message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120.0f, 0.0f)) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                dismiss(modal->id);
                m_openModalId = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    } else {
        m_openModalId = 0;
    }

    // --- Toasts: bottom-left stack, never take focus ------------------------
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 12.0f;
    float y = vp->WorkPos.y + vp->WorkSize.y - pad;
    for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
        const Notice& n = *it;
        if (n.presentation != Presentation::Toast) continue;
        char winId[64];
        std::snprintf(winId, sizeof(winId), "##entity_notice_toast_%llu",
                      static_cast<unsigned long long>(n.id));
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, y), ImGuiCond_Always,
                                ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f), ImVec2(520.0f, FLT_MAX));
        ImGui::Begin(winId, nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoDocking);
        ImGui::PushStyleColor(ImGuiCol_Text, severityColor(n.severity));
        ImGui::TextUnformatted(n.title.c_str());
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(n.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PushID(static_cast<int>(n.id));
        if (ImGui::SmallButton("Dismiss")) {
            dismiss(n.id);
        }
        ImGui::PopID();
        y -= ImGui::GetWindowHeight() + 6.0f;
        ImGui::End();
    }
}

} // namespace entity
