// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "CommandEmitter.hpp"

#include "entity/plugin/PluginContext.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace entity::dmx {

namespace {

using entity::plugin::IPluginContext;
using entity::plugin::LogLevel;

constexpr std::uint8_t kEdgeThreshold = 128;

bool rose(std::uint8_t a, std::uint8_t b) {
    return a < kEdgeThreshold && b >= kEdgeThreshold;
}

// Convenience: log the firing breadcrumb. Always tagged "[dmx-artnet]"
// so the integration tests can grep on a stable prefix; the per-mapping
// label + tag let humans tell channels apart in the log.
void logFire(IPluginContext* ctx, const Mapping& m,
             std::uint16_t universe, std::uint16_t channel1Based,
             std::uint8_t value, const char* tag) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "fired %s (%s u=%u ch=%u value=%u label='%s')",
                  m.commandType.c_str(),
                  tag ? tag : "",
                  unsigned(universe), unsigned(channel1Based), unsigned(value),
                  m.label.c_str());
    ctx->log(LogLevel::Info, buf);
}

// Build the params JSON for a mapping fire. Returns "" for commands
// with no params; substitutes `{value}` and `{cue}` placeholders in
// paramsTemplate when present.
std::string buildParams(const Mapping& m, double value, double cueNumber) {
    if (m.paramsTemplate.empty()) {
        // For FireCue with no template, cue number is required.
        if (m.commandType == "FireCue") {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{\"number\":%.6g}", cueNumber);
            return buf;
        }
        // For SetInputChannel-style continuous, supply the value if
        // someone wires it that way — but Continuous mapping also has
        // no built-in channel name; the template is the typical
        // authoring path. Leave empty.
        return "";
    }

    // Replace {value} / {cue} tokens.
    std::string s = m.paramsTemplate;
    auto replace = [&](const char* needle, double v) {
        std::string n = needle;
        auto pos = s.find(n);
        while (pos != std::string::npos) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            s.replace(pos, n.size(), buf);
            pos = s.find(n, pos);
        }
    };
    replace("{value}", value);
    replace("{cue}",   cueNumber);
    return s;
}

double scale(const Mapping& m, std::uint8_t raw) {
    const double t = static_cast<double>(raw) / 255.0;
    return m.scaleMin + (m.scaleMax - m.scaleMin) * t;
}

} // namespace

void emitMappingFires(IPluginContext*                ctx,
                       const std::vector<Mapping>&    mappings,
                       std::uint16_t                  universe,
                       const std::uint8_t*            before,
                       const std::uint8_t*            after,
                       const char*                    tag) {
    if (ctx == nullptr || before == nullptr || after == nullptr) return;

    for (const auto& m : mappings) {
        if (m.universe != universe) continue;
        if (m.startChannel == 0) continue; // 1-based; 0 is invalid

        // Channel block: indices [startChannel-1, startChannel-1+count)
        // in 0-based array space, clamped to [0, 512).
        const std::uint16_t startIdx = static_cast<std::uint16_t>(m.startChannel - 1);
        if (startIdx >= 512) continue;
        const std::uint16_t endIdx = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(startIdx + std::uint32_t(m.channelCount), 512u));

        switch (m.kind) {
        case TriggerKind::ThresholdEdge: {
            // Edge on ANY channel in the block fires once (rare for the
            // block size > 1, but handles "this fader anywhere in the
            // block").
            for (std::uint16_t i = startIdx; i < endIdx; ++i) {
                if (rose(before[i], after[i])) {
                    const std::string params = buildParams(m, after[i], 0.0);
                    if (ctx->enqueueCommand(m.commandType, params)) {
                        logFire(ctx, m, universe,
                                static_cast<std::uint16_t>(i + 1),
                                after[i], tag);
                    }
                    break; // one fire per mapping per frame
                }
            }
            break;
        }
        case TriggerKind::Continuous: {
            // Fire only when channel value changed. Single-channel
            // mapping (count > 1 takes the first channel only).
            if (before[startIdx] != after[startIdx]) {
                const double value = scale(m, after[startIdx]);
                const std::string params = buildParams(m, value, 0.0);
                if (ctx->enqueueCommand(m.commandType, params)) {
                    logFire(ctx, m, universe, m.startChannel, after[startIdx], tag);
                }
            }
            break;
        }
        case TriggerKind::CueNumberSlot: {
            // Single-channel cue selector. Fires on change to non-zero.
            if (before[startIdx] != after[startIdx] && after[startIdx] != 0) {
                // Map raw 0-255 to a cue number via scale range
                // (default 0..255 -> raw number-as-cue).
                const double cue = scale(m, after[startIdx]);
                const std::string params = buildParams(m, after[startIdx], cue);
                if (ctx->enqueueCommand(m.commandType, params)) {
                    logFire(ctx, m, universe, m.startChannel, after[startIdx], tag);
                }
            }
            break;
        }
        case TriggerKind::ContiguousCueBlock: {
            // Channel-N-fires-cue-N. Edge per channel within the block.
            for (std::uint16_t i = startIdx; i < endIdx; ++i) {
                if (rose(before[i], after[i])) {
                    const double cue = static_cast<double>(i - startIdx + 1);
                    const std::string params = buildParams(m, after[i], cue);
                    if (ctx->enqueueCommand(m.commandType, params)) {
                        logFire(ctx, m, universe,
                                static_cast<std::uint16_t>(i + 1),
                                after[i], tag);
                    }
                }
            }
            break;
        }
        }
    }
}

} // namespace entity::dmx
