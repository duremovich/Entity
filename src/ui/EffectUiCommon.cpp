#include "entity/ui/EffectUiCommon.hpp"

#include "entity/effects/EffectKindRegistry.hpp"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace entity::ui {

bool renderEffectKindMenu(
    const effects::EffectKindRegistry* registry,
    const std::function<void(const effects::EffectKind&)>& onPick) {
    if (!registry) {
        ImGui::TextDisabled("(EffectKindRegistry not bound)");
        return false;
    }

    const auto& kinds = registry->kinds();
    std::vector<const effects::EffectKind*> sorted;
    sorted.reserve(kinds.size());
    for (const auto& [_, k] : kinds) sorted.push_back(&k);
    std::sort(sorted.begin(), sorted.end(),
        [](const effects::EffectKind* a, const effects::EffectKind* b) {
            if (a->category != b->category) return a->category < b->category;
            return a->displayName < b->displayName;
        });

    bool picked = false;
    std::string lastCategory;
    for (const effects::EffectKind* k : sorted) {
        if (k->category != lastCategory) {
            if (!lastCategory.empty()) ImGui::Separator();
            ImGui::TextDisabled("%s", k->category.c_str());
            lastCategory = k->category;
        }
        if (ImGui::MenuItem(k->displayName.c_str())) {
            if (onPick) onPick(*k);
            picked = true;
        }
    }
    if (sorted.empty()) {
        ImGui::TextDisabled("(no effect kinds registered)");
    }
    return picked;
}

} // namespace entity::ui
