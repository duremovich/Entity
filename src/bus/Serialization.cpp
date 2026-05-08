// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich

#include "entity/bus/Serialization.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string_view>

namespace entity::bus {

namespace {

using nlohmann::json;

// ---------------------------------------------------------------------------
// Enum <-> string. Enums on the wire are strings so adding a new value never
// shifts the meaning of older payloads.
// ---------------------------------------------------------------------------

const char* blendModeName(BlendMode m) {
    switch (m) {
        case BlendMode::Normal:     return "Normal";
        case BlendMode::Add:        return "Add";
        case BlendMode::Multiply:   return "Multiply";
        case BlendMode::Screen:     return "Screen";
        case BlendMode::Overlay:    return "Overlay";
        case BlendMode::SoftLight:  return "SoftLight";
        case BlendMode::HardLight:  return "HardLight";
        case BlendMode::ColorDodge: return "ColorDodge";
        case BlendMode::ColorBurn:  return "ColorBurn";
        case BlendMode::Darken:     return "Darken";
        case BlendMode::Lighten:    return "Lighten";
        case BlendMode::Difference: return "Difference";
        case BlendMode::Exclusion:  return "Exclusion";
    }
    return "Normal";
}

BlendMode blendModeFromString(std::string_view s) {
    if (s == "Normal")     return BlendMode::Normal;
    if (s == "Add")        return BlendMode::Add;
    if (s == "Multiply")   return BlendMode::Multiply;
    if (s == "Screen")     return BlendMode::Screen;
    if (s == "Overlay")    return BlendMode::Overlay;
    if (s == "SoftLight")  return BlendMode::SoftLight;
    if (s == "HardLight")  return BlendMode::HardLight;
    if (s == "ColorDodge") return BlendMode::ColorDodge;
    if (s == "ColorBurn")  return BlendMode::ColorBurn;
    if (s == "Darken")     return BlendMode::Darken;
    if (s == "Lighten")    return BlendMode::Lighten;
    if (s == "Difference") return BlendMode::Difference;
    if (s == "Exclusion")  return BlendMode::Exclusion;
    throw std::invalid_argument("unknown BlendMode: " + std::string(s));
}

const char* transportStateName(TransportState s) {
    switch (s) {
        case TransportState::Stopped: return "Stopped";
        case TransportState::Playing: return "Playing";
        case TransportState::Paused:  return "Paused";
    }
    return "Stopped";
}

TransportState transportStateFromString(std::string_view s) {
    if (s == "Stopped") return TransportState::Stopped;
    if (s == "Playing") return TransportState::Playing;
    if (s == "Paused")  return TransportState::Paused;
    throw std::invalid_argument("unknown TransportState: " + std::string(s));
}

const char* mediaTypeName(MediaType t) {
    switch (t) {
        case MediaType::Unknown:         return "Unknown";
        case MediaType::VideoProRes4444: return "VideoProRes4444";
        case MediaType::VideoHAP:        return "VideoHAP";
        case MediaType::VideoHAPAlpha:   return "VideoHAPAlpha";
        case MediaType::VideoHAPQ:       return "VideoHAPQ";
        case MediaType::VideoHAPR:       return "VideoHAPR";
        case MediaType::PNGSequence:     return "PNGSequence";
        case MediaType::DPXSequence:     return "DPXSequence";
    }
    return "Unknown";
}

MediaType mediaTypeFromString(std::string_view s) {
    if (s == "Unknown")         return MediaType::Unknown;
    if (s == "VideoProRes4444") return MediaType::VideoProRes4444;
    if (s == "VideoHAP")        return MediaType::VideoHAP;
    if (s == "VideoHAPAlpha")   return MediaType::VideoHAPAlpha;
    if (s == "VideoHAPQ")       return MediaType::VideoHAPQ;
    if (s == "VideoHAPR")       return MediaType::VideoHAPR;
    if (s == "PNGSequence")     return MediaType::PNGSequence;
    if (s == "DPXSequence")     return MediaType::DPXSequence;
    throw std::invalid_argument("unknown MediaType: " + std::string(s));
}

// ---------------------------------------------------------------------------
// Per-message to_json / from_json. ordered_json keeps key order stable so
// serialize() output is byte-deterministic.
// ---------------------------------------------------------------------------

using ojson = nlohmann::ordered_json;

ojson encode(const ClipRenderState& c) {
    ojson j = ojson::object();
    j["entity"] = c.entity;
    j["slot"] = c.slot;
    j["mediaFrame"] = c.mediaFrame;
    j["ocioOverride"] = c.ocioOverride;
    j["transformMatrix"] = c.transformMatrix;
    j["opacity"] = c.opacity;
    j["blendMode"] = blendModeName(c.blendMode);
    j["targetScreen"] = c.targetScreen;
    j["sectionFadeMultiplier"] = c.sectionFadeMultiplier;
    j["zOrder"] = c.zOrder;
    return j;
}

ClipRenderState decodeClipRenderState(const json& j) {
    ClipRenderState c;
    c.entity = j.at("entity").get<std::uint64_t>();
    c.slot = j.at("slot").get<int>();
    c.mediaFrame = j.at("mediaFrame").get<FrameNumber>();
    c.ocioOverride = j.at("ocioOverride").get<std::string>();
    const auto& arr = j.at("transformMatrix");
    if (!arr.is_array() || arr.size() != 16) {
        throw std::invalid_argument("transformMatrix must be array of 16 floats");
    }
    for (std::size_t i = 0; i < 16; ++i) c.transformMatrix[i] = arr[i].get<float>();
    c.opacity = j.at("opacity").get<float>();
    c.blendMode = blendModeFromString(j.at("blendMode").get<std::string>());
    c.targetScreen = j.at("targetScreen").get<std::uint64_t>();
    // Phase D added sectionFadeMultiplier; older payloads (and the
    // hand-rolled "TruncatedTransformMatrix" malformed-payload test in
    // MessageBusSerializationTests) omit the key — default to 1.0 so a
    // missing key is a no-op envelope rather than a parse failure.
    c.sectionFadeMultiplier = j.value("sectionFadeMultiplier", 1.0f);
    c.zOrder = j.value("zOrder", std::uint32_t{0});
    return c;
}

ojson encode(const WantedFrame& w) {
    ojson j = ojson::object();
    j["entity"] = w.entity;
    j["mediaFrame"] = w.mediaFrame;
    j["lookahead"] = w.lookahead;
    return j;
}

WantedFrame decodeWantedFrame(const json& j) {
    WantedFrame w;
    w.entity = j.at("entity").get<std::uint64_t>();
    w.mediaFrame = j.at("mediaFrame").get<FrameNumber>();
    w.lookahead = j.at("lookahead").get<int>();
    return w;
}

ojson encode(const RenderFrame& m) {
    ojson j = ojson::object();
    j["frameNumber"] = m.frameNumber;
    j["deltaTime"] = m.deltaTime;
    j["playState"] = transportStateName(m.playState);
    auto clips = ojson::array();
    for (const auto& c : m.activeClips) clips.push_back(encode(c));
    j["activeClips"] = std::move(clips);
    auto wanted = ojson::array();
    for (const auto& w : m.wantedFrames) wanted.push_back(encode(w));
    j["wantedFrames"] = std::move(wanted);
    return j;
}

RenderFrame decodeRenderFrame(const json& j) {
    RenderFrame m;
    m.frameNumber = j.at("frameNumber").get<FrameNumber>();
    m.deltaTime = j.at("deltaTime").get<double>();
    m.playState = transportStateFromString(j.at("playState").get<std::string>());
    for (const auto& c : j.at("activeClips")) m.activeClips.push_back(decodeClipRenderState(c));
    for (const auto& w : j.at("wantedFrames")) m.wantedFrames.push_back(decodeWantedFrame(w));
    return m;
}

ojson encode(const RequestComposeCapture& m) {
    ojson j = ojson::object();
    j["correlationId"] = m.correlationId;
    j["slot"] = m.slot;
    j["hashOnly"] = m.hashOnly;
    j["fullWindow"] = m.fullWindow;
    j["pngPath"] = m.pngPath;
    j["goldenHashPath"] = m.goldenHashPath;
    return j;
}

RequestComposeCapture decodeRequestComposeCapture(const json& j) {
    RequestComposeCapture m;
    m.correlationId = j.at("correlationId").get<std::uint64_t>();
    m.slot = j.at("slot").get<int>();
    m.hashOnly = j.at("hashOnly").get<bool>();
    m.fullWindow = j.at("fullWindow").get<bool>();
    m.pngPath = j.at("pngPath").get<std::string>();
    m.goldenHashPath = j.at("goldenHashPath").get<std::string>();
    return m;
}

ojson encode(const CaptureCompleted& m) {
    ojson j = ojson::object();
    j["correlationId"] = m.correlationId;
    j["hexHash"] = m.hexHash;
    j["ok"] = m.ok;
    j["errorMessage"] = m.errorMessage;
    return j;
}

CaptureCompleted decodeCaptureCompleted(const json& j) {
    CaptureCompleted m;
    m.correlationId = j.at("correlationId").get<std::uint64_t>();
    m.hexHash = j.at("hexHash").get<std::string>();
    m.ok = j.at("ok").get<bool>();
    m.errorMessage = j.at("errorMessage").get<std::string>();
    return m;
}

ojson encode(const ProvisionClipResources& m) {
    ojson j = ojson::object();
    j["entity"] = m.entity;
    // Wire key stays "assetPath" -- the field went strong-typed in
    // subtask 9 but the on-the-wire shape didn't. A future hash
    // migration is what actually changes the wire.
    j["assetPath"] = m.assetId.value();
    j["mediaType"] = mediaTypeName(m.mediaType);
    j["framerate"] = m.framerate;
    j["totalMediaFrames"] = m.totalMediaFrames;
    return j;
}

ProvisionClipResources decodeProvisionClipResources(const json& j) {
    ProvisionClipResources m;
    m.entity = j.at("entity").get<std::uint64_t>();
    m.assetId = AssetId{j.at("assetPath").get<std::string>()};
    m.mediaType = mediaTypeFromString(j.at("mediaType").get<std::string>());
    m.framerate = j.at("framerate").get<double>();
    m.totalMediaFrames = j.at("totalMediaFrames").get<FrameNumber>();
    return m;
}

ojson encode(const ResourcesProvisioned& m) {
    ojson j = ojson::object();
    j["entity"] = m.entity;
    j["descriptorSlot"] = m.descriptorSlot;
    j["ok"] = m.ok;
    j["errorMessage"] = m.errorMessage;
    return j;
}

ResourcesProvisioned decodeResourcesProvisioned(const json& j) {
    ResourcesProvisioned m;
    m.entity = j.at("entity").get<std::uint64_t>();
    m.descriptorSlot = j.at("descriptorSlot").get<int>();
    m.ok = j.at("ok").get<bool>();
    m.errorMessage = j.at("errorMessage").get<std::string>();
    return m;
}

ojson encode(const SetOutputEnabled& m) {
    ojson j = ojson::object();
    j["entity"] = m.entity;
    j["enabled"] = m.enabled;
    return j;
}

SetOutputEnabled decodeSetOutputEnabled(const json& j) {
    SetOutputEnabled m;
    m.entity = j.at("entity").get<std::uint64_t>();
    m.enabled = j.at("enabled").get<bool>();
    return m;
}

ojson encode(const ApplySettings& m) {
    ojson j = ojson::object();
    j["frameCacheBytes"] = m.frameCacheBytes;
    j["ocioConfigPath"] = m.ocioConfigPath;
    return j;
}

ApplySettings decodeApplySettings(const json& j) {
    ApplySettings m;
    m.frameCacheBytes = j.at("frameCacheBytes").get<std::uint64_t>();
    m.ocioConfigPath = j.at("ocioConfigPath").get<std::string>();
    return m;
}

ojson encode(const DeviceLost& m) {
    ojson j = ojson::object();
    j["hresult"] = m.hresult;
    j["reason"] = m.reason;
    return j;
}

DeviceLost decodeDeviceLost(const json& j) {
    DeviceLost m;
    m.hresult = j.at("hresult").get<int>();
    m.reason = j.at("reason").get<std::string>();
    return m;
}

ojson encode(const FrameDropped& m) {
    ojson j = ojson::object();
    j["entity"] = m.entity;
    j["mediaFrame"] = m.mediaFrame;
    j["reason"] = m.reason;
    return j;
}

FrameDropped decodeFrameDropped(const json& j) {
    FrameDropped m;
    m.entity = j.at("entity").get<std::uint64_t>();
    m.mediaFrame = j.at("mediaFrame").get<FrameNumber>();
    m.reason = j.at("reason").get<std::string>();
    return m;
}

} // namespace

const char* messageTypeName(const Message& msg) noexcept {
    return std::visit([](const auto& m) -> const char* {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, RenderFrame>)             return "RenderFrame";
        else if constexpr (std::is_same_v<T, RequestComposeCapture>)  return "RequestComposeCapture";
        else if constexpr (std::is_same_v<T, CaptureCompleted>)       return "CaptureCompleted";
        else if constexpr (std::is_same_v<T, ProvisionClipResources>) return "ProvisionClipResources";
        else if constexpr (std::is_same_v<T, ResourcesProvisioned>)   return "ResourcesProvisioned";
        else if constexpr (std::is_same_v<T, SetOutputEnabled>)       return "SetOutputEnabled";
        else if constexpr (std::is_same_v<T, ApplySettings>)          return "ApplySettings";
        else if constexpr (std::is_same_v<T, DeviceLost>)             return "DeviceLost";
        else if constexpr (std::is_same_v<T, FrameDropped>)           return "FrameDropped";
        else return "Unknown";
    }, msg);
}

std::vector<std::uint8_t> serialize(const Message& msg) {
    ojson env = ojson::object();
    env["type"] = messageTypeName(msg);
    env["data"] = std::visit([](const auto& m) -> ojson { return encode(m); }, msg);
    std::string s = env.dump();
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::optional<Message> deserialize(std::span<const std::uint8_t> bytes) {
    json j;
    try {
        j = json::parse(bytes.begin(), bytes.end());
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string() || !j.contains("data")) {
        return std::nullopt;
    }
    const std::string type = j["type"].get<std::string>();
    const json& data = j["data"];
    try {
        if (type == "RenderFrame")             return Message{decodeRenderFrame(data)};
        if (type == "RequestComposeCapture")   return Message{decodeRequestComposeCapture(data)};
        if (type == "CaptureCompleted")        return Message{decodeCaptureCompleted(data)};
        if (type == "ProvisionClipResources")  return Message{decodeProvisionClipResources(data)};
        if (type == "ResourcesProvisioned")    return Message{decodeResourcesProvisioned(data)};
        if (type == "SetOutputEnabled")        return Message{decodeSetOutputEnabled(data)};
        if (type == "ApplySettings")           return Message{decodeApplySettings(data)};
        if (type == "DeviceLost")              return Message{decodeDeviceLost(data)};
        if (type == "FrameDropped")            return Message{decodeFrameDropped(data)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Message> deserialize(const std::vector<std::uint8_t>& bytes) {
    return deserialize(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

} // namespace entity::bus
