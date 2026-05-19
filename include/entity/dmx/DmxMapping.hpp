// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

// Shared DmxMapping type + JSON serializers. Lives in core's include
// tree so both the dmx-artnet plugin and the editor's
// ShowControlWindow reference the same struct without duplication.
// Apache 2.0 so the plugin can include it under plugin-api boundary
// rules.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace entity::dmx {

// How a single mapping interprets channel-byte changes.
enum class TriggerKind : std::uint8_t {
    // Channel value crossing < 128 -> >= 128 fires once. Standard for
    // transport / cue triggers driven by a lighting console fader.
    ThresholdEdge       = 0,
    // Channel value change fires every frame, value scaled into
    // [scaleMin, scaleMax] and forwarded as the command's primary
    // parameter. Continuous analog.
    Continuous          = 1,
    // Channel value IS the cue number. Fires on change to non-zero.
    CueNumberSlot       = 2,
    // Each channel in [startChannel, startChannel + channelCount) is
    // its own cue trigger; index within the block (1-based) = cue number.
    ContiguousCueBlock  = 3,
};

struct Mapping {
    std::uint16_t  universe{0};
    std::uint16_t  startChannel{1};   // 1-based
    std::uint16_t  channelCount{1};

    TriggerKind    kind{TriggerKind::ThresholdEdge};

    // Dispatcher type name (e.g. "Play", "Pause", "FireCue").
    std::string    commandType;

    // Optional params template for commands that take structured
    // arguments. Supports {value} and {cue} placeholders that the
    // emitter substitutes at fire time. Empty for no-arg commands.
    std::string    paramsTemplate;

    // Scale range applied to Continuous + CueNumberSlot. Default
    // 0..255 = raw value forwarded unchanged.
    double         scaleMin{0.0};
    double         scaleMax{255.0};

    // Human-readable label for the UI. Optional.
    std::string    label;
};

// JSON round-trip. Empty/malformed input -> empty vector.
std::vector<Mapping> parseMappingsJson(std::string_view raw);

// Inverse of parseMappingsJson. Round-trip stable.
std::string serializeMappingsJson(const std::vector<Mapping>& mappings);

// Enum <-> string for TriggerKind. Used by the UI + the JSON
// serializer; exposed so the table editor can render dropdowns.
const char* kindToString(TriggerKind k);
TriggerKind parseKind(const std::string& s);

} // namespace entity::dmx
