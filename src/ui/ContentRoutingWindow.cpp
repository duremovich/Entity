#include "entity/ui/ContentRoutingWindow.hpp"

#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingAssetOps.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/Screen.hpp"
#include "entity/timeline/Timeline.hpp"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace entity {

ContentRoutingWindow::ContentRoutingWindow(Timeline* timeline)
    : m_timeline(timeline) {}

// L1 placeholder (ADR-0022). The pre-ADR-0022 implementation of this
// window edited the inline `ContentRouting` component on the selected
// layer. After the L1 split (ContentRouting → ContentRoutingAsset +
// ContentRoutingRef), the routing data lives on a library asset that
// can be shared by multiple layers. Authoring that library properly is
// L2 work; for now this window resolves the selection's
// ContentRoutingRef and reports what asset drives the layer, leaving
// authoring affordances disabled. PropertyWindow's Content Routing
// dropdown is the live editor in L1.
void ContentRoutingWindow::render() {
    ImGui::TextWrapped(
        "Content Routing library editor — L2 work in progress.");
    ImGui::TextWrapped(
        "Use Properties \xe2\x86\x92 Content Routing on a selected clip "
        "or generative layer to bind it to a routing in the library. "
        "Direct routings are auto-generated per Screen.");
    ImGui::Separator();

    if (!m_timeline) {
        ImGui::TextDisabled("No timeline.");
        return;
    }

    auto& registry = m_timeline->getRegistry();
    const entt::entity selected = m_timeline->getSelectedClip();
    if (selected == entt::null || !registry.valid(selected)) {
        ImGui::TextDisabled("Select a clip or generative layer to inspect");
        ImGui::TextDisabled("its current routing.");
        return;
    }

    const bool isClip       = registry.all_of<Clip>(selected);
    const bool isGenerative = registry.all_of<GenerativeLayer>(selected);
    if (!isClip && !isGenerative) {
        ImGui::TextDisabled("Selected layer doesn't produce content");
        ImGui::TextDisabled("(content routing is only available on clip");
        ImGui::TextDisabled("and generative layers).");
        return;
    }

    if (isClip) {
        const auto& clip = registry.get<Clip>(selected);
        ImGui::Text("Selected clip: %s", clip.filepath.c_str());
    } else {
        ImGui::Text("Selected generative layer (entity %u)",
                     static_cast<uint32_t>(selected));
    }

    const auto* asset = routing::tryGetAsset(registry, selected);
    if (!asset) {
        ImGui::TextDisabled("Routing: Default (All visible screens)");
        return;
    }

    const char* kindLabel = "Direct";
    if (asset->kind == RouteMode::Tiled) kindLabel = "Tiled";
    ImGui::Text("Routing: %s  (%s)", asset->name.c_str(), kindLabel);
    if (asset->autoBoundScreen != entt::null) {
        ImGui::TextDisabled("Auto-direct asset (Screen-bound)");
    }

    ImGui::Separator();
    ImGui::Text("Targets:");
    if (asset->targets.empty()) {
        ImGui::TextDisabled("  (empty — renders on all visible screens)");
    }
    for (const auto& t : asset->targets) {
        std::string screenName = "(all visible)";
        if (t.screen != entt::null &&
            registry.valid(t.screen) &&
            registry.all_of<Screen>(t.screen)) {
            screenName = registry.get<Screen>(t.screen).name;
        }
        ImGui::BulletText("%s   uv=[%.3f, %.3f, %.3f, %.3f]",
                           screenName.c_str(),
                           t.uvRect[0], t.uvRect[1], t.uvRect[2], t.uvRect[3]);
    }
}

} // namespace entity
