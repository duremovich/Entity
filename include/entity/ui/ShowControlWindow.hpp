#pragma once

#include "EditorWindow.hpp"

namespace entity {

class Engine;

/**
 * ShowControlWindow -- per-project show-control mapping editor.
 *
 * v1 (#59): a single "DMX" tab carrying the project's DMX channel
 * mapping table as raw JSON. The dmx-artnet plugin reads the same
 * string through IPluginContext::getStringSetting("dmxMappingsJson")
 * and falls back to its baked default mappings when empty.
 *
 * Future tabs: "OSC" (project-configurable address->command rewrites),
 * "MIDI" (MIDI Show Control protocol). Tabs land alongside the
 * existing DMX tab without further window plumbing.
 *
 * Editing convention: the text area is the canonical authoring
 * surface. Edits dirty the project. A typed table editor is a
 * follow-up that can be added without a schema change.
 */
class ShowControlWindow : public EditorWindow {
public:
    explicit ShowControlWindow(Engine* engine);
    ~ShowControlWindow() override = default;

    void render() override;
    const char* getName() const override { return "Show Control"; }

private:
    Engine* m_engine{nullptr};
};

} // namespace entity
