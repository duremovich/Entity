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

ojson encode(const ScreenSnapshot& s) {
    ojson j = ojson::object();
    j["entity"]           = s.entity;
    j["name"]             = s.name;
    j["visible"]          = s.visible;
    j["width"]            = s.width;
    j["height"]           = s.height;
    j["renderTargetSlot"] = s.renderTargetSlot;
    j["renderTargetValid"]= s.renderTargetValid;
    j["position"]         = s.position;
    j["rotation"]         = s.rotation;
    j["scale"]            = s.scale;
    j["modelEntity"]      = s.modelEntity;
    return j;
}

ScreenSnapshot decodeScreenSnapshot(const json& j) {
    ScreenSnapshot s;
    s.entity            = j.at("entity").get<std::uint64_t>();
    s.name              = j.at("name").get<std::string>();
    s.visible           = j.at("visible").get<bool>();
    s.width             = j.at("width").get<std::uint32_t>();
    s.height            = j.at("height").get<std::uint32_t>();
    s.renderTargetSlot  = j.at("renderTargetSlot").get<std::uint32_t>();
    s.renderTargetValid = j.at("renderTargetValid").get<bool>();
    const auto& pos = j.at("position");
    for (int i = 0; i < 3; ++i) s.position[i] = pos[i].get<float>();
    const auto& rot = j.at("rotation");
    for (int i = 0; i < 3; ++i) s.rotation[i] = rot[i].get<float>();
    const auto& sc = j.at("scale");
    for (int i = 0; i < 3; ++i) s.scale[i] = sc[i].get<float>();
    s.modelEntity = j.at("modelEntity").get<std::uint64_t>();
    return s;
}

ojson encode(const OutputSurfaceSnapshot& s) {
    ojson j = ojson::object();
    j["entity"]         = s.entity;
    j["visible"]        = s.visible;
    j["outputIndex"]    = s.outputIndex;
    j["surfaceIndex"]   = s.surfaceIndex;
    auto corners = ojson::array();
    for (const auto& c : s.corners) corners.push_back(ojson{c[0], c[1]});
    j["corners"] = std::move(corners);
    auto uvs = ojson::array();
    for (const auto& u : s.sourceUVs) uvs.push_back(ojson{u[0], u[1]});
    j["sourceUVs"]      = std::move(uvs);
    j["softEdgeLeft"]   = s.softEdgeLeft;
    j["softEdgeRight"]  = s.softEdgeRight;
    j["softEdgeTop"]    = s.softEdgeTop;
    j["softEdgeBottom"] = s.softEdgeBottom;
    j["brightness"]     = s.brightness;
    j["gamma"]          = s.gamma;
    return j;
}

OutputSurfaceSnapshot decodeOutputSurfaceSnapshot(const json& j) {
    OutputSurfaceSnapshot s;
    s.entity       = j.at("entity").get<std::uint64_t>();
    s.visible      = j.at("visible").get<bool>();
    s.outputIndex  = j.at("outputIndex").get<std::uint32_t>();
    s.surfaceIndex = j.at("surfaceIndex").get<std::uint32_t>();
    const auto& corners = j.at("corners");
    for (int i = 0; i < 4; ++i) {
        s.corners[i][0] = corners[i][0].get<float>();
        s.corners[i][1] = corners[i][1].get<float>();
    }
    const auto& uvs = j.at("sourceUVs");
    for (int i = 0; i < 4; ++i) {
        s.sourceUVs[i][0] = uvs[i][0].get<float>();
        s.sourceUVs[i][1] = uvs[i][1].get<float>();
    }
    s.softEdgeLeft   = j.at("softEdgeLeft").get<float>();
    s.softEdgeRight  = j.at("softEdgeRight").get<float>();
    s.softEdgeTop    = j.at("softEdgeTop").get<float>();
    s.softEdgeBottom = j.at("softEdgeBottom").get<float>();
    s.brightness     = j.at("brightness").get<float>();
    s.gamma          = j.at("gamma").get<float>();
    return s;
}

ojson encode(const CalibrationPointSnapshot& cp) {
    ojson j = ojson::object();
    j["worldPos"]    = cp.worldPos;
    j["projectorUV"] = cp.projectorUV;
    return j;
}

CalibrationPointSnapshot decodeCalibrationPointSnapshot(const json& j) {
    CalibrationPointSnapshot cp;
    const auto& wp = j.at("worldPos");
    for (int i = 0; i < 3; ++i) cp.worldPos[i] = wp[i].get<float>();
    const auto& uv = j.at("projectorUV");
    for (int i = 0; i < 2; ++i) cp.projectorUV[i] = uv[i].get<float>();
    return cp;
}

ojson encode(const ProjectorSnapshot& p) {
    ojson j = ojson::object();
    j["entity"]           = p.entity;
    j["position"]         = p.position;
    j["rotation"]         = p.rotation;
    j["fovDegrees"]       = p.fovDegrees;
    j["nearClip"]         = p.nearClip;
    j["farClip"]          = p.farClip;
    j["distortionK1"]     = p.distortionK1;
    j["distortionK2"]     = p.distortionK2;
    j["useResidualWarp"]  = p.useResidualWarp;
    j["isCalibrated"]     = p.isCalibrated;
    j["targetSurfaceCount"] = p.targetSurfaceCount;
    auto ts = ojson::array();
    for (int i = 0; i < p.targetSurfaceCount; ++i) ts.push_back(p.targetSurfaces[i]);
    j["targetSurfaces"] = std::move(ts);
    auto cps = ojson::array();
    for (const auto& cp : p.calibrationPoints) cps.push_back(encode(cp));
    j["calibrationPoints"] = std::move(cps);
    return j;
}

ProjectorSnapshot decodeProjectorSnapshot(const json& j) {
    ProjectorSnapshot p;
    p.entity = j.at("entity").get<std::uint64_t>();
    const auto& pos = j.at("position");
    for (int i = 0; i < 3; ++i) p.position[i] = pos[i].get<float>();
    const auto& rot = j.at("rotation");
    for (int i = 0; i < 3; ++i) p.rotation[i] = rot[i].get<float>();
    p.fovDegrees      = j.at("fovDegrees").get<float>();
    p.nearClip        = j.at("nearClip").get<float>();
    p.farClip         = j.at("farClip").get<float>();
    p.distortionK1    = j.at("distortionK1").get<float>();
    p.distortionK2    = j.at("distortionK2").get<float>();
    p.useResidualWarp = j.at("useResidualWarp").get<bool>();
    p.isCalibrated    = j.at("isCalibrated").get<bool>();
    p.targetSurfaceCount = j.at("targetSurfaceCount").get<int>();
    const auto& ts = j.at("targetSurfaces");
    for (int i = 0; i < std::min(p.targetSurfaceCount, 8); ++i)
        p.targetSurfaces[i] = ts[i].get<std::uint64_t>();
    for (const auto& cp : j.at("calibrationPoints"))
        p.calibrationPoints.push_back(decodeCalibrationPointSnapshot(cp));
    return p;
}

ojson encode(const OutputSnapshot& o) {
    ojson j = ojson::object();
    j["entity"]               = o.entity;
    j["name"]                 = o.name;
    j["outputType"]           = o.outputType;
    j["enabled"]              = o.enabled;
    j["isPhysical"]           = o.isPhysical;
    j["outputWindowSlot"]     = o.outputWindowSlot;
    j["physicalDisplayIndex"] = o.physicalDisplayIndex;
    j["width"]                = o.width;
    j["height"]               = o.height;
    j["inputRegionX"]         = o.inputRegionX;
    j["inputRegionY"]         = o.inputRegionY;
    j["inputRegionWidth"]     = o.inputRegionWidth;
    j["inputRegionHeight"]    = o.inputRegionHeight;
    j["brightness"]           = o.brightness;
    j["gamma"]                = o.gamma;
    j["sourceProjector"]      = o.sourceProjector;
    j["sourceScreen"]         = o.sourceScreen;
    {
        ojson co = ojson::object();
        co["enabled"]         = o.calibrationOverlay.enabled;
        co["numPoints"]       = o.calibrationOverlay.numPoints;
        co["activeIndex"]     = o.calibrationOverlay.activeIndex;
        co["precisionCursor"] = o.calibrationOverlay.precisionCursor;
        auto pts = ojson::array();
        for (const auto& p : o.calibrationOverlay.points) {
            auto pair = ojson::array();
            pair.push_back(p[0]);
            pair.push_back(p[1]);
            pts.push_back(std::move(pair));
        }
        co["points"]          = std::move(pts);
        j["calibrationOverlay"] = std::move(co);
    }
    j["windowX"]              = o.windowX;
    j["windowY"]              = o.windowY;
    j["outputIndex"]          = o.outputIndex;
    return j;
}

OutputSnapshot decodeOutputSnapshot(const json& j) {
    OutputSnapshot o;
    o.entity               = j.at("entity").get<std::uint64_t>();
    o.name                 = j.at("name").get<std::string>();
    o.outputType           = j.at("outputType").get<int>();
    o.enabled              = j.at("enabled").get<bool>();
    o.isPhysical           = j.at("isPhysical").get<bool>();
    o.outputWindowSlot     = j.at("outputWindowSlot").get<std::uint32_t>();
    o.physicalDisplayIndex = j.at("physicalDisplayIndex").get<std::int32_t>();
    o.width                = j.at("width").get<std::int32_t>();
    o.height               = j.at("height").get<std::int32_t>();
    o.inputRegionX         = j.at("inputRegionX").get<float>();
    o.inputRegionY         = j.at("inputRegionY").get<float>();
    o.inputRegionWidth     = j.at("inputRegionWidth").get<float>();
    o.inputRegionHeight    = j.at("inputRegionHeight").get<float>();
    o.brightness           = j.at("brightness").get<float>();
    o.gamma                = j.at("gamma").get<float>();
    o.sourceProjector      = j.at("sourceProjector").get<std::uint64_t>();
    o.sourceScreen         = j.at("sourceScreen").get<std::uint64_t>();
    if (j.contains("calibrationOverlay")) {
        const auto& co = j.at("calibrationOverlay");
        o.calibrationOverlay.enabled         = co.at("enabled").get<bool>();
        o.calibrationOverlay.numPoints       = co.at("numPoints").get<std::int32_t>();
        o.calibrationOverlay.activeIndex     = co.at("activeIndex").get<std::int32_t>();
        o.calibrationOverlay.precisionCursor = co.at("precisionCursor").get<bool>();
        const auto& pts = co.at("points");
        const size_t n = std::min<size_t>(pts.size(), o.calibrationOverlay.points.size());
        for (size_t i = 0; i < n; ++i) {
            o.calibrationOverlay.points[i][0] = pts[i][0].get<float>();
            o.calibrationOverlay.points[i][1] = pts[i][1].get<float>();
        }
    }
    o.windowX              = j.at("windowX").get<std::int32_t>();
    o.windowY              = j.at("windowY").get<std::int32_t>();
    if (j.contains("outputIndex"))
        o.outputIndex      = j.at("outputIndex").get<std::uint32_t>();
    return o;
}

// Forward decls for the BakedTrack codec — defined further down the file but
// needed here by GenerativeLayerSnapshot (animation tracks for stall-safe
// show-thread re-evaluation).
ojson encode(const BakedTrack& t);
BakedTrack decodeBakedTrack(const json& j);

ojson encode(const GenerativeLayerSnapshot& g) {
    ojson j = ojson::object();
    j["entity"]            = g.entity;
    j["kind"]              = static_cast<int>(g.kind);
    j["targetScreen"]      = g.targetScreen;
    j["mode"]              = g.mode;
    auto routesArr = ojson::array();
    for (const auto& r : g.routes) {
        ojson routeJ = ojson::object();
        routeJ["screen"] = r.screen;
        auto uvArr = ojson::array();
        for (float v : r.uvRect) uvArr.push_back(v);
        routeJ["uvRect"] = std::move(uvArr);
        routesArr.push_back(std::move(routeJ));
    }
    j["routes"]            = std::move(routesArr);
    j["opacity"]           = g.opacity;
    j["blendMode"]         = g.blendMode;
    j["zOrder"]            = g.zOrder;
    j["muncher_simFrame"]  = g.muncher_simFrame;
    j["muncher_x"]         = g.muncher_x;
    j["muncher_y"]         = g.muncher_y;
    j["muncher_inputX"]    = g.muncher_inputX;
    j["muncher_inputY"]    = g.muncher_inputY;
    auto ghostArr = ojson::array();
    for (float v : g.muncher_ghosts) ghostArr.push_back(v);
    j["muncher_ghosts"]    = std::move(ghostArr);
    // Pack 256-bit grids (walls + pellets) as 64-char hex strings instead
    // of arrays of uint64. nlohmann's ordered_json round-trip of UINT64_MAX
    // values triggers a deserialization stall further down the pipeline
    // (the screenshot readback never lands when the wire payload contains
    // raw `~0ull` numbers in this codebase's ordered_json build). Hex-
    // string encoding sidesteps it.
    auto packHex = [](const std::array<std::uint64_t, 4>& bits) {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(4 * 16);
        for (std::uint64_t v : bits) {
            for (int i = 15; i >= 0; --i) {
                out.push_back(kHex[(v >> (i * 4)) & 0xF]);
            }
        }
        return out;
    };
    j["muncher_wallBits"]   = packHex(g.muncher_wallBits);
    j["muncher_pelletBits"] = packHex(g.muncher_pelletBits);
    j["muncher_score"]     = g.muncher_score;
    j["muncher_lives"]     = g.muncher_lives;
    j["textTextureSlot"]   = g.textTextureSlot;
    j["textBakedWidth"]    = g.textBakedWidth;
    j["textBakedHeight"]   = g.textBakedHeight;
    auto xfArr = ojson::array();
    for (float v : g.transformMatrix) xfArr.push_back(v);
    j["transformMatrix"]   = std::move(xfArr);
    j["renderTargetSlot"]  = g.renderTargetSlot;
    j["startFrame"]        = g.startFrame;
    j["duration"]          = g.duration;
    auto posArr = ojson::array();
    for (float v : g.position) posArr.push_back(v);
    j["position"]          = std::move(posArr);
    auto rotArr = ojson::array();
    for (float v : g.rotation) rotArr.push_back(v);
    j["rotation"]          = std::move(rotArr);
    auto sclArr = ojson::array();
    for (float v : g.scale) sclArr.push_back(v);
    j["scale"]             = std::move(sclArr);
    auto tracksArr = ojson::array();
    for (const auto& t : g.tracks) tracksArr.push_back(encode(t));
    j["tracks"]            = std::move(tracksArr);
    return j;
}

GenerativeLayerSnapshot decodeGenerativeLayerSnapshot(const json& j) {
    GenerativeLayerSnapshot g;
    g.entity            = j.value("entity",            std::uint64_t{0});
    int kindInt         = j.value("kind",              0);
    g.kind              = (kindInt == 1)
                            ? GenerativeLayerSnapshot::Kind::Text
                            : GenerativeLayerSnapshot::Kind::Muncher;  // forward-compat: unknown ↦ Muncher
    g.targetScreen      = j.value("targetScreen",      std::uint64_t{UINT64_MAX});
    int gModeRaw        = j.value("mode",              0);
    g.mode              = (gModeRaw == 1) ? 1 : 0;
    if (j.contains("routes")) {
        for (const auto& rj : j.at("routes")) {
            ContentLayerRoute r;
            r.screen = rj.value("screen", std::uint64_t{UINT64_MAX});
            if (rj.contains("uvRect")) {
                const auto& uv = rj.at("uvRect");
                for (std::size_t i = 0; i < r.uvRect.size() && i < uv.size(); ++i)
                    r.uvRect[i] = uv[i].get<float>();
            }
            g.routes.push_back(r);
        }
    }
    g.opacity           = j.value("opacity",           1.0f);
    g.blendMode         = j.value("blendMode",         0);
    g.zOrder            = j.value("zOrder",            std::uint32_t{0});
    g.muncher_simFrame  = j.value("muncher_simFrame",  std::uint64_t{0});
    g.muncher_x         = j.value("muncher_x",         0.5f);
    g.muncher_y         = j.value("muncher_y",         0.5f);
    g.muncher_inputX    = j.value("muncher_inputX",    0.0f);
    g.muncher_inputY    = j.value("muncher_inputY",    0.0f);
    if (j.contains("muncher_ghosts")) {
        const auto& arr = j.at("muncher_ghosts");
        for (std::size_t i = 0; i < g.muncher_ghosts.size() && i < arr.size(); ++i)
            g.muncher_ghosts[i] = arr[i].get<float>();
    }
    // Hex-string encoded grids (see encode above for rationale). 16 hex
    // chars per uint64, packed high-nibble first.
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return 0;
    };
    auto unpackHex = [&hexVal](const std::string& hex,
                                std::array<std::uint64_t, 4>& out) {
        for (std::size_t wi = 0; wi < out.size(); ++wi) {
            std::uint64_t v = 0;
            for (int i = 0; i < 16; ++i) {
                const std::size_t hi = wi * 16 + static_cast<std::size_t>(i);
                if (hi >= hex.size()) break;
                v = (v << 4) | static_cast<std::uint64_t>(hexVal(hex[hi]));
            }
            out[wi] = v;
        }
    };
    if (j.contains("muncher_wallBits"))
        unpackHex(j.at("muncher_wallBits").get<std::string>(), g.muncher_wallBits);
    if (j.contains("muncher_pelletBits"))
        unpackHex(j.at("muncher_pelletBits").get<std::string>(), g.muncher_pelletBits);
    g.muncher_score     = j.value("muncher_score",     std::uint16_t{0});
    g.muncher_lives     = j.value("muncher_lives",     std::uint16_t{3});
    g.textTextureSlot   = j.value("textTextureSlot",   std::int32_t{-1});
    g.textBakedWidth    = j.value("textBakedWidth",    std::uint32_t{0});
    g.textBakedHeight   = j.value("textBakedHeight",   std::uint32_t{0});
    if (j.contains("transformMatrix")) {
        const auto& arr = j.at("transformMatrix");
        for (std::size_t i = 0; i < g.transformMatrix.size() && i < arr.size(); ++i)
            g.transformMatrix[i] = arr[i].get<float>();
    }
    g.renderTargetSlot  = j.value("renderTargetSlot",  std::int32_t{-1});
    g.startFrame        = j.value("startFrame",        FrameNumber{0});
    g.duration          = j.value("duration",          FrameNumber{0});
    auto readVec3 = [&](const char* key, std::array<float, 3>& dst) {
        if (j.contains(key)) {
            const auto& arr = j.at(key);
            for (std::size_t i = 0; i < dst.size() && i < arr.size(); ++i)
                dst[i] = arr[i].get<float>();
        }
    };
    readVec3("position", g.position);
    readVec3("rotation", g.rotation);
    readVec3("scale",    g.scale);
    if (j.contains("tracks")) {
        for (const auto& t : j.at("tracks")) g.tracks.push_back(decodeBakedTrack(t));
    }
    return g;
}

// Forward decls for the EffectSnapshot codec used by ContentLayerSnapshot.
ojson encode(const EffectSnapshot& e);
EffectSnapshot decodeEffectSnapshot(const json& j);

ojson encode(const ContentLayerSnapshot& c) {
    ojson j = ojson::object();
    j["entity"]                = c.entity;
    j["targetScreen"]          = c.targetScreen;
    j["mode"]                  = c.mode;
    auto routesArr = ojson::array();
    for (const auto& r : c.routes) {
        ojson routeJ = ojson::object();
        routeJ["screen"] = r.screen;
        auto uvArr = ojson::array();
        for (float v : r.uvRect) uvArr.push_back(v);
        routeJ["uvRect"] = std::move(uvArr);
        routesArr.push_back(std::move(routeJ));
    }
    j["routes"]                = std::move(routesArr);
    auto xfArr = ojson::array();
    for (float v : c.transformMatrix) xfArr.push_back(v);
    j["transformMatrix"]       = std::move(xfArr);
    j["opacity"]               = c.opacity;
    j["sectionFadeMultiplier"] = c.sectionFadeMultiplier;
    j["blendMode"]             = c.blendMode;
    j["zOrder"]                = c.zOrder;
    j["sourceKind"]            = static_cast<int>(c.sourceKind);
    j["sourceSlot"]            = c.sourceSlot;
    j["colorSpace"]            = c.colorSpace;
    j["ocioColorSpace"]        = c.ocioColorSpace;
    auto fx = ojson::array();
    for (const auto& e : c.effects) fx.push_back(encode(e));
    j["effects"]               = std::move(fx);
    j["effectChainSlotA"]      = c.effectChainSlotA;
    j["effectChainSlotB"]      = c.effectChainSlotB;
    j["postEffectsSlot"]       = c.postEffectsSlot;
    return j;
}

ContentLayerSnapshot decodeContentLayerSnapshot(const json& j) {
    ContentLayerSnapshot c;
    c.entity                = j.value("entity",                std::uint64_t{0});
    c.targetScreen          = j.value("targetScreen",          std::uint64_t{UINT64_MAX});
    // Plane A routing (ADR-0021). Prefer `routes` when present; clamp
    // unknown mode values to Direct (0) per bus rule 2 — wire is
    // forward-compatible across new RouteMode values.
    int modeRaw             = j.value("mode",                  0);
    c.mode                  = (modeRaw == 1) ? 1 : 0;
    if (j.contains("routes")) {
        for (const auto& rj : j.at("routes")) {
            ContentLayerRoute r;
            r.screen = rj.value("screen", std::uint64_t{UINT64_MAX});
            if (rj.contains("uvRect")) {
                const auto& uv = rj.at("uvRect");
                for (std::size_t i = 0; i < r.uvRect.size() && i < uv.size(); ++i)
                    r.uvRect[i] = uv[i].get<float>();
            }
            c.routes.push_back(r);
        }
    } else if (c.targetScreen != UINT64_MAX) {
        // Legacy payload with no `routes` field: materialize one route
        // from `targetScreen` so PASS 2 sees an explicit destination.
        // UINT64_MAX = "all visible screens" stays as the empty-routes
        // sentinel; the show-side bake expands it at SceneSnapshot time.
        ContentLayerRoute r;
        r.screen = c.targetScreen;
        c.routes.push_back(r);
    }
    if (j.contains("transformMatrix")) {
        const auto& arr = j.at("transformMatrix");
        for (std::size_t i = 0; i < c.transformMatrix.size() && i < arr.size(); ++i)
            c.transformMatrix[i] = arr[i].get<float>();
    }
    c.opacity               = j.value("opacity",               1.0f);
    c.sectionFadeMultiplier = j.value("sectionFadeMultiplier", 1.0f);
    c.blendMode             = j.value("blendMode",             0);
    c.zOrder                = j.value("zOrder",                std::uint32_t{0});
    int sk                  = j.value("sourceKind",            0);
    c.sourceKind            = (sk == static_cast<int>(ContentLayerSnapshot::SourceKind::Compose))
                                ? ContentLayerSnapshot::SourceKind::Compose
                                : ContentLayerSnapshot::SourceKind::Video;
    c.sourceSlot            = j.value("sourceSlot",            std::int32_t{-1});
    c.colorSpace            = j.value("colorSpace",            0);
    c.ocioColorSpace        = j.value("ocioColorSpace",        std::string{});
    if (j.contains("effects")) {
        for (const auto& e : j.at("effects")) {
            c.effects.push_back(decodeEffectSnapshot(e));
        }
    }
    c.effectChainSlotA      = j.value("effectChainSlotA",      std::int32_t{-1});
    c.effectChainSlotB      = j.value("effectChainSlotB",      std::int32_t{-1});
    c.postEffectsSlot       = j.value("postEffectsSlot",       std::int32_t{-1});
    return c;
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
    auto screens = ojson::array();
    for (const auto& s : m.screens) screens.push_back(encode(s));
    j["screens"] = std::move(screens);
    auto surfaces = ojson::array();
    for (const auto& s : m.surfaces) surfaces.push_back(encode(s));
    j["surfaces"] = std::move(surfaces);
    auto projectors = ojson::array();
    for (const auto& p : m.projectors) projectors.push_back(encode(p));
    j["projectors"] = std::move(projectors);
    auto outputs = ojson::array();
    for (const auto& o : m.outputs) outputs.push_back(encode(o));
    j["outputs"] = std::move(outputs);
    auto gens = ojson::array();
    for (const auto& g : m.generativeLayers) gens.push_back(encode(g));
    j["generativeLayers"] = std::move(gens);
    auto contents = ojson::array();
    for (const auto& c : m.contentLayers) contents.push_back(encode(c));
    j["contentLayers"] = std::move(contents);
    return j;
}

RenderFrame decodeRenderFrame(const json& j) {
    RenderFrame m;
    m.frameNumber = j.at("frameNumber").get<FrameNumber>();
    m.deltaTime = j.at("deltaTime").get<double>();
    m.playState = transportStateFromString(j.at("playState").get<std::string>());
    for (const auto& c : j.at("activeClips")) m.activeClips.push_back(decodeClipRenderState(c));
    for (const auto& w : j.at("wantedFrames")) m.wantedFrames.push_back(decodeWantedFrame(w));
    // Stage 2 snapshot fields — default to empty arrays for payloads that
    // predate this stage (e.g. hand-crafted test strings in older unit tests).
    if (j.contains("screens"))
        for (const auto& s : j.at("screens")) m.screens.push_back(decodeScreenSnapshot(s));
    if (j.contains("surfaces"))
        for (const auto& s : j.at("surfaces")) m.surfaces.push_back(decodeOutputSurfaceSnapshot(s));
    if (j.contains("projectors"))
        for (const auto& p : j.at("projectors")) m.projectors.push_back(decodeProjectorSnapshot(p));
    if (j.contains("outputs"))
        for (const auto& o : j.at("outputs")) m.outputs.push_back(decodeOutputSnapshot(o));
    if (j.contains("generativeLayers"))
        for (const auto& g : j.at("generativeLayers"))
            m.generativeLayers.push_back(decodeGenerativeLayerSnapshot(g));
    if (j.contains("contentLayers"))
        for (const auto& c : j.at("contentLayers"))
            m.contentLayers.push_back(decodeContentLayerSnapshot(c));
    return m;
}

ojson encode(const BakedKeyframe& k) {
    ojson j = ojson::object();
    j["frame"]         = k.frame;
    j["value"]         = k.value;
    j["interpolation"] = k.interpolation;
    j["easeIn"]        = k.easeIn;
    j["easeOut"]       = k.easeOut;
    return j;
}

BakedKeyframe decodeBakedKeyframe(const json& j) {
    BakedKeyframe k;
    k.frame         = j.at("frame").get<FrameNumber>();
    k.value         = j.at("value").get<float>();
    k.interpolation = j.at("interpolation").get<int>();
    k.easeIn        = j.at("easeIn").get<float>();
    k.easeOut       = j.at("easeOut").get<float>();
    return k;
}

ojson encode(const BakedTrack& t) {
    ojson j = ojson::object();
    j["property"] = t.property;
    j["enabled"]  = t.enabled;
    auto kfs = ojson::array();
    for (const auto& k : t.keyframes) kfs.push_back(encode(k));
    j["keyframes"] = std::move(kfs);
    return j;
}

BakedTrack decodeBakedTrack(const json& j) {
    BakedTrack t;
    t.property = j.at("property").get<int>();
    t.enabled  = j.at("enabled").get<bool>();
    if (j.contains("keyframes")) {
        for (const auto& kf : j.at("keyframes")) {
            t.keyframes.push_back(decodeBakedKeyframe(kf));
        }
    }
    return t;
}

ojson encode(const BakedEffectTrack& t) {
    ojson j = ojson::object();
    j["paramName"] = t.paramName;
    j["enabled"]   = t.enabled;
    auto kfs = ojson::array();
    for (const auto& k : t.keyframes) kfs.push_back(encode(k));
    j["keyframes"] = std::move(kfs);
    return j;
}

BakedEffectTrack decodeBakedEffectTrack(const json& j) {
    BakedEffectTrack t;
    t.paramName = j.value("paramName", std::string{});
    t.enabled   = j.value("enabled",   true);
    if (j.contains("keyframes")) {
        for (const auto& kf : j.at("keyframes")) {
            t.keyframes.push_back(decodeBakedKeyframe(kf));
        }
    }
    return t;
}

ojson encode(const EffectSnapshot& e) {
    ojson j = ojson::object();
    j["kindIdHash"] = e.kindIdHash;
    j["enabled"]    = e.enabled;
    // paramBlob is a flat byte array — encoded as a JSON array of
    // uint8 values to keep the envelope text-only. Typical effects
    // ship ≤256-byte blobs so the overhead vs. base64 is negligible.
    auto blob = ojson::array();
    for (std::uint8_t b : e.paramBlob) blob.push_back(b);
    j["paramBlob"] = std::move(blob);
    auto tracks = ojson::array();
    for (const auto& t : e.tracks) tracks.push_back(encode(t));
    j["tracks"] = std::move(tracks);
    return j;
}

EffectSnapshot decodeEffectSnapshot(const json& j) {
    EffectSnapshot e;
    e.kindIdHash = j.value("kindIdHash", std::uint32_t{0});
    e.enabled    = j.value("enabled",    true);
    if (j.contains("paramBlob")) {
        const auto& arr = j.at("paramBlob");
        e.paramBlob.reserve(arr.size());
        for (const auto& b : arr) {
            e.paramBlob.push_back(b.get<std::uint8_t>());
        }
    }
    if (j.contains("tracks")) {
        for (const auto& t : j.at("tracks")) {
            e.tracks.push_back(decodeBakedEffectTrack(t));
        }
    }
    return e;
}

ojson encode(const LayerEffectsSnapshot& l) {
    ojson j = ojson::object();
    j["entity"] = l.entity;
    auto fx = ojson::array();
    for (const auto& e : l.effects) fx.push_back(encode(e));
    j["effects"] = std::move(fx);
    j["slotA"]   = l.slotA;
    j["slotB"]   = l.slotB;
    j["width"]   = l.width;
    j["height"]  = l.height;
    return j;
}

LayerEffectsSnapshot decodeLayerEffectsSnapshot(const json& j) {
    LayerEffectsSnapshot l;
    l.entity = j.value("entity", std::uint64_t{0});
    if (j.contains("effects")) {
        for (const auto& e : j.at("effects")) {
            l.effects.push_back(decodeEffectSnapshot(e));
        }
    }
    l.slotA  = j.value("slotA",  std::int32_t{-1});
    l.slotB  = j.value("slotB",  std::int32_t{-1});
    l.width  = j.value("width",  std::uint32_t{0});
    l.height = j.value("height", std::uint32_t{0});
    return l;
}

ojson encode(const ClipCatalogEntry& e) {
    ojson j = ojson::object();
    j["entity"]               = e.entity;
    j["startFrame"]           = e.startFrame;
    j["duration"]             = e.duration;
    j["mediaStartFrame"]      = e.mediaStartFrame;
    j["mediaOutFrame"]        = e.mediaOutFrame;
    j["framerate"]            = e.framerate;
    j["playbackMode"]         = e.playbackMode;
    j["sectionBehavior"]      = e.sectionBehavior;
    j["endAlignsWithSectionBreak"] = e.endAlignsWithSectionBreak;
    j["endingBreakFadeSeconds"] = e.endingBreakFadeSeconds;
    j["descriptorSlot"]       = e.descriptorSlot;
    j["transformMatrix"]      = e.transformMatrix;
    j["opacity"]              = e.opacity;
    j["blendMode"]            = e.blendMode;
    j["zOrder"]               = e.zOrder;
    j["targetScreen"]         = e.targetScreen;
    j["mode"]                 = e.mode;
    auto ceRoutesArr = ojson::array();
    for (const auto& r : e.routes) {
        ojson routeJ = ojson::object();
        routeJ["screen"] = r.screen;
        auto uvArr = ojson::array();
        for (float v : r.uvRect) uvArr.push_back(v);
        routeJ["uvRect"] = std::move(uvArr);
        ceRoutesArr.push_back(std::move(routeJ));
    }
    j["routes"]               = std::move(ceRoutesArr);
    j["ocioOverride"]         = e.ocioOverride;
    j["hasPhase"]             = e.hasPhase;
    j["phase_inContinuation"]     = e.phase_inContinuation;
    j["phase_sourcePhaseFrames"]  = e.phase_sourcePhaseFrames;
    j["phase_tailHoldMediaFrame"] = e.phase_tailHoldMediaFrame;
    j["phase_postBreakMediaAnchor"] = e.phase_postBreakMediaAnchor;
    j["phase_anchorTimelineFrame"]  = e.phase_anchorTimelineFrame;
    j["phase_continuationStartTimeNs"] = e.phase_continuationStartTimeNs;
    j["phase_continuationSeedFrames"]  = e.phase_continuationSeedFrames;
    j["position"]             = e.position;
    j["rotation"]             = e.rotation;
    j["scale"]                = e.scale;
    auto tracks = ojson::array();
    for (const auto& t : e.tracks) tracks.push_back(encode(t));
    j["tracks"] = std::move(tracks);
    return j;
}

ClipCatalogEntry decodeClipCatalogEntry(const json& j) {
    ClipCatalogEntry e;
    e.entity          = j.at("entity").get<std::uint64_t>();
    e.startFrame      = j.at("startFrame").get<FrameNumber>();
    e.duration        = j.at("duration").get<FrameNumber>();
    e.mediaStartFrame = j.at("mediaStartFrame").get<FrameNumber>();
    e.mediaOutFrame   = j.at("mediaOutFrame").get<FrameNumber>();
    e.framerate       = j.at("framerate").get<double>();
    e.playbackMode    = j.at("playbackMode").get<int>();
    e.sectionBehavior = j.at("sectionBehavior").get<int>();
    // Optional for backward compat with pre-2026-05-23 payloads — default
    // false matches the struct default (no extension).
    e.endAlignsWithSectionBreak = j.value("endAlignsWithSectionBreak", false);
    // Optional for backward compat with pre-2026-05-23-follow-up payloads
    // — default 0.0 = no fade at end => extension can still fire when
    // endAlignsWithSectionBreak is true, preserving prior single-clip
    // behavior. New payloads carry the actual fadeSeconds.
    e.endingBreakFadeSeconds = j.value("endingBreakFadeSeconds", 0.0);
    e.descriptorSlot  = j.at("descriptorSlot").get<int>();
    const auto& arr   = j.at("transformMatrix");
    if (!arr.is_array() || arr.size() != 16)
        throw std::invalid_argument("ClipCatalogEntry transformMatrix must be array of 16 floats");
    for (std::size_t i = 0; i < 16; ++i) e.transformMatrix[i] = arr[i].get<float>();
    e.opacity         = j.at("opacity").get<float>();
    e.blendMode       = j.at("blendMode").get<int>();
    e.zOrder          = j.at("zOrder").get<std::uint32_t>();
    e.targetScreen    = j.at("targetScreen").get<std::uint64_t>();
    int ceModeRaw     = j.value("mode", 0);
    e.mode            = (ceModeRaw == 1) ? 1 : 0;
    if (j.contains("routes")) {
        for (const auto& rj : j.at("routes")) {
            ContentLayerRoute r;
            r.screen = rj.value("screen", std::uint64_t{UINT64_MAX});
            if (rj.contains("uvRect")) {
                const auto& uv = rj.at("uvRect");
                for (std::size_t i = 0; i < r.uvRect.size() && i < uv.size(); ++i)
                    r.uvRect[i] = uv[i].get<float>();
            }
            e.routes.push_back(r);
        }
    }
    e.ocioOverride    = j.at("ocioOverride").get<std::string>();
    e.hasPhase        = j.at("hasPhase").get<bool>();
    e.phase_inContinuation     = j.at("phase_inContinuation").get<bool>();
    e.phase_sourcePhaseFrames  = j.at("phase_sourcePhaseFrames").get<double>();
    e.phase_tailHoldMediaFrame = j.at("phase_tailHoldMediaFrame").get<FrameNumber>();
    e.phase_postBreakMediaAnchor = j.at("phase_postBreakMediaAnchor").get<FrameNumber>();
    e.phase_anchorTimelineFrame  = j.at("phase_anchorTimelineFrame").get<FrameNumber>();
    // Wall-clock continuation anchor (NEW-08) — optional for backward compat;
    // defaults match the struct (0 = "no anchor", show side uses the snapshot
    // phase_sourcePhaseFrames instead).
    e.phase_continuationStartTimeNs = j.value("phase_continuationStartTimeNs", std::int64_t{0});
    e.phase_continuationSeedFrames  = j.value("phase_continuationSeedFrames", 0.0);
    // Animation fields (added for NEW-07) — optional for backward compat with
    // any pre-recorded test fixtures or older payloads. Defaults match the
    // struct defaults so static (un-animated) clips stay correct.
    if (j.contains("position")) {
        const auto& arr = j.at("position");
        if (!arr.is_array() || arr.size() != 3)
            throw std::invalid_argument("ClipCatalogEntry position must be array of 3 floats");
        for (std::size_t i = 0; i < 3; ++i) e.position[i] = arr[i].get<float>();
    }
    if (j.contains("rotation")) {
        const auto& arr = j.at("rotation");
        if (!arr.is_array() || arr.size() != 3)
            throw std::invalid_argument("ClipCatalogEntry rotation must be array of 3 floats");
        for (std::size_t i = 0; i < 3; ++i) e.rotation[i] = arr[i].get<float>();
    }
    if (j.contains("scale")) {
        const auto& arr = j.at("scale");
        if (!arr.is_array() || arr.size() != 3)
            throw std::invalid_argument("ClipCatalogEntry scale must be array of 3 floats");
        for (std::size_t i = 0; i < 3; ++i) e.scale[i] = arr[i].get<float>();
    }
    if (j.contains("tracks")) {
        for (const auto& t : j.at("tracks")) e.tracks.push_back(decodeBakedTrack(t));
    }
    return e;
}

ojson encode(const ObjectAnimationLayerSnapshot& oa) {
    ojson j = ojson::object();
    j["targetScreen"] = oa.targetScreen;
    j["startFrame"]   = oa.startFrame;
    j["duration"]     = oa.duration;
    j["trackIndex"]   = oa.trackIndex;
    j["endBehavior"]  = (oa.endBehavior == OAEndBehavior::Reset) ? "Reset" : "Hold";
    auto tracks = ojson::array();
    for (const auto& t : oa.tracks) tracks.push_back(encode(t));
    j["tracks"] = std::move(tracks);
    return j;
}

ObjectAnimationLayerSnapshot decodeObjectAnimationLayerSnapshot(const json& j) {
    ObjectAnimationLayerSnapshot oa;
    oa.targetScreen = j.value("targetScreen", std::uint64_t{0});
    oa.startFrame   = j.value("startFrame",   FrameNumber{0});
    oa.duration     = j.value("duration",     FrameNumber{0});
    oa.trackIndex   = j.value("trackIndex",   std::uint32_t{UINT32_MAX});
    const std::string eb = j.value("endBehavior", std::string{"Hold"});
    oa.endBehavior = (eb == "Reset") ? OAEndBehavior::Reset : OAEndBehavior::Hold;
    if (j.contains("tracks"))
        for (const auto& t : j.at("tracks")) oa.tracks.push_back(decodeBakedTrack(t));
    return oa;
}

// Forward declarations for SignalLayerSnapshot codec — defined after
// SignalEmit at the bottom of this anon namespace (requires signalArgType*
// helpers), called from encode/decode SceneSnapshot above.
ojson encode(const SignalLayerSnapshot& s);
SignalLayerSnapshot decodeSignalLayerSnapshot(const json& j);

ojson encode(const SceneSnapshot& s) {
    ojson j = ojson::object();
    auto screens = ojson::array();
    for (const auto& ss : s.screens) screens.push_back(encode(ss));
    j["screens"] = std::move(screens);
    auto surfaces = ojson::array();
    for (const auto& ms : s.surfaces) surfaces.push_back(encode(ms));
    j["surfaces"] = std::move(surfaces);
    auto projectors = ojson::array();
    for (const auto& ps : s.projectors) projectors.push_back(encode(ps));
    j["projectors"] = std::move(projectors);
    auto outputs = ojson::array();
    for (const auto& os : s.outputs) outputs.push_back(encode(os));
    j["outputs"] = std::move(outputs);
    auto catalog = ojson::array();
    for (const auto& ce : s.clipCatalog) catalog.push_back(encode(ce));
    j["clipCatalog"] = std::move(catalog);
    auto oaCatalog = ojson::array();
    for (const auto& oa : s.objectAnimationLayers) oaCatalog.push_back(encode(oa));
    j["objectAnimationLayers"] = std::move(oaCatalog);
    auto genCatalog = ojson::array();
    for (const auto& gl : s.generativeLayers) genCatalog.push_back(encode(gl));
    j["generativeLayers"] = std::move(genCatalog);
    auto contentCatalog = ojson::array();
    for (const auto& cl : s.contentLayers) contentCatalog.push_back(encode(cl));
    j["contentLayers"] = std::move(contentCatalog);
    auto layerFx = ojson::array();
    for (const auto& le : s.layerEffects) layerFx.push_back(encode(le));
    j["layerEffects"] = std::move(layerFx);
    auto signalLayersArr = ojson::array();
    for (const auto& sl : s.signalLayers) signalLayersArr.push_back(encode(sl));
    j["signalLayers"] = std::move(signalLayersArr);
    return j;
}

SceneSnapshot decodeSceneSnapshot(const json& j) {
    SceneSnapshot s;
    if (j.contains("screens"))
        for (const auto& ss : j.at("screens")) s.screens.push_back(decodeScreenSnapshot(ss));
    if (j.contains("surfaces"))
        for (const auto& ms : j.at("surfaces")) s.surfaces.push_back(decodeOutputSurfaceSnapshot(ms));
    if (j.contains("projectors"))
        for (const auto& ps : j.at("projectors")) s.projectors.push_back(decodeProjectorSnapshot(ps));
    if (j.contains("outputs"))
        for (const auto& os : j.at("outputs")) s.outputs.push_back(decodeOutputSnapshot(os));
    if (j.contains("clipCatalog"))
        for (const auto& ce : j.at("clipCatalog")) s.clipCatalog.push_back(decodeClipCatalogEntry(ce));
    if (j.contains("objectAnimationLayers"))
        for (const auto& oa : j.at("objectAnimationLayers"))
            s.objectAnimationLayers.push_back(decodeObjectAnimationLayerSnapshot(oa));
    if (j.contains("generativeLayers"))
        for (const auto& gl : j.at("generativeLayers"))
            s.generativeLayers.push_back(decodeGenerativeLayerSnapshot(gl));
    if (j.contains("contentLayers"))
        for (const auto& cl : j.at("contentLayers"))
            s.contentLayers.push_back(decodeContentLayerSnapshot(cl));
    if (j.contains("layerEffects"))
        for (const auto& le : j.at("layerEffects"))
            s.layerEffects.push_back(decodeLayerEffectsSnapshot(le));
    if (j.contains("signalLayers"))
        for (const auto& sl : j.at("signalLayers"))
            s.signalLayers.push_back(decodeSignalLayerSnapshot(sl));
    return s;
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

ojson encode(const ScreenRenderTargetAllocated& m) {
    ojson j = ojson::object();
    j["entity"] = m.entity;
    j["slot"]   = m.slot;
    j["width"]  = m.width;
    j["height"] = m.height;
    return j;
}

ScreenRenderTargetAllocated decodeScreenRenderTargetAllocated(const json& j) {
    ScreenRenderTargetAllocated m;
    m.entity = j.at("entity").get<std::uint64_t>();
    m.slot   = j.at("slot").get<std::uint32_t>();
    m.width  = j.at("width").get<std::uint32_t>();
    m.height = j.at("height").get<std::uint32_t>();
    return m;
}

ojson encode(const GenerativeLayerRenderTargetAllocated& m) {
    ojson j = ojson::object();
    j["entity"] = m.entity;
    j["slot"]   = m.slot;
    j["width"]  = m.width;
    j["height"] = m.height;
    return j;
}

GenerativeLayerRenderTargetAllocated decodeGenerativeLayerRenderTargetAllocated(const json& j) {
    GenerativeLayerRenderTargetAllocated m;
    m.entity = j.at("entity").get<std::uint64_t>();
    m.slot   = j.at("slot").get<std::uint32_t>();
    m.width  = j.at("width").get<std::uint32_t>();
    m.height = j.at("height").get<std::uint32_t>();
    return m;
}

ojson encode(const EffectChainRenderTargetAllocated& m) {
    ojson j = ojson::object();
    j["entity"] = m.entity;
    j["slot"]   = m.slot;
    j["side"]   = m.side;
    j["width"]  = m.width;
    j["height"] = m.height;
    return j;
}

EffectChainRenderTargetAllocated decodeEffectChainRenderTargetAllocated(const json& j) {
    EffectChainRenderTargetAllocated m;
    m.entity = j.at("entity").get<std::uint64_t>();
    m.slot   = j.at("slot").get<std::uint32_t>();
    m.side   = j.value("side", std::uint8_t{0});
    m.width  = j.at("width").get<std::uint32_t>();
    m.height = j.at("height").get<std::uint32_t>();
    return m;
}

ojson encode(const EffectCompileFailed& m) {
    ojson j = ojson::object();
    j["kindIdHash"]   = m.kindIdHash;
    j["errorMessage"] = m.errorMessage;
    return j;
}

EffectCompileFailed decodeEffectCompileFailed(const json& j) {
    EffectCompileFailed m;
    m.kindIdHash   = j.at("kindIdHash").get<std::uint32_t>();
    m.errorMessage = j.value("errorMessage", std::string{});
    return m;
}

ojson encode(const CreateOutputWindowRequest& m) {
    ojson j = ojson::object();
    j["entity"]     = m.entity;
    j["title"]      = m.title;
    j["x"]          = m.x;
    j["y"]          = m.y;
    j["width"]      = m.width;
    j["height"]     = m.height;
    j["borderless"] = m.borderless;
    return j;
}

CreateOutputWindowRequest decodeCreateOutputWindowRequest(const json& j) {
    CreateOutputWindowRequest m;
    m.entity     = j.at("entity").get<std::uint64_t>();
    m.title      = j.at("title").get<std::string>();
    m.x          = j.at("x").get<std::int32_t>();
    m.y          = j.at("y").get<std::int32_t>();
    m.width      = j.at("width").get<std::int32_t>();
    m.height     = j.at("height").get<std::int32_t>();
    m.borderless = j.at("borderless").get<bool>();
    return m;
}

ojson encode(const OutputWindowReady& m) {
    ojson j = ojson::object();
    j["entity"]           = m.entity;
    j["outputWindowSlot"] = m.outputWindowSlot;
    j["ok"]               = m.ok;
    j["errorMessage"]     = m.errorMessage;
    return j;
}

OutputWindowReady decodeOutputWindowReady(const json& j) {
    OutputWindowReady m;
    m.entity           = j.at("entity").get<std::uint64_t>();
    m.outputWindowSlot = j.at("outputWindowSlot").get<std::uint32_t>();
    m.ok               = j.at("ok").get<bool>();
    m.errorMessage     = j.at("errorMessage").get<std::string>();
    return m;
}

ojson encode(const SectionBreakDetected& m) {
    ojson j = ojson::object();
    j["breakFrame"] = m.breakFrame;
    return j;
}

SectionBreakDetected decodeSectionBreakDetected(const json& j) {
    SectionBreakDetected m;
    m.breakFrame = j.at("breakFrame").get<FrameNumber>();
    return m;
}

// ---------------------------------------------------------------------------
// SignalArg / SignalEmit — transport-neutral signal event (Phase 1).
// ---------------------------------------------------------------------------

const char* signalArgTypeName(SignalArg::Type t) {
    switch (t) {
        case SignalArg::Type::Int:    return "Int";
        case SignalArg::Type::Float:  return "Float";
        case SignalArg::Type::String: return "String";
    }
    return "Int";
}

SignalArg::Type signalArgTypeFromString(std::string_view s) {
    if (s == "Int")    return SignalArg::Type::Int;
    if (s == "Float")  return SignalArg::Type::Float;
    if (s == "String") return SignalArg::Type::String;
    throw std::invalid_argument("unknown SignalArg::Type: " + std::string(s));
}

const char* signalTransportName(SignalEmit::Transport t) {
    switch (t) {
        case SignalEmit::Transport::OSC: return "OSC";
    }
    return "OSC";
}

SignalEmit::Transport signalTransportFromString(std::string_view s) {
    if (s == "OSC") return SignalEmit::Transport::OSC;
    throw std::invalid_argument("unknown SignalEmit::Transport: " + std::string(s));
}

ojson encode(const SignalArg& a) {
    ojson j = ojson::object();
    j["type"] = signalArgTypeName(a.type);
    j["i"]    = a.i;
    j["f"]    = a.f;
    j["s"]    = a.s;
    return j;
}

SignalArg decodeSignalArg(const json& j) {
    SignalArg a;
    a.type = signalArgTypeFromString(j.at("type").get<std::string>());
    a.i    = j.at("i").get<std::int32_t>();
    a.f    = j.at("f").get<float>();
    a.s    = j.at("s").get<std::string>();
    return a;
}

ojson encode(const SignalEmit& m) {
    ojson j = ojson::object();
    j["transport"] = signalTransportName(m.transport);
    j["address"]   = m.address;
    auto argsArr = ojson::array();
    for (const auto& a : m.args) argsArr.push_back(encode(a));
    j["args"] = std::move(argsArr);
    return j;
}

SignalEmit decodeSignalEmit(const json& j) {
    SignalEmit m;
    m.transport = signalTransportFromString(j.at("transport").get<std::string>());
    m.address   = j.at("address").get<std::string>();
    for (const auto& a : j.at("args")) m.args.push_back(decodeSignalArg(a));
    return m;
}

// ---------------------------------------------------------------------------
// SignalLayerSnapshot — Phase 4 bake (editor → show for SignalOutputSystem).
// Break-tolerant: every field uses j.value() with a safe default so older
// payloads (pre-Phase-4 SceneSnapshot) silently produce empty signalLayers.
// ---------------------------------------------------------------------------

ojson encode(const SignalLayerSnapshot& s) {
    ojson j = ojson::object();
    j["entity"]             = s.entity;
    j["startFrame"]         = s.startFrame;
    j["duration"]           = s.duration;
    j["mode"]               = s.mode;
    j["transport"]          = s.transport;
    j["valueSource"]        = s.valueSource;
    j["address"]            = s.address;
    auto argsArr = ojson::array();
    for (const auto& a : s.args) argsArr.push_back(encode(a));
    j["args"]               = std::move(argsArr);
    j["valueBoundArgIndex"] = s.valueBoundArgIndex;
    j["srcMin"]             = s.srcMin;
    j["srcMax"]             = s.srcMax;
    j["outMin"]             = s.outMin;
    j["outMax"]             = s.outMax;
    j["outType"]            = s.outType;
    auto tracksArr = ojson::array();
    for (const auto& t : s.tracks) tracksArr.push_back(encode(t));
    j["tracks"]             = std::move(tracksArr);
    return j;
}

SignalLayerSnapshot decodeSignalLayerSnapshot(const json& j) {
    SignalLayerSnapshot s;
    s.entity             = j.value("entity",             std::uint64_t{0});
    s.startFrame         = j.value("startFrame",         FrameNumber{0});
    s.duration           = j.value("duration",           FrameNumber{0});
    s.mode               = j.value("mode",               0);
    s.transport          = j.value("transport",          0);
    s.valueSource        = j.value("valueSource",        0);
    s.address            = j.value("address",            std::string{});
    if (j.contains("args"))
        for (const auto& a : j.at("args")) s.args.push_back(decodeSignalArg(a));
    s.valueBoundArgIndex = j.value("valueBoundArgIndex", std::int32_t{-1});
    s.srcMin             = j.value("srcMin",             0.0f);
    s.srcMax             = j.value("srcMax",             1.0f);
    s.outMin             = j.value("outMin",             0.0f);
    s.outMax             = j.value("outMax",             1.0f);
    s.outType            = j.value("outType",            0);
    if (j.contains("tracks"))
        for (const auto& t : j.at("tracks")) s.tracks.push_back(decodeBakedTrack(t));
    return s;
}

} // namespace

const char* messageTypeName(const Message& msg) noexcept {
    return std::visit([](const auto& m) -> const char* {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, RenderFrame>)             return "RenderFrame";
        else if constexpr (std::is_same_v<T, SceneSnapshot>)          return "SceneSnapshot";
        else if constexpr (std::is_same_v<T, RequestComposeCapture>)  return "RequestComposeCapture";
        else if constexpr (std::is_same_v<T, CaptureCompleted>)       return "CaptureCompleted";
        else if constexpr (std::is_same_v<T, ProvisionClipResources>) return "ProvisionClipResources";
        else if constexpr (std::is_same_v<T, ResourcesProvisioned>)          return "ResourcesProvisioned";
        else if constexpr (std::is_same_v<T, ScreenRenderTargetAllocated>) return "ScreenRenderTargetAllocated";
        else if constexpr (std::is_same_v<T, GenerativeLayerRenderTargetAllocated>) return "GenerativeLayerRenderTargetAllocated";
        else if constexpr (std::is_same_v<T, EffectChainRenderTargetAllocated>) return "EffectChainRenderTargetAllocated";
        else if constexpr (std::is_same_v<T, EffectCompileFailed>)        return "EffectCompileFailed";
        else if constexpr (std::is_same_v<T, SetOutputEnabled>)            return "SetOutputEnabled";
        else if constexpr (std::is_same_v<T, ApplySettings>)               return "ApplySettings";
        else if constexpr (std::is_same_v<T, DeviceLost>)                  return "DeviceLost";
        else if constexpr (std::is_same_v<T, FrameDropped>)                return "FrameDropped";
        else if constexpr (std::is_same_v<T, CreateOutputWindowRequest>)   return "CreateOutputWindowRequest";
        else if constexpr (std::is_same_v<T, OutputWindowReady>)           return "OutputWindowReady";
        else if constexpr (std::is_same_v<T, SectionBreakDetected>)        return "SectionBreakDetected";
        else if constexpr (std::is_same_v<T, SignalEmit>)                  return "SignalEmit";
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
        if (type == "SceneSnapshot")           return Message{decodeSceneSnapshot(data)};
        if (type == "RequestComposeCapture")   return Message{decodeRequestComposeCapture(data)};
        if (type == "CaptureCompleted")        return Message{decodeCaptureCompleted(data)};
        if (type == "ProvisionClipResources")  return Message{decodeProvisionClipResources(data)};
        if (type == "ResourcesProvisioned")    return Message{decodeResourcesProvisioned(data)};
        if (type == "SetOutputEnabled")        return Message{decodeSetOutputEnabled(data)};
        if (type == "ApplySettings")           return Message{decodeApplySettings(data)};
        if (type == "DeviceLost")                    return Message{decodeDeviceLost(data)};
        if (type == "FrameDropped")                  return Message{decodeFrameDropped(data)};
        if (type == "ScreenRenderTargetAllocated")   return Message{decodeScreenRenderTargetAllocated(data)};
        if (type == "GenerativeLayerRenderTargetAllocated") return Message{decodeGenerativeLayerRenderTargetAllocated(data)};
        if (type == "EffectChainRenderTargetAllocated") return Message{decodeEffectChainRenderTargetAllocated(data)};
        if (type == "EffectCompileFailed")            return Message{decodeEffectCompileFailed(data)};
        if (type == "CreateOutputWindowRequest")     return Message{decodeCreateOutputWindowRequest(data)};
        if (type == "OutputWindowReady")             return Message{decodeOutputWindowReady(data)};
        if (type == "SectionBreakDetected")          return Message{decodeSectionBreakDetected(data)};
        if (type == "SignalEmit")                    return Message{decodeSignalEmit(data)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Message> deserialize(const std::vector<std::uint8_t>& bytes) {
    return deserialize(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

} // namespace entity::bus
