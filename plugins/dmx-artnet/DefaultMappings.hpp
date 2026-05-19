// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace entity::dmx {

// How a single mapping interprets channel-byte changes.
enum class TriggerKind : std::uint8_t {
    // Channel value crossing < 128 -> >= 128 fires once. Standard for
    // transport / cue triggers driven by a lighting console fader.
    ThresholdEdge       = 0,
    // Channel value change fires every frame, value scaled into
    // [scaleMin, scaleMax] and forwarded as the command's primary
    // parameter (intended for SetInputChannel). Continuous analog.
    Continuous          = 1,
    // Channel value IS the cue number (1..255 -> FireCue). Fires on
    // change to a non-zero value. Lighting-console style.
    CueNumberSlot       = 2,
    // Each channel in [startChannel, startChannel + channelCount) is its
    // own cue trigger; index within the block (1-based) = cue number.
    // QLab-style.
    ContiguousCueBlock  = 3,
};

// One mapping entry. Phase 5's project-mappings UI authors these as
// rows in a table; Phase 1's DefaultMappings hand-codes a baked set.
struct Mapping {
    std::uint16_t  universe{0};
    std::uint16_t  startChannel{1};   // 1-based, like DMX consoles
    std::uint16_t  channelCount{1};

    TriggerKind    kind{TriggerKind::ThresholdEdge};

    // The dispatcher type name (e.g. "Play", "Pause", "FireCue").
    std::string    commandType;

    // Optional params template — for commands like SeekToFrame that
    // expect a stable key. Single-key/value JSON; CommandEmitter
    // substitutes the channel-derived value at fire time. Empty for
    // commands with no params (Play/Pause/SectionGo).
    std::string    paramsTemplate;

    // Used by Continuous (and by CueNumberSlot for an optional value
    // remap). Default range = raw 0..255.
    double         scaleMin{0.0};
    double         scaleMax{255.0};

    // Human-readable label for the UI. Optional.
    std::string    label;
};

// Baked default mappings used iff the active project has no
// dmxMappings array (Phase 5). Out-of-box experience: an operator
// enables the plugin and sends Universe 0 Channel 1 high -> Play
// fires without first learning the mapping editor.
const std::vector<Mapping>& bakedDefaultMappings();

} // namespace entity::dmx
