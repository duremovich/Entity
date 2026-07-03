// OSC inbound mapping table tests.
// Exercises parseInboundMappingsJson, matchPattern, expandTemplate, and
// defaultRoutes from OscInboundMappings.hpp — the same logic the receiver
// plugin uses for per-project routing.

#include "../../plugins/osc-receiver/OscInboundMappings.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace osc_inbound;

// Helper: encode an int32 as 4 big-endian bytes.
static std::vector<std::uint8_t> i32Bytes(std::int32_t v) {
    return {
        std::uint8_t(v >> 24), std::uint8_t(v >> 16),
        std::uint8_t(v >>  8), std::uint8_t(v      )
    };
}

// Helper: encode a float32 as 4 big-endian bytes.
static std::vector<std::uint8_t> f32Bytes(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return {
        std::uint8_t(bits >> 24), std::uint8_t(bits >> 16),
        std::uint8_t(bits >>  8), std::uint8_t(bits      )
    };
}

// ---------------------------------------------------------------------------
// defaultRoutes

TEST(OscMappingTable, DefaultRoutesNotEmpty) {
    auto routes = defaultRoutes();
    EXPECT_FALSE(routes.empty());
}

TEST(OscMappingTable, DefaultRoutesContainsPlay) {
    auto routes = defaultRoutes();
    bool found = std::any_of(routes.begin(), routes.end(), [](const InboundRoute& r) {
        return r.addressPattern == "/entity/play" &&
               !r.commands.empty() &&
               r.commands[0].commandType == "Play";
    });
    EXPECT_TRUE(found);
}

TEST(OscMappingTable, DefaultRoutesContainsCueCaptureRoute) {
    auto routes = defaultRoutes();
    bool found = std::any_of(routes.begin(), routes.end(), [](const InboundRoute& r) {
        return r.addressPattern == "/entity/cue/{number}/go" &&
               r.captureKey == "number";
    });
    EXPECT_TRUE(found);
}

TEST(OscMappingTable, DefaultRoutesContainsCueArmRoute) {
    auto routes = defaultRoutes();
    bool found = std::any_of(routes.begin(), routes.end(), [](const InboundRoute& r) {
        return r.addressPattern == "/entity/cue/{number}/arm" &&
               r.captureKey == "number" &&
               !r.commands.empty() &&
               r.commands[0].commandType == "ArmCue";
    });
    EXPECT_TRUE(found);
}

TEST(OscMappingTable, DefaultRoutesStopIsMultiCommand) {
    auto routes = defaultRoutes();
    auto it = std::find_if(routes.begin(), routes.end(), [](const InboundRoute& r) {
        return r.addressPattern == "/entity/stop";
    });
    ASSERT_NE(it, routes.end());
    EXPECT_GE(it->commands.size(), 2u); // Pause + SeekToFrame
}

// ---------------------------------------------------------------------------
// parseInboundMappingsJson

TEST(OscMappingTable, EmptyJsonFallsBackToDefaults) {
    auto routes = parseInboundMappingsJson("");
    EXPECT_FALSE(routes.empty()); // default routes returned
    bool hasPlay = std::any_of(routes.begin(), routes.end(), [](const InboundRoute& r) {
        return r.addressPattern == "/entity/play";
    });
    EXPECT_TRUE(hasPlay);
}

TEST(OscMappingTable, NotAnArrayFallsBackToDefaults) {
    auto routes = parseInboundMappingsJson("{\"oops\":true}");
    EXPECT_FALSE(routes.empty());
}

TEST(OscMappingTable, EmptyArrayFallsBackToDefaults) {
    auto routes = parseInboundMappingsJson("[]");
    EXPECT_FALSE(routes.empty()); // empty → fallback to defaults
}

TEST(OscMappingTable, ParsesSingleRoute) {
    const char* json = R"([
        {
            "address": "/my/play",
            "commands": [{"type": "Play", "params": ""}]
        }
    ])";
    auto routes = parseInboundMappingsJson(json);
    ASSERT_EQ(routes.size(), 1u);
    EXPECT_EQ(routes[0].addressPattern, "/my/play");
    ASSERT_EQ(routes[0].commands.size(), 1u);
    EXPECT_EQ(routes[0].commands[0].commandType, "Play");
    EXPECT_EQ(routes[0].commands[0].paramsTemplate, "");
}

TEST(OscMappingTable, ParsesCaptureKey) {
    const char* json = R"([
        {
            "address": "/show/cue/{N}/go",
            "captureKey": "N",
            "commands": [{"type": "FireCue", "params": "{\"number\":$capturef}"}]
        }
    ])";
    auto routes = parseInboundMappingsJson(json);
    ASSERT_EQ(routes.size(), 1u);
    EXPECT_EQ(routes[0].captureKey, "N");
    EXPECT_EQ(routes[0].commands[0].paramsTemplate, "{\"number\":$capturef}");
}

TEST(OscMappingTable, CaptureKeyDerivedFromPatternWhenAbsent) {
    const char* json = R"([
        {
            "address": "/entity/cue/{number}/go",
            "commands": [{"type": "FireCue", "params": ""}]
        }
    ])";
    auto routes = parseInboundMappingsJson(json);
    ASSERT_EQ(routes.size(), 1u);
    // captureKey not given → derived from {number} in pattern
    EXPECT_EQ(routes[0].captureKey, "number");
}

TEST(OscMappingTable, ParsesMultiCommandRoute) {
    const char* json = R"([
        {
            "address": "/custom/stop",
            "commands": [
                {"type": "Pause", "params": ""},
                {"type": "SeekToFrame", "params": "{\"frame\":0}"}
            ]
        }
    ])";
    auto routes = parseInboundMappingsJson(json);
    ASSERT_EQ(routes.size(), 1u);
    ASSERT_EQ(routes[0].commands.size(), 2u);
    EXPECT_EQ(routes[0].commands[0].commandType, "Pause");
    EXPECT_EQ(routes[0].commands[1].commandType, "SeekToFrame");
}

TEST(OscMappingTable, MalformedJsonFallsBackToDefaults) {
    auto routes = parseInboundMappingsJson("[{bad");
    EXPECT_FALSE(routes.empty());
}

TEST(OscMappingTable, UnknownFieldsIgnored) {
    const char* json = R"([
        {
            "address": "/foo",
            "unknown_field": "ignored",
            "commands": [{"type": "Play", "params": ""}]
        }
    ])";
    auto routes = parseInboundMappingsJson(json);
    ASSERT_EQ(routes.size(), 1u);
    EXPECT_EQ(routes[0].addressPattern, "/foo");
}

// ---------------------------------------------------------------------------
// matchPattern

TEST(OscMappingTable, LiteralMatchHit) {
    std::string_view cap;
    EXPECT_TRUE(matchPattern("/entity/play", "/entity/play", cap));
    EXPECT_TRUE(cap.empty());
}

TEST(OscMappingTable, LiteralMatchMiss) {
    std::string_view cap;
    EXPECT_FALSE(matchPattern("/entity/play", "/entity/pause", cap));
}

TEST(OscMappingTable, CaptureMatch) {
    std::string_view cap;
    EXPECT_TRUE(matchPattern("/entity/cue/{number}/go", "/entity/cue/5/go", cap));
    EXPECT_EQ(cap, "5");
}

TEST(OscMappingTable, CaptureMatchDecimalNumber) {
    std::string_view cap;
    EXPECT_TRUE(matchPattern("/show/cue/{N}/go", "/show/cue/1.5/go", cap));
    EXPECT_EQ(cap, "1.5");
}

TEST(OscMappingTable, CaptureMatchMiss_WrongPrefix) {
    std::string_view cap;
    EXPECT_FALSE(matchPattern("/entity/cue/{number}/go", "/other/cue/5/go", cap));
}

TEST(OscMappingTable, CaptureMatchMiss_WrongSuffix) {
    std::string_view cap;
    EXPECT_FALSE(matchPattern("/entity/cue/{number}/go", "/entity/cue/5/fire", cap));
}

TEST(OscMappingTable, CaptureMatchEmptySegmentFails) {
    // The capture segment must be non-empty.
    std::string_view cap;
    EXPECT_FALSE(matchPattern("/entity/cue/{number}/go", "/entity/cue//go", cap));
}

// ---------------------------------------------------------------------------
// expandTemplate

// No tokens — pass through unchanged.
TEST(OscMappingTable, ExpandTemplate_Literal) {
    auto result = expandTemplate("{\"frame\":0}", {}, ",i", nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "{\"frame\":0}");
}

TEST(OscMappingTable, ExpandTemplate_Empty) {
    auto result = expandTemplate("", {}, ",i", nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "");
}

// $arg0i with int32 argument.
TEST(OscMappingTable, ExpandTemplate_Arg0i) {
    auto args = i32Bytes(100);
    auto result = expandTemplate("{\"frame\":$arg0i}", {},
                                  ",i", args.data(), args.data() + args.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "{\"frame\":100}");
}

// $arg0i missing arg → nullopt.
TEST(OscMappingTable, ExpandTemplate_Arg0i_NoArg_ReturnsNullopt) {
    auto result = expandTemplate("{\"frame\":$arg0i}", {}, ",i", nullptr, nullptr);
    EXPECT_FALSE(result.has_value());
}

// $arg0f with float argument.
TEST(OscMappingTable, ExpandTemplate_Arg0f) {
    auto args = f32Bytes(0.5f);
    auto result = expandTemplate("{\"value\":$arg0f}", {},
                                  ",f", args.data(), args.data() + args.size());
    ASSERT_TRUE(result.has_value());
    // 0.5 in %.10g is "0.5"
    EXPECT_EQ(*result, "{\"value\":0.5}");
}

// $capturef with a numeric capture.
TEST(OscMappingTable, ExpandTemplate_Capturef) {
    auto result = expandTemplate("{\"number\":$capturef}", "5",
                                  ",", nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "{\"number\":5}");
}

// $capturef with a decimal capture (fractional cue number).
TEST(OscMappingTable, ExpandTemplate_Capturef_Decimal) {
    auto result = expandTemplate("{\"number\":$capturef}", "1.5",
                                  ",", nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "{\"number\":1.5}");
}

// $capturei with an integer capture.
TEST(OscMappingTable, ExpandTemplate_Capturei) {
    auto result = expandTemplate("{\"number\":$capturei}", "42",
                                  ",", nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "{\"number\":42}");
}

// $capturef with empty capture → nullopt.
TEST(OscMappingTable, ExpandTemplate_Capturef_EmptyCapture_Nullopt) {
    auto result = expandTemplate("{\"number\":$capturef}", "",
                                  ",", nullptr, nullptr);
    EXPECT_FALSE(result.has_value());
}

// Unknown $token passes through literally.
TEST(OscMappingTable, ExpandTemplate_UnknownToken_Passthrough) {
    auto result = expandTemplate("$unknown", {}, ",", nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "$unknown");
}

// ---------------------------------------------------------------------------
// defaultRoutes — muncher routes removed (ADR-0028)

TEST(OscMappingTable, DefaultRoutesHasNoMuncher) {
    auto routes = defaultRoutes();
    for (const auto& r : routes) {
        EXPECT_EQ(r.addressPattern.find("/entity/muncher"), std::string::npos)
            << "Unexpected muncher route: " << r.addressPattern;
    }
}

// ---------------------------------------------------------------------------
// parseLayerAddress

TEST(OscLayerAddress, ParsesCoreParams) {
    auto a = parseLayerAddress("/entity/layer/video1/opacity");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, "video1");
    EXPECT_EQ(a->kind, LayerParamKind::Opacity);

    auto b = parseLayerAddress("/entity/layer/wall_left/pos/x");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->kind, LayerParamKind::PosX);

    auto c = parseLayerAddress("/entity/layer/m1/remote");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->kind, LayerParamKind::Remote);

    auto d = parseLayerAddress("/entity/layer/t1/text");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->kind, LayerParamKind::Text);

    auto e = parseLayerAddress("/entity/layer/wall_left/scale/y");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->kind, LayerParamKind::ScaleY);

    auto f = parseLayerAddress("/entity/layer/r1/rotation");
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->kind, LayerParamKind::Rotation);
    EXPECT_STREQ(f->paramString, "rotation");
}

TEST(OscLayerAddress, RejectsBadInput) {
    EXPECT_FALSE(parseLayerAddress("/entity/play").has_value());
    EXPECT_FALSE(parseLayerAddress("/entity/layer/").has_value());
    EXPECT_FALSE(parseLayerAddress("/entity/layer/video1").has_value());
    EXPECT_FALSE(parseLayerAddress("/entity/layer/Video1/opacity").has_value());
    EXPECT_FALSE(parseLayerAddress("/entity/layer/video1/bogus").has_value());
    EXPECT_FALSE(parseLayerAddress("").has_value());
    EXPECT_FALSE(parseLayerAddress("/entity/layer//opacity").has_value());
}

TEST(OscLayerAddress, IsLayerNamespacePositive) {
    // These have the /entity/layer/ prefix — dispatch must trap them even
    // when parseLayerAddress returns nullopt (unknown param, bad id charset).
    EXPECT_TRUE(isLayerNamespace("/entity/layer/video1/bogus"));
    EXPECT_TRUE(isLayerNamespace("/entity/layer/Video1/opacity"));  // uppercase id
    EXPECT_TRUE(isLayerNamespace("/entity/layer/video1/opacity"));
    EXPECT_TRUE(isLayerNamespace("/entity/layer/x/text"));
}

TEST(OscLayerAddress, IsLayerNamespaceNegative) {
    // Non-layer addresses must not be caught by the prefix check.
    EXPECT_FALSE(isLayerNamespace("/entity/play"));
    EXPECT_FALSE(isLayerNamespace("/entity/layer/"));  // exactly the prefix, no trailing content
    EXPECT_FALSE(isLayerNamespace("/entity/muncher/up"));
    EXPECT_FALSE(isLayerNamespace(""));
    EXPECT_FALSE(isLayerNamespace("/other/layer/video1/opacity"));
}

TEST(OscLayerAddress, IdAndParamStringRoundTrip) {
    auto a = parseLayerAddress("/entity/layer/my_layer_1/pos/y");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, "my_layer_1");
    EXPECT_EQ(a->kind, LayerParamKind::PosY);
    EXPECT_STREQ(a->paramString, "pos/y");
}

// ---------------------------------------------------------------------------
// firstStringArg

TEST(OscLayerAddress, FirstStringArgFindsString) {
    // Build a wire-format OSC 's' argument: null-terminated + 4-byte pad.
    // "hello" = 5 chars + null = 6, padded to 8 bytes.
    std::vector<std::uint8_t> args = {
        'h','e','l','l','o','\0','\0','\0'
    };
    auto result = firstStringArg(",s", args.data(), args.data() + args.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello");
}

TEST(OscLayerAddress, FirstStringArgSkipsPrecedingNumericArg) {
    // typeTag ",fs" — first arg is float (4 bytes), second is string.
    std::vector<std::uint8_t> args;
    // float 0.0 = 4 zero bytes
    args.insert(args.end(), {0,0,0,0});
    // "hi" + null padded to 4
    args.insert(args.end(), {'h','i','\0','\0'});
    auto result = firstStringArg(",fs", args.data(), args.data() + args.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hi");
}

TEST(OscLayerAddress, FirstStringArgNoStringReturnsNullopt) {
    std::vector<std::uint8_t> args = {0,0,0,0};
    auto result = firstStringArg(",f", args.data(), args.data() + args.size());
    EXPECT_FALSE(result.has_value());
}

TEST(OscLayerAddress, FirstStringArgEmptyTypeTagReturnsNullopt) {
    auto result = firstStringArg("", nullptr, nullptr);
    EXPECT_FALSE(result.has_value());
}
