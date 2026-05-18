#include "entity/ui/FeedMapEditorWindow.hpp"

#include "entity/command/CommandDispatcher.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/ui/ContentRoutingCanvas.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace entity {

namespace {

// Identity uvRect — same value used elsewhere for the "fills the
// whole source" sentinel. Kept local so this TU doesn't depend on a
// helper from ContentRoutingWindow.cpp's anon namespace.
constexpr std::array<float, 4> kIdentityUV{0.0f, 0.0f, 1.0f, 1.0f};

bool getSelectedClipResolution(entt::registry& reg, Timeline* timeline,
                                std::uint32_t& outW, std::uint32_t& outH) {
    if (!timeline) return false;
    const entt::entity sel = timeline->getSelectedClip();
    if (sel == entt::null || !reg.valid(sel)) return false;
    const auto* clip = reg.try_get<Clip>(sel);
    if (!clip || clip->width == 0 || clip->height == 0) return false;
    outW = clip->width;
    outH = clip->height;
    return true;
}

} // namespace

FeedMapEditorWindow::FeedMapEditorWindow(Timeline* timeline)
    : m_timeline(timeline) {
    // Hidden by default — user opens via the Windows menu or the
    // ContentRoutingWindow "Edit in Feed Map Editor..." button.
    m_visible = false;
}

void FeedMapEditorWindow::applyPreBeginStyles() const {
    ImGui::SetNextWindowSize(ImVec2(1200.0f, 720.0f), ImGuiCond_FirstUseEver);
}

void FeedMapEditorWindow::editAsset(entt::entity asset) {
    m_selectedAsset = asset;
    m_drag = {};
    m_view = {};
    // Set visible *immediately* so WindowManager renders this window
    // on the next iteration (it skips hidden windows, which was
    // swallowing the focus request). Then ask ImGui to bring the
    // window to front — works whether it's currently floating, hidden,
    // or docked as a background tab.
    m_visible = true;
    ImGui::SetWindowFocus(getName());
    m_pendingFocus = true;
}

void FeedMapEditorWindow::render() {
    if (m_pendingFocus) {
        ImGui::SetWindowFocus();
        m_pendingFocus = false;
    }

    if (!m_timeline) {
        ImGui::TextDisabled("No timeline.");
        return;
    }
    auto& reg = m_timeline->getRegistry();

    // Drop selection if the asset was destroyed or is no longer a
    // FeedMap (kind change in Content Routing).
    if (m_selectedAsset != entt::null) {
        if (!reg.valid(m_selectedAsset) ||
            !reg.all_of<ContentRoutingAsset>(m_selectedAsset)) {
            m_selectedAsset = entt::null;
        } else {
            const auto& a = reg.get<ContentRoutingAsset>(m_selectedAsset);
            if (a.kind != RouteMode::FeedMap) {
                m_selectedAsset = entt::null;
            }
        }
    }

    // ===== Toolbar (top row) =====
    // Asset dropdown — Feed Map kind only, sorted by name.
    struct Choice { entt::entity e; std::string name; };
    std::vector<Choice> choices;
    for (auto [e, a] : reg.view<ContentRoutingAsset>().each()) {
        if (a.kind == RouteMode::FeedMap) {
            choices.push_back({e, a.name});
        }
    }
    std::sort(choices.begin(), choices.end(),
              [](const Choice& l, const Choice& r) { return l.name < r.name; });

    int currentIdx = -1;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].e == m_selectedAsset) {
            currentIdx = static_cast<int>(i);
            break;
        }
    }
    if (currentIdx < 0 && !choices.empty()) {
        m_selectedAsset = choices[0].e;
        currentIdx = 0;
    }

    ImGui::Text("Asset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    std::vector<const char*> labels;
    labels.reserve(choices.size());
    for (const auto& c : choices) labels.push_back(c.name.c_str());
    if (!choices.empty()) {
        if (ImGui::Combo("##fm_asset", &currentIdx,
                          labels.data(), static_cast<int>(labels.size()))) {
            m_selectedAsset = choices[static_cast<std::size_t>(currentIdx)].e;
            m_drag = {};
            m_view = {};
        }
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(no Feed Map assets — create one in Content Routing)");
        ImGui::Separator();
        ImGui::TextDisabled("Once a Feed Map exists, pick it above to start editing.");
        return;
    }

    if (m_selectedAsset == entt::null) {
        ImGui::Separator();
        ImGui::TextDisabled("Select a Feed Map asset above to edit.");
        return;
    }
    auto& asset = reg.get<ContentRoutingAsset>(m_selectedAsset);

    // Source canvas size + Match Clip.
    ImGui::SameLine();
    ImGui::TextDisabled(" | ");
    ImGui::SameLine();
    int sw = static_cast<int>(asset.sourceWidth);
    int sh = static_cast<int>(asset.sourceHeight);
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("W##fm_w", &sw, 0, 0)) {
        asset.sourceWidth = static_cast<std::uint32_t>(std::max(1, sw));
        m_view = {};
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("H##fm_h", &sh, 0, 0)) {
        asset.sourceHeight = static_cast<std::uint32_t>(std::max(1, sh));
        m_view = {};
    }
    ImGui::SameLine();
    {
        std::uint32_t clipW = 0, clipH = 0;
        const bool canMatch = getSelectedClipResolution(reg, m_timeline, clipW, clipH);
        if (!canMatch) ImGui::BeginDisabled();
        if (ImGui::Button("Match Clip##fm_match")) {
            asset.sourceWidth = clipW;
            asset.sourceHeight = clipH;
            m_view = {};
        }
        if (!canMatch) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Select a clip with a known resolution on the timeline.");
            }
        }
    }

    // Region + view buttons on the same toolbar row.
    ImGui::SameLine();
    ImGui::TextDisabled(" | ");
    ImGui::SameLine();
    if (ImGui::Button("+ Add Region##fm_add")) {
        RouteTarget nt;
        nt.uvRect = kIdentityUV;
        nt.name = "Region " + std::to_string(asset.targets.size() + 1);
        asset.targets.push_back(nt);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit##fm_fit")) {
        m_view = {};  // sentinel zoom=0 triggers auto-fit on next draw
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(wheel = zoom, middle-drag = pan)");
    ImGui::SameLine();
    if (asset.targets.size() > 0) {
        ImGui::TextDisabled("  %zu region%s",
                             asset.targets.size(),
                             asset.targets.size() == 1 ? "" : "s");
    }
    ImGui::Separator();

    // ===== Canvas — fills everything below the toolbar =====
    void* texId = nullptr;
    if (m_renderer && m_timeline) {
        const entt::entity sel = m_timeline->getSelectedClip();
        if (sel != entt::null && reg.valid(sel)) {
            if (const auto* vt = reg.try_get<VideoTexture>(sel)) {
                if (vt->isAllocated()) {
                    texId = m_renderer->getVideoTextureIDForSlot(vt->descriptorSlot);
                }
            }
        }
    }
    const ImVec2 canvasAvail = ImGui::GetContentRegionAvail();
    drawContentRoutingCanvas(reg, asset, texId, m_drag, m_view, canvasAvail);
}

} // namespace entity
