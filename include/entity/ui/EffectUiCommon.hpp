#pragma once

#include <functional>

namespace entity::effects {
class EffectKindRegistry;
struct EffectKind;
}

namespace entity::ui {

// Renders the registry's effect kinds as MenuItems grouped by category
// (stable sort: category, then display name), calling `onPick` for a
// clicked kind. Shared by PropertyWindow's "+ Add Effect" popup and the
// EffectGraphWindow background context menu so the two pickers can't
// drift. Call inside an open popup/menu. Returns true if a kind was
// picked this frame.
bool renderEffectKindMenu(
    const effects::EffectKindRegistry* registry,
    const std::function<void(const effects::EffectKind&)>& onPick);

} // namespace entity::ui
