#pragma once

#include "EditorWindow.hpp"

namespace entity {

class Engine;

/**
 * ShowControlWindow -- per-project show-control mapping editor.
 *
 * Tabs:
 *   DMX    -- channel-to-command mapping table (project field dmxMappingsJson)
 *   OSC In -- inbound address-pattern-to-command route table (oscInboundMappingsJson)
 *   OSC Out -- outbound event enable/address overrides (oscOutboundMappingsJson)
 *
 * Future tabs: "MIDI" (MIDI Show Control protocol).
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
