// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "DefaultMappings.hpp"

namespace entity::dmx {

namespace {

std::vector<Mapping> buildBaked() {
    std::vector<Mapping> out;
    // Universe 0 channels 1-4: transport.
    out.push_back({0, 1, 1, TriggerKind::ThresholdEdge, "Play",      "", 0, 255, "Play"});
    out.push_back({0, 2, 1, TriggerKind::ThresholdEdge, "Pause",     "", 0, 255, "Pause"});
    out.push_back({0, 3, 1, TriggerKind::ThresholdEdge, "Pause",     "", 0, 255, "Stop"});
    // We model "Stop" as Pause + SeekToFrame:0 by emitting both edges
    // from channel 3 — but ThresholdEdge fires one command per mapping
    // entry. Stash a second mapping so the second action lands too.
    Mapping stopSeek;
    stopSeek.universe       = 0;
    stopSeek.startChannel   = 3;
    stopSeek.channelCount   = 1;
    stopSeek.kind           = TriggerKind::ThresholdEdge;
    stopSeek.commandType    = "SeekToFrame";
    stopSeek.paramsTemplate = "{\"frame\":0}";
    stopSeek.label          = "Stop (seek 0)";
    out.push_back(stopSeek);

    out.push_back({0, 4, 1, TriggerKind::ThresholdEdge, "SectionGo", "", 0, 255, "Section GO"});

    // Channels 5-8: FireCue 1-4 via ContiguousCueBlock. One block
    // mapping covers all four channels; the emitter expands per
    // channel within the block.
    Mapping cues;
    cues.universe     = 0;
    cues.startChannel = 5;
    cues.channelCount = 4;
    cues.kind         = TriggerKind::ContiguousCueBlock;
    cues.commandType  = "FireCue";
    cues.label        = "FireCue 1-4";
    out.push_back(cues);
    return out;
}

} // namespace

const std::vector<Mapping>& bakedDefaultMappings() {
    static const std::vector<Mapping> kBaked = buildBaked();
    return kBaked;
}

} // namespace entity::dmx
