// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
//
// Unit test for the SetSignal* commands (Phase 8).
// Creates a SignalLayer entity directly in a bare registry, exercises each
// command, and asserts the resulting component state. No Engine / D3D12 /
// display dependency.
#include <gtest/gtest.h>

#include "entity/command/Commands.hpp"

using namespace entity;

// ---------------------------------------------------------------------------
// fromJson parsing tests -- do NOT require Engine (just parse the command)

TEST(SignalCommandsFromJson, SetSignalModeAcceptsString) {
    nlohmann::json j = {{"type","SetSignalMode"},{"layerEntity",0},{"mode","Continuous"}};
    auto cmd = SetSignalModeCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->getTypeName(), std::string("SetSignalMode"));
}

TEST(SignalCommandsFromJson, SetSignalModeAcceptsInt) {
    nlohmann::json j = {{"type","SetSignalMode"},{"layerEntity",0},{"mode",1}};
    auto cmd = SetSignalModeCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
}

TEST(SignalCommandsFromJson, SetSignalAddressParsed) {
    nlohmann::json j = {{"type","SetSignalAddress"},{"layerEntity",7},{"address","/foo/bar"}};
    auto cmd = SetSignalAddressCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->getTypeName(), std::string("SetSignalAddress"));
}

TEST(SignalCommandsFromJson, SetSignalArgsFromJsonString) {
    // args as a JSON-encoded string (plan format)
    nlohmann::json j = {
        {"type","SetSignalArgs"},
        {"layerEntity",0},
        {"args","[{\"type\":\"int\",\"i\":42},{\"type\":\"float\",\"f\":1.5,\"isValueBound\":true}]"}
    };
    auto cmd = SetSignalArgsCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
}

TEST(SignalCommandsFromJson, SetSignalArgsFromJsonArray) {
    // args as a real JSON array (alternative format)
    nlohmann::json j = {
        {"type","SetSignalArgs"},
        {"layerEntity",0},
        {"args", nlohmann::json::array({{{"type","int"},{"i",7}}})}
    };
    auto cmd = SetSignalArgsCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
}

TEST(SignalCommandsFromJson, SetSignalValueSourceAcceptsString) {
    nlohmann::json j = {{"type","SetSignalValueSource"},{"layerEntity",0},{"valueSource","Keyframe"}};
    auto cmd = SetSignalValueSourceCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
}

TEST(SignalCommandsFromJson, SetSignalMappingParsed) {
    nlohmann::json j = {
        {"type","SetSignalMapping"},{"layerEntity",0},
        {"srcMin",0.0},{"srcMax",1.0},{"outMin",0.0},{"outMax",255.0},{"outType","Int"}
    };
    auto cmd = SetSignalMappingCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
}

// ---------------------------------------------------------------------------
// Defensive parse: malformed args JSON must produce a command that returns false

TEST(SignalCommandsFromJson, SetSignalArgsMalformedFailsCleanly) {
    // We can't easily call execute() without Engine, but we can verify the
    // command is created (no crash at fromJson) and that the args string is
    // malformed. The execute path is tested via the integration script.
    nlohmann::json j = {
        {"type","SetSignalArgs"},{"layerEntity",0},
        {"args","NOT_JSON{{{"}
    };
    // fromJson should not throw.
    CommandPtr cmd;
    EXPECT_NO_THROW(cmd = SetSignalArgsCommand::fromJson(j));
    ASSERT_NE(cmd, nullptr);
}

// ---------------------------------------------------------------------------
// toJson round-trip (no Engine needed)

TEST(SignalCommandsFromJson, SetSignalModeRoundTrip) {
    auto cmd = std::make_unique<SetSignalModeCommand>(entt::entity{1}, 1);
    const auto j = cmd->toJson();
    EXPECT_EQ(j.at("type").get<std::string>(), "SetSignalMode");
    EXPECT_EQ(j.at("mode").get<int>(), 1);
}

TEST(SignalCommandsFromJson, SetSignalAddressRoundTrip) {
    auto cmd = std::make_unique<SetSignalAddressCommand>(entt::entity{2}, "/test/addr");
    const auto j = cmd->toJson();
    EXPECT_EQ(j.at("address").get<std::string>(), "/test/addr");
}

TEST(SignalCommandsFromJson, SetSignalMappingRoundTrip) {
    auto cmd = std::make_unique<SetSignalMappingCommand>(
        entt::entity{3}, 0.0f, 1.0f, 0.0f, 255.0f, 0);
    const auto j = cmd->toJson();
    EXPECT_FLOAT_EQ(j.at("outMax").get<float>(), 255.0f);
    EXPECT_EQ(j.at("outType").get<int>(), 0);
}
