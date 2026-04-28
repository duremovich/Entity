#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace entity;
using namespace entity::bus;

namespace {

template <class T>
T roundTripExpect(const T& original) {
    Message msg = original;
    auto bytes = serialize(msg);
    auto decoded = deserialize(bytes);
    EXPECT_TRUE(decoded.has_value());
    EXPECT_TRUE(std::holds_alternative<T>(*decoded));
    return std::get<T>(*decoded);
}

template <class T>
void expectByteStable(const T& original) {
    Message msg = original;
    auto bytes1 = serialize(msg);
    auto decoded = deserialize(bytes1);
    ASSERT_TRUE(decoded.has_value());
    auto bytes2 = serialize(*decoded);
    EXPECT_EQ(bytes1, bytes2)
        << "serialize→deserialize→serialize must produce byte-identical output for "
        << messageTypeName(msg);
}

} // namespace

// ----------------------------------------------------------------------------
// Round-trip every message kind. Field-by-field for the biggest one
// (RenderFrame); spot-checks for the smaller payloads.
// ----------------------------------------------------------------------------

TEST(MessageBusSerialization, RenderFrameRoundTrip) {
    RenderFrame rf;
    rf.frameNumber = 42;
    rf.deltaTime = 1.0 / 60.0;
    rf.playState = TransportState::Playing;

    ClipRenderState clip{};
    clip.entity = 12345ull;
    clip.slot = 3;
    clip.mediaFrame = 7;
    clip.ocioOverride = "ACES2065-1";
    clip.transformMatrix = {
        2.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 2.0f, 0.0f,
        10.5f, -3.25f, 0.125f, 1.0f
    };
    clip.opacity = 0.75f;
    clip.blendMode = BlendMode::Multiply;
    clip.targetScreen = 99ull;
    rf.activeClips.push_back(clip);

    rf.wantedFrames.push_back(WantedFrame{12345ull, 8, 4});

    auto out = roundTripExpect(rf);
    EXPECT_EQ(out.frameNumber, rf.frameNumber);
    EXPECT_DOUBLE_EQ(out.deltaTime, rf.deltaTime);
    EXPECT_EQ(out.playState, rf.playState);
    ASSERT_EQ(out.activeClips.size(), 1u);
    const auto& oc = out.activeClips[0];
    EXPECT_EQ(oc.entity, clip.entity);
    EXPECT_EQ(oc.slot, clip.slot);
    EXPECT_EQ(oc.mediaFrame, clip.mediaFrame);
    EXPECT_EQ(oc.ocioOverride, clip.ocioOverride);
    EXPECT_EQ(oc.transformMatrix, clip.transformMatrix);
    EXPECT_FLOAT_EQ(oc.opacity, clip.opacity);
    EXPECT_EQ(oc.blendMode, clip.blendMode);
    EXPECT_EQ(oc.targetScreen, clip.targetScreen);
    ASSERT_EQ(out.wantedFrames.size(), 1u);
    EXPECT_EQ(out.wantedFrames[0].entity, 12345ull);
    EXPECT_EQ(out.wantedFrames[0].mediaFrame, 8);
    EXPECT_EQ(out.wantedFrames[0].lookahead, 4);

    expectByteStable(rf);
}

TEST(MessageBusSerialization, RenderFrameEmpty) {
    RenderFrame rf;
    auto out = roundTripExpect(rf);
    EXPECT_EQ(out.frameNumber, 0);
    EXPECT_EQ(out.playState, TransportState::Stopped);
    EXPECT_TRUE(out.activeClips.empty());
    EXPECT_TRUE(out.wantedFrames.empty());
    expectByteStable(rf);
}

TEST(MessageBusSerialization, RenderFrameAllBlendModes) {
    // Loop every BlendMode through the enum<->string codec to catch
    // a missing case in either direction.
    const BlendMode all[] = {
        BlendMode::Normal, BlendMode::Add, BlendMode::Multiply, BlendMode::Screen,
        BlendMode::Overlay, BlendMode::SoftLight, BlendMode::HardLight,
        BlendMode::ColorDodge, BlendMode::ColorBurn, BlendMode::Darken,
        BlendMode::Lighten, BlendMode::Difference, BlendMode::Exclusion,
    };
    for (auto bm : all) {
        RenderFrame rf;
        ClipRenderState c{};
        c.blendMode = bm;
        rf.activeClips.push_back(c);
        auto out = roundTripExpect(rf);
        ASSERT_EQ(out.activeClips.size(), 1u);
        EXPECT_EQ(out.activeClips[0].blendMode, bm)
            << "BlendMode round-trip failed for value " << static_cast<int>(bm);
    }
}

TEST(MessageBusSerialization, RequestComposeCaptureRoundTrip) {
    RequestComposeCapture m{};
    m.correlationId = 0xDEADBEEFCAFEBABEull;
    m.slot = 2;
    m.hashOnly = false;
    m.pngPath = "tests/output/foo.png";
    m.goldenHashPath = "tests/goldens/foo/frame_0.hash";
    auto out = roundTripExpect(m);
    EXPECT_EQ(out.correlationId, m.correlationId);
    EXPECT_EQ(out.slot, m.slot);
    EXPECT_EQ(out.hashOnly, m.hashOnly);
    EXPECT_EQ(out.pngPath, m.pngPath);
    EXPECT_EQ(out.goldenHashPath, m.goldenHashPath);
    expectByteStable(m);
}

TEST(MessageBusSerialization, CaptureCompletedRoundTrip) {
    CaptureCompleted m{};
    m.correlationId = 7ull;
    m.hexHash = "884c5ee8a4a18c65";
    m.ok = true;
    auto out = roundTripExpect(m);
    EXPECT_EQ(out.correlationId, m.correlationId);
    EXPECT_EQ(out.hexHash, m.hexHash);
    EXPECT_EQ(out.ok, m.ok);
    EXPECT_EQ(out.errorMessage, m.errorMessage);
    expectByteStable(m);

    CaptureCompleted err{};
    err.correlationId = 8ull;
    err.ok = false;
    err.errorMessage = "compose target not allocated";
    auto out2 = roundTripExpect(err);
    EXPECT_FALSE(out2.ok);
    EXPECT_EQ(out2.errorMessage, err.errorMessage);
}

TEST(MessageBusSerialization, ProvisionClipResourcesAllMediaTypes) {
    const MediaType all[] = {
        MediaType::Unknown, MediaType::VideoProRes4444, MediaType::VideoHAP,
        MediaType::VideoHAPAlpha, MediaType::VideoHAPQ, MediaType::VideoHAPR,
        MediaType::PNGSequence, MediaType::DPXSequence,
    };
    for (auto mt : all) {
        ProvisionClipResources m{};
        m.entity = 5ull;
        m.assetPath = "C:/media/test.mov";
        m.mediaType = mt;
        m.framerate = 23.976;
        m.totalMediaFrames = 1024;
        auto out = roundTripExpect(m);
        EXPECT_EQ(out.entity, m.entity);
        EXPECT_EQ(out.assetPath, m.assetPath);
        EXPECT_EQ(out.mediaType, mt);
        EXPECT_DOUBLE_EQ(out.framerate, m.framerate);
        EXPECT_EQ(out.totalMediaFrames, m.totalMediaFrames);
    }
}

TEST(MessageBusSerialization, ResourcesProvisionedRoundTrip) {
    ResourcesProvisioned m{};
    m.entity = 11ull;
    m.descriptorSlot = 4;
    m.ok = true;
    auto out = roundTripExpect(m);
    EXPECT_EQ(out.entity, m.entity);
    EXPECT_EQ(out.descriptorSlot, m.descriptorSlot);
    EXPECT_TRUE(out.ok);
    expectByteStable(m);

    ResourcesProvisioned err{};
    err.entity = 12ull;
    err.descriptorSlot = -1;
    err.ok = false;
    err.errorMessage = "out of descriptor slots";
    auto out2 = roundTripExpect(err);
    EXPECT_FALSE(out2.ok);
    EXPECT_EQ(out2.errorMessage, err.errorMessage);
}

TEST(MessageBusSerialization, SetOutputEnabledRoundTrip) {
    SetOutputEnabled on{1ull, true};
    auto outOn = roundTripExpect(on);
    EXPECT_EQ(outOn.entity, 1ull);
    EXPECT_TRUE(outOn.enabled);
    expectByteStable(on);

    SetOutputEnabled off{2ull, false};
    auto outOff = roundTripExpect(off);
    EXPECT_FALSE(outOff.enabled);
}

TEST(MessageBusSerialization, ApplySettingsRoundTrip) {
    ApplySettings m{};
    m.frameCacheBytes = 512ull * 1024ull * 1024ull;
    m.ocioConfigPath = "C:/ocio/aces_1.3/config.ocio";
    auto out = roundTripExpect(m);
    EXPECT_EQ(out.frameCacheBytes, m.frameCacheBytes);
    EXPECT_EQ(out.ocioConfigPath, m.ocioConfigPath);
    expectByteStable(m);
}

TEST(MessageBusSerialization, DeviceLostRoundTrip) {
    DeviceLost m{};
    m.hresult = static_cast<int>(0x887A0005); // DXGI_ERROR_DEVICE_REMOVED
    m.reason = "GPU hung";
    auto out = roundTripExpect(m);
    EXPECT_EQ(out.hresult, m.hresult);
    EXPECT_EQ(out.reason, m.reason);
    expectByteStable(m);
}

TEST(MessageBusSerialization, FrameDroppedRoundTrip) {
    FrameDropped m{};
    m.entity = 99ull;
    m.mediaFrame = 1234;
    m.reason = "decoder underrun";
    auto out = roundTripExpect(m);
    EXPECT_EQ(out.entity, m.entity);
    EXPECT_EQ(out.mediaFrame, m.mediaFrame);
    EXPECT_EQ(out.reason, m.reason);
    expectByteStable(m);
}

TEST(MessageBusSerialization, AllPlayStates) {
    for (auto ps : {TransportState::Stopped, TransportState::Playing, TransportState::Paused}) {
        RenderFrame rf;
        rf.playState = ps;
        auto out = roundTripExpect(rf);
        EXPECT_EQ(out.playState, ps);
    }
}

// ----------------------------------------------------------------------------
// Wire-format / failure modes.
// ----------------------------------------------------------------------------

TEST(MessageBusSerialization, MessageTypeNameMatchesEnvelope) {
    Message msg = RenderFrame{};
    auto bytes = serialize(msg);
    std::string s(bytes.begin(), bytes.end());
    auto j = nlohmann::json::parse(s);
    EXPECT_EQ(j.at("type").get<std::string>(), std::string(messageTypeName(msg)));
    EXPECT_EQ(messageTypeName(Message{RequestComposeCapture{}}),  std::string("RequestComposeCapture"));
    EXPECT_EQ(messageTypeName(Message{CaptureCompleted{}}),       std::string("CaptureCompleted"));
    EXPECT_EQ(messageTypeName(Message{ProvisionClipResources{}}), std::string("ProvisionClipResources"));
    EXPECT_EQ(messageTypeName(Message{ResourcesProvisioned{}}),   std::string("ResourcesProvisioned"));
    EXPECT_EQ(messageTypeName(Message{SetOutputEnabled{}}),       std::string("SetOutputEnabled"));
    EXPECT_EQ(messageTypeName(Message{ApplySettings{}}),          std::string("ApplySettings"));
    EXPECT_EQ(messageTypeName(Message{DeviceLost{}}),             std::string("DeviceLost"));
    EXPECT_EQ(messageTypeName(Message{FrameDropped{}}),           std::string("FrameDropped"));
}

TEST(MessageBusSerialization, MalformedJsonReturnsNullopt) {
    std::string bad = "{not valid json";
    std::vector<std::uint8_t> bytes(bad.begin(), bad.end());
    EXPECT_FALSE(deserialize(bytes).has_value());
}

TEST(MessageBusSerialization, UnknownTypeReturnsNullopt) {
    std::string bad = R"({"type":"NonExistent","data":{}})";
    std::vector<std::uint8_t> bytes(bad.begin(), bad.end());
    EXPECT_FALSE(deserialize(bytes).has_value());
}

TEST(MessageBusSerialization, MissingEnvelopeFieldsReturnsNullopt) {
    std::string bad = R"({"data":{}})";
    std::vector<std::uint8_t> bytes(bad.begin(), bad.end());
    EXPECT_FALSE(deserialize(bytes).has_value());
}

TEST(MessageBusSerialization, BadFieldTypeReturnsNullopt) {
    // RenderFrame with frameNumber as a string instead of an int.
    std::string bad = R"({"type":"RenderFrame","data":{"frameNumber":"oops","deltaTime":0.0,"playState":"Stopped","activeClips":[],"wantedFrames":[]}})";
    std::vector<std::uint8_t> bytes(bad.begin(), bad.end());
    EXPECT_FALSE(deserialize(bytes).has_value());
}

TEST(MessageBusSerialization, UnknownEnumValueReturnsNullopt) {
    std::string bad = R"({"type":"RenderFrame","data":{"frameNumber":0,"deltaTime":0.0,"playState":"Yodeling","activeClips":[],"wantedFrames":[]}})";
    std::vector<std::uint8_t> bytes(bad.begin(), bad.end());
    EXPECT_FALSE(deserialize(bytes).has_value());
}

TEST(MessageBusSerialization, TransformMatrixWrongLengthRejected) {
    std::string bad = R"({"type":"RenderFrame","data":{"frameNumber":0,"deltaTime":0.0,"playState":"Stopped","activeClips":[{"entity":0,"slot":0,"mediaFrame":0,"ocioOverride":"","transformMatrix":[1,0,0],"opacity":1.0,"blendMode":"Normal","targetScreen":0}],"wantedFrames":[]}})";
    std::vector<std::uint8_t> bytes(bad.begin(), bad.end());
    EXPECT_FALSE(deserialize(bytes).has_value());
}
