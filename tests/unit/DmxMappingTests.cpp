// SPDX-License-Identifier: Apache-2.0
// JSON round-trip + edge-case tests for the per-project DMX mapping
// table. The shared DmxMapping types live in core (include/entity/dmx/)
// so both the editor's ShowControlWindow and the dmx-artnet plugin's
// MappingResolver reference the same struct without duplication.

#include "entity/dmx/DmxMapping.hpp"

#include <gtest/gtest.h>

using entity::dmx::Mapping;
using entity::dmx::TriggerKind;
using entity::dmx::parseMappingsJson;
using entity::dmx::serializeMappingsJson;

TEST(DmxMappingTests, EmptyStringYieldsEmptyVector) {
    EXPECT_TRUE(parseMappingsJson("").empty());
}

TEST(DmxMappingTests, EmptyArrayYieldsEmptyVector) {
    EXPECT_TRUE(parseMappingsJson("[]").empty());
}

TEST(DmxMappingTests, MalformedJsonFallsBackToEmpty) {
    EXPECT_TRUE(parseMappingsJson("not json").empty());
    EXPECT_TRUE(parseMappingsJson("{\"oops\":true}").empty()); // object, not array
    EXPECT_TRUE(parseMappingsJson("[{").empty());
}

TEST(DmxMappingTests, ParsesSingleThresholdEdgeRow) {
    const char* raw = R"([{
        "universe": 5,
        "startChannel": 100,
        "channelCount": 1,
        "kind": "ThresholdEdge",
        "commandType": "SectionGo",
        "label": "Op-controlled GO"
    }])";
    auto parsed = parseMappingsJson(raw);
    ASSERT_EQ(parsed.size(), 1u);
    const auto& m = parsed[0];
    EXPECT_EQ(m.universe, 5);
    EXPECT_EQ(m.startChannel, 100);
    EXPECT_EQ(m.channelCount, 1);
    EXPECT_EQ(m.kind, TriggerKind::ThresholdEdge);
    EXPECT_EQ(m.commandType, "SectionGo");
    EXPECT_EQ(m.label, "Op-controlled GO");
    // Defaults preserved when JSON omits them.
    EXPECT_EQ(m.scaleMin, 0.0);
    EXPECT_EQ(m.scaleMax, 255.0);
    EXPECT_TRUE(m.paramsTemplate.empty());
}

TEST(DmxMappingTests, ParsesAllFourTriggerKinds) {
    const char* raw = R"([
      {"universe": 0, "startChannel": 1, "kind": "ThresholdEdge",      "commandType": "Play"},
      {"universe": 0, "startChannel": 2, "kind": "Continuous",         "commandType": "SetInputChannel"},
      {"universe": 0, "startChannel": 3, "kind": "CueNumberSlot",      "commandType": "FireCue"},
      {"universe": 0, "startChannel": 4, "channelCount": 8, "kind": "ContiguousCueBlock", "commandType": "FireCue"}
    ])";
    auto parsed = parseMappingsJson(raw);
    ASSERT_EQ(parsed.size(), 4u);
    EXPECT_EQ(parsed[0].kind, TriggerKind::ThresholdEdge);
    EXPECT_EQ(parsed[1].kind, TriggerKind::Continuous);
    EXPECT_EQ(parsed[2].kind, TriggerKind::CueNumberSlot);
    EXPECT_EQ(parsed[3].kind, TriggerKind::ContiguousCueBlock);
    EXPECT_EQ(parsed[3].channelCount, 8);
}

TEST(DmxMappingTests, UnknownKindFallsBackToThresholdEdge) {
    const char* raw = R"([{"universe": 0, "startChannel": 1, "kind": "Surprise"}])";
    auto parsed = parseMappingsJson(raw);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].kind, TriggerKind::ThresholdEdge);
}

TEST(DmxMappingTests, RoundTripPreservesAllFields) {
    std::vector<Mapping> original;
    {
        Mapping m;
        m.universe       = 3;
        m.startChannel   = 17;
        m.channelCount   = 4;
        m.kind           = TriggerKind::Continuous;
        m.commandType    = "SetInputChannel";
        m.paramsTemplate = R"({"channel":"muncher.input.x","value":{value}})";
        m.scaleMin       = -1.0;
        m.scaleMax       = 1.0;
        m.label          = "Joystick X";
        original.push_back(m);
    }
    {
        Mapping m;
        m.universe     = 0;
        m.startChannel = 1;
        m.channelCount = 1;
        m.kind         = TriggerKind::ThresholdEdge;
        m.commandType  = "Play";
        original.push_back(m);
    }

    const std::string json = serializeMappingsJson(original);
    const auto reparsed = parseMappingsJson(json);

    ASSERT_EQ(reparsed.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_EQ(reparsed[i].universe,       original[i].universe);
        EXPECT_EQ(reparsed[i].startChannel,   original[i].startChannel);
        EXPECT_EQ(reparsed[i].channelCount,   original[i].channelCount);
        EXPECT_EQ(reparsed[i].kind,           original[i].kind);
        EXPECT_EQ(reparsed[i].commandType,    original[i].commandType);
        EXPECT_EQ(reparsed[i].paramsTemplate, original[i].paramsTemplate);
        EXPECT_DOUBLE_EQ(reparsed[i].scaleMin, original[i].scaleMin);
        EXPECT_DOUBLE_EQ(reparsed[i].scaleMax, original[i].scaleMax);
        EXPECT_EQ(reparsed[i].label,          original[i].label);
    }
}

TEST(DmxMappingTests, SerializerOmitsDefaultScaleRange) {
    Mapping m;
    m.universe = 0;
    m.startChannel = 1;
    m.commandType = "Play";
    m.kind = TriggerKind::ThresholdEdge;
    // Default scale 0..255 should NOT be serialized.
    const std::string json = serializeMappingsJson({m});
    EXPECT_EQ(json.find("scaleMin"), std::string::npos);
    EXPECT_EQ(json.find("scaleMax"), std::string::npos);
    // Custom range SHOULD be serialized.
    m.scaleMin = 1.0;
    m.scaleMax = 100.0;
    const std::string json2 = serializeMappingsJson({m});
    EXPECT_NE(json2.find("scaleMin"), std::string::npos);
    EXPECT_NE(json2.find("scaleMax"), std::string::npos);
}
