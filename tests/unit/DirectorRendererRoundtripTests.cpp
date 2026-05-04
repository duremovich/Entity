#include <gtest/gtest.h>

#include "entity/bus/InMemoryMessageTransport.hpp"
#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/renderer/PlaybackPresenter.hpp"

#include <entt/entt.hpp>

#include <cstring>
#include <optional>
#include <variant>
#include <vector>

using namespace entity;

// Phase D entry, subtask 8 -- end-to-end round-trip from a synthetic
// `bus::RenderFrame` through real `InMemoryMessageTransport`, drained
// Renderer-side, decoded, fed into PlaybackPresenter against a recording
// IRenderer mock. Verifies the wire is the only contract: the presenter
// can't see the Director-side state directly, only what arrives through
// the bus, and the GPU upload calls land in the slots the message asked
// for in the order it asked for.

namespace {

struct UploadCall {
    uint32_t slot{0};
    uint32_t width{0};
    uint32_t height{0};
    TextureFormat format{TextureFormat::RGBA8_UNORM};
    // Snapshot of the first byte so a test can pin per-frame identity
    // without needing to copy the whole buffer.
    uint8_t firstByte{0};
};

// Recording IRenderer mock. Only `uploadVideoFrameToSlot` is meaningful;
// every other method is stubbed to a no-op or trivial default. The mock
// is deliberately narrow -- the test surface is the upload pass, and
// adding behaviour to the other methods would bury the contract under
// noise.
class RecordingRenderer final : public IRenderer {
public:
    std::vector<UploadCall> uploads;

    Result initialize(GLFWwindow*, uint32_t, uint32_t) override { return Result::Success; }
    void   shutdown() override {}
    Result resize(uint32_t, uint32_t) override { return Result::Success; }
    bool   isInitialized() const override { return true; }
    bool   isDeviceLost() const override { return false; }

    void     beginFrame() override {}
    void     endFrame() override {}
    void     clear(float, float, float, float) override {}
    uint32_t getCurrentBackBufferIndex() const override { return 0; }
    void     beginImGuiFrame() override {}
    void     endImGuiFrame() override {}

    uint32_t allocateVideoTextureSlot() override { return 0; }
    void     freeVideoTextureSlot(uint32_t) override {}
    bool     uploadVideoFrameToSlot(uint32_t slot,
                                    const uint8_t* rgba,
                                    uint32_t width,
                                    uint32_t height) override {
        return uploadVideoFrameToSlot(slot, rgba, width, height, TextureFormat::RGBA8_UNORM);
    }
    bool     uploadVideoFrameToSlot(uint32_t slot,
                                    const uint8_t* data,
                                    uint32_t width,
                                    uint32_t height,
                                    TextureFormat format) override {
        UploadCall c;
        c.slot = slot;
        c.width = width;
        c.height = height;
        c.format = format;
        c.firstByte = (data && width > 0 && height > 0) ? data[0] : 0;
        uploads.push_back(c);
        return true;
    }
    TextureRef getVideoTexture(uint32_t slot) const override {
        return TextureRef::video(slot);
    }

    uint32_t   createComposeTarget(uint32_t, uint32_t) override { return 0; }
    bool       resizeComposeTarget(uint32_t, uint32_t, uint32_t) override { return true; }
    void       beginComposeTarget(uint32_t) override {}
    void       endComposeTarget() override {}
    TextureRef getComposeTargetTexture(uint32_t slot) const override {
        return TextureRef::compose(slot);
    }
    void*      getComposeTargetTextureID(uint32_t) const override { return nullptr; }
    uint32_t   getComposeTargetWidth(uint32_t) const override { return 0; }
    uint32_t   getComposeTargetHeight(uint32_t) const override { return 0; }
    bool       isComposeTargetReady(uint32_t) const override { return false; }

    void drawColoredQuad(const glm::mat4&, const glm::vec4&, float) override {}
    void drawTexturedQuad(TextureRef, const glm::mat4&, float, BlendMode,
                          TextureColorSpace, const std::string&) override {}
    void drawMappingSurface(TextureRef, const glm::vec2[4], const glm::vec2[4],
                            const glm::vec4&, float, float, float) override {}
    void drawMeshTriangle(TextureRef, const glm::vec2[3], const glm::vec2[3]) override {}

    uint32_t createOutputWindow(const char*, int32_t, int32_t, uint32_t, uint32_t) override {
        return UINT32_MAX;
    }
    void     destroyOutputWindow(uint32_t) override {}
    void     resizeOutputWindow(uint32_t, uint32_t, uint32_t) override {}
    uint32_t getOutputWindowWidth(uint32_t) const override { return 0; }
    uint32_t getOutputWindowHeight(uint32_t) const override { return 0; }
    void     beginOutputFrame(uint32_t) override {}
    void     clearOutputFrame(uint32_t, float, float, float, float) override {}
    void     endOutputFrame(uint32_t) override {}

    bool captureComposeTargetToPNG(const std::string&, uint32_t) override { return false; }
    bool captureBackBufferToPNG(const std::string&) override { return false; }
    bool readComposeTargetPixels(uint32_t, uint32_t&, uint32_t&,
                                  std::vector<uint8_t>&) override {
        return false;
    }
};

// Drains exactly one D2R RenderFrame off the transport and feeds it to
// the presenter. Mirrors the Engine::render() bus loop verbatim so the
// test exercises the same sequence the production code does.
void drainAndApply(bus::IMessageTransport& transport, PlaybackPresenter& presenter) {
    transport.drain(bus::Direction::D2R, [&](std::vector<std::uint8_t>&& bytes) {
        auto msg = bus::deserialize(bytes);
        ASSERT_TRUE(msg.has_value()) << "deserialize failed -- wire format broken?";
        ASSERT_TRUE(std::holds_alternative<bus::RenderFrame>(*msg));
        presenter.present(std::get<bus::RenderFrame>(*msg));
    });
}

DecodedFrame makeFrame(uint32_t width, uint32_t height, uint8_t fillByte) {
    DecodedFrame f;
    f.width = width;
    f.height = height;
    f.format = TextureFormat::RGBA8_UNORM;
    f.colorSpace = TextureColorSpace::Linear;
    f.ocioColorSpace = "Linear Rec.709 (sRGB)";
    f.data.assign(static_cast<size_t>(width) * height * 4, fillByte);
    f.valid.store(true);
    return f;
}

} // namespace

class DirectorRendererRoundtripTest : public ::testing::Test {
protected:
    entt::registry registry;
    FrameCache cache{8 * 1024 * 1024};   // 8 MiB; plenty for a few small frames
    RecordingRenderer renderer;
    PlaybackPresenter presenter{registry, &renderer, &cache};
    bus::InMemoryMessageTransport transport;

    // Builds a clip entity primed with a VideoTexture (descriptorSlot
    // populated) and pre-warmed with a single-pixel-fill frame at the
    // given mediaFrame. Returns the entity for use in RenderFrame.
    entt::entity primeClip(uint32_t descriptorSlot,
                           FrameNumber mediaFrame,
                           uint8_t fillByte,
                           uint32_t width = 4,
                           uint32_t height = 4) {
        auto e = registry.create();
        auto& tex = registry.emplace<VideoTexture>(e);
        tex.descriptorSlot = descriptorSlot;
        cache.put(e, mediaFrame, makeFrame(width, height, fillByte));
        return e;
    }
};

TEST_F(DirectorRendererRoundtripTest, RoundTripUploadsLandInOrderedSlots) {
    // Director side: prime two clips with cached frames at distinct media
    // frames and synthesize a RenderFrame describing them.
    auto e0 = primeClip(/*slot*/3, /*mediaFrame*/5, /*fill*/0xAA);
    auto e1 = primeClip(/*slot*/7, /*mediaFrame*/9, /*fill*/0xBB);

    bus::RenderFrame rf;
    rf.frameNumber = 5;
    rf.deltaTime = 1.0 / 60.0;
    rf.playState = TransportState::Playing;

    bus::ClipRenderState c0;
    c0.entity = static_cast<uint64_t>(e0);
    c0.slot = 3;
    c0.mediaFrame = 5;
    rf.activeClips.push_back(c0);

    bus::ClipRenderState c1;
    c1.entity = static_cast<uint64_t>(e1);
    c1.slot = 7;
    c1.mediaFrame = 9;
    rf.activeClips.push_back(c1);

    // Renderer side: send through real transport, drain, deserialize,
    // present. None of this can short-circuit through Director-side state
    // -- the message is the only data path.
    transport.send(bus::Direction::D2R, bus::serialize(bus::Message{rf}));
    drainAndApply(transport, presenter);

    ASSERT_EQ(renderer.uploads.size(), 2u);
    EXPECT_EQ(renderer.uploads[0].slot, 3u);
    EXPECT_EQ(renderer.uploads[0].firstByte, 0xAAu);
    EXPECT_EQ(renderer.uploads[0].format, TextureFormat::RGBA8_UNORM);
    EXPECT_EQ(renderer.uploads[1].slot, 7u);
    EXPECT_EQ(renderer.uploads[1].firstByte, 0xBBu);
}

TEST_F(DirectorRendererRoundtripTest, RoundTripStampsOcioOverrideOnVideoTexture) {
    // Subtask 8 keeps the C.12 #9 invariant -- when the message body
    // carries a per-clip OCIO override string, the Renderer-side stamp
    // wins over the decoder's default. This belongs at the bus boundary,
    // not just inside the presenter, so a serialized override that
    // round-trips through JSON still applies.
    auto e = primeClip(/*slot*/2, /*mediaFrame*/0, /*fill*/0x10);

    bus::RenderFrame rf;
    rf.playState = TransportState::Playing;
    bus::ClipRenderState c;
    c.entity = static_cast<uint64_t>(e);
    c.slot = 2;
    c.mediaFrame = 0;
    c.ocioOverride = "ACEScg";
    rf.activeClips.push_back(c);

    transport.send(bus::Direction::D2R, bus::serialize(bus::Message{rf}));
    drainAndApply(transport, presenter);

    ASSERT_EQ(renderer.uploads.size(), 1u);
    const auto& tex = registry.get<VideoTexture>(e);
    EXPECT_EQ(tex.ocioColorSpace, "ACEScg");
}

TEST_F(DirectorRendererRoundtripTest, ClipsWithoutCachedFrameAreSkippedWhenNotPlaying) {
    // No cached frame at the requested mediaFrame and we are NOT playing
    // -- the nearest-fallback gate must hold. Verifies that an empty
    // active set on the wire produces zero upload calls.
    auto e = registry.create();
    auto& tex = registry.emplace<VideoTexture>(e);
    tex.descriptorSlot = 4;

    bus::RenderFrame rf;
    rf.playState = TransportState::Paused;
    bus::ClipRenderState c;
    c.entity = static_cast<uint64_t>(e);
    c.slot = 4;
    c.mediaFrame = 99;
    rf.activeClips.push_back(c);

    transport.send(bus::Direction::D2R, bus::serialize(bus::Message{rf}));
    drainAndApply(transport, presenter);

    EXPECT_TRUE(renderer.uploads.empty());
}

TEST_F(DirectorRendererRoundtripTest, RepeatedFrameSkipsRedundantUpload) {
    // ClipDecodeState's lastDecodedFrame is the per-clip "we already put
    // this on the GPU" memo. Renderer-side stamps it after a successful
    // upload; subsequent ticks at the same mediaFrame must short-circuit
    // before hitting the cache.
    auto e = primeClip(/*slot*/1, /*mediaFrame*/0, /*fill*/0x55);
    registry.emplace<ClipDecodeState>(e);

    bus::RenderFrame rf;
    rf.playState = TransportState::Playing;
    bus::ClipRenderState c;
    c.entity = static_cast<uint64_t>(e);
    c.slot = 1;
    c.mediaFrame = 0;
    rf.activeClips.push_back(c);

    // First tick: upload happens.
    transport.send(bus::Direction::D2R, bus::serialize(bus::Message{rf}));
    drainAndApply(transport, presenter);
    ASSERT_EQ(renderer.uploads.size(), 1u);

    // Second tick: same body, same frame -- presenter should detect the
    // ClipDecodeState memo and skip.
    transport.send(bus::Direction::D2R, bus::serialize(bus::Message{rf}));
    drainAndApply(transport, presenter);
    EXPECT_EQ(renderer.uploads.size(), 1u);
}

TEST_F(DirectorRendererRoundtripTest, BytesOnTheWireAreStableAcrossReSerialization) {
    // The wire format is supposed to be deterministic so an integration
    // test golden hash can pin it. Direct check: serialize twice, expect
    // byte-identical output. This duplicates one of the
    // MessageBusSerializationTests but specifically through the
    // RenderFrame variant the round-trip exercises.
    bus::RenderFrame rf;
    rf.frameNumber = 42;
    rf.deltaTime = 0.0166;
    rf.playState = TransportState::Playing;
    bus::ClipRenderState c;
    c.entity = 7;
    c.slot = 3;
    c.mediaFrame = 12;
    c.ocioOverride = "ACEScg";
    c.opacity = 0.5f;
    c.blendMode = BlendMode::Multiply;
    c.targetScreen = 99;
    rf.activeClips.push_back(c);

    auto a = bus::serialize(bus::Message{rf});
    auto b = bus::serialize(bus::Message{rf});
    EXPECT_EQ(a, b);

    auto round = bus::deserialize(a);
    ASSERT_TRUE(round.has_value());
    auto c2 = bus::serialize(*round);
    EXPECT_EQ(a, c2);
}
