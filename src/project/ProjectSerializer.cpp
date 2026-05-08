/**
 * ProjectSerializer Implementation
 *
 * Saves and loads Entity project files in JSON format.
 */

#include "entity/project/ProjectSerializer.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/MappingSurface.hpp"
#include "entity/components/OutputDisplay.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/media/ObjLoader.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <unordered_set>

using json = nlohmann::json;

namespace entity {

std::string ProjectSerializer::s_lastError;

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

        // Tracks
        json tracksJson = json::array();
        const auto& registry = timeline.getRegistry();

        for (entt::entity trackEntity : timeline.getTracks()) {
            const auto* track = registry.try_get<TimelineTrack>(trackEntity);
            if (!track) continue;

            json trackJson;
            trackJson["index"] = track->trackIndex;

            // Clips in this track
            json clipsJson = json::array();
            for (entt::entity clipEntity : track->clips) {
                const auto* clip = registry.try_get<Clip>(clipEntity);
                if (!clip) continue;

                json clipJson;

                // Clip data
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

                // targetScreen: persist by Screen name since entt::entity
                // values aren't stable across sessions. Empty string = null
                // (clip renders to all screens), matching the default.
                std::string targetScreenName;
                if (clip->targetScreen != entt::null &&
                    registry.valid(clip->targetScreen) &&
                    registry.all_of<Screen>(clip->targetScreen)) {
                    targetScreenName = registry.get<Screen>(clip->targetScreen).name;
                }
                clipJson["targetScreenName"] = targetScreenName;

                // Transform component (if exists)
                const auto* transform = registry.try_get<Transform>(clipEntity);
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
                }

                // MediaLayer component (if exists)
                const auto* layer = registry.try_get<MediaLayer>(clipEntity);
                if (layer) {
                    clipJson["layer"]["zOrder"] = layer->zOrder;
                    clipJson["layer"]["opacity"] = layer->opacity;
                    clipJson["layer"]["blendMode"] = blendModeToJson(layer->blendMode);
                    clipJson["layer"]["visible"] = layer->visible;
                }

                clipsJson.push_back(clipJson);
            }

            trackJson["clips"] = clipsJson;
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
        }

        // Serialize mapping surfaces
        json surfacesJson = json::array();
        auto surfaceView = registry.view<MappingSurface>();
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
            sj["scale"]    = {screen.scale[0],    screen.scale[1],    screen.scale[2]};
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
                cpsJson.push_back(cpj);
            }
            pj["calibrationPoints"] = cpsJson;

            projectorsJson.push_back(pj);
        }
        project["projectors"] = projectorsJson;

        // Serialize timeline sections. Phase B refactored from regions to
        // break-points; the loader migrates pre-Phase-B `{start, end}` entries
        // by emitting two break points with the same name/color.
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
        // Forward-compatible: v8 loaders ignore the unknown "cues" key.
        json cuesJson = json::array();
        for (const auto& cue : timeline.getCueTags()) {
            json cj;
            cj["number"]    = cue.number;
            cj["timestamp"] = cue.timestamp;
            cj["label"]     = cue.label;
            cuesJson.push_back(cj);
        }
        project["cues"] = cuesJson;

        // Write to file with pretty formatting
        std::ofstream file(savePath);
        if (!file.is_open()) {
            s_lastError = "Failed to open file for writing: " + savePath.string();
            return false;
        }

        file << project.dump(2);  // 2-space indentation
        file.close();

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
        // reloading (or autosave-then-load) would pile up duplicates. Physical
        // output windows must have been released by the caller *before* this
        // call (Engine::loadProject does that), because once we clear the
        // OutputDisplay component, the renderer slot ID is lost.
        // Destroying the entities entirely (not just the component) because
        // these are currently "component-per-entity" with no other components
        // attached worth preserving.
        {
            std::vector<entt::entity> toDestroy;
            auto clearView = registry.view<MappingSurface>();
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
                    if (!modelFilepath.empty() && std::filesystem::exists(modelFilepath)) {
                        model.mesh = ObjLoader::load(modelFilepath);
                    } else {
                        model.mesh = createDefaultScreenMesh();
                    }
                    std::cout << "[ProjectSerializer] Loaded model: " << name
                              << " (from " << (modelFilepath.empty() ? "built-in plane" : modelFilepath) << ")" << std::endl;
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
                if (sj.contains("scale") && sj["scale"].is_array() && sj["scale"].size() >= 3) {
                    screen.scale = {sj["scale"][0].get<float>(),
                                    sj["scale"][1].get<float>(),
                                    sj["scale"][2].get<float>()};
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

                std::cout << "[ProjectSerializer] Loaded screen: " << name
                          << " (" << screen.width << "x" << screen.height << ")" << std::endl;
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
        }

        // Load tracks
        if (project.contains("tracks")) {
            for (const auto& trackJson : project["tracks"]) {
                // Create track
                std::string trackName = "Track " + std::to_string(trackJson.value("index", 0));
                entt::entity trackEntity = timeline.createTrack(trackName);
                auto* track = registry.try_get<TimelineTrack>(trackEntity);
                if (!track) continue;

                // Load clips
                if (trackJson.contains("clips")) {
                    for (const auto& clipJson : trackJson["clips"]) {
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
                        // (renders to all screens).
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

                        // Recalculate duration in timeline frames from totalMediaFrames
                        // This ensures correct timing even for old project files where duration was in source frames
                        if (clip.totalMediaFrames > 0 && clip.framerate > 0) {
                            double timelineFrameRate = timeline.getFrameRate();
                            clip.duration = static_cast<FrameNumber>(std::ceil(
                                clip.totalMediaFrames * (timelineFrameRate / clip.framerate)));
                        } else {
                            clip.duration = jsonDuration;  // Fallback for malformed clips
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

                        // Add clip to track
                        track->clips.push_back(clipEntity);

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
                auto& surface = registry.emplace<MappingSurface>(surfaceEntity);

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
                        proj.calibrationPoints.push_back(cp);
                    }
                }

                std::cout << "[ProjectSerializer] Loaded projector: " << proj.name
                          << " (" << proj.calibrationPoints.size() << " cal points"
                          << (proj.isCalibrated ? ", calibrated" : ", uncalibrated")
                          << (proj.useResidualWarp ? ", warp on" : "") << ")" << std::endl;
            }
        }

        // Load timeline sections. v10 shape: each entry is a break-point
        // marker `{breakFrame, color, fadeSeconds}`. Pre-v10 entries may
        // also carry a "name" string (silently ignored — Phase 4 dropped
        // section names). Pre-Phase-B `{start, end}` entries migrate
        // inline to TWO break points (start and end) with the same color
        // and fadeSeconds=0.
        timeline.clearSections();
        if (project.contains("sections")) {
            for (const auto& sj : project["sections"]) {
                if (sj.contains("breakFrame")) {
                    timeline.addSectionBreak(
                        sj.value("breakFrame", static_cast<Timecode>(0)),
                        sj.value("color", static_cast<uint32_t>(0xFF6090C8)),
                        sj.value("fadeSeconds", 0.0));
                } else if (sj.contains("start") && sj.contains("end")) {
                    const auto color = sj.value("color", static_cast<uint32_t>(0xFF6090C8));
                    const auto start = sj.value("start", static_cast<Timecode>(0));
                    const auto end   = sj.value("end", static_cast<Timecode>(0));
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
                cue.number    = cj.value("number", 0.0);
                cue.timestamp = cj.value("timestamp", static_cast<Timecode>(0));
                cue.label     = cj.value("label", std::string{});
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
