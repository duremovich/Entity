/**
 * ProjectSerializer Implementation
 *
 * Saves and loads Entity project files in JSON format.
 */

#include "entity/project/ProjectSerializer.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/MappingSurface.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

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

bool ProjectSerializer::save(const Timeline& timeline, const std::filesystem::path& filepath) {
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
                clipJson["totalMediaFrames"] = clip->totalMediaFrames;
                clipJson["playbackMode"] = static_cast<int>(clip->playbackMode);
                clipJson["framerate"] = clip->framerate;
                clipJson["width"] = clip->width;
                clipJson["height"] = clip->height;
                clipJson["hasAlpha"] = clip->hasAlpha;

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
                              MediaLoadCallback mediaLoadCallback) {
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
                        clip.duration = clipJson.value("duration", 0);
                        clip.mediaStartFrame = clipJson.value("mediaStartFrame", 0);
                        clip.totalMediaFrames = clipJson.value("totalMediaFrames", clip.duration);  // Default to duration for old projects
                        clip.playbackMode = static_cast<PlaybackMode>(clipJson.value("playbackMode", 0));  // Default to Freeze
                        clip.framerate = clipJson.value("framerate", 30.0);
                        clip.width = clipJson.value("width", 0);
                        clip.height = clipJson.value("height", 0);
                        clip.hasAlpha = clipJson.value("hasAlpha", false);
                        clip.loaded = false;
                        clip.decoding = false;

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
