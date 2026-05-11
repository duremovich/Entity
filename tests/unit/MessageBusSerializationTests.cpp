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
    clip.sectionFadeMultiplier = 0.42f;
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
    EXPECT_FLOAT_EQ(oc.sectionFadeMultiplier, clip.sectionFadeMultiplier);
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
    m.fullWindow = true;
    m.pngPath = "tests/output/foo.png";
    m.goldenHashPath = "tests/goldens/foo/frame_0.hash";
    auto out = roundTripExpect(m);
    EXPECT_EQ(out.correlationId, m.correlationId);
    EXPECT_EQ(out.slot, m.slot);
    EXPECT_EQ(out.hashOnly, m.hashOnly);
    EXPECT_EQ(out.fullWindow, m.fullWindow);
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
        m.assetId = entity::AssetId{"C:/media/test.mov"};
        m.mediaType = mt;
        m.framerate = 23.976;
        m.totalMediaFrames = 1024;
        auto out = roundTripExpect(m);
        EXPECT_EQ(out.entity, m.entity);
        EXPECT_EQ(out.assetId, m.assetId);
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

// ----------------------------------------------------------------------------
// Stage 2: scene snapshot round-trips (ScreenSnapshot, MappingSurfaceSnapshot,
// ProjectorSnapshot, OutputSnapshot) carried on RenderFrame.
// ----------------------------------------------------------------------------

TEST(MessageBusSerialization, ScreenSnapshotRoundTrip) {
    RenderFrame rf;
    ScreenSnapshot s;
    s.entity            = 101ull;
    s.name              = "Main Screen";
    s.visible           = true;
    s.width             = 3840;
    s.height            = 2160;
    s.renderTargetSlot  = 2;
    s.renderTargetValid = true;
    s.position          = {1.0f, 0.5f, -2.0f};
    s.rotation          = {5.0f, 30.0f, 0.0f};
    s.scale             = {2.0f, 1.0f, 1.0f};
    s.modelEntity       = 999ull;
    rf.screens.push_back(s);

    auto out = roundTripExpect(rf);
    ASSERT_EQ(out.screens.size(), 1u);
    const auto& o = out.screens[0];
    EXPECT_EQ(o.entity, s.entity);
    EXPECT_EQ(o.name, s.name);
    EXPECT_EQ(o.visible, s.visible);
    EXPECT_EQ(o.width, s.width);
    EXPECT_EQ(o.height, s.height);
    EXPECT_EQ(o.renderTargetSlot, s.renderTargetSlot);
    EXPECT_EQ(o.renderTargetValid, s.renderTargetValid);
    EXPECT_EQ(o.position, s.position);
    EXPECT_EQ(o.rotation, s.rotation);
    EXPECT_EQ(o.scale, s.scale);
    EXPECT_EQ(o.modelEntity, s.modelEntity);
    expectByteStable(rf);
}

TEST(MessageBusSerialization, MappingSurfaceSnapshotRoundTrip) {
    RenderFrame rf;
    MappingSurfaceSnapshot m;
    m.entity        = 202ull;
    m.visible       = true;
    m.outputIndex   = 1;
    m.surfaceIndex  = 3;
    m.corners       = {{{{-0.8f, 0.9f}}, {{0.8f, 0.9f}}, {{0.8f, -0.9f}}, {{-0.8f, -0.9f}}}};
    m.sourceUVs     = {{{{0.1f, 0.1f}}, {{0.9f, 0.1f}}, {{0.9f, 0.9f}}, {{0.1f, 0.9f}}}};
    m.softEdgeLeft  = 0.05f;
    m.softEdgeRight = 0.10f;
    m.softEdgeTop   = 0.02f;
    m.softEdgeBottom = 0.03f;
    m.brightness    = 0.85f;
    m.gamma         = 1.1f;
    rf.surfaces.push_back(m);

    auto out = roundTripExpect(rf);
    ASSERT_EQ(out.surfaces.size(), 1u);
    const auto& o = out.surfaces[0];
    EXPECT_EQ(o.entity, m.entity);
    EXPECT_EQ(o.visible, m.visible);
    EXPECT_EQ(o.outputIndex, m.outputIndex);
    EXPECT_EQ(o.surfaceIndex, m.surfaceIndex);
    EXPECT_EQ(o.corners, m.corners);
    EXPECT_EQ(o.sourceUVs, m.sourceUVs);
    EXPECT_FLOAT_EQ(o.softEdgeLeft,   m.softEdgeLeft);
    EXPECT_FLOAT_EQ(o.softEdgeRight,  m.softEdgeRight);
    EXPECT_FLOAT_EQ(o.softEdgeTop,    m.softEdgeTop);
    EXPECT_FLOAT_EQ(o.softEdgeBottom, m.softEdgeBottom);
    EXPECT_FLOAT_EQ(o.brightness, m.brightness);
    EXPECT_FLOAT_EQ(o.gamma, m.gamma);
    expectByteStable(rf);
}

TEST(MessageBusSerialization, ProjectorSnapshotRoundTrip) {
    RenderFrame rf;
    ProjectorSnapshot p;
    p.entity              = 303ull;
    p.position            = {0.0f, 3.0f, 5.0f};
    p.rotation            = {-20.0f, 0.0f, 0.0f};
    p.fovDegrees          = 55.0f;
    p.nearClip            = 0.2f;
    p.farClip             = 40.0f;
    p.distortionK1        = 0.01f;
    p.distortionK2        = -0.002f;
    p.useResidualWarp     = true;
    p.isCalibrated        = true;
    p.targetSurfaces[0]   = 101ull;
    p.targetSurfaces[1]   = 202ull;
    p.targetSurfaceCount  = 2;
    CalibrationPointSnapshot cp;
    cp.worldPos    = {1.0f, 0.0f, 0.5f};
    cp.projectorUV = {0.45f, 0.55f};
    p.calibrationPoints.push_back(cp);
    rf.projectors.push_back(p);

    auto out = roundTripExpect(rf);
    ASSERT_EQ(out.projectors.size(), 1u);
    const auto& o = out.projectors[0];
    EXPECT_EQ(o.entity, p.entity);
    EXPECT_EQ(o.position, p.position);
    EXPECT_EQ(o.rotation, p.rotation);
    EXPECT_FLOAT_EQ(o.fovDegrees, p.fovDegrees);
    EXPECT_FLOAT_EQ(o.nearClip, p.nearClip);
    EXPECT_FLOAT_EQ(o.farClip, p.farClip);
    EXPECT_FLOAT_EQ(o.distortionK1, p.distortionK1);
    EXPECT_FLOAT_EQ(o.distortionK2, p.distortionK2);
    EXPECT_EQ(o.useResidualWarp, p.useResidualWarp);
    EXPECT_EQ(o.isCalibrated, p.isCalibrated);
    EXPECT_EQ(o.targetSurfaceCount, p.targetSurfaceCount);
    EXPECT_EQ(o.targetSurfaces[0], p.targetSurfaces[0]);
    EXPECT_EQ(o.targetSurfaces[1], p.targetSurfaces[1]);
    ASSERT_EQ(o.calibrationPoints.size(), 1u);
    EXPECT_EQ(o.calibrationPoints[0].worldPos, cp.worldPos);
    EXPECT_EQ(o.calibrationPoints[0].projectorUV, cp.projectorUV);
    expectByteStable(rf);
}

TEST(MessageBusSerialization, OutputSnapshotRoundTrip) {
    RenderFrame rf;
    OutputSnapshot o;
    o.entity                = 404ull;
    o.name                  = "Projector Left";
    o.outputType            = 0; // Physical
    o.enabled               = true;
    o.isPhysical            = true;
    o.outputWindowSlot      = 5;
    o.physicalDisplayIndex  = 1;
    o.width                 = 1920;
    o.height                = 1080;
    o.inputRegionX          = 0.0f;
    o.inputRegionY          = 0.0f;
    o.inputRegionWidth      = 0.5f;
    o.inputRegionHeight     = 1.0f;
    o.brightness            = 0.9f;
    o.gamma                 = 1.05f;
    o.sourceProjector       = 303ull;
    o.sourceScreen          = 0ull;
    o.calibrationOverlay.enabled         = true;
    o.calibrationOverlay.numPoints       = 3;
    o.calibrationOverlay.activeIndex     = 1;
    o.calibrationOverlay.precisionCursor = true;
    o.calibrationOverlay.points[0]       = {0.10f, 0.20f};
    o.calibrationOverlay.points[1]       = {0.30f, 0.40f};
    o.calibrationOverlay.points[2]       = {0.50f, 0.60f};
    o.windowX               = 1920;
    o.windowY               = 0;
    o.outputIndex           = 2;
    rf.outputs.push_back(o);

    auto out = roundTripExpect(rf);
    ASSERT_EQ(out.outputs.size(), 1u);
    const auto& r = out.outputs[0];
    EXPECT_EQ(r.entity, o.entity);
    EXPECT_EQ(r.name, o.name);
    EXPECT_EQ(r.outputType, o.outputType);
    EXPECT_EQ(r.enabled, o.enabled);
    EXPECT_EQ(r.isPhysical, o.isPhysical);
    EXPECT_EQ(r.outputWindowSlot, o.outputWindowSlot);
    EXPECT_EQ(r.physicalDisplayIndex, o.physicalDisplayIndex);
    EXPECT_EQ(r.width, o.width);
    EXPECT_EQ(r.height, o.height);
    EXPECT_FLOAT_EQ(r.inputRegionX, o.inputRegionX);
    EXPECT_FLOAT_EQ(r.inputRegionY, o.inputRegionY);
    EXPECT_FLOAT_EQ(r.inputRegionWidth, o.inputRegionWidth);
    EXPECT_FLOAT_EQ(r.inputRegionHeight, o.inputRegionHeight);
    EXPECT_FLOAT_EQ(r.brightness, o.brightness);
    EXPECT_FLOAT_EQ(r.gamma, o.gamma);
    EXPECT_EQ(r.sourceProjector, o.sourceProjector);
    EXPECT_EQ(r.sourceScreen, o.sourceScreen);
    EXPECT_EQ(r.calibrationOverlay.enabled,         o.calibrationOverlay.enabled);
    EXPECT_EQ(r.calibrationOverlay.numPoints,       o.calibrationOverlay.numPoints);
    EXPECT_EQ(r.calibrationOverlay.activeIndex,     o.calibrationOverlay.activeIndex);
    EXPECT_EQ(r.calibrationOverlay.precisionCursor, o.calibrationOverlay.precisionCursor);
    EXPECT_FLOAT_EQ(r.calibrationOverlay.points[0][0], o.calibrationOverlay.points[0][0]);
    EXPECT_FLOAT_EQ(r.calibrationOverlay.points[0][1], o.calibrationOverlay.points[0][1]);
    EXPECT_FLOAT_EQ(r.calibrationOverlay.points[2][0], o.calibrationOverlay.points[2][0]);
    EXPECT_FLOAT_EQ(r.calibrationOverlay.points[2][1], o.calibrationOverlay.points[2][1]);
    EXPECT_EQ(r.windowX, o.windowX);
    EXPECT_EQ(r.windowY, o.windowY);
    EXPECT_EQ(r.outputIndex, o.outputIndex);
    expectByteStable(rf);
}

TEST(MessageBusSerialization, RenderFrameWithAllSnapshotTypes) {
    RenderFrame rf;
    rf.frameNumber = 100;
    rf.screens.push_back(ScreenSnapshot{101ull, "S1", true, 1920, 1080, 0, true});
    rf.surfaces.push_back(MappingSurfaceSnapshot{202ull, true, 0, 0});
    rf.projectors.push_back(ProjectorSnapshot{303ull});
    rf.outputs.push_back(OutputSnapshot{404ull, "Out1", 0, true, true});
    auto out = roundTripExpect(rf);
    EXPECT_EQ(out.frameNumber, 100);
    EXPECT_EQ(out.screens.size(), 1u);
    EXPECT_EQ(out.surfaces.size(), 1u);
    EXPECT_EQ(out.projectors.size(), 1u);
    EXPECT_EQ(out.outputs.size(), 1u);
    EXPECT_EQ(out.screens[0].entity, 101ull);
    EXPECT_EQ(out.projectors[0].entity, 303ull);
    EXPECT_EQ(out.outputs[0].outputIndex, 0u);
    expectByteStable(rf);
}
