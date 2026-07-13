/**
 * ProjectSerializer Implementation
 *
 * Saves and loads Entity project files in JSON format.
 */

#include "entity/project/ProjectSerializer.hpp"
#include "entity/core/AtomicFile.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/ScaleLock.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectParam.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingAssetOps.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/MunchersGameState.hpp"
#include "entity/components/TextLayerState.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/AudioSource.hpp"
#include "entity/components/RemotePatch.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/LayerTrackUiState.hpp"
#include "entity/components/TrackUiState.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/OutputSurface.hpp"
#include "entity/components/OutputDisplay.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/Prop.hpp"
#include "entity/media/ObjLoader.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <unordered_set>

using json = nlohmann::json;

namespace entity {

std::string ProjectSerializer::s_lastError;

// v25 — emit the LayerTrackUiState component to the layer JSON when it
// holds any collapsed groups. Editor-only; never reaches the bus.
static void emitCollapsedGroupsJson(const entt::registry& registry,
                                    entt::entity layerEntity,
                                    json& layerJson) {
    const auto* ui = registry.try_get<LayerTrackUiState>(layerEntity);
    if (!ui || ui->collapsedGroups.empty()) return;
    json arr = json::array();
    for (const auto& p : ui->collapsedGroups) arr.push_back(p);
    layerJson["collapsedGroups"] = std::move(arr);
}

// v25 — re-attach the LayerTrackUiState component on a freshly loaded
// layer entity if the JSON carries any collapsed-group entries.
static void applyCollapsedGroupsJson(entt::registry& registry,
                                     entt::entity layerEntity,
                                     const json& layerJson) {
    if (!layerJson.contains("collapsedGroups")) return;
    const auto& arr = layerJson["collapsedGroups"];
    if (!arr.is_array() || arr.empty()) return;
    auto& ui = registry.get_or_emplace<LayerTrackUiState>(layerEntity);
    ui.collapsedGroups.clear();
    ui.collapsedGroups.reserve(arr.size());
    for (const auto& s : arr) {
        if (s.is_string()) ui.collapsedGroups.push_back(s.get<std::string>());
    }
    if (ui.collapsedGroups.empty()) {
        registry.remove<LayerTrackUiState>(layerEntity);
    }
}

// Helper: MediaType to string for JSON
static std::string mediaTypeToJson(MediaType type) {
    switch (type) {
        case MediaType::VideoProRes4444: return "prores4444";
        case MediaType::VideoHAP: return "hap";
        case MediaType::VideoHAPAlpha: return "hap_alpha";
        case MediaType::VideoHAPQ: return "hap_q";
        case MediaType::PNGSequence: return "png_sequence";
        case MediaType::DPXSequence: return "dpx_sequence";
        default: return "unknown";
    }
}

// Helper: String to MediaType from JSON
static MediaType jsonToMediaType(const std::string& str) {
    if (str == "prores4444") return MediaType::VideoProRes4444;
    if (str == "hap") return MediaType::VideoHAP;
    if (str == "hap_alpha") return MediaType::VideoHAPAlpha;
    if (str == "hap_q") return MediaType::VideoHAPQ;
    if (str == "png_sequence") return MediaType::PNGSequence;
    if (str == "dpx_sequence") return MediaType::DPXSequence;
    return MediaType::Unknown;
}

// Helper: BlendMode to string for JSON
static std::string blendModeToJson(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal: return "normal";
        case BlendMode::Add: return "add";
        case BlendMode::Multiply: return "multiply";
        case BlendMode::Screen: return "screen";
        case BlendMode::Overlay: return "overlay";
        case BlendMode::SoftLight: return "soft_light";
        case BlendMode::HardLight: return "hard_light";
        case BlendMode::ColorDodge: return "color_dodge";
        case BlendMode::ColorBurn: return "color_burn";
        case BlendMode::Darken: return "darken";
        case BlendMode::Lighten: return "lighten";
        case BlendMode::Difference: return "difference";
        case BlendMode::Exclusion: return "exclusion";
        default: return "normal";
    }
}

// Helper: PathKind to string for JSON. Strings instead of ints to keep
// .entity files diff-friendly. v7 always emits this; v6 loaders default
// to "linked" for missing entries.
static std::string pathKindToJson(ProjectManager::PathKind k) {
    switch (k) {
        case ProjectManager::PathKind::Managed: return "managed";
        case ProjectManager::PathKind::Linked:  return "linked";
    }
    return "linked";
}

static ProjectManager::PathKind jsonToPathKind(const std::string& s) {
    if (s == "managed") return ProjectManager::PathKind::Managed;
    return ProjectManager::PathKind::Linked;
}

static std::string animatablePropertyToJson(AnimatableProperty p) {
    switch (p) {
        case AnimatableProperty::PositionX: return "PositionX";
        case AnimatableProperty::PositionY: return "PositionY";
        case AnimatableProperty::Rotation:  return "Rotation";
        case AnimatableProperty::ScaleX:    return "ScaleX";
        case AnimatableProperty::ScaleY:    return "ScaleY";
        case AnimatableProperty::Opacity:   return "Opacity";
        case AnimatableProperty::PositionZ: return "PositionZ";
        case AnimatableProperty::RotationX: return "RotationX";
        case AnimatableProperty::RotationY: return "RotationY";
        case AnimatableProperty::ScaleZ:    return "ScaleZ";
        case AnimatableProperty::RotationZ: return "RotationZ";
        default: return "PositionX";
    }
}

static AnimatableProperty jsonToAnimatableProperty(const std::string& s) {
    if (s == "PositionX") return AnimatableProperty::PositionX;
    if (s == "PositionY") return AnimatableProperty::PositionY;
    if (s == "Rotation")  return AnimatableProperty::Rotation;
    if (s == "ScaleX")    return AnimatableProperty::ScaleX;
    if (s == "ScaleY")    return AnimatableProperty::ScaleY;
    if (s == "Opacity")   return AnimatableProperty::Opacity;
    if (s == "PositionZ") return AnimatableProperty::PositionZ;
    if (s == "RotationX") return AnimatableProperty::RotationX;
    if (s == "RotationY") return AnimatableProperty::RotationY;
    if (s == "ScaleZ")    return AnimatableProperty::ScaleZ;
    if (s == "RotationZ") return AnimatableProperty::RotationZ;
    return AnimatableProperty::PositionX;
}

static std::string interpolationTypeToJson(InterpolationType t) {
    switch (t) {
        case InterpolationType::Linear:    return "linear";
        case InterpolationType::Step:      return "step";
        case InterpolationType::EaseIn:    return "ease_in";
        case InterpolationType::EaseOut:   return "ease_out";
        case InterpolationType::EaseInOut: return "ease_in_out";
        default: return "linear";
    }
}

static InterpolationType jsonToInterpolationType(const std::string& s) {
    if (s == "step")       return InterpolationType::Step;
    if (s == "ease_in")    return InterpolationType::EaseIn;
    if (s == "ease_out")   return InterpolationType::EaseOut;
    if (s == "ease_in_out") return InterpolationType::EaseInOut;
    return InterpolationType::Linear;
}

// Serialize AnimatedProperties keyframe tracks to JSON. Returns an empty array
// if the component is absent. Callers embed the result as "animatedProperties".
static json serializeAnimatedProperties(const AnimatedProperties* ap) {
    json tracksJson = json::array();
    if (!ap) return tracksJson;
    for (const auto& track : ap->tracks) {
        json tj;
        tj["property"] = animatablePropertyToJson(track.property);
        tj["enabled"]  = track.enabled;
        json kfsJson = json::array();
        for (const auto& kf : track.keyframes) {
            json kj;
            kj["frame"]         = kf.frame;
            kj["value"]         = kf.value;
            kj["interpolation"] = interpolationTypeToJson(kf.interpolation);
            kj["easeIn"]        = kf.easeIn;
            kj["easeOut"]       = kf.easeOut;
            kfsJson.push_back(kj);
        }
        tj["keyframes"] = kfsJson;
        tracksJson.push_back(tj);
    }
    return tracksJson;
}

// Serialize a layer entity's EffectChain (issue #54). Returns an empty
// array if the layer has no chain. v16+. Each entry carries the kind's
// stable string ID + enabled flag + graph-editor x/y + a flat array of
// param values (all params currently typed Float; richer types arrive
// when v1 grows beyond Float).
static json serializeEffectChain(const entt::registry& registry,
                                  entt::entity layerEntity) {
    json effectsJson = json::array();
    const auto* chain = registry.try_get<EffectChain>(layerEntity);
    if (!chain) return effectsJson;

    for (auto fxEnt : chain->nodes) {
        if (!registry.valid(fxEnt)) continue;
        const auto* fx     = registry.try_get<Effect>(fxEnt);
        const auto* params = registry.try_get<EffectParameters>(fxEnt);
        if (!fx) continue;

        json fxJson;
        // We don't have the registry-side reverse map (kindIdHash → stableId)
        // here without an EffectKindRegistry pointer. Serializing the hash
        // works but isn't wire-friendly. The bake side (PlaybackTimeAuthority)
        // is the canonical place for the stableId mapping; the project
        // serializer takes the pragmatic approach and stores the hash plus
        // the stableId for engine-shipped kinds (lookup happens at load).
        //
        // Phase 6 (user-authored HLSL) will store stableId by writing it
        // through from the SetEffect* commands that created the effect, or
        // by threading the registry pointer in.
        fxJson["kindIdHash"] = fx->kindId;
        fxJson["enabled"]    = fx->enabled;
        fxJson["graphX"]     = fx->graphX;
        fxJson["graphY"]     = fx->graphY;

        json paramsJson = json::array();
        if (params) {
            for (const auto& v : params->values) {
                // v1 engine effects are all Float. Bool / Int / Vec types
                // arrive in Phase 6 when user-authored effects can declare
                // them — the array shape upgrades to object entries then.
                paramsJson.push_back(v.f4[0]);
            }
        }
        fxJson["params"] = paramsJson;

        effectsJson.push_back(fxJson);
    }
    return effectsJson;
}

// Deserialize a layer's effect chain from JSON. Idempotent — skips
// gracefully if the key is missing (pre-v16 projects). Creates new effect
// entities, attaches Effect / EffectParameters / EffectAnimatedParameters,
// and appends to the layer's EffectChain.
static void deserializeEffectChain(const json& entryJson,
                                    entt::registry& registry,
                                    entt::entity layerEntity) {
    if (!entryJson.contains("effects")) return;
    const auto& effectsJson = entryJson["effects"];
    if (!effectsJson.is_array() || effectsJson.empty()) return;

    auto& chain = registry.get_or_emplace<EffectChain>(layerEntity);
    chain.nodes.reserve(chain.nodes.size() + effectsJson.size());

    for (const auto& fxJson : effectsJson) {
        const auto fxEnt = registry.create();

        Effect& fx = registry.emplace<Effect>(fxEnt);
        fx.kindId     = fxJson.value("kindIdHash", std::uint32_t{0});
        fx.enabled    = fxJson.value("enabled",    true);
        fx.graphX     = fxJson.value("graphX",     0.0f);
        fx.graphY     = fxJson.value("graphY",     0.0f);
        fx.ownerLayer = layerEntity;

        EffectParameters& params = registry.emplace<EffectParameters>(fxEnt);
        if (fxJson.contains("params") && fxJson["params"].is_array()) {
            for (const auto& p : fxJson["params"]) {
                // Float-only v1 — see serializeEffectChain.
                params.values.push_back(ParamValue::makeFloat(p.get<float>()));
            }
        }
        registry.emplace<EffectAnimatedParameters>(fxEnt);

        chain.nodes.push_back(fxEnt);
    }
    if (!chain.nodes.empty() && chain.outputNode == entt::null) {
        chain.outputNode = chain.nodes.back();
    }
}

// Deserialize AnimatedProperties keyframe tracks from JSON. Idempotent — can
// be called on any entry that has an "animatedProperties" array; skips
// gracefully if the key is missing (pre-v15 entries or entries with no
// keyframes).
static void deserializeAnimatedProperties(const json& entryJson,
                                          AnimatedProperties& ap) {
    if (!entryJson.contains("animatedProperties")) return;
    const auto& tracksJson = entryJson["animatedProperties"];
    if (!tracksJson.is_array()) return;
    ap.tracks.clear();
    for (const auto& tj : tracksJson) {
        AnimatableProperty prop = jsonToAnimatableProperty(tj.value("property", "PositionX"));
        auto& track = ap.getOrCreateTrack(prop);
        track.enabled = tj.value("enabled", true);
        if (tj.contains("keyframes") && tj["keyframes"].is_array()) {
            for (const auto& kj : tj["keyframes"]) {
                FrameNumber frame = kj.value("frame", static_cast<FrameNumber>(0));
                float value = kj.value("value", 0.0f);
                InterpolationType interp = jsonToInterpolationType(
                    kj.value("interpolation", std::string{"linear"}));
                track.addKeyframe(frame, value, interp);
                // Restore bezier handles (post-addKeyframe the kf is guaranteed
                // to exist at that frame — find it and patch easeIn/Out).
                auto it = std::lower_bound(track.keyframes.begin(), track.keyframes.end(), frame,
                    [](const Keyframe& k, FrameNumber f) { return k.frame < f; });
                if (it != track.keyframes.end() && it->frame == frame) {
                    it->easeIn  = kj.value("easeIn",  0.42f);
                    it->easeOut = kj.value("easeOut", 0.58f);
                }
            }
        }
    }
}

// Helper: String to BlendMode from JSON
static BlendMode jsonToBlendMode(const std::string& str) {
    if (str == "normal") return BlendMode::Normal;
    if (str == "add") return BlendMode::Add;
    if (str == "multiply") return BlendMode::Multiply;
    if (str == "screen") return BlendMode::Screen;
    if (str == "overlay") return BlendMode::Overlay;
    if (str == "soft_light") return BlendMode::SoftLight;
    if (str == "hard_light") return BlendMode::HardLight;
    if (str == "color_dodge") return BlendMode::ColorDodge;
    if (str == "color_burn") return BlendMode::ColorBurn;
    if (str == "darken") return BlendMode::Darken;
    if (str == "lighten") return BlendMode::Lighten;
    if (str == "difference") return BlendMode::Difference;
    if (str == "exclusion") return BlendMode::Exclusion;
    return BlendMode::Normal;
}

bool ProjectSerializer::save(const Timeline& timeline, const std::filesystem::path& filepath,
                              const ProjectManager* projectMgr) {
    try {
        // Ensure file has correct extension
        std::filesystem::path savePath = filepath;
        if (savePath.extension() != FILE_EXTENSION) {
            savePath += FILE_EXTENSION;
        }

        json project;

        // Project metadata
        project["version"] = PROJECT_VERSION;
        project["format"] = "entity_project";

        // Timeline settings
        project["timeline"]["duration"] = timeline.getDuration();
        project["timeline"]["framerate"] = timeline.getFrameRate();
        project["timeline"]["currentTime"] = timeline.getCurrentTime();
        // v28: global "hide shy tracks" editor view-state (Phase 9). Additive —
        // legacy files lack the key and load as false.
        project["timeline"]["hideShyTracks"] = timeline.getHideShyTracks();

        // Tracks
        json tracksJson = json::array();
        const auto& registry = timeline.getRegistry();

        for (entt::entity trackEntity : timeline.getTracks()) {
            const auto* track = registry.try_get<TimelineTrack>(trackEntity);
            if (!track) continue;

            json trackJson;
            trackJson["index"] = track->trackIndex;

            // v28: per-track name and shy fields (TrackUiState).
            // Missing on load = empty name ("Track N" fallback) + shy=false.
            if (const auto* ui = registry.try_get<TrackUiState>(trackEntity)) {
                trackJson["name"] = ui->name;
                trackJson["shy"]  = ui->shy;
            }

            // Layers in this track (v15: unified "layers[]" replacing "clips[]").
            // Each entry has a "kind" discriminator: "clip" or "object_animation".
            json layersJson = json::array();
            for (entt::entity layerEntity : track->layers) {
                const auto* oal = registry.try_get<ObjectAnimationLayer>(layerEntity);
                const auto* clip = registry.try_get<Clip>(layerEntity);

                if (clip && !oal) {
                    // --- kind: "clip" ---
                    json clipJson;
                    clipJson["kind"] = "clip";

                    clipJson["filepath"] = clip->filepath;
                    clipJson["mediaType"] = mediaTypeToJson(clip->mediaType);
                    clipJson["startFrame"] = clip->startFrame;
                    clipJson["duration"] = clip->duration;
                    clipJson["mediaStartFrame"] = clip->mediaStartFrame;
                    clipJson["mediaOutFrame"] = clip->mediaOutFrame;
                    clipJson["totalMediaFrames"] = clip->totalMediaFrames;
                    clipJson["playbackMode"] = static_cast<int>(clip->playbackMode);
                    clipJson["sectionBehavior"] =
                        (clip->sectionBehavior == SectionBehavior::Locked) ? "Locked" : "Normal";
                    clipJson["framerate"] = clip->framerate;
                    clipJson["width"] = clip->width;
                    clipJson["height"] = clip->height;
                    clipJson["hasAlpha"] = clip->hasAlpha;
                    clipJson["frameBlending"] = clip->frameBlending;

                    // Plane A content routing (ADR-0022). v20+ stores the
                    // routing as a *named reference* into the top-level
                    // `contentRoutingAssets` library; the asset itself
                    // carries kind/targets. The legacy v19 `contentRouting`
                    // inline JSON is still emitted for one project-format
                    // version so older readers degrade gracefully (snapshot
                    // bake on this build reads ContentRoutingRef, but a
                    // hypothetical reverse-compat v19 reader would pick up
                    // the inline data). The legacy `targetScreenName` key
                    // is no longer written.
                    if (const auto* ref = registry.try_get<ContentRoutingRef>(layerEntity)) {
                        if (ref->asset != entt::null &&
                            registry.valid(ref->asset)) {
                            if (const auto* a =
                                    registry.try_get<ContentRoutingAsset>(ref->asset)) {
                                clipJson["contentRoutingAssetName"] = a->name;
                            }
                        }
                    }
                    // Emit the v19 inline `contentRouting` JSON for one
                    // more project-format version. Prefer the ref→asset
                    // path; fall back to a still-registered inline
                    // ContentRouting (test fixtures, hand-built entities
                    // that haven't migrated yet) so legacy data survives
                    // the save-then-reload round-trip until L2 finalizes
                    // the cutover.
                    auto writeInlineRouting =
                        [&](RouteMode mode, const std::vector<RouteTarget>& targets) {
                        json crJson;
                        crJson["mode"] = (mode == RouteMode::Tiled) ? "Tiled" : "Direct";
                        json targetsArr = json::array();
                        for (const auto& t : targets) {
                            json tJson;
                            std::string screenName;
                            if (t.screen != entt::null &&
                                registry.valid(t.screen) &&
                                registry.all_of<Screen>(t.screen)) {
                                screenName = registry.get<Screen>(t.screen).name;
                            }
                            tJson["screenName"] = screenName;
                            tJson["uvRect"] = { t.uvRect[0], t.uvRect[1], t.uvRect[2], t.uvRect[3] };
                            targetsArr.push_back(tJson);
                        }
                        crJson["targets"] = std::move(targetsArr);
                        clipJson["contentRouting"] = std::move(crJson);
                    };

                    if (const auto* asset = routing::tryGetAsset(registry, layerEntity)) {
                        writeInlineRouting(asset->kind, asset->targets);
                    } else if (const auto* cr =
                                registry.try_get<ContentRouting>(layerEntity)) {
                        writeInlineRouting(cr->mode, cr->targets);
                    }

                    // Transform component (if exists)
                    const auto* transform = registry.try_get<Transform>(layerEntity);
                    if (transform) {
                        clipJson["transform"]["position"] = {
                            transform->position.x,
                            transform->position.y,
                            transform->position.z
                        };
                        clipJson["transform"]["rotation"] = {
                            transform->rotation.x,
                            transform->rotation.y,
                            transform->rotation.z
                        };
                        clipJson["transform"]["scale"] = {
                            transform->scale.x,
                            transform->scale.y,
                            transform->scale.z
                        };
                        // Uniform-scale lock. Only written when unlocked — absent
                        // means locked, which is the default and what every project
                        // saved before this component existed implies.
                        if (const auto* lock = registry.try_get<ScaleLock>(layerEntity)) {
                            if (!lock->uniform) clipJson["transform"]["scaleLock"] = false;
                        }
                    }

                    // MediaLayer component (if exists)
                    const auto* mediaLayer = registry.try_get<MediaLayer>(layerEntity);
                    if (mediaLayer) {
                        clipJson["layer"]["zOrder"] = mediaLayer->zOrder;
                        clipJson["layer"]["opacity"] = mediaLayer->opacity;
                        clipJson["layer"]["blendMode"] = blendModeToJson(mediaLayer->blendMode);
                        clipJson["layer"]["visible"] = mediaLayer->visible;
                    }

                    // AnimatedProperties (if any keyframes exist)
                    const auto* ap = registry.try_get<AnimatedProperties>(layerEntity);
                    clipJson["animatedProperties"] = serializeAnimatedProperties(ap);

                    // Effect chain (issue #54, v16+). Empty array when the
                    // layer has no EffectChain component — forward/backward
                    // compat free.
                    clipJson["effects"] = serializeEffectChain(registry, layerEntity);

                    // v23 — AudioSource (optional; only written when clip has audio).
                    const auto* audioSrc = registry.try_get<AudioSource>(layerEntity);
                    if (audioSrc && audioSrc->hasAudioStream) {
                        json audioJson;
                        audioJson["gain"] = audioSrc->gain;
                        audioJson["mute"] = audioSrc->mute;
                        audioJson["solo"] = audioSrc->solo;
                        clipJson["audio"] = std::move(audioJson);
                    }

                    // v27 — RemotePatch (optional; only when layer is patched).
                    if (const auto* rp = registry.try_get<RemotePatch>(layerEntity)) {
                        json rpJson;
                        rpJson["id"]    = rp->patchId;
                        rpJson["armed"] = rp->armedByDefault;
                        clipJson["remotePatch"] = std::move(rpJson);
                    }

                    // v25 — twirl-down collapsed group paths.
                    emitCollapsedGroupsJson(registry, layerEntity, clipJson);

                    layersJson.push_back(clipJson);

                } else if (oal) {
                    // --- kind: "object_animation" ---
                    json oaJson;
                    oaJson["kind"] = "object_animation";

                    // Layer placement fields. ObjectAnimationLayer doesn't own
                    // startFrame/duration — those live on the Layer component.
                    const auto* lay = registry.try_get<Layer>(layerEntity);
                    oaJson["startFrame"] = lay ? lay->startFrame : 0;
                    oaJson["duration"]   = lay ? lay->duration   : 0;
                    oaJson["name"]       = lay ? lay->name       : "";
                    if (lay) {
                        oaJson["color"] = { lay->color[0], lay->color[1],
                                            lay->color[2], lay->color[3] };
                    }

                    oaJson["sectionBehavior"] =
                        (oal->sectionBehavior == SectionBehavior::Locked) ? "Locked" : "Normal";

                    // ADR-0020 / v17: what happens to the target Screen/Prop
                    // when the playhead leaves the layer window. Default Hold
                    // (last evaluated values keep applying); operator can
                    // opt-in to Reset (snap back to Stage-configured base).
                    oaJson["endBehavior"] =
                        (oal->endBehavior == ObjectAnimationLayer::EndBehavior::Reset)
                            ? "Reset" : "Hold";

                    // target: persist by Screen name (or Prop name if the
                    // target entity has a Prop component). entt::entity
                    // values are not stable across sessions.
                    std::string targetName;
                    if (oal->target != entt::null && registry.valid(oal->target)) {
                        if (registry.all_of<Screen>(oal->target)) {
                            targetName = registry.get<Screen>(oal->target).name;
                        } else if (registry.all_of<Prop>(oal->target)) {
                            targetName = registry.get<Prop>(oal->target).name;
                        }
                    }
                    oaJson["targetName"] = targetName;

                    // AnimatedProperties keyframe tracks
                    const auto* ap = registry.try_get<AnimatedProperties>(layerEntity);
                    oaJson["animatedProperties"] = serializeAnimatedProperties(ap);

                    // v25 — twirl-down collapsed group paths.
                    emitCollapsedGroupsJson(registry, layerEntity, oaJson);

                    layersJson.push_back(oaJson);

                } else if (const auto* genLayer = registry.try_get<GenerativeLayer>(layerEntity)) {
                    // --- kind: "generative" (v21+) ---
                    json genJson;
                    genJson["kind"] = "generative";

                    const auto* lay = registry.try_get<Layer>(layerEntity);
                    genJson["startFrame"] = lay ? lay->startFrame : 0;
                    genJson["duration"]   = lay ? lay->duration   : 0;
                    genJson["name"]       = lay ? lay->name       : "";
                    if (lay) {
                        genJson["color"] = { lay->color[0], lay->color[1],
                                             lay->color[2], lay->color[3] };
                    }

                    // targetScreen by name (entity IDs are not stable across sessions)
                    std::string tgtName;
                    if (genLayer->targetScreen != entt::null
                        && registry.valid(genLayer->targetScreen)
                        && registry.all_of<Screen>(genLayer->targetScreen)) {
                        tgtName = registry.get<Screen>(genLayer->targetScreen).name;
                    }
                    genJson["targetScreenName"] = tgtName;
                    genJson["renderWidth"]  = genLayer->renderWidth;
                    genJson["renderHeight"] = genLayer->renderHeight;

                    // ContentRoutingRef (v20+)
                    if (const auto* ref = registry.try_get<ContentRoutingRef>(layerEntity)) {
                        if (ref->asset != entt::null && registry.valid(ref->asset)) {
                            if (const auto* a = registry.try_get<ContentRoutingAsset>(ref->asset))
                                genJson["contentRoutingAssetName"] = a->name;
                        }
                    }

                    // Transform
                    if (const auto* t = registry.try_get<Transform>(layerEntity)) {
                        genJson["transform"]["position"] = { t->position.x, t->position.y, t->position.z };
                        genJson["transform"]["rotation"] = { t->rotation.x, t->rotation.y, t->rotation.z };
                        genJson["transform"]["scale"]    = { t->scale.x,    t->scale.y,    t->scale.z    };
                        if (const auto* lock = registry.try_get<ScaleLock>(layerEntity)) {
                            if (!lock->uniform) genJson["transform"]["scaleLock"] = false;
                        }
                    }

                    // MediaLayer
                    if (const auto* ml = registry.try_get<MediaLayer>(layerEntity)) {
                        genJson["layer"]["zOrder"]    = ml->zOrder;
                        genJson["layer"]["opacity"]   = ml->opacity;
                        genJson["layer"]["blendMode"] = blendModeToJson(ml->blendMode);
                        genJson["layer"]["visible"]   = ml->visible;
                    }

                    // sub-kind discriminator + kind-specific persistent state
                    if (registry.all_of<TextLayerState>(layerEntity)) {
                        genJson["sub_kind"] = "text";
                        const auto& tls = registry.get<TextLayerState>(layerEntity);
                        json tsJson;
                        tsJson["text"]       = tls.text;
                        tsJson["fontFamily"] = tls.fontFamily;
                        tsJson["fontSize"]   = tls.fontSize;
                        tsJson["color"]      = { tls.color.r, tls.color.g,
                                                 tls.color.b, tls.color.a };
                        tsJson["alignment"]  = static_cast<int>(tls.alignment);
                        tsJson["bold"]       = tls.bold;
                        tsJson["italic"]     = tls.italic;
                        genJson["text_state"] = tsJson;
                    } else {
                        genJson["sub_kind"] = "muncher";
                        // MunchersGameState resets each session — no persistent fields.
                    }

                    // AnimatedProperties keyframe tracks (additive, optional —
                    // empty array when the generative layer has no keyframes).
                    {
                        const auto* ap =
                            registry.try_get<AnimatedProperties>(layerEntity);
                        genJson["animatedProperties"] = serializeAnimatedProperties(ap);
                    }

                    // v27 — RemotePatch (optional; only when layer is patched).
                    if (const auto* rp = registry.try_get<RemotePatch>(layerEntity)) {
                        json rpJson;
                        rpJson["id"]    = rp->patchId;
                        rpJson["armed"] = rp->armedByDefault;
                        genJson["remotePatch"] = std::move(rpJson);
                    }

                    // v25 — twirl-down collapsed group paths.
                    emitCollapsedGroupsJson(registry, layerEntity, genJson);

                    layersJson.push_back(genJson);
                } else if (const auto* sigLayer = registry.try_get<SignalLayer>(layerEntity)) {
                    // --- kind: "signal" (v26+) ---
                    json sigJson;
                    sigJson["kind"] = "signal";

                    const auto* lay = registry.try_get<Layer>(layerEntity);
                    sigJson["startFrame"] = lay ? lay->startFrame : 0;
                    sigJson["duration"]   = lay ? lay->duration   : 0;
                    sigJson["name"]       = lay ? lay->name       : "";
                    if (lay) {
                        sigJson["color"] = { lay->color[0], lay->color[1],
                                             lay->color[2], lay->color[3] };
                    }

                    sigJson["mode"]        = static_cast<int>(sigLayer->mode);
                    sigJson["transport"]   = static_cast<int>(sigLayer->transport);
                    sigJson["valueSource"] = static_cast<int>(sigLayer->valueSource);
                    sigJson["address"]     = sigLayer->address;
                    sigJson["srcMin"]      = sigLayer->srcMin;
                    sigJson["srcMax"]      = sigLayer->srcMax;
                    sigJson["outMin"]      = sigLayer->outMin;
                    sigJson["outMax"]      = sigLayer->outMax;
                    sigJson["outType"]     = static_cast<int>(sigLayer->outType);

                    json argsArr = json::array();
                    for (const auto& a : sigLayer->args) {
                        json aj;
                        aj["type"]         = static_cast<int>(a.type);
                        aj["i"]            = a.i;
                        aj["f"]            = a.f;
                        aj["s"]            = a.s;
                        aj["isValueBound"] = a.isValueBound;
                        argsArr.push_back(aj);
                    }
                    sigJson["args"] = argsArr;

                    // AnimatedProperties (Keyframe value source).
                    {
                        const auto* ap = registry.try_get<AnimatedProperties>(layerEntity);
                        sigJson["animatedProperties"] = serializeAnimatedProperties(ap);
                    }

                    layersJson.push_back(sigJson);
                }
                // Entities with neither Clip, ObjectAnimationLayer, GenerativeLayer, nor SignalLayer
                // are unrecognized and skipped — forward-compat guard.
            }

            trackJson["layers"] = layersJson;
            tracksJson.push_back(trackJson);
        }

        project["tracks"] = tracksJson;

        // Project-wide media library + import prefs (v4+). Optional —
        // callers that only care about timeline content can pass nullptr.
        if (projectMgr) {
            json libJson = json::array();
            for (const auto& entry : projectMgr->loadedMediaFiles()) {
                json ej;
                ej["originalPath"] = entry.originalPath;
                ej["transcodedPath"] = entry.transcodedPath;
                ej["variant"] = entry.variant;
                // Phase C.12 #9 — emit only when set so older loaders that
                // don't know the key keep working unchanged.
                if (!entry.inputColorSpaceOverride.empty()) {
                    ej["inputColorSpaceOverride"] = entry.inputColorSpaceOverride;
                }
                // ADR-0009 / v7. pathKind is always emitted; archive
                // fields are optional and only present when populated.
                ej["pathKind"] = pathKindToJson(entry.pathKind);
                if (!entry.archivedOriginal.empty()) {
                    ej["archivedOriginal"] = entry.archivedOriginal;
                }
                if (!entry.originalCodec.empty()) {
                    ej["originalCodec"] = entry.originalCodec;
                }
                // v12 — probe cache. Emit only when we've actually probed
                // the file (size > 0); pre-v12 / never-probed entries
                // serialize without the keys and re-probe on next open.
                if (entry.lastProbeSizeBytes > 0) {
                    ej["lastProbeSizeBytes"] = entry.lastProbeSizeBytes;
                    ej["lastProbeMtimeUnix"] = entry.lastProbeMtimeUnix;
                    if (entry.cachedProbe.valid) {
                        json pj;
                        pj["valid"]            = entry.cachedProbe.valid;
                        pj["isHap"]            = entry.cachedProbe.isHap;
                        pj["width"]            = entry.cachedProbe.width;
                        pj["height"]           = entry.cachedProbe.height;
                        pj["framerate"]        = entry.cachedProbe.framerate;
                        pj["totalFrames"]      = entry.cachedProbe.totalFrames;
                        pj["hasAlpha"]         = entry.cachedProbe.hasAlpha;
                        pj["sourceCodecName"]  = entry.cachedProbe.sourceCodecName;
                        pj["displayCodecName"] = entry.cachedProbe.displayCodecName;
                        pj["tier"]             = static_cast<int>(entry.cachedProbe.tier);
                        ej["probe"] = pj;
                    }
                }
                libJson.push_back(ej);
            }
            project["mediaLibrary"] = libJson;
            project["nonHapImportPolicy"] = static_cast<int>(projectMgr->nonHapImportPolicy());

            // v22 — per-project DMX channel mapping table (#13 / #59).
            // Round-tripped as a raw JSON string; the typed struct
            // lives in the dmx-artnet plugin (Apache-2.0) so the GPL
            // serializer keeps the boundary clean. Empty -> plugin
            // uses its baked default mapping table.
            project["dmxMappingsJson"] = projectMgr->getDmxMappingsJson();

            // Freeform (no schema bump) — per-project OSC mapping tables.
            // Same boundary rationale as dmxMappingsJson: raw JSON string,
            // typed structs live in the Apache-2.0 plugin boundary.
            // Empty string -> plugin falls back to hardcoded defaults.
            // Missing on older project files -> stays empty on load.
            project["oscInboundMappingsJson"]  = projectMgr->getOscInboundMappingsJson();
            project["oscOutboundMappingsJson"] = projectMgr->getOscOutboundMappingsJson();

            // v23 — project-level audio master state.
            project["audioMasterGain"] = projectMgr->getAudioMasterGain();
            project["audioMasterMute"] = projectMgr->getAudioMasterMute();

            // v28 (additive) — editor layout embed (item 3). Opaque JSON blob
            // built by Engine from WindowManager (imguiIni + hiddenWindows +
            // layoutLocked + focusedTabs). Parse + embed as a structured
            // sub-object so the file is human-inspectable; omit the key
            // entirely when empty (headless saves / no layout captured) so
            // legacy readers and headless round-trips are unaffected.
            {
                const std::string layoutBlob = projectMgr->getEditorLayoutJson();
                if (!layoutBlob.empty()) {
                    try {
                        project["editorLayout"] = json::parse(layoutBlob);
                    } catch (const json::parse_error& e) {
                        std::cerr << "[ProjectSerializer] editorLayout blob is not "
                                     "valid JSON; skipping embed: " << e.what() << std::endl;
                    }
                }
            }

            // v18 — object library (parallel of mediaLibrary for 3D models).
            // Same pathKind + size-validity convention; no transcode /
            // archive fields because models don't have those flows.
            json objLibJson = json::array();
            for (const auto& entry : projectMgr->loadedObjectFiles()) {
                json ej;
                ej["originalPath"] = entry.originalPath;
                ej["pathKind"]     = pathKindToJson(entry.pathKind);
                if (entry.lastProbeSizeBytes > 0) {
                    ej["lastProbeSizeBytes"] = entry.lastProbeSizeBytes;
                    ej["lastProbeMtimeUnix"] = entry.lastProbeMtimeUnix;
                }
                objLibJson.push_back(ej);
            }
            project["objectLibrary"] = objLibJson;
        }

        // Serialize mapping surfaces
        json surfacesJson = json::array();
        auto surfaceView = registry.view<OutputSurface>();
        for (auto [entity, surface] : surfaceView.each()) {
            json surfaceJson;
            surfaceJson["name"] = surface.name;
            surfaceJson["surfaceIndex"] = surface.surfaceIndex;
            surfaceJson["visible"] = surface.visible;
            surfaceJson["brightness"] = surface.brightness;
            surfaceJson["gamma"] = surface.gamma;
            surfaceJson["gridSubdivisions"] = surface.gridSubdivisions;
            surfaceJson["outputIndex"] = surface.outputIndex;

            // Save corners
            json cornersJson = json::array();
            for (int i = 0; i < 4; ++i) {
                cornersJson.push_back({surface.corners[i].x, surface.corners[i].y});
            }
            surfaceJson["corners"] = cornersJson;

            // Save source UVs
            json uvsJson = json::array();
            for (int i = 0; i < 4; ++i) {
                uvsJson.push_back({surface.sourceUVs[i].x, surface.sourceUVs[i].y});
            }
            surfaceJson["sourceUVs"] = uvsJson;

            // Save soft edges
            surfaceJson["softEdge"]["left"] = surface.softEdge.left;
            surfaceJson["softEdge"]["right"] = surface.softEdge.right;
            surfaceJson["softEdge"]["top"] = surface.softEdge.top;
            surfaceJson["softEdge"]["bottom"] = surface.softEdge.bottom;

            surfacesJson.push_back(surfaceJson);
        }
        project["mappingSurfaces"] = surfacesJson;

        // Serialize Models. Mesh data is not persisted; on load, file-backed
        // models reload from their OBJ, and models with an empty filepath are
        // treated as the built-in default 16:9 plane (createDefaultScreenMesh).
        // GPU slot fields (vertexBufferSlot/indexBufferSlot) are runtime-only.
        json modelsJson = json::array();
        auto modelView = registry.view<Model>();
        for (auto [entity, model] : modelView.each()) {
            json mj;
            mj["name"] = model.name;
            mj["filepath"] = model.filepath;
            modelsJson.push_back(mj);
        }
        project["models"] = modelsJson;

        // Serialize Screens. modelEntity is resolved by Model name on load
        // since entt::entity values aren't stable across sessions.
        // renderTargetSlot/renderTargetValid are runtime-only.
        json screensJson = json::array();
        auto screenView = registry.view<Screen>();
        for (auto [entity, screen] : screenView.each()) {
            json sj;
            sj["name"] = screen.name;
            sj["width"] = screen.width;
            sj["height"] = screen.height;
            sj["position"] = {screen.position[0], screen.position[1], screen.position[2]};
            sj["rotation"] = {screen.rotation[0], screen.rotation[1], screen.rotation[2]};
            sj["size"]     = {screen.size[0],     screen.size[1],     screen.size[2]};
            sj["visible"] = screen.visible;
            sj["opacity"] = screen.opacity;
            sj["zOrder"]  = screen.zOrder;

            std::string modelName;
            if (screen.modelEntity != entt::null &&
                registry.valid(screen.modelEntity) &&
                registry.all_of<Model>(screen.modelEntity)) {
                modelName = registry.get<Model>(screen.modelEntity).name;
            }
            sj["modelName"] = modelName;

            screensJson.push_back(sj);
        }
        project["screens"] = screensJson;

        // Serialize ContentRoutingAsset library (ADR-0022, v20+). Auto-direct
        // assets that haven't been customized are skipped — RoutingLibrarySystem
        // regenerates them on load from the Screen set. Customized auto-direct
        // assets (renamed, edited uvRect, kind promoted to Tiled) and all
        // user-created assets are serialized so the link survives reload.
        {
            json assetsArr = json::array();
            const std::array<float, 4> identityUV{0.0f, 0.0f, 1.0f, 1.0f};
            for (auto [aEntity, asset] : registry.view<ContentRoutingAsset>().each()) {
                if (asset.autoBoundScreen != entt::null) {
                    const bool pristine =
                        asset.kind == RouteMode::Direct &&
                        asset.targets.size() == 1 &&
                        asset.targets[0].screen == asset.autoBoundScreen &&
                        asset.targets[0].uvRect == identityUV &&
                        asset.name == asset.lastSyncedScreenName;
                    if (pristine) continue;  // regenerated on load
                }
                json aJson;
                aJson["name"] = asset.name;
                switch (asset.kind) {
                    case RouteMode::Direct:  aJson["kind"] = "Direct";  break;
                    case RouteMode::Tiled:   aJson["kind"] = "Tiled";   break;
                    case RouteMode::FeedMap: aJson["kind"] = "FeedMap"; break;
                }
                if (asset.kind == RouteMode::Tiled) {
                    aJson["tiledCount"] = asset.tiledCount;
                    aJson["tiledAxis"]  = asset.tiledAxis;
                }
                if (asset.kind == RouteMode::FeedMap) {
                    aJson["sourceWidth"]  = asset.sourceWidth;
                    aJson["sourceHeight"] = asset.sourceHeight;
                }
                if (asset.autoBoundScreen != entt::null &&
                    registry.valid(asset.autoBoundScreen) &&
                    registry.all_of<Screen>(asset.autoBoundScreen)) {
                    aJson["autoBoundScreenName"] =
                        registry.get<Screen>(asset.autoBoundScreen).name;
                }
                if (!asset.lastSyncedScreenName.empty()) {
                    aJson["lastSyncedScreenName"] = asset.lastSyncedScreenName;
                }
                json targetsArr = json::array();
                for (const auto& t : asset.targets) {
                    json tj;
                    std::string sn;
                    if (t.screen != entt::null && registry.valid(t.screen) &&
                        registry.all_of<Screen>(t.screen)) {
                        sn = registry.get<Screen>(t.screen).name;
                    }
                    tj["screenName"] = sn;
                    tj["uvRect"] = { t.uvRect[0], t.uvRect[1], t.uvRect[2], t.uvRect[3] };
                    if (!t.name.empty()) tj["name"] = t.name;
                    targetsArr.push_back(tj);
                }
                aJson["targets"] = std::move(targetsArr);
                assetsArr.push_back(std::move(aJson));
            }
            project["contentRoutingAssets"] = std::move(assetsArr);
        }

        // Serialize Props (v13). Pre-vis-only stage geometry — see Prop.hpp.
        // Same modelName-by-string convention as Screens since entt::entity
        // values aren't stable across sessions.
        json propsJson = json::array();
        auto propView = registry.view<Prop>();
        for (auto [entity, prop] : propView.each()) {
            json pj;
            pj["name"]     = prop.name;
            pj["position"] = {prop.position[0], prop.position[1], prop.position[2]};
            pj["rotation"] = {prop.rotation[0], prop.rotation[1], prop.rotation[2]};
            pj["size"]     = {prop.size[0],     prop.size[1],     prop.size[2]};
            pj["visible"]  = prop.visible;
            pj["opacity"]  = prop.opacity;  // v19+, stage-view-only fade
            pj["displayColor"] = {prop.displayColor[0], prop.displayColor[1],
                                  prop.displayColor[2], prop.displayColor[3]};

            std::string modelName;
            if (prop.modelEntity != entt::null &&
                registry.valid(prop.modelEntity) &&
                registry.all_of<Model>(prop.modelEntity)) {
                modelName = registry.get<Model>(prop.modelEntity).name;
            }
            pj["modelName"] = modelName;

            propsJson.push_back(pj);
        }
        project["props"] = propsJson;

        // Serialize output displays (Phase C #1)
        // outputWindowSlot is runtime-only and deliberately skipped — it's a
        // renderer slot ID reassigned every session. sourceScreen is an
        // entt::entity which isn't stable across sessions, so persist the
        // referenced Screen's name and re-resolve on load.
        json outputsJson = json::array();
        auto outputView = registry.view<OutputDisplay>();
        for (auto [entity, out] : outputView.each()) {
            json oj;
            oj["name"] = out.name;
            oj["outputIndex"] = out.outputIndex;
            oj["type"] = static_cast<int>(out.type);
            oj["enabled"] = out.enabled;
            oj["width"] = out.width;
            oj["height"] = out.height;
            oj["refreshRate"] = out.refreshRate;
            oj["physicalDisplayIndex"] = out.physicalDisplayIndex;
            oj["deviceName"] = out.deviceName;
            oj["displayName"] = out.displayName;
            oj["brightness"] = out.brightness;
            oj["gamma"] = out.gamma;
            oj["ocioDisplay"] = out.ocioDisplay;
            oj["ocioView"] = out.ocioView;
            oj["fullscreen"] = out.fullscreen;
            oj["windowX"] = out.windowX;
            oj["windowY"] = out.windowY;
            oj["windowWidth"] = out.windowWidth;
            oj["windowHeight"] = out.windowHeight;

            json ir;
            ir["x"] = out.inputRegion.x;
            ir["y"] = out.inputRegion.y;
            ir["width"] = out.inputRegion.width;
            ir["height"] = out.inputRegion.height;
            oj["inputRegion"] = ir;

            // Persist sourceScreen by name (entt::entity values aren't stable
            // across save/load). Empty string = "first visible screen" fallback.
            std::string sourceName;
            if (out.sourceScreen != entt::null &&
                registry.valid(out.sourceScreen) &&
                registry.all_of<Screen>(out.sourceScreen)) {
                sourceName = registry.get<Screen>(out.sourceScreen).name;
            }
            oj["sourceScreenName"] = sourceName;

            outputsJson.push_back(oj);
        }
        project["outputs"] = outputsJson;

        // Serialize Projectors (v8). Pose, FOV, lens distortion, calibration
        // points, and warp opt-in all persist so the user's calibration work
        // survives close-and-reopen — the whole point of the feature is the
        // user spending time placing crosshairs against physical features.
        // Entity refs (linkedOutput → OutputDisplay, targetSurfaces → Screens)
        // persist by name string, same pattern as Clip→Screen + Output→Screen.
        json projectorsJson = json::array();
        auto projView = registry.view<Projector>();
        for (auto [entity, proj] : projView.each()) {
            json pj;
            pj["name"]            = proj.name;
            pj["position"]        = { proj.position[0], proj.position[1], proj.position[2] };
            pj["rotation"]        = { proj.rotation[0], proj.rotation[1], proj.rotation[2] };
            pj["fovDegrees"]      = proj.fovDegrees;
            pj["nearClip"]        = proj.nearClip;
            pj["farClip"]         = proj.farClip;
            pj["enabled"]         = proj.enabled;
            pj["isCalibrated"]    = proj.isCalibrated;
            pj["distortionK1"]    = proj.distortionK1;
            pj["distortionK2"]    = proj.distortionK2;
            pj["useResidualWarp"] = proj.useResidualWarp;

            // linkedOutput → OutputDisplay name. Empty string = unlinked.
            std::string linkedOutputName;
            if (proj.linkedOutput != entt::null &&
                registry.valid(proj.linkedOutput) &&
                registry.all_of<OutputDisplay>(proj.linkedOutput)) {
                linkedOutputName = registry.get<OutputDisplay>(proj.linkedOutput).name;
            }
            pj["linkedOutputName"] = linkedOutputName;

            // targetSurfaces → array of Screen names. Empty array (or missing
            // key on load) = "project onto all visible Screens" — see
            // Projector::targetSurfaceCount semantics.
            json targetNames = json::array();
            for (int i = 0; i < proj.targetSurfaceCount && i < Projector::MAX_TARGETS; ++i) {
                entt::entity te = proj.targetSurfaces[i];
                if (te != entt::null && registry.valid(te) && registry.all_of<Screen>(te)) {
                    targetNames.push_back(registry.get<Screen>(te).name);
                }
            }
            pj["targetSurfaceNames"] = targetNames;

            // Calibration points — the load-bearing payload. Each entry
            // carries enough to re-run the solver from scratch.
            json cpsJson = json::array();
            for (const auto& cp : proj.calibrationPoints) {
                json cpj;
                cpj["worldPos"]    = { cp.worldPos[0], cp.worldPos[1], cp.worldPos[2] };
                cpj["projectorUV"] = { cp.projectorUV[0], cp.projectorUV[1] };
                cpj["isAligned"]   = cp.isAligned;
                // sourceScreen → Screen name (same indirection targetSurfaces uses).
                // Empty string = legacy / untagged point.
                std::string sourceScreenName;
                if (cp.sourceScreen != entt::null &&
                    registry.valid(cp.sourceScreen) &&
                    registry.all_of<Screen>(cp.sourceScreen)) {
                    sourceScreenName = registry.get<Screen>(cp.sourceScreen).name;
                }
                cpj["sourceScreenName"] = sourceScreenName;
                cpsJson.push_back(cpj);
            }
            pj["calibrationPoints"] = cpsJson;

            projectorsJson.push_back(pj);
        }
        project["projectors"] = projectorsJson;

        // Serialize timeline sections. breakFrame is an integer timeline
        // frame (v24+). The loader migrates pre-v24 microsecond values and
        // pre-Phase-B `{start, end}` regional entries.
        json sectionsJson = json::array();
        for (const auto& sec : timeline.getSections()) {
            json sj;
            sj["breakFrame"]  = sec.breakFrame;
            sj["color"]       = sec.color;
            sj["fadeSeconds"] = sec.fadeSeconds;
            sectionsJson.push_back(sj);
        }
        project["sections"] = sectionsJson;

        // Serialize cue tags (Phase A — numbered timeline markers, v9).
        // v24+: cue position is an integer timeline frame under "frame".
        json cuesJson = json::array();
        for (const auto& cue : timeline.getCueTags()) {
            json cj;
            cj["number"] = cue.number;
            cj["frame"]  = cue.frame;
            cj["label"]  = cue.label;
            cuesJson.push_back(cj);
        }
        project["cues"] = cuesJson;

        // Write-temp-then-rename so a crash mid-save can never destroy the
        // previous good project file. See entity/core/AtomicFile.hpp.
        if (!writeFileAtomic(savePath, project.dump(2), &s_lastError)) {
            return false;
        }

        std::cout << "[ProjectSerializer] Saved project to " << savePath.string() << std::endl;
        return true;

    } catch (const std::exception& e) {
        s_lastError = std::string("Save failed: ") + e.what();
        std::cerr << "[ProjectSerializer] " << s_lastError << std::endl;
        return false;
    }
}

bool ProjectSerializer::load(Timeline& timeline, const std::filesystem::path& filepath,
                              MediaLoadCallback mediaLoadCallback,
                              ProjectManager* projectMgr) {
    try {
        // Check file exists
        if (!std::filesystem::exists(filepath)) {
            s_lastError = "File not found: " + filepath.string();
            return false;
        }

        // Read file
        std::ifstream file(filepath);
        if (!file.is_open()) {
            s_lastError = "Failed to open file for reading: " + filepath.string();
            return false;
        }

        json project = json::parse(file);
        file.close();

        // Validate format
        if (!project.contains("format") || project["format"] != "entity_project") {
            s_lastError = "Invalid project file format";
            return false;
        }

        int version = project.value("version", 0);
        if (version > PROJECT_VERSION) {
            s_lastError = "Project file version " + std::to_string(version) +
                          " is newer than supported version " + std::to_string(PROJECT_VERSION);
            return false;
        }

        // Clear existing timeline
        timeline.clear();
        auto& registry = timeline.getRegistry();

        // Clear project-scoped entity types that we serialize. Without this,
        // reloading (or autosave-then-load) would pile up duplicates.
        // Physical output windows need no pre-release by the caller (issue
        // #76): window slots live in OutputManager's show-thread-owned map,
        // not the OutputDisplay component, so clearing components can't
        // leak them — the show thread's reconciliation sweep destroys the
        // windows once these entities vanish from the post-load snapshot.
        // Destroying the entities entirely (not just the component) because
        // these are currently "component-per-entity" with no other components
        // attached worth preserving.
        {
            std::vector<entt::entity> toDestroy;
            auto clearView = registry.view<OutputSurface>();
            for (auto e : clearView) toDestroy.push_back(e);
            auto clearOutView = registry.view<OutputDisplay>();
            for (auto e : clearOutView) toDestroy.push_back(e);
            auto clearProjView = registry.view<Projector>();
            for (auto e : clearProjView) toDestroy.push_back(e);
            for (auto e : toDestroy) {
                if (registry.valid(e)) registry.destroy(e);
            }
        }

        // Load Models. Match existing Models by name and update in place
        // (so we don't leak GPU resources tied to stale entities); create
        // new entities for saved models with no existing match; destroy
        // existing models whose names don't appear in the saved set.
        //
        // Only touch Models when the project JSON contains a "models" key —
        // v1 projects predate this and rely on the default Model/Screen
        // created by Engine::createDefaultScreen at init.
        if (project.contains("models")) {
            std::unordered_set<std::string> savedModelNames;
            for (const auto& mj : project["models"]) {
                savedModelNames.insert(mj.value("name", ""));
            }

            std::vector<entt::entity> toDestroy;
            auto existing = registry.view<Model>();
            for (auto [e, m] : existing.each()) {
                if (savedModelNames.find(m.name) == savedModelNames.end()) {
                    toDestroy.push_back(e);
                }
            }
            for (auto e : toDestroy) {
                if (registry.valid(e)) registry.destroy(e);
            }

            for (const auto& mj : project["models"]) {
                std::string name = mj.value("name", "Model");
                std::string modelFilepath = mj.value("filepath", "");

                entt::entity modelEntity = entt::null;
                auto view = registry.view<Model>();
                for (auto [e, m] : view.each()) {
                    if (m.name == name) { modelEntity = e; break; }
                }

                if (modelEntity == entt::null) {
                    modelEntity = registry.create();
                    auto& model = registry.emplace<Model>(modelEntity);
                    model.name = name;
                    model.filepath = modelFilepath;
                    // Resolve through ProjectManager so Managed (project-
                    // relative) paths land at the project root and the
                    // `_v<tag>` auto-roll picks the latest. Falls back to
                    // the path-as-stored for pre-v18 projects with no
                    // object library and Linked absolute paths.
                    std::string resolved = modelFilepath;
                    if (projectMgr && !modelFilepath.empty()) {
                        resolved = projectMgr->meshPathFor(modelFilepath);
                    }
                    if (!resolved.empty() && std::filesystem::exists(resolved)) {
                        model.mesh = ObjLoader::load(resolved);
                    } else {
                        model.mesh = createDefaultScreenMesh();
                    }
                    std::cout << "[ProjectSerializer] Loaded model: " << name
                              << " (from " << (modelFilepath.empty() ? "built-in plane" : resolved) << ")" << std::endl;
                }
                // Existing matches keep their mesh + GPU handles as-is.
            }
        }

        // Load Screens. Same preserve-by-name strategy as Models — existing
        // Screens that match a saved name keep their renderTargetSlot intact
        // (compose-target slots currently can't be released, so destroying
        // and recreating would leak).
        if (project.contains("screens")) {
            std::unordered_set<std::string> savedScreenNames;
            for (const auto& sj : project["screens"]) {
                savedScreenNames.insert(sj.value("name", ""));
            }

            std::vector<entt::entity> toDestroy;
            auto existing = registry.view<Screen>();
            for (auto [e, s] : existing.each()) {
                if (savedScreenNames.find(s.name) == savedScreenNames.end()) {
                    toDestroy.push_back(e);
                }
            }
            for (auto e : toDestroy) {
                if (registry.valid(e)) registry.destroy(e);
            }

            for (const auto& sj : project["screens"]) {
                std::string name = sj.value("name", "Screen");

                entt::entity screenEntity = entt::null;
                auto view = registry.view<Screen>();
                for (auto [e, s] : view.each()) {
                    if (s.name == name) { screenEntity = e; break; }
                }
                if (screenEntity == entt::null) {
                    screenEntity = registry.create();
                    registry.emplace<Screen>(screenEntity);
                }
                auto& screen = registry.get<Screen>(screenEntity);

                screen.name    = name;
                screen.width   = sj.value("width",  1920u);
                screen.height  = sj.value("height", 1080u);
                screen.visible = sj.value("visible", true);
                screen.opacity = sj.value("opacity", 1.0f);
                screen.zOrder  = sj.value("zOrder",  0);

                if (sj.contains("position") && sj["position"].is_array() && sj["position"].size() >= 3) {
                    screen.position = {sj["position"][0].get<float>(),
                                       sj["position"][1].get<float>(),
                                       sj["position"][2].get<float>()};
                }
                if (sj.contains("rotation") && sj["rotation"].is_array() && sj["rotation"].size() >= 3) {
                    screen.rotation = {sj["rotation"][0].get<float>(),
                                       sj["rotation"][1].get<float>(),
                                       sj["rotation"][2].get<float>()};
                }
                // Size (v14+) is real-world meters. Legacy v≤13 stored "scale"
                // as a unitless multiplier on the mesh's native bounds; we
                // migrate it to "size" = legacyScale × nativeBounds after the
                // model is resolved below, so the visual result matches the
                // pre-v14 render. Either key may be absent on partial files —
                // the component default ({4, 2.25, 0} for screens) wins.
                bool hasLegacyScale = false;
                std::array<float, 3> legacyScale{1.0f, 1.0f, 1.0f};
                if (sj.contains("size") && sj["size"].is_array() && sj["size"].size() >= 3) {
                    screen.size = {sj["size"][0].get<float>(),
                                   sj["size"][1].get<float>(),
                                   sj["size"][2].get<float>()};
                } else if (sj.contains("scale") && sj["scale"].is_array() && sj["scale"].size() >= 3) {
                    legacyScale = {sj["scale"][0].get<float>(),
                                   sj["scale"][1].get<float>(),
                                   sj["scale"][2].get<float>()};
                    hasLegacyScale = true;
                }

                // Resolve modelEntity by Model name. Missing model leaves
                // screen.modelEntity as null — Screen still renders its
                // compose target but has no geometry.
                std::string modelName = sj.value("modelName", "");
                screen.modelEntity = entt::null;
                if (!modelName.empty()) {
                    auto modelViewForLookup = registry.view<Model>();
                    for (auto [me, m] : modelViewForLookup.each()) {
                        if (m.name == modelName) { screen.modelEntity = me; break; }
                    }
                }

                // v13→v14: convert legacy unitless scale into meters using
                // the resolved model's native bounds (or the default 16:9
                // plane fallback for screens with no model).
                if (hasLegacyScale) {
                    const MeshData* mesh = nullptr;
                    if (screen.modelEntity != entt::null &&
                        registry.valid(screen.modelEntity)) {
                        if (const auto* m = registry.try_get<Model>(screen.modelEntity))
                            if (m->mesh.isValid()) mesh = &m->mesh;
                    }
                    const auto bounds = meshNativeBounds(mesh);
                    screen.size = {
                        legacyScale[0] * bounds[0],
                        legacyScale[1] * bounds[1],
                        legacyScale[2] * bounds[2],
                    };
                }

                std::cout << "[ProjectSerializer] Loaded screen: " << name
                          << " (" << screen.width << "x" << screen.height << ")" << std::endl;
            }
        }

        // Load ContentRoutingAsset library (ADR-0022, v20+). Done after
        // Screens are loaded so we can resolve auto-bound + per-target
        // Screen names to entities. Auto-direct assets for Screens not
        // mentioned in the saved library get regenerated on demand by the
        // routing helpers (ensureAutoDirectAsset) when a clip points at
        // them; any remaining unreferenced ones land on the next
        // RoutingLibrarySystem tick.
        //
        // Strategy: walk current ContentRoutingAsset entities, destroy any
        // that won't be re-emplaced from the saved library (avoids stale
        // pre-load assets persisting). Pre-v20 files have no
        // contentRoutingAssets key — the loop is skipped and refs are
        // materialized later from per-clip inline data.
        {
            std::vector<entt::entity> existingAssets;
            for (auto [e, _] : registry.view<ContentRoutingAsset>().each()) {
                existingAssets.push_back(e);
            }
            for (auto e : existingAssets) {
                if (registry.valid(e)) registry.destroy(e);
            }

            if (project.contains("contentRoutingAssets") &&
                project["contentRoutingAssets"].is_array()) {
                auto findScreenByName = [&](const std::string& n) -> entt::entity {
                    if (n.empty()) return entt::null;
                    for (auto [e, s] : registry.view<Screen>().each()) {
                        if (s.name == n) return e;
                    }
                    return entt::null;
                };
                for (const auto& aJson : project["contentRoutingAssets"]) {
                    auto assetEntity = registry.create();
                    auto& asset = registry.emplace<ContentRoutingAsset>(assetEntity);
                    asset.name = aJson.value("name", std::string{});
                    {
                        const std::string kindStr =
                            aJson.value("kind", std::string{"Direct"});
                        if (kindStr == "Tiled")        asset.kind = RouteMode::Tiled;
                        else if (kindStr == "FeedMap") asset.kind = RouteMode::FeedMap;
                        else                           asset.kind = RouteMode::Direct;
                    }
                    asset.tiledCount   = static_cast<std::uint8_t>(
                        aJson.value("tiledCount", 1));
                    asset.tiledAxis    = static_cast<std::uint8_t>(
                        aJson.value("tiledAxis", 0));
                    asset.sourceWidth  = aJson.value("sourceWidth",  1920u);
                    asset.sourceHeight = aJson.value("sourceHeight", 1080u);
                    asset.autoBoundScreen = findScreenByName(
                        aJson.value("autoBoundScreenName", std::string{}));
                    asset.lastSyncedScreenName = aJson.value("lastSyncedScreenName",
                                                              std::string{});
                    if (aJson.contains("targets") && aJson["targets"].is_array()) {
                        for (const auto& tj : aJson["targets"]) {
                            RouteTarget t;
                            t.screen = findScreenByName(tj.value("screenName", std::string{}));
                            t.name = tj.value("name", std::string{});
                            if (tj.contains("uvRect") && tj["uvRect"].is_array() &&
                                tj["uvRect"].size() >= 4) {
                                t.uvRect = {
                                    tj["uvRect"][0].get<float>(),
                                    tj["uvRect"][1].get<float>(),
                                    tj["uvRect"][2].get<float>(),
                                    tj["uvRect"][3].get<float>()
                                };
                            }
                            asset.targets.push_back(t);
                        }
                    }
                }
            }

            // Recreate pristine auto-direct assets the save side skipped.
            // serializeContentRoutingAssets() omits an auto-direct asset that
            // is still pristine (Direct, single target == its bound Screen,
            // identity uvRect, name == lastSyncedScreenName) because it's
            // regenerated from the Screen set. Normally RoutingLibrarySystem
            // recreates them on the next editor tick — but that runs AFTER the
            // per-clip / per-layer contentRoutingAssetName lookups below, so a
            // ref pointing at a pristine asset would fail to resolve and fall
            // back to entt::null ("Default / All Visible"). ensureAutoDirectAsset
            // is idempotent: it no-ops for any Screen whose (possibly
            // customized) auto-direct asset already came back from the library
            // block above (findAutoDirectAsset matches on autoBoundScreen).
            for (auto [screenEntity, screen] : registry.view<Screen>().each()) {
                routing::ensureAutoDirectAsset(registry, screenEntity);
            }
        }

        // Load Props (v13+). Same name-preservation strategy as Screens —
        // existing Props matching a saved name keep their entity; orphans
        // get destroyed; saved entries with no match get fresh entities.
        // Pre-v13 files have no "props" key → no-op (props vector stays as
        // whatever existed in the registry, which on a fresh load is empty
        // since the previous block already culled non-screens implicitly
        // via per-component handling — props never showed up before this
        // version so there's nothing legacy to clean).
        if (project.contains("props")) {
            std::unordered_set<std::string> savedPropNames;
            for (const auto& pj : project["props"]) {
                savedPropNames.insert(pj.value("name", ""));
            }

            std::vector<entt::entity> toDestroy;
            for (auto [e, p] : registry.view<Prop>().each()) {
                if (savedPropNames.find(p.name) == savedPropNames.end()) {
                    toDestroy.push_back(e);
                }
            }
            for (auto e : toDestroy) {
                if (registry.valid(e)) registry.destroy(e);
            }

            for (const auto& pj : project["props"]) {
                std::string name = pj.value("name", "Prop");

                entt::entity propEntity = entt::null;
                for (auto [e, p] : registry.view<Prop>().each()) {
                    if (p.name == name) { propEntity = e; break; }
                }
                if (propEntity == entt::null) {
                    propEntity = registry.create();
                    registry.emplace<Prop>(propEntity);
                }
                auto& prop = registry.get<Prop>(propEntity);

                prop.name    = name;
                prop.visible = pj.value("visible", true);
                prop.opacity = pj.value("opacity", 1.0f);  // v19+; pre-v19 defaults to fully opaque

                if (pj.contains("position") && pj["position"].is_array() && pj["position"].size() >= 3) {
                    prop.position = {pj["position"][0].get<float>(),
                                     pj["position"][1].get<float>(),
                                     pj["position"][2].get<float>()};
                }
                if (pj.contains("rotation") && pj["rotation"].is_array() && pj["rotation"].size() >= 3) {
                    prop.rotation = {pj["rotation"][0].get<float>(),
                                     pj["rotation"][1].get<float>(),
                                     pj["rotation"][2].get<float>()};
                }
                // Size (v14+) is real-world meters; v≤13 stored a unitless
                // multiplier under "scale" — migrate after model resolution.
                bool hasLegacyPropScale = false;
                std::array<float, 3> legacyPropScale{1.0f, 1.0f, 1.0f};
                if (pj.contains("size") && pj["size"].is_array() && pj["size"].size() >= 3) {
                    prop.size = {pj["size"][0].get<float>(),
                                 pj["size"][1].get<float>(),
                                 pj["size"][2].get<float>()};
                } else if (pj.contains("scale") && pj["scale"].is_array() && pj["scale"].size() >= 3) {
                    legacyPropScale = {pj["scale"][0].get<float>(),
                                       pj["scale"][1].get<float>(),
                                       pj["scale"][2].get<float>()};
                    hasLegacyPropScale = true;
                }
                if (pj.contains("displayColor") && pj["displayColor"].is_array()
                    && pj["displayColor"].size() >= 4) {
                    prop.displayColor = {pj["displayColor"][0].get<float>(),
                                         pj["displayColor"][1].get<float>(),
                                         pj["displayColor"][2].get<float>(),
                                         pj["displayColor"][3].get<float>()};
                }

                // Resolve modelEntity by Model name. Missing model leaves
                // prop.modelEntity null — drawProp handles that gracefully
                // (renders an "(no mesh)" placeholder cross at the origin).
                std::string modelName = pj.value("modelName", "");
                prop.modelEntity = entt::null;
                if (!modelName.empty()) {
                    for (auto [me, m] : registry.view<Model>().each()) {
                        if (m.name == modelName) { prop.modelEntity = me; break; }
                    }
                }

                // v13→v14: convert legacy unitless scale into meters using
                // the resolved model's native bounds. Props without a mesh
                // fall back to a 1m³ default (same as the new defaults).
                if (hasLegacyPropScale) {
                    const MeshData* mesh = nullptr;
                    if (prop.modelEntity != entt::null &&
                        registry.valid(prop.modelEntity)) {
                        if (const auto* m = registry.try_get<Model>(prop.modelEntity))
                            if (m->mesh.isValid()) mesh = &m->mesh;
                    }
                    const auto bounds = mesh && mesh->isValid()
                        ? meshNativeBounds(mesh)
                        : std::array<float, 3>{1.0f, 1.0f, 1.0f};
                    prop.size = {
                        legacyPropScale[0] * bounds[0],
                        legacyPropScale[1] * bounds[1],
                        legacyPropScale[2] * bounds[2],
                    };
                }

                std::cout << "[ProjectSerializer] Loaded prop: " << name << std::endl;
            }
        }

        // Load timeline settings
        if (project.contains("timeline")) {
            const auto& timelineJson = project["timeline"];
            if (timelineJson.contains("duration")) {
                timeline.setDuration(timelineJson["duration"].get<Timecode>());
            }
            if (timelineJson.contains("framerate")) {
                timeline.setFrameRate(timelineJson["framerate"].get<double>());
            }
            if (timelineJson.contains("currentTime")) {
                timeline.seek(timelineJson["currentTime"].get<Timecode>());
            }
            // v28: global hide-shy toggle (Phase 9). Missing = false (legacy).
            timeline.setHideShyTracks(timelineJson.value("hideShyTracks", false));
        }

        // Media library (v4+). Populated before clips so the per-clip
        // media-load callback can resolve transcoded paths via
        // ProjectManager::decoderPathFor.
        if (projectMgr) {
            if (project.contains("mediaLibrary")) {
                for (const auto& ej : project["mediaLibrary"]) {
                    const std::string original   = ej.value("originalPath",   "");
                    const std::string transcoded = ej.value("transcodedPath", "");
                    const std::string variant    = ej.value("variant",        "");
                    const std::string inputCsOverride = ej.value("inputColorSpaceOverride", "");
                    // ADR-0009 / v7. Missing pathKind = pre-v7 entry =
                    // Linked (absolute path; matches legacy behavior).
                    const std::string pathKindStr = ej.value("pathKind", "linked");
                    const std::string archived   = ej.value("archivedOriginal", "");
                    const std::string origCodec  = ej.value("originalCodec",    "");
                    if (original.empty()) continue;
                    auto& entry = projectMgr->addMediaFile(original);
                    entry.transcodedPath = transcoded;
                    entry.variant        = variant;
                    entry.inputColorSpaceOverride = inputCsOverride;
                    entry.pathKind         = jsonToPathKind(pathKindStr);
                    entry.archivedOriginal = archived;
                    entry.originalCodec    = origCodec;
                    // v12 — probe cache. Pre-v12 files lack these keys;
                    // defaults (size=0, valid=false) force a one-time
                    // re-probe at load that the next save persists.
                    entry.lastProbeSizeBytes = ej.value("lastProbeSizeBytes",
                                                       static_cast<std::int64_t>(0));
                    entry.lastProbeMtimeUnix = ej.value("lastProbeMtimeUnix",
                                                       static_cast<std::int64_t>(0));
                    if (ej.contains("probe")) {
                        const auto& pj = ej["probe"];
                        entry.cachedProbe.valid       = pj.value("valid", false);
                        entry.cachedProbe.isHap       = pj.value("isHap", false);
                        entry.cachedProbe.width       = pj.value("width",
                                                          static_cast<std::uint32_t>(0));
                        entry.cachedProbe.height      = pj.value("height",
                                                          static_cast<std::uint32_t>(0));
                        entry.cachedProbe.framerate   = pj.value("framerate", 0.0);
                        entry.cachedProbe.totalFrames = pj.value("totalFrames",
                                                          static_cast<FrameNumber>(0));
                        entry.cachedProbe.hasAlpha    = pj.value("hasAlpha", false);
                        entry.cachedProbe.sourceCodecName  = pj.value("sourceCodecName",  "");
                        entry.cachedProbe.displayCodecName = pj.value("displayCodecName", "");
                        entry.cachedProbe.tier        = static_cast<CodecTier>(
                                                          pj.value("tier", 0));
                    }
                }
            }
            // v18 — object library. Pre-v18 projects don't carry this
            // key; the library starts empty and any existing Model
            // entities load via their stored filepath as Linked.
            if (project.contains("objectLibrary")) {
                for (const auto& ej : project["objectLibrary"]) {
                    const std::string original = ej.value("originalPath", "");
                    if (original.empty()) continue;
                    const std::string pathKindStr = ej.value("pathKind", "linked");
                    auto& entry = projectMgr->addObjectFile(original);
                    entry.pathKind = jsonToPathKind(pathKindStr);
                    entry.lastProbeSizeBytes = ej.value("lastProbeSizeBytes",
                                                       static_cast<std::int64_t>(0));
                    entry.lastProbeMtimeUnix = ej.value("lastProbeMtimeUnix",
                                                       static_cast<std::int64_t>(0));
                }
            }

            // Prefer v5 field. Fall back to v4 autoTranscodeOnImport if present
            // (true → AlwaysTranscode, false → NeverTranscode — matches the
            // boolean semantics the user had set).
            if (project.contains("nonHapImportPolicy")) {
                const int raw = project["nonHapImportPolicy"].get<int>();
                projectMgr->setNonHapImportPolicy(
                    static_cast<ProjectManager::NonHapImportPolicy>(raw));
            } else if (project.contains("autoTranscodeOnImport")) {
                const bool autoOn = project["autoTranscodeOnImport"].get<bool>();
                projectMgr->setNonHapImportPolicy(
                    autoOn ? ProjectManager::NonHapImportPolicy::AlwaysTranscode
                           : ProjectManager::NonHapImportPolicy::NeverTranscode);
            }

            // v22 — per-project DMX channel mapping table (#13 / #59).
            // Missing on pre-v22 files -> stays empty -> plugin uses
            // baked defaults.
            if (project.contains("dmxMappingsJson") &&
                project["dmxMappingsJson"].is_string()) {
                projectMgr->setDmxMappingsJson(
                    project["dmxMappingsJson"].get<std::string>());
            }

            // Freeform — per-project OSC mapping tables. Missing on
            // older files -> stays empty -> plugin uses hardcoded defaults.
            if (project.contains("oscInboundMappingsJson") &&
                project["oscInboundMappingsJson"].is_string()) {
                projectMgr->setOscInboundMappingsJson(
                    project["oscInboundMappingsJson"].get<std::string>());
            }
            if (project.contains("oscOutboundMappingsJson") &&
                project["oscOutboundMappingsJson"].is_string()) {
                projectMgr->setOscOutboundMappingsJson(
                    project["oscOutboundMappingsJson"].get<std::string>());
            }

            // v23 — audio master state. Missing on older files -> stays at
            // default (gain=1.0, mute=false) which matches AudioEngine default.
            if (project.contains("audioMasterGain") && project["audioMasterGain"].is_number()) {
                projectMgr->setAudioMasterGain(project["audioMasterGain"].get<float>());
            }
            if (project.contains("audioMasterMute") && project["audioMasterMute"].is_boolean()) {
                projectMgr->setAudioMasterMute(project["audioMasterMute"].get<bool>());
            }

            // v28 (additive) — editor layout embed. Store the sub-object back
            // as an opaque blob string for Engine to apply to WindowManager. Missing
            // on legacy files -> clear so a stale layout from a previously
            // loaded project doesn't bleed into this one.
            if (project.contains("editorLayout") && project["editorLayout"].is_object()) {
                projectMgr->setEditorLayoutJson(project["editorLayout"].dump());
            } else {
                projectMgr->setEditorLayoutJson("");
            }
        }

        // Load tracks
        if (project.contains("tracks")) {
            for (const auto& trackJson : project["tracks"]) {
                // Create track
                std::string trackName = "Track " + std::to_string(trackJson.value("index", 0));
                entt::entity trackEntity = timeline.createTrack(trackName);
                auto* track = registry.try_get<TimelineTrack>(trackEntity);
                if (!track) continue;

                // v28: restore TrackUiState (name + shy). Legacy files won't
                // have these keys — absence means name="" (falls back to
                // "Track N" in the renderer) and shy=false.
                if (trackJson.contains("name") || trackJson.contains("shy")) {
                    TrackUiState uiState;
                    uiState.name = trackJson.value("name", std::string{});
                    uiState.shy  = trackJson.value("shy", false);
                    registry.emplace_or_replace<TrackUiState>(trackEntity, std::move(uiState));
                }

                // Accept "layers" (v15) or "clips" (legacy v14). When reading
                // legacy "clips", every entry is treated as kind="clip".
                bool isLegacyClipsKey = false;
                const json* entriesPtr = nullptr;
                if (trackJson.contains("layers")) {
                    entriesPtr = &trackJson["layers"];
                } else if (trackJson.contains("clips")) {
                    entriesPtr = &trackJson["clips"];
                    isLegacyClipsKey = true;
                }

                if (entriesPtr) {
                    for (const auto& entryJson : *entriesPtr) {
                        // Determine kind. Legacy "clips[]" entries are always Clip.
                        const std::string kind = isLegacyClipsKey
                            ? "clip"
                            : entryJson.value("kind", "clip");

                        if (kind == "object_animation") {
                            // --- ObjectAnimationLayer ---
                            entt::entity layerEntity = registry.create();

                            auto& lay = registry.emplace<Layer>(layerEntity);
                            lay.kind       = Layer::Kind::ObjectAnimation;
                            lay.startFrame = entryJson.value("startFrame", static_cast<FrameNumber>(0));
                            lay.duration   = entryJson.value("duration",   static_cast<FrameNumber>(0));
                            lay.name       = entryJson.value("name", std::string{});
                            if (auto* tt = registry.try_get<TimelineTrack>(trackEntity)) {
                                lay.trackIndex = tt->trackIndex;
                            }
                            if (entryJson.contains("color") && entryJson["color"].is_array()
                                && entryJson["color"].size() >= 4) {
                                lay.color = { entryJson["color"][0].get<float>(),
                                              entryJson["color"][1].get<float>(),
                                              entryJson["color"][2].get<float>(),
                                              entryJson["color"][3].get<float>() };
                            }

                            auto& oal = registry.emplace<ObjectAnimationLayer>(layerEntity);
                            {
                                const auto sb = entryJson.value("sectionBehavior",
                                                                std::string{"Normal"});
                                oal.sectionBehavior = (sb == "Locked")
                                    ? SectionBehavior::Locked : SectionBehavior::Normal;
                            }
                            {
                                // ADR-0020 / v17. Pre-v17 projects without the
                                // key load with the new Hold default — the
                                // explicit choice for older projects is the
                                // show-friendly behavior, not legacy snap-back.
                                const auto eb = entryJson.value("endBehavior",
                                                                std::string{"Hold"});
                                oal.endBehavior = (eb == "Reset")
                                    ? ObjectAnimationLayer::EndBehavior::Reset
                                    : ObjectAnimationLayer::EndBehavior::Hold;
                            }

                            // Resolve target by name. Try Screen first, then
                            // Prop. Missing / stale name leaves target null —
                            // the user can re-assign in the Properties panel.
                            const std::string targetName = entryJson.value("targetName", "");
                            oal.target = entt::null;
                            if (!targetName.empty()) {
                                for (auto [se, s] : registry.view<Screen>().each()) {
                                    if (s.name == targetName) { oal.target = se; break; }
                                }
                                if (oal.target == entt::null) {
                                    for (auto [pe, p] : registry.view<Prop>().each()) {
                                        if (p.name == targetName) { oal.target = pe; break; }
                                    }
                                }
                            }

                            // AnimatedProperties (absent = no keyframes yet)
                            if (entryJson.contains("animatedProperties")
                                && entryJson["animatedProperties"].is_array()
                                && !entryJson["animatedProperties"].empty()) {
                                auto& ap = registry.emplace<AnimatedProperties>(layerEntity);
                                deserializeAnimatedProperties(entryJson, ap);
                            }

                            // v25 — twirl-down collapsed group paths.
                            applyCollapsedGroupsJson(registry, layerEntity, entryJson);

                            track->layers.push_back(layerEntity);
                            std::cout << "[ProjectSerializer] Loaded OA layer: "
                                      << lay.name << std::endl;
                            continue;
                        }

                        // --- kind: "generative" (v21+) ---
                        if (kind == "generative") {
                            entt::entity layerEntity = registry.create();

                            auto& lay = registry.emplace<Layer>(layerEntity);
                            lay.kind       = Layer::Kind::Generative;
                            lay.startFrame = entryJson.value("startFrame", static_cast<FrameNumber>(0));
                            lay.duration   = entryJson.value("duration",   static_cast<FrameNumber>(0));
                            lay.name       = entryJson.value("name", std::string{});
                            if (track) lay.trackIndex = track->trackIndex;
                            if (entryJson.contains("color") && entryJson["color"].is_array()
                                && entryJson["color"].size() >= 4) {
                                lay.color = { entryJson["color"][0].get<float>(),
                                              entryJson["color"][1].get<float>(),
                                              entryJson["color"][2].get<float>(),
                                              entryJson["color"][3].get<float>() };
                            }

                            auto& gen = registry.emplace<GenerativeLayer>(layerEntity);
                            gen.renderWidth  = entryJson.value("renderWidth",  static_cast<uint32_t>(1920));
                            gen.renderHeight = entryJson.value("renderHeight", static_cast<uint32_t>(1080));

                            const std::string tgtName = entryJson.value("targetScreenName", "");
                            gen.targetScreen = entt::null;
                            if (!tgtName.empty()) {
                                for (auto [se, s] : registry.view<Screen>().each()) {
                                    if (s.name == tgtName) { gen.targetScreen = se; break; }
                                }
                            }

                            // ContentRoutingRef (v20+). Same fall-through as the
                            // clip path: a named lookup that fails (e.g. a
                            // pristine auto-direct asset whose name diverged from
                            // its Screen) must not silently leave the layer
                            // unrouted — bind it to the target Screen's
                            // auto-direct asset so the operator's pick survives
                            // reload instead of reverting to "All Visible".
                            bool routingRefBound = false;
                            if (entryJson.contains("contentRoutingAssetName")) {
                                const std::string assetName =
                                    entryJson.value("contentRoutingAssetName", "");
                                if (!assetName.empty()) {
                                    for (auto [ae, a] : registry.view<ContentRoutingAsset>().each()) {
                                        if (a.name == assetName) {
                                            registry.emplace<ContentRoutingRef>(layerEntity).asset = ae;
                                            routingRefBound = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (!routingRefBound && gen.targetScreen != entt::null) {
                                routing::setLayerTargetScreen(registry, layerEntity,
                                                              gen.targetScreen);
                            }

                            registry.emplace<Transform>(layerEntity);
                            if (entryJson.contains("transform")) {
                                auto& t = registry.get<Transform>(layerEntity);
                                const auto& tj = entryJson["transform"];
                                if (tj.contains("position") && tj["position"].size() >= 3)
                                    t.setPosition({ tj["position"][0], tj["position"][1], tj["position"][2] });
                                if (tj.contains("rotation") && tj["rotation"].size() >= 3)
                                    t.setRotation({ tj["rotation"][0], tj["rotation"][1], tj["rotation"][2] });
                                if (tj.contains("scale") && tj["scale"].size() >= 3)
                                    t.setScale({ tj["scale"][0], tj["scale"][1], tj["scale"][2] });
                                // Absent == locked (the pre-component default).
                                registry.emplace_or_replace<ScaleLock>(
                                    layerEntity, ScaleLock{tj.value("scaleLock", true)});
                            }

                            auto& ml = registry.emplace<MediaLayer>(layerEntity);
                            if (entryJson.contains("layer")) {
                                const auto& lj = entryJson["layer"];
                                ml.zOrder    = lj.value("zOrder",    static_cast<uint32_t>(0));
                                ml.opacity   = lj.value("opacity",   1.0f);
                                ml.blendMode = jsonToBlendMode(lj.value("blendMode", std::string{"Normal"}));
                                ml.visible   = lj.value("visible",   true);
                            }

                            const std::string subKind = entryJson.value("sub_kind", "muncher");
                            if (subKind == "text") {
                                auto& tls = registry.emplace<TextLayerState>(layerEntity);
                                if (entryJson.contains("text_state")) {
                                    const auto& ts = entryJson["text_state"];
                                    tls.text       = ts.value("text",       std::string{"Text"});
                                    tls.fontFamily = ts.value("fontFamily", std::string{"Segoe UI"});
                                    tls.fontSize   = ts.value("fontSize",   96.0f);
                                    if (ts.contains("color") && ts["color"].is_array()
                                        && ts["color"].size() >= 4) {
                                        tls.color = { ts["color"][0].get<float>(),
                                                      ts["color"][1].get<float>(),
                                                      ts["color"][2].get<float>(),
                                                      ts["color"][3].get<float>() };
                                    }
                                    tls.alignment = static_cast<TextAlignment>(ts.value("alignment", 1));
                                    tls.bold      = ts.value("bold",   false);
                                    tls.italic    = ts.value("italic", false);
                                }
                                tls.dirty = true;  // force re-bake on first tick
                            } else {
                                // Muncher or unrecognised sub-kind — emplace Muncher state
                                // (game state resets each session; no persistent fields)
                                registry.emplace<MunchersGameState>(layerEntity);
                            }

                            // AnimatedProperties keyframes (absent = no keyframes).
                            if (entryJson.contains("animatedProperties")
                                && entryJson["animatedProperties"].is_array()
                                && !entryJson["animatedProperties"].empty()) {
                                auto& ap = registry.emplace<AnimatedProperties>(layerEntity);
                                deserializeAnimatedProperties(entryJson, ap);
                            }

                            // v25 — twirl-down collapsed group paths.
                            applyCollapsedGroupsJson(registry, layerEntity, entryJson);

                            // v27 — RemotePatch. storeSlot stays -1 here;
                            // Engine::bindRemotePatchesAfterLoad() rebinds.
                            if (entryJson.contains("remotePatch") &&
                                entryJson["remotePatch"].is_object()) {
                                const auto& rj = entryJson["remotePatch"];
                                auto& rp = registry.emplace_or_replace<RemotePatch>(layerEntity);
                                rp.patchId        = rj.value("id", std::string{});
                                rp.armedByDefault = rj.value("armed", false);
                                rp.storeSlot      = -1;
                            }

                            track->layers.push_back(layerEntity);
                            std::cout << "[ProjectSerializer] Loaded generative layer ("
                                      << subKind << "): " << lay.name << std::endl;
                            continue;
                        }

                        // --- kind: "signal" (v26+) ---
                        if (kind == "signal") {
                            entt::entity layerEntity = registry.create();

                            auto& lay = registry.emplace<Layer>(layerEntity);
                            lay.kind       = Layer::Kind::Signal;
                            lay.startFrame = entryJson.value("startFrame", static_cast<FrameNumber>(0));
                            lay.duration   = entryJson.value("duration",   static_cast<FrameNumber>(1));
                            lay.name       = entryJson.value("name", std::string{});
                            if (auto* tt = registry.try_get<TimelineTrack>(trackEntity))
                                lay.trackIndex = tt->trackIndex;
                            if (entryJson.contains("color") && entryJson["color"].is_array()
                                && entryJson["color"].size() >= 4) {
                                lay.color = { entryJson["color"][0].get<float>(),
                                              entryJson["color"][1].get<float>(),
                                              entryJson["color"][2].get<float>(),
                                              entryJson["color"][3].get<float>() };
                            }

                            auto& sig = registry.emplace<SignalLayer>(layerEntity);
                            sig.mode        = static_cast<SignalLayer::Mode>(
                                entryJson.value("mode", 0));
                            sig.transport   = static_cast<SignalLayer::Transport>(
                                entryJson.value("transport", 0));
                            sig.valueSource = static_cast<SignalLayer::ValueSource>(
                                entryJson.value("valueSource", 0));
                            sig.address     = entryJson.value("address", std::string{"/entity/signal"});
                            sig.srcMin      = entryJson.value("srcMin", 0.0f);
                            sig.srcMax      = entryJson.value("srcMax", 1.0f);
                            sig.outMin      = entryJson.value("outMin", 0.0f);
                            sig.outMax      = entryJson.value("outMax", 1.0f);
                            sig.outType     = static_cast<SignalLayer::Arg::Type>(
                                entryJson.value("outType", 0));

                            if (entryJson.contains("args") && entryJson["args"].is_array()) {
                                for (const auto& aj : entryJson["args"]) {
                                    SignalLayer::Arg a;
                                    a.type = static_cast<SignalLayer::Arg::Type>(
                                        aj.value("type", 0));
                                    a.i    = aj.value("i", std::int32_t{0});
                                    a.f    = aj.value("f", 0.0f);
                                    a.s    = aj.value("s", std::string{});
                                    a.isValueBound = aj.value("isValueBound", false);
                                    sig.args.push_back(a);
                                }
                            }

                            // AnimatedProperties (Keyframe value source).
                            if (entryJson.contains("animatedProperties")
                                && entryJson["animatedProperties"].is_array()
                                && !entryJson["animatedProperties"].empty()) {
                                auto& ap = registry.emplace<AnimatedProperties>(layerEntity);
                                deserializeAnimatedProperties(entryJson, ap);
                            } else {
                                registry.emplace<AnimatedProperties>(layerEntity);
                            }

                            track->layers.push_back(layerEntity);
                            std::cout << "[ProjectSerializer] Loaded signal layer: "
                                      << lay.name << std::endl;
                            continue;
                        }

                        // --- kind: "clip" ---
                        if (kind != "clip") {
                            std::cerr << "[ProjectSerializer] Unknown layer kind '"
                                      << kind << "' — skipping" << std::endl;
                            continue;
                        }
                        // Alias entryJson as clipJson for readability.
                        const json& clipJson = entryJson;

                        // Create clip entity
                        entt::entity clipEntity = registry.create();

                        // Create Clip component
                        auto& clip = registry.emplace<Clip>(clipEntity);
                        clip.filepath = clipJson.value("filepath", "");
                        clip.mediaType = jsonToMediaType(clipJson.value("mediaType", "unknown"));
                        clip.startFrame = clipJson.value("startFrame", 0);
                        clip.mediaStartFrame = clipJson.value("mediaStartFrame", 0);
                        clip.playbackMode = static_cast<PlaybackMode>(clipJson.value("playbackMode", 0));  // Default to Freeze
                        {
                            const auto sb = clipJson.value("sectionBehavior", std::string{"Normal"});
                            clip.sectionBehavior = (sb == "Locked") ? SectionBehavior::Locked : SectionBehavior::Normal;
                        }
                        clip.framerate = clipJson.value("framerate", 30.0);
                        clip.width = clipJson.value("width", 0);
                        clip.height = clipJson.value("height", 0);
                        clip.hasAlpha = clipJson.value("hasAlpha", false);
                        clip.frameBlending = clipJson.value("frameBlending", false);
                        clip.loaded = false;
                        clip.decoding = false;

                        // Resolve targetScreen by name. Empty/missing = null
                        // (renders to all screens). Legacy alias kept in sync
                        // with the new ContentRouting component for one
                        // project-format version (ADR-0021 § Backward
                        // compatibility).
                        std::string targetScreenName = clipJson.value("targetScreenName", "");
                        clip.targetScreen = entt::null;
                        if (!targetScreenName.empty()) {
                            auto screenView = registry.view<Screen>();
                            for (auto [se, s] : screenView.each()) {
                                if (s.name == targetScreenName) {
                                    clip.targetScreen = se;
                                    break;
                                }
                            }
                        }

                        // Plane A content routing (ADR-0021). Prefer the new
                        // `contentRouting` JSON object when present; fall back
                        // to migrating from the legacy `targetScreenName`
                        // alias otherwise. Mode names that aren't recognized
                        // (e.g. a future Cylindrical from a forward-rolled
                        // project) clamp to Direct.
                        auto& cr = registry.emplace<ContentRouting>(clipEntity);
                        cr.mode = RouteMode::Direct;
                        if (clipJson.contains("contentRouting")
                            && clipJson["contentRouting"].is_object()) {
                            const auto& crJson = clipJson["contentRouting"];
                            const std::string modeStr = crJson.value("mode", std::string{"Direct"});
                            cr.mode = (modeStr == "Tiled") ? RouteMode::Tiled : RouteMode::Direct;
                            if (crJson.contains("targets") && crJson["targets"].is_array()) {
                                auto screenView = registry.view<Screen>();
                                for (const auto& tJson : crJson["targets"]) {
                                    RouteTarget t;
                                    const std::string screenName = tJson.value("screenName", std::string{});
                                    if (!screenName.empty()) {
                                        for (auto [se, s] : screenView.each()) {
                                            if (s.name == screenName) {
                                                t.screen = se;
                                                break;
                                            }
                                        }
                                    }
                                    if (tJson.contains("uvRect") && tJson["uvRect"].is_array()) {
                                        const auto& uv = tJson["uvRect"];
                                        for (std::size_t i = 0; i < t.uvRect.size() && i < uv.size(); ++i)
                                            t.uvRect[i] = uv[i].get<float>();
                                    }
                                    cr.targets.push_back(t);
                                }
                            }
                            // Keep the legacy alias in sync so any code that
                            // hasn't yet migrated reads a coherent value.
                            clip.targetScreen = (cr.targets.size() == 1)
                                ? cr.targets[0].screen
                                : entt::null;
                        } else if (clip.targetScreen != entt::null) {
                            // Legacy migration: derive ContentRouting from
                            // the single-screen alias.
                            RouteTarget t;
                            t.screen = clip.targetScreen;
                            cr.targets.push_back(t);
                        }

                        // Plane A library reference (ADR-0022, v20+). v20
                        // saves point at a named ContentRoutingAsset; older
                        // files migrate from the inline `cr` populated
                        // above. The library was loaded earlier in this
                        // function so name lookups resolve now.
                        bool routingRefBound = false;
                        if (clipJson.contains("contentRoutingAssetName")) {
                            const std::string assetName = clipJson.value(
                                "contentRoutingAssetName", std::string{});
                            entt::entity foundAsset = entt::null;
                            for (auto [ae, a] :
                                 registry.view<ContentRoutingAsset>().each()) {
                                if (a.name == assetName) { foundAsset = ae; break; }
                            }
                            if (foundAsset != entt::null) {
                                auto& ref = registry.emplace<ContentRoutingRef>(clipEntity);
                                ref.asset = foundAsset;
                                routingRefBound = true;
                            }
                            // else: asset genuinely missing (renamed Screen,
                            // hand-edited file) — fall through to the inline
                            // contentRouting migration below.
                        }
                        if (!routingRefBound) {
                            if (cr.mode == RouteMode::Direct &&
                                cr.targets.size() == 1 &&
                                cr.targets[0].screen != entt::null) {
                                routing::setLayerTargetScreen(registry, clipEntity,
                                                                cr.targets[0].screen);
                            } else if (!cr.targets.empty()) {
                                std::vector<RouteTarget> copy = cr.targets;
                                routing::setLayerCustomRouting(registry, clipEntity,
                                                                cr.mode, std::move(copy));
                            } else {
                                auto& ref = registry.emplace<ContentRoutingRef>(clipEntity);
                                ref.asset = entt::null;
                            }
                        }

                        // Load totalMediaFrames and duration
                        // totalMediaFrames is always in source frames
                        FrameNumber jsonDuration = clipJson.value("duration", 0);
                        clip.totalMediaFrames = clipJson.value("totalMediaFrames", jsonDuration);

                        // mediaOutFrame migration. When the JSON key exists
                        // (post-decoupling saves), trust it. When it's
                        // missing (legacy projects), derive an INCLUSIVE
                        // out-point (last frame played) so playback matches
                        // the old implicit boundary exactly:
                        //   mediaOutFrame = mediaStartFrame + duration * (sourceFps / timelineFps) - 1
                        // clamped to [mediaStartFrame, totalMediaFrames - 1].
                        // The -1 makes the result inclusive; the old
                        // wrap-math boundary `sourceLength = totalMediaFrames`
                        // exactly equals `mediaOutFrame - mediaStartFrame + 1`
                        // when mediaOutFrame = totalMediaFrames - 1 and
                        // mediaStartFrame = 0, so unmodified imports load
                        // identically.
                        if (clipJson.contains("mediaOutFrame")) {
                            clip.mediaOutFrame = clipJson.value("mediaOutFrame", static_cast<FrameNumber>(-1));
                        } else if (clip.totalMediaFrames > 0 && clip.framerate > 0) {
                            const double timelineFrameRate = timeline.getFrameRate() > 0.0
                                ? timeline.getFrameRate() : 30.0;
                            const double frameRateRatio = clip.framerate / timelineFrameRate;
                            const FrameNumber derived = clip.mediaStartFrame +
                                static_cast<FrameNumber>(std::floor(jsonDuration * frameRateRatio)) - 1;
                            clip.mediaOutFrame = std::min(derived, clip.totalMediaFrames - 1);
                            if (clip.mediaOutFrame < clip.mediaStartFrame) {
                                clip.mediaOutFrame = clip.totalMediaFrames - 1;
                            }
                        } else {
                            clip.mediaOutFrame = clip.totalMediaFrames > 0
                                ? clip.totalMediaFrames - 1 : -1;
                        }

                        // Trust the persisted duration (timeline frames).
                        // The previous unconditional recalc-from-totalMediaFrames
                        // was a one-time migration for projects saved before the
                        // mixed-frame-rate fix; in practice it has been silently
                        // destroying user-edited durations on every save/load
                        // cycle (worst case: stills, which always reverted to 1
                        // timeline frame). Very old source-frame durations may
                        // now load slightly short — acceptable trade vs.
                        // breaking every user trim.
                        if (jsonDuration > 0) {
                            clip.duration = jsonDuration;
                        } else if (clip.totalMediaFrames > 0 && clip.framerate > 0) {
                            double timelineFrameRate = timeline.getFrameRate();
                            clip.duration = static_cast<FrameNumber>(std::ceil(
                                clip.totalMediaFrames * (timelineFrameRate / clip.framerate)));
                        } else {
                            clip.duration = jsonDuration;  // 0 — malformed clip.
                        }

                        // Load Transform if present
                        if (clipJson.contains("transform")) {
                            const auto& transformJson = clipJson["transform"];
                            auto& transform = registry.emplace<Transform>(clipEntity);

                            if (transformJson.contains("position") && transformJson["position"].is_array()) {
                                transform.position = glm::vec3(
                                    transformJson["position"][0].get<float>(),
                                    transformJson["position"][1].get<float>(),
                                    transformJson["position"][2].get<float>()
                                );
                            }
                            if (transformJson.contains("rotation") && transformJson["rotation"].is_array()) {
                                transform.rotation = glm::vec3(
                                    transformJson["rotation"][0].get<float>(),
                                    transformJson["rotation"][1].get<float>(),
                                    transformJson["rotation"][2].get<float>()
                                );
                            }
                            if (transformJson.contains("scale") && transformJson["scale"].is_array()) {
                                transform.scale = glm::vec3(
                                    transformJson["scale"][0].get<float>(),
                                    transformJson["scale"][1].get<float>(),
                                    transformJson["scale"][2].get<float>()
                                );
                            }
                            transform.dirty = true;
                            // Absent == locked (the pre-component default).
                            registry.emplace_or_replace<ScaleLock>(
                                clipEntity, ScaleLock{transformJson.value("scaleLock", true)});
                        }

                        // Load MediaLayer if present
                        if (clipJson.contains("layer")) {
                            const auto& layerJson = clipJson["layer"];
                            auto& layer = registry.emplace<MediaLayer>(clipEntity);
                            layer.zOrder = layerJson.value("zOrder", 0);
                            layer.opacity = layerJson.value("opacity", 1.0f);
                            layer.blendMode = jsonToBlendMode(layerJson.value("blendMode", "normal"));
                            layer.visible = layerJson.value("visible", true);
                        }

                        // Attach Layer component. v14 legacy "clips[]" entries
                        // default to Kind::Clip. v15 "layers[]" always emits
                        // "kind"; we still read it tolerantly for forward compat.
                        {
                            auto& lay = registry.emplace<Layer>(clipEntity);
                            lay.kind       = Layer::Kind::Clip;
                            lay.startFrame = clip.startFrame;
                            lay.duration   = clip.duration;
                            if (auto* tt = registry.try_get<TimelineTrack>(trackEntity)) {
                                lay.trackIndex = tt->trackIndex;
                            }
                            // only 'clip' entries reach this branch — OA, generative,
                            // and future kinds are all caught above.
                        }

                        // Restore AnimatedProperties keyframes (v15+). Pre-v15
                        // clip entries have an empty array; pre-v14 entries
                        // lack the key entirely — both are no-ops here.
                        if (clipJson.contains("animatedProperties")
                            && clipJson["animatedProperties"].is_array()
                            && !clipJson["animatedProperties"].empty()) {
                            auto& ap = registry.emplace_or_replace<AnimatedProperties>(clipEntity);
                            deserializeAnimatedProperties(clipJson, ap);
                        }

                        // Restore effect chain (v16+, issue #54). Pre-v16
                        // clips have no "effects" key — no-op.
                        deserializeEffectChain(clipJson, registry, clipEntity);

                        // v23 — AudioSource gain/mute/solo. Only user-authored
                        // fields; hasAudioStream and source* are re-derived when
                        // the decode worker opens the file.
                        if (clipJson.contains("audio") && clipJson["audio"].is_object()) {
                            const auto& aj = clipJson["audio"];
                            auto& as = registry.emplace_or_replace<AudioSource>(clipEntity);
                            as.gain = aj.value("gain", 1.0f);
                            as.mute = aj.value("mute", false);
                            as.solo = aj.value("solo", false);
                        }

                        // v27 — RemotePatch. storeSlot stays -1 here;
                        // Engine::bindRemotePatchesAfterLoad() rebinds all
                        // patches against the RemoteControlStore after load.
                        if (clipJson.contains("remotePatch") &&
                            clipJson["remotePatch"].is_object()) {
                            const auto& rj = clipJson["remotePatch"];
                            auto& rp = registry.emplace_or_replace<RemotePatch>(clipEntity);
                            rp.patchId        = rj.value("id", std::string{});
                            rp.armedByDefault = rj.value("armed", false);
                            rp.storeSlot      = -1;
                        }

                        // v25 — twirl-down collapsed group paths.
                        applyCollapsedGroupsJson(registry, clipEntity, clipJson);

                        // Add layer to track
                        track->layers.push_back(clipEntity);

                        // Trigger media load callback if provided
                        if (mediaLoadCallback && !clip.filepath.empty()) {
                            mediaLoadCallback(clipEntity, clip.filepath);
                        }
                    }
                }
            }
        }

        // Load mapping surfaces
        if (project.contains("mappingSurfaces")) {
            for (const auto& surfaceJson : project["mappingSurfaces"]) {
                entt::entity surfaceEntity = registry.create();
                auto& surface = registry.emplace<OutputSurface>(surfaceEntity);

                surface.name = surfaceJson.value("name", "Surface");
                surface.surfaceIndex = surfaceJson.value("surfaceIndex", 0);
                surface.visible = surfaceJson.value("visible", true);
                surface.brightness = surfaceJson.value("brightness", 1.0f);
                surface.gamma = surfaceJson.value("gamma", 1.0f);
                surface.gridSubdivisions = surfaceJson.value("gridSubdivisions", 1);
                surface.outputIndex = surfaceJson.value("outputIndex", 0);

                // Load corners
                if (surfaceJson.contains("corners") && surfaceJson["corners"].is_array()) {
                    const auto& cornersJson = surfaceJson["corners"];
                    for (int i = 0; i < 4 && i < static_cast<int>(cornersJson.size()); ++i) {
                        if (cornersJson[i].is_array() && cornersJson[i].size() >= 2) {
                            surface.corners[i].x = cornersJson[i][0].get<float>();
                            surface.corners[i].y = cornersJson[i][1].get<float>();
                        }
                    }
                }

                // Load source UVs
                if (surfaceJson.contains("sourceUVs") && surfaceJson["sourceUVs"].is_array()) {
                    const auto& uvsJson = surfaceJson["sourceUVs"];
                    for (int i = 0; i < 4 && i < static_cast<int>(uvsJson.size()); ++i) {
                        if (uvsJson[i].is_array() && uvsJson[i].size() >= 2) {
                            surface.sourceUVs[i].x = uvsJson[i][0].get<float>();
                            surface.sourceUVs[i].y = uvsJson[i][1].get<float>();
                        }
                    }
                }

                // Load soft edges
                if (surfaceJson.contains("softEdge")) {
                    const auto& edgeJson = surfaceJson["softEdge"];
                    surface.softEdge.left = edgeJson.value("left", 0.0f);
                    surface.softEdge.right = edgeJson.value("right", 0.0f);
                    surface.softEdge.top = edgeJson.value("top", 0.0f);
                    surface.softEdge.bottom = edgeJson.value("bottom", 0.0f);
                }

                std::cout << "[ProjectSerializer] Loaded mapping surface: " << surface.name << std::endl;
            }
        }

        // Load output displays
        if (project.contains("outputs")) {
            for (const auto& oj : project["outputs"]) {
                entt::entity outEntity = registry.create();
                auto& out = registry.emplace<OutputDisplay>(outEntity);

                out.name = oj.value("name", "Output");
                out.outputIndex = oj.value("outputIndex", 0u);
                int typeInt = oj.value("type", 0);
                out.type = static_cast<OutputType>(typeInt);
                out.enabled = oj.value("enabled", false);
                out.width = oj.value("width", 1920);
                out.height = oj.value("height", 1080);
                out.refreshRate = oj.value("refreshRate", 60.0f);
                out.physicalDisplayIndex = oj.value("physicalDisplayIndex", -1);
                out.deviceName = oj.value("deviceName", "");
                out.displayName = oj.value("displayName", "");
                out.brightness = oj.value("brightness", 1.0f);
                out.gamma = oj.value("gamma", 1.0f);
                out.ocioDisplay = oj.value("ocioDisplay", std::string{});
                out.ocioView = oj.value("ocioView", std::string{});
                out.fullscreen = oj.value("fullscreen", false);
                out.windowX = oj.value("windowX", 100);
                out.windowY = oj.value("windowY", 100);
                out.windowWidth = oj.value("windowWidth", 1280);
                out.windowHeight = oj.value("windowHeight", 720);

                if (oj.contains("inputRegion")) {
                    const auto& ir = oj["inputRegion"];
                    out.inputRegion.x = ir.value("x", 0.0f);
                    out.inputRegion.y = ir.value("y", 0.0f);
                    out.inputRegion.width = ir.value("width", 1.0f);
                    out.inputRegion.height = ir.value("height", 1.0f);
                    out.inputRegion.updatePixelCoords(out.width, out.height);
                }

                // Runtime-only fields: not persisted. Renderer slot gets
                // allocated when Engine::loadProject brings the window back up.
                out.outputWindowSlot = UINT32_MAX;

                // Resolve sourceScreen by name. Screens are loaded earlier
                // in this function, so matching against the registry finds
                // whatever the saved project contained (falls back to the
                // default "Main Screen" for v1 projects).
                std::string sourceName = oj.value("sourceScreenName", "");
                out.sourceScreen = entt::null;
                if (!sourceName.empty()) {
                    auto screenView = registry.view<Screen>();
                    for (auto [se, s] : screenView.each()) {
                        if (s.name == sourceName) {
                            out.sourceScreen = se;
                            break;
                        }
                    }
                }

                std::cout << "[ProjectSerializer] Loaded output: " << out.name
                          << " (index " << out.outputIndex
                          << (out.enabled ? ", enabled" : ", disabled") << ")" << std::endl;
            }
        }

        // Load Projectors (v8). Must run AFTER Screens + OutputDisplays since
        // linkedOutput / targetSurfaces resolve by name against entities the
        // earlier passes created. Missing "projectors" key (v7 and older) =
        // no projectors loaded; user creates fresh ones via ScreensWindow.
        if (project.contains("projectors")) {
            for (const auto& pj : project["projectors"]) {
                entt::entity projEntity = registry.create();
                auto& proj = registry.emplace<Projector>(projEntity);

                proj.name            = pj.value("name", std::string("Projector"));
                proj.fovDegrees      = pj.value("fovDegrees", 50.0f);
                proj.nearClip        = pj.value("nearClip", 0.1f);
                proj.farClip         = pj.value("farClip", 50.0f);
                proj.enabled         = pj.value("enabled", true);
                proj.isCalibrated    = pj.value("isCalibrated", false);
                proj.distortionK1    = pj.value("distortionK1", 0.0f);
                proj.distortionK2    = pj.value("distortionK2", 0.0f);
                proj.useResidualWarp = pj.value("useResidualWarp", false);

                if (pj.contains("position") && pj["position"].is_array() && pj["position"].size() >= 3) {
                    proj.position = { pj["position"][0].get<float>(),
                                      pj["position"][1].get<float>(),
                                      pj["position"][2].get<float>() };
                }
                if (pj.contains("rotation") && pj["rotation"].is_array() && pj["rotation"].size() >= 3) {
                    proj.rotation = { pj["rotation"][0].get<float>(),
                                      pj["rotation"][1].get<float>(),
                                      pj["rotation"][2].get<float>() };
                }

                // Resolve linkedOutput by OutputDisplay name. Missing or stale
                // (output deleted between save and load) leaves linkedOutput
                // null — calibration window's UI flow handles re-linking.
                std::string linkedOutputName = pj.value("linkedOutputName", std::string{});
                proj.linkedOutput = entt::null;
                if (!linkedOutputName.empty()) {
                    auto outView = registry.view<OutputDisplay>();
                    for (auto [oe, o] : outView.each()) {
                        if (o.name == linkedOutputName) { proj.linkedOutput = oe; break; }
                    }
                }

                // Resolve targetSurfaces by Screen name. Names that don't
                // resolve (Screen renamed/deleted) drop silently; final
                // targetSurfaceCount may be less than what was saved. Empty
                // array = "project onto all visible Screens" (the default).
                proj.targetSurfaceCount = 0;
                if (pj.contains("targetSurfaceNames") && pj["targetSurfaceNames"].is_array()) {
                    for (const auto& nameJson : pj["targetSurfaceNames"]) {
                        if (proj.targetSurfaceCount >= Projector::MAX_TARGETS) break;
                        if (!nameJson.is_string()) continue;
                        const std::string name = nameJson.get<std::string>();
                        if (name.empty()) continue;
                        auto screenView = registry.view<Screen>();
                        for (auto [se, s] : screenView.each()) {
                            if (s.name == name) {
                                proj.targetSurfaces[proj.targetSurfaceCount++] = se;
                                break;
                            }
                        }
                    }
                }

                // Calibration points. The user spent time placing each one;
                // they're the load-bearing payload of this whole serializer
                // change.
                proj.calibrationPoints.clear();
                if (pj.contains("calibrationPoints") && pj["calibrationPoints"].is_array()) {
                    proj.calibrationPoints.reserve(pj["calibrationPoints"].size());
                    for (const auto& cpJson : pj["calibrationPoints"]) {
                        CalibrationPoint cp;
                        if (cpJson.contains("worldPos") &&
                            cpJson["worldPos"].is_array() &&
                            cpJson["worldPos"].size() >= 3) {
                            cp.worldPos = { cpJson["worldPos"][0].get<float>(),
                                            cpJson["worldPos"][1].get<float>(),
                                            cpJson["worldPos"][2].get<float>() };
                        }
                        if (cpJson.contains("projectorUV") &&
                            cpJson["projectorUV"].is_array() &&
                            cpJson["projectorUV"].size() >= 2) {
                            cp.projectorUV = { cpJson["projectorUV"][0].get<float>(),
                                               cpJson["projectorUV"][1].get<float>() };
                        }
                        cp.isAligned = cpJson.value("isAligned", false);
                        // sourceScreen resolves by Screen name. Missing field
                        // or unknown name → entt::null (legacy / orphan).
                        cp.sourceScreen = entt::null;
                        if (cpJson.contains("sourceScreenName") &&
                            cpJson["sourceScreenName"].is_string()) {
                            const std::string sname =
                                cpJson["sourceScreenName"].get<std::string>();
                            if (!sname.empty()) {
                                auto screenView = registry.view<Screen>();
                                for (auto [se, s] : screenView.each()) {
                                    if (s.name == sname) {
                                        cp.sourceScreen = se;
                                        break;
                                    }
                                }
                            }
                        }
                        proj.calibrationPoints.push_back(cp);
                    }
                }

                std::cout << "[ProjectSerializer] Loaded projector: " << proj.name
                          << " (" << proj.calibrationPoints.size() << " cal points"
                          << (proj.isCalibrated ? ", calibrated" : ", uncalibrated")
                          << (proj.useResidualWarp ? ", warp on" : "") << ")" << std::endl;
            }
        }

        // Load timeline sections. v24+ shape: each entry is a break-point
        // marker `{breakFrame, color, fadeSeconds}` with breakFrame an
        // integer timeline frame. Pre-v24 stored breakFrame in microseconds
        // — migrate via timeToFrame (the frame rate was applied above).
        // Pre-v10 entries may also carry a "name" string (silently ignored).
        // Pre-Phase-B `{start, end}` entries migrate inline to TWO break
        // points (start and end), always from pre-v24 microseconds.
        timeline.clearSections();
        if (project.contains("sections")) {
            for (const auto& sj : project["sections"]) {
                if (sj.contains("breakFrame")) {
                    const FrameNumber breakFrame = (version >= 24)
                        ? sj.value("breakFrame", FrameNumber{0})
                        : timeline.timeToFrame(sj.value("breakFrame", Timecode{0}));
                    timeline.addSectionBreak(
                        breakFrame,
                        sj.value("color", static_cast<uint32_t>(0xFF6090C8)),
                        sj.value("fadeSeconds", 0.0));
                } else if (sj.contains("start") && sj.contains("end")) {
                    const auto color = sj.value("color", static_cast<uint32_t>(0xFF6090C8));
                    const FrameNumber start =
                        timeline.timeToFrame(sj.value("start", Timecode{0}));
                    const FrameNumber end =
                        timeline.timeToFrame(sj.value("end", Timecode{0}));
                    timeline.addSectionBreak(start, color, 0.0);
                    if (end > start) {
                        timeline.addSectionBreak(end, color, 0.0);
                    }
                }
            }
            std::cout << "[ProjectSerializer] Loaded " << timeline.getSections().size()
                      << " section breaks" << std::endl;
        }

        // Load cue tags (added in v9). Missing array = empty (pre-v9 files).
        timeline.clearCueTags();
        if (project.contains("cues")) {
            for (const auto& cj : project["cues"]) {
                CueTag cue;
                cue.number = cj.value("number", 0.0);
                // v24+ stores the cue position as an integer frame under
                // "frame"; pre-v24 stored microseconds under "timestamp".
                cue.frame = (version >= 24)
                    ? cj.value("frame", FrameNumber{0})
                    : timeline.timeToFrame(cj.value("timestamp", Timecode{0}));
                cue.label = cj.value("label", std::string{});
                timeline.addCueTag(std::move(cue));
            }
            std::cout << "[ProjectSerializer] Loaded " << timeline.getCueTags().size()
                      << " cues" << std::endl;
        }

        std::cout << "[ProjectSerializer] Loaded project from " << filepath.string() << std::endl;
        return true;

    } catch (const json::parse_error& e) {
        s_lastError = std::string("JSON parse error: ") + e.what();
        std::cerr << "[ProjectSerializer] " << s_lastError << std::endl;
        return false;
    } catch (const std::exception& e) {
        s_lastError = std::string("Load failed: ") + e.what();
        std::cerr << "[ProjectSerializer] " << s_lastError << std::endl;
        return false;
    }
}

} // namespace entity
