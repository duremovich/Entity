#include "entity/command/Commands.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/core/Engine.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Screen.hpp"
#include <imgui.h>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <optional>

namespace entity {

// ============================================================================
// Transport Commands
// ============================================================================

bool PlayCommand::execute(Engine& engine) {
    if (auto* timeline = engine.getTimeline()) {
        timeline->play();
        return true;
    }
    return false;
}

CommandPtr PlayCommand::fromJson(const nlohmann::json& j) {
    (void)j;
    return std::make_unique<PlayCommand>();
}

bool PauseCommand::execute(Engine& engine) {
    if (auto* timeline = engine.getTimeline()) {
        timeline->pause();
        return true;
    }
    return false;
}

CommandPtr PauseCommand::fromJson(const nlohmann::json& j) {
    (void)j;
    return std::make_unique<PauseCommand>();
}

bool TogglePlayPauseCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    if (timeline->getPlaybackState() == PlaybackState::Playing) {
        timeline->pause();
    } else {
        timeline->play();
    }
    return true;
}

CommandPtr TogglePlayPauseCommand::fromJson(const nlohmann::json& j) {
    (void)j;
    return std::make_unique<TogglePlayPauseCommand>();
}

bool SeekCommand::execute(Engine& engine) {
    if (auto* timeline = engine.getTimeline()) {
        timeline->seek(m_time);
        return true;
    }
    return false;
}

std::string SeekCommand::getDescription() const {
    return "Seek to " + std::to_string(m_time / 1000000.0) + "s";
}

CommandPtr SeekCommand::fromJson(const nlohmann::json& j) {
    Timecode time = j.value("time", 0);
    return std::make_unique<SeekCommand>(time);
}

bool SeekToFrameCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    // Convert frame to timecode
    double frameRate = timeline->getFrameRate();
    Timecode time = static_cast<Timecode>((m_frame / frameRate) * 1000000.0);
    timeline->seek(time);
    return true;
}

std::string SeekToFrameCommand::getDescription() const {
    return "Seek to frame " + std::to_string(m_frame);
}

CommandPtr SeekToFrameCommand::fromJson(const nlohmann::json& j) {
    FrameNumber frame = j.value("frame", 0);
    return std::make_unique<SeekToFrameCommand>(frame);
}

// ============================================================================
// Navigation Commands
// ============================================================================

bool SeekToStartCommand::execute(Engine& engine) {
    if (auto* timeline = engine.getTimeline()) {
        timeline->seek(0);
        return true;
    }
    return false;
}

CommandPtr SeekToStartCommand::fromJson(const nlohmann::json& j) {
    (void)j;
    return std::make_unique<SeekToStartCommand>();
}

bool SeekToEndCommand::execute(Engine& engine) {
    if (auto* timeline = engine.getTimeline()) {
        timeline->seek(timeline->getDuration());
        return true;
    }
    return false;
}

CommandPtr SeekToEndCommand::fromJson(const nlohmann::json& j) {
    (void)j;
    return std::make_unique<SeekToEndCommand>();
}

bool StepForwardCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    // Pause if playing
    if (timeline->getPlaybackState() == PlaybackState::Playing) {
        timeline->pause();
    }

    Timecode currentTime = timeline->getCurrentTime();
    Timecode frameTime = static_cast<Timecode>(1000000.0 / timeline->getFrameRate()) * m_frames;
    Timecode newTime = currentTime + frameTime;

    if (newTime < timeline->getDuration()) {
        timeline->seek(newTime);
    }
    return true;
}

CommandPtr StepForwardCommand::fromJson(const nlohmann::json& j) {
    int32_t frames = j.value("frames", 1);
    return std::make_unique<StepForwardCommand>(frames);
}

bool StepBackwardCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    // Pause if playing
    if (timeline->getPlaybackState() == PlaybackState::Playing) {
        timeline->pause();
    }

    Timecode currentTime = timeline->getCurrentTime();
    Timecode frameTime = static_cast<Timecode>(1000000.0 / timeline->getFrameRate()) * m_frames;

    if (currentTime > frameTime) {
        timeline->seek(currentTime - frameTime);
    } else {
        timeline->seek(0);
    }
    return true;
}

CommandPtr StepBackwardCommand::fromJson(const nlohmann::json& j) {
    int32_t frames = j.value("frames", 1);
    return std::make_unique<StepBackwardCommand>(frames);
}

// ============================================================================
// Clip Commands
// ============================================================================

bool SelectClipCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    entt::entity clipEntity = entt::null;
    entt::entity trackEntity = entt::null;

    // If we have an entity ID, use it directly
    if (m_entityId.has_value()) {
        clipEntity = static_cast<entt::entity>(*m_entityId);
        // Find which track contains this clip
        auto& registry = engine.getRegistry();
        const auto& tracks = timeline->getTracks();
        for (entt::entity te : tracks) {
            auto* track = registry.try_get<TimelineTrack>(te);
            if (track) {
                for (entt::entity ce : track->clips) {
                    if (ce == clipEntity) {
                        trackEntity = te;
                        break;
                    }
                }
            }
            if (trackEntity != entt::null) break;
        }
    }
    // Otherwise, find by track and clip index
    else if (m_trackIndex.has_value() && m_clipIndex.has_value()) {
        const auto& tracks = timeline->getTracks();
        if (*m_trackIndex < 0 || *m_trackIndex >= static_cast<int>(tracks.size())) {
            std::cerr << "[SelectClip] Invalid track index: " << *m_trackIndex << std::endl;
            return false;
        }

        trackEntity = tracks[*m_trackIndex];
        auto& registry = engine.getRegistry();
        auto* track = registry.try_get<TimelineTrack>(trackEntity);
        if (!track) {
            std::cerr << "[SelectClip] Track has no TimelineTrack component" << std::endl;
            return false;
        }

        if (*m_clipIndex < 0 || *m_clipIndex >= static_cast<int>(track->clips.size())) {
            std::cerr << "[SelectClip] Invalid clip index: " << *m_clipIndex << std::endl;
            return false;
        }

        clipEntity = track->clips[*m_clipIndex];
    } else {
        return false;
    }

    // Select the clip
    timeline->setSelectedClip(clipEntity);

    // Optionally expand the clip to show property tracks
    if (m_expand) {
        // Also expand the track so the clip is visible
        if (trackEntity != entt::null) {
            timeline->setTrackExpanded(trackEntity, true);
        }
        timeline->setClipExpanded(clipEntity, true);
    }

    std::cout << "[SelectClip] Selected clip " << static_cast<uint32_t>(clipEntity)
              << " (expand=" << (m_expand ? "true" : "false") << ")" << std::endl;
    return true;
}

nlohmann::json SelectClipCommand::toJson() const {
    nlohmann::json j = {{"type", "SelectClip"}};
    if (m_entityId.has_value()) {
        j["entityId"] = *m_entityId;
    }
    if (m_trackIndex.has_value()) {
        j["trackIndex"] = *m_trackIndex;
    }
    if (m_clipIndex.has_value()) {
        j["clipIndex"] = *m_clipIndex;
    }
    if (m_expand) {
        j["expand"] = true;
    }
    return j;
}

std::string SelectClipCommand::getDescription() const {
    if (m_entityId.has_value()) {
        return "Select clip entity " + std::to_string(*m_entityId);
    }
    if (m_trackIndex.has_value() && m_clipIndex.has_value()) {
        return "Select clip [track " + std::to_string(*m_trackIndex) +
               ", clip " + std::to_string(*m_clipIndex) + "]";
    }
    return "SelectClip";
}

CommandPtr SelectClipCommand::fromJson(const nlohmann::json& j) {
    bool expand = j.value("expand", false);
    if (j.contains("entityId")) {
        return std::make_unique<SelectClipCommand>(j["entityId"].get<uint32_t>(), expand);
    }
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    return std::make_unique<SelectClipCommand>(trackIndex, clipIndex, expand);
}

bool DeselectAllCommand::execute(Engine& engine) {
    if (auto* timeline = engine.getTimeline()) {
        timeline->setSelectedClip(entt::null);
        return true;
    }
    return false;
}

CommandPtr DeselectAllCommand::fromJson(const nlohmann::json& j) {
    (void)j;
    return std::make_unique<DeselectAllCommand>();
}

bool SplitClipCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    entt::entity target;
    if (m_entityId.has_value()) {
        target = static_cast<entt::entity>(*m_entityId);
    } else {
        target = timeline->getSelectedClip();
    }

    if (target == entt::null) {
        std::cerr << "[SplitClip] No clip selected" << std::endl;
        return false;
    }

    FrameNumber frame = m_frame.value_or(timeline->getCurrentFrame());
    entt::entity newClip = timeline->splitClip(target, frame);

    return newClip != entt::null;
}

nlohmann::json SplitClipCommand::toJson() const {
    nlohmann::json j = {{"type", "SplitClip"}};
    if (m_entityId.has_value()) {
        j["entityId"] = *m_entityId;
    }
    if (m_frame.has_value()) {
        j["frame"] = *m_frame;
    }
    return j;
}

std::string SplitClipCommand::getDescription() const {
    std::string desc = "Split clip";
    if (m_frame.has_value()) {
        desc += " at frame " + std::to_string(*m_frame);
    } else {
        desc += " at playhead";
    }
    return desc;
}

CommandPtr SplitClipCommand::fromJson(const nlohmann::json& j) {
    if (j.contains("entityId") && j.contains("frame")) {
        return std::make_unique<SplitClipCommand>(
            j["entityId"].get<uint32_t>(),
            j["frame"].get<FrameNumber>()
        );
    }
    return std::make_unique<SplitClipCommand>();
}

bool DuplicateClipCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    entt::entity target;
    if (m_entityId.has_value()) {
        target = static_cast<entt::entity>(*m_entityId);
    } else {
        target = timeline->getSelectedClip();
    }

    if (target == entt::null) {
        std::cerr << "[DuplicateClip] No clip selected" << std::endl;
        return false;
    }

    entt::entity newClip = timeline->duplicateClip(target);
    return newClip != entt::null;
}

nlohmann::json DuplicateClipCommand::toJson() const {
    nlohmann::json j = {{"type", "DuplicateClip"}};
    if (m_entityId.has_value()) {
        j["entityId"] = *m_entityId;
    }
    return j;
}

CommandPtr DuplicateClipCommand::fromJson(const nlohmann::json& j) {
    if (j.contains("entityId")) {
        return std::make_unique<DuplicateClipCommand>(j["entityId"].get<uint32_t>());
    }
    return std::make_unique<DuplicateClipCommand>();
}

bool DeleteClipCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    entt::entity target;
    if (m_entityId.has_value()) {
        target = static_cast<entt::entity>(*m_entityId);
    } else {
        target = timeline->getSelectedClip();
    }

    if (target == entt::null) {
        std::cerr << "[DeleteClip] No clip selected" << std::endl;
        return false;
    }

    timeline->deleteClip(target);
    timeline->setSelectedClip(entt::null);
    return true;
}

nlohmann::json DeleteClipCommand::toJson() const {
    nlohmann::json j = {{"type", "DeleteClip"}};
    if (m_entityId.has_value()) {
        j["entityId"] = *m_entityId;
    }
    return j;
}

CommandPtr DeleteClipCommand::fromJson(const nlohmann::json& j) {
    if (j.contains("entityId")) {
        return std::make_unique<DeleteClipCommand>(j["entityId"].get<uint32_t>());
    }
    return std::make_unique<DeleteClipCommand>();
}

// ============================================================================
// Media Commands
// ============================================================================

bool ImportVideoCommand::execute(Engine& engine) {
    return engine.importVideo(m_filepath, m_trackIndex, m_position);
}

nlohmann::json ImportVideoCommand::toJson() const {
    return {
        {"type", "ImportVideo"},
        {"filepath", m_filepath},
        {"trackIndex", m_trackIndex},
        {"position", m_position}
    };
}

std::string ImportVideoCommand::getDescription() const {
    return "Import video: " + m_filepath;
}

CommandPtr ImportVideoCommand::fromJson(const nlohmann::json& j) {
    std::string filepath = j.value("filepath", "");
    int trackIndex = j.value("trackIndex", 0);
    Timecode position = j.value("position", 0);
    return std::make_unique<ImportVideoCommand>(filepath, trackIndex, position);
}

// ============================================================================
// Project Commands
// ============================================================================

bool SaveProjectCommand::execute(Engine& engine) {
    if (m_filepath.empty()) {
        return engine.saveProject();
    }
    return engine.saveProject(m_filepath);
}

nlohmann::json SaveProjectCommand::toJson() const {
    nlohmann::json j = {{"type", "SaveProject"}};
    if (!m_filepath.empty()) {
        j["filepath"] = m_filepath;
    }
    return j;
}

std::string SaveProjectCommand::getDescription() const {
    if (m_filepath.empty()) {
        return "Save project";
    }
    return "Save project to: " + m_filepath;
}

CommandPtr SaveProjectCommand::fromJson(const nlohmann::json& j) {
    std::string filepath = j.value("filepath", "");
    return std::make_unique<SaveProjectCommand>(filepath);
}

bool LoadProjectCommand::execute(Engine& engine) {
    if (!std::filesystem::exists(m_filepath)) {
        std::cerr << "[LoadProject] File not found: " << m_filepath << std::endl;
        return false;
    }
    return engine.loadProject(m_filepath);
}

nlohmann::json LoadProjectCommand::toJson() const {
    return {
        {"type", "LoadProject"},
        {"filepath", m_filepath}
    };
}

std::string LoadProjectCommand::getDescription() const {
    return "Load project: " + m_filepath;
}

CommandPtr LoadProjectCommand::fromJson(const nlohmann::json& j) {
    std::string filepath = j.value("filepath", "");
    return std::make_unique<LoadProjectCommand>(filepath);
}

// ============================================================================
// Script Control Commands
// ============================================================================

bool WaitFramesCommand::execute(Engine& engine) {
    std::cout << "[WaitFrames] Waiting " << m_count << " frames" << std::endl;

    if (auto* dispatcher = engine.getCommandDispatcher()) {
        dispatcher->setWaitFrames(m_count);
        return true;
    }
    return false;
}

std::string WaitFramesCommand::getDescription() const {
    return "Wait " + std::to_string(m_count) + " frames";
}

CommandPtr WaitFramesCommand::fromJson(const nlohmann::json& j) {
    uint32_t count = j.value("count", 1);
    return std::make_unique<WaitFramesCommand>(count);
}

bool CaptureScreenshotCommand::execute(Engine& engine) {
    std::cout << "[CaptureScreenshot] Capturing to: " << m_filepath << std::endl;

    bool success = false;
    if (m_region == Region::FullWindow) {
        success = engine.captureWindowScreenshot(m_filepath);
    } else {
        success = engine.captureScreenshot(m_filepath);
    }

    // Add to script results if dispatcher is tracking
    if (success) {
        if (auto* dispatcher = engine.getCommandDispatcher()) {
            dispatcher->addScreenshotToResults(m_filepath);
        }
    }

    return success;
}

nlohmann::json CaptureScreenshotCommand::toJson() const {
    std::string regionStr = (m_region == Region::FullWindow) ? "fullWindow" : "composeTarget";
    return {
        {"type", "CaptureScreenshot"},
        {"filepath", m_filepath},
        {"region", regionStr}
    };
}

std::string CaptureScreenshotCommand::getDescription() const {
    return "Capture screenshot: " + m_filepath;
}

CommandPtr CaptureScreenshotCommand::fromJson(const nlohmann::json& j) {
    std::string filepath = j.value("filepath", "screenshot.png");
    Region region = Region::ComposeTarget;
    std::string regionStr = j.value("region", "composeTarget");
    if (regionStr == "fullWindow") {
        region = Region::FullWindow;
    }
    return std::make_unique<CaptureScreenshotCommand>(filepath, region);
}

bool CaptureHashCommand::execute(Engine& engine) {
    std::cout << "[CaptureHash] Hashing compose target " << m_composeSlot
              << " -> " << m_hashFilepath << std::endl;
    return engine.captureHash(m_hashFilepath, m_goldenFilepath, m_composeSlot);
}

nlohmann::json CaptureHashCommand::toJson() const {
    nlohmann::json j = {
        {"type", "CaptureHash"},
        {"hashFilepath", m_hashFilepath},
        {"composeSlot", m_composeSlot}
    };
    if (!m_goldenFilepath.empty()) {
        j["goldenFilepath"] = m_goldenFilepath;
    }
    return j;
}

std::string CaptureHashCommand::getDescription() const {
    return "Capture hash: " + m_hashFilepath +
           (m_goldenFilepath.empty() ? "" : " vs " + m_goldenFilepath);
}

CommandPtr CaptureHashCommand::fromJson(const nlohmann::json& j) {
    std::string hashFilepath = j.value("hashFilepath", "output.hash");
    std::string goldenFilepath = j.value("goldenFilepath", "");
    uint32_t composeSlot = j.value("composeSlot", 0u);
    return std::make_unique<CaptureHashCommand>(hashFilepath, goldenFilepath, composeSlot);
}

// ============================================================================
// Application Commands
// ============================================================================

bool ExitCommand::execute(Engine& engine) {
    engine.requestExit();
    return true;
}

CommandPtr ExitCommand::fromJson(const nlohmann::json& j) {
    (void)j;
    return std::make_unique<ExitCommand>();
}

// ============================================================================
// Property Commands (for debugging and scripting)
// ============================================================================

bool SetClipBlendModeCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "SetClipBlendMode: Invalid track index " << m_trackIndex << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) {
        std::cerr << "SetClipBlendMode: Invalid clip index " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->clips[m_clipIndex];
    auto* layer = registry.try_get<MediaLayer>(clipEntity);
    if (!layer) {
        std::cerr << "SetClipBlendMode: Clip has no MediaLayer component" << std::endl;
        return false;
    }

    if (!m_previousMode.has_value()) {
        m_previousMode = layer->blendMode;
    }
    layer->blendMode = m_blendMode;
    std::cout << "[SetClipBlendMode] Track " << m_trackIndex << ", Clip " << m_clipIndex
              << " -> " << static_cast<int>(m_blendMode) << std::endl;
    return true;
}

bool SetClipBlendModeCommand::undo(Engine& engine) {
    if (!m_previousMode.has_value()) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) return false;
    auto* layer = registry.try_get<MediaLayer>(track->clips[m_clipIndex]);
    if (!layer) return false;
    layer->blendMode = *m_previousMode;
    return true;
}

// Canonical name table for all BlendMode enum values. Keep in sync with
// include/entity/core/Types.hpp and shaders/common.hlsli.
namespace {
constexpr std::pair<BlendMode, const char*> kBlendModeNames[] = {
    {BlendMode::Normal,     "Normal"},
    {BlendMode::Add,        "Add"},
    {BlendMode::Multiply,   "Multiply"},
    {BlendMode::Screen,     "Screen"},
    {BlendMode::Overlay,    "Overlay"},
    {BlendMode::SoftLight,  "SoftLight"},
    {BlendMode::HardLight,  "HardLight"},
    {BlendMode::ColorDodge, "ColorDodge"},
    {BlendMode::ColorBurn,  "ColorBurn"},
    {BlendMode::Darken,     "Darken"},
    {BlendMode::Lighten,    "Lighten"},
    {BlendMode::Difference, "Difference"},
    {BlendMode::Exclusion,  "Exclusion"},
};

const char* blendModeToString(BlendMode mode) {
    for (const auto& [m, name] : kBlendModeNames) {
        if (m == mode) return name;
    }
    return "Normal";
}

BlendMode blendModeFromString(const std::string& s) {
    for (const auto& [m, name] : kBlendModeNames) {
        if (s == name) return m;
    }
    return BlendMode::Normal;
}
} // namespace

nlohmann::json SetClipBlendModeCommand::toJson() const {
    return {{"type", "SetClipBlendMode"}, {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex}, {"blendMode", blendModeToString(m_blendMode)}};
}

std::string SetClipBlendModeCommand::getDescription() const {
    return "Set blend mode for track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr SetClipBlendModeCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string modeStr = j.value("blendMode", "Normal");
    BlendMode mode = blendModeFromString(modeStr);
    return std::make_unique<SetClipBlendModeCommand>(trackIndex, clipIndex, mode);
}

bool SetClipOpacityCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "SetClipOpacity: Invalid track index " << m_trackIndex << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) {
        std::cerr << "SetClipOpacity: Invalid clip index " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->clips[m_clipIndex];
    auto* layer = registry.try_get<MediaLayer>(clipEntity);
    if (!layer) {
        std::cerr << "SetClipOpacity: Clip has no MediaLayer component" << std::endl;
        return false;
    }

    if (!m_previousOpacity.has_value()) {
        m_previousOpacity = layer->opacity;
    }
    layer->opacity = std::clamp(m_opacity, 0.0f, 1.0f);
    std::cout << "[SetClipOpacity] Track " << m_trackIndex << ", Clip " << m_clipIndex
              << " -> " << layer->opacity << std::endl;
    return true;
}

bool SetClipOpacityCommand::undo(Engine& engine) {
    if (!m_previousOpacity.has_value()) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) return false;
    auto* layer = registry.try_get<MediaLayer>(track->clips[m_clipIndex]);
    if (!layer) return false;
    layer->opacity = *m_previousOpacity;
    return true;
}

nlohmann::json SetClipOpacityCommand::toJson() const {
    return {{"type", "SetClipOpacity"}, {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex}, {"opacity", m_opacity}};
}

std::string SetClipOpacityCommand::getDescription() const {
    return "Set opacity for track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex) + " to " + std::to_string(m_opacity);
}

CommandPtr SetClipOpacityCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    float opacity = j.value("opacity", 1.0f);
    return std::make_unique<SetClipOpacityCommand>(trackIndex, clipIndex, opacity);
}

bool LogClipStateCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();

    std::cout << "\n=== CLIP STATE LOG ===" << std::endl;
    std::cout << "Timeline frame: " << timeline->getCurrentFrame() << std::endl;
    std::cout << "Playback: " << (timeline->getPlaybackState() == PlaybackState::Playing ? "Playing" : "Paused") << std::endl;

    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        if (m_trackIndex.has_value() && static_cast<int>(ti) != m_trackIndex.value()) continue;

        auto* track = registry.try_get<TimelineTrack>(tracks[ti]);
        if (!track) continue;

        std::cout << "\nTrack " << ti << " (" << track->clips.size() << " clips):" << std::endl;

        for (size_t ci = 0; ci < track->clips.size(); ++ci) {
            if (m_clipIndex.has_value() && static_cast<int>(ci) != m_clipIndex.value()) continue;

            entt::entity clipEntity = track->clips[ci];
            auto* clip = registry.try_get<Clip>(clipEntity);
            auto* layer = registry.try_get<MediaLayer>(clipEntity);

            std::cout << "  Clip " << ci << " (entity " << static_cast<uint32_t>(clipEntity) << "):" << std::endl;
            if (clip) {
                std::cout << "    File: " << clip->filepath << std::endl;
                std::cout << "    Start: " << clip->startFrame << ", Duration: " << clip->duration << std::endl;
            }
            if (layer) {
                std::cout << "    Opacity: " << layer->opacity << std::endl;
                std::cout << "    BlendMode: " << static_cast<int>(layer->blendMode) << std::endl;
                std::cout << "    zOrder: " << layer->zOrder << std::endl;
                std::cout << "    Visible: " << (layer->visible ? "yes" : "no") << std::endl;
            }
        }
    }
    std::cout << "======================\n" << std::endl;
    return true;
}

nlohmann::json LogClipStateCommand::toJson() const {
    nlohmann::json j = {{"type", "LogClipState"}};
    if (m_trackIndex.has_value()) j["trackIndex"] = m_trackIndex.value();
    if (m_clipIndex.has_value()) j["clipIndex"] = m_clipIndex.value();
    return j;
}

CommandPtr LogClipStateCommand::fromJson(const nlohmann::json& j) {
    if (j.contains("trackIndex") && j.contains("clipIndex")) {
        return std::make_unique<LogClipStateCommand>(
            j["trackIndex"].get<int>(), j["clipIndex"].get<int>());
    }
    return std::make_unique<LogClipStateCommand>();
}

// ============================================================================
// SelectTabCommand
// ============================================================================

bool SelectTabCommand::execute(Engine& engine) {
    std::cout << "[SelectTab] Selecting tab: " << m_tabName << std::endl;

    // Use ImGui to focus the window, which will bring its tab to front
    ImGui::SetWindowFocus(m_tabName.c_str());
    return true;
}

nlohmann::json SelectTabCommand::toJson() const {
    return {
        {"type", "SelectTab"},
        {"tabName", m_tabName}
    };
}

CommandPtr SelectTabCommand::fromJson(const nlohmann::json& j) {
    std::string tabName = j.value("tabName", "Stage");
    return std::make_unique<SelectTabCommand>(tabName);
}

// ============================================================================
// SetClipRotationCommand
// ============================================================================

bool SetClipRotationCommand::execute(Engine& engine) {
    std::cout << "[SetClipRotation] Track " << m_trackIndex << ", Clip " << m_clipIndex
              << " -> (" << m_rotX << ", " << m_rotY << ", " << m_rotZ << ")" << std::endl;

    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[SetClipRotation] No timeline!" << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();

    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[SetClipRotation] Invalid track index: " << m_trackIndex << std::endl;
        return false;
    }

    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) {
        std::cerr << "[SetClipRotation] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->clips[m_clipIndex];
    auto* transform = registry.try_get<Transform>(clipEntity);
    if (!transform) {
        std::cerr << "[SetClipRotation] Clip has no Transform component!" << std::endl;
        return false;
    }

    if (!m_previousRotation.has_value()) {
        m_previousRotation = std::array<float, 3>{
            transform->rotation.x, transform->rotation.y, transform->rotation.z};
    }
    transform->setRotation(glm::vec3(m_rotX, m_rotY, m_rotZ));
    std::cout << "[SetClipRotation] Set rotation to (" << m_rotX << ", " << m_rotY << ", " << m_rotZ << ")" << std::endl;
    return true;
}

bool SetClipRotationCommand::undo(Engine& engine) {
    if (!m_previousRotation.has_value()) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) return false;
    auto* transform = registry.try_get<Transform>(track->clips[m_clipIndex]);
    if (!transform) return false;
    const auto& prev = *m_previousRotation;
    transform->setRotation(glm::vec3(prev[0], prev[1], prev[2]));
    return true;
}

nlohmann::json SetClipRotationCommand::toJson() const {
    return {
        {"type", "SetClipRotation"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex},
        {"rotationX", m_rotX},
        {"rotationY", m_rotY},
        {"rotationZ", m_rotZ}
    };
}

std::string SetClipRotationCommand::getDescription() const {
    return "Set rotation for track " + std::to_string(m_trackIndex) + ", clip " + std::to_string(m_clipIndex);
}

CommandPtr SetClipRotationCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    float rotX = j.value("rotationX", 0.0f);
    float rotY = j.value("rotationY", 0.0f);
    float rotZ = j.value("rotationZ", 0.0f);
    return std::make_unique<SetClipRotationCommand>(trackIndex, clipIndex, rotX, rotY, rotZ);
}

// ============================================================================
// AddKeyframeCommand
// ============================================================================

bool AddKeyframeCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[AddKeyframe] No timeline!" << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();

    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[AddKeyframe] Invalid track index: " << m_trackIndex << std::endl;
        return false;
    }

    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) {
        std::cerr << "[AddKeyframe] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->clips[m_clipIndex];

    // Get or create AnimatedProperties component
    auto& animProps = registry.get_or_emplace<AnimatedProperties>(clipEntity);

    // Parse property name
    AnimatableProperty property;
    if (m_property == "PositionX") property = AnimatableProperty::PositionX;
    else if (m_property == "PositionY") property = AnimatableProperty::PositionY;
    else if (m_property == "Rotation") property = AnimatableProperty::Rotation;
    else if (m_property == "ScaleX") property = AnimatableProperty::ScaleX;
    else if (m_property == "ScaleY") property = AnimatableProperty::ScaleY;
    else if (m_property == "Opacity") property = AnimatableProperty::Opacity;
    else {
        std::cerr << "[AddKeyframe] Unknown property: " << m_property << std::endl;
        return false;
    }

    // Parse interpolation type
    InterpolationType interp = InterpolationType::Linear;
    if (m_interpolation == "Step") interp = InterpolationType::Step;
    else if (m_interpolation == "EaseIn") interp = InterpolationType::EaseIn;
    else if (m_interpolation == "EaseOut") interp = InterpolationType::EaseOut;
    else if (m_interpolation == "EaseInOut") interp = InterpolationType::EaseInOut;

    // Add the keyframe
    animProps.addKeyframe(property, m_frame, m_value, interp);

    std::cout << "[AddKeyframe] Track " << m_trackIndex << ", Clip " << m_clipIndex
              << " -> " << m_property << " @ frame " << m_frame
              << " = " << m_value << " (" << m_interpolation << ")" << std::endl;

    return true;
}

nlohmann::json AddKeyframeCommand::toJson() const {
    return {
        {"type", "AddKeyframe"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex},
        {"property", m_property},
        {"frame", m_frame},
        {"value", m_value},
        {"interpolation", m_interpolation}
    };
}

std::string AddKeyframeCommand::getDescription() const {
    return "Add keyframe for " + m_property + " at frame " + std::to_string(m_frame);
}

CommandPtr AddKeyframeCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string property = j.value("property", "PositionX");
    FrameNumber frame = j.value("frame", 0);
    float value = j.value("value", 0.0f);
    std::string interpolation = j.value("interpolation", "Linear");
    return std::make_unique<AddKeyframeCommand>(trackIndex, clipIndex, property, frame, value, interpolation);
}

// ============================================================================
// ClearKeyframesCommand
// ============================================================================

bool ClearKeyframesCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[ClearKeyframes] No timeline!" << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();

    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[ClearKeyframes] Invalid track index: " << m_trackIndex << std::endl;
        return false;
    }

    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) {
        std::cerr << "[ClearKeyframes] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->clips[m_clipIndex];

    // Remove AnimatedProperties component if it exists
    if (registry.any_of<AnimatedProperties>(clipEntity)) {
        registry.remove<AnimatedProperties>(clipEntity);
        std::cout << "[ClearKeyframes] Cleared all keyframes from track " << m_trackIndex
                  << ", clip " << m_clipIndex << std::endl;
    } else {
        std::cout << "[ClearKeyframes] No keyframes to clear on track " << m_trackIndex
                  << ", clip " << m_clipIndex << std::endl;
    }

    return true;
}

nlohmann::json ClearKeyframesCommand::toJson() const {
    return {
        {"type", "ClearKeyframes"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex}
    };
}

std::string ClearKeyframesCommand::getDescription() const {
    return "Clear all keyframes from track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr ClearKeyframesCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    return std::make_unique<ClearKeyframesCommand>(trackIndex, clipIndex);
}

// ============================================================================
// AddScreenCommand
// ============================================================================

bool AddScreenCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();

    // Create screen entity
    entt::entity entity = registry.create();
    auto& screen = registry.emplace<Screen>(entity);

    screen.name = m_name;
    screen.width = m_width;
    screen.height = m_height;
    screen.visible = true;
    screen.renderTargetSlot = UINT32_MAX;
    screen.renderTargetValid = false;

    std::cout << "[AddScreen] Created screen: " << m_name
              << " (entity=" << static_cast<uint32_t>(entity)
              << ", " << m_width << "x" << m_height << ")" << std::endl;

    return true;
}

nlohmann::json AddScreenCommand::toJson() const {
    return {
        {"type", "AddScreen"},
        {"name", m_name},
        {"width", m_width},
        {"height", m_height}
    };
}

std::string AddScreenCommand::getDescription() const {
    return "Add screen: " + m_name + " (" + std::to_string(m_width) + "x" + std::to_string(m_height) + ")";
}

CommandPtr AddScreenCommand::fromJson(const nlohmann::json& j) {
    std::string name = j.value("name", "New Screen");
    uint32_t width = j.value("width", 1920);
    uint32_t height = j.value("height", 1080);
    return std::make_unique<AddScreenCommand>(name, width, height);
}

// ============================================================================
// SetClipTargetScreenCommand
// ============================================================================

bool SetClipTargetScreenCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[SetClipTargetScreen] No timeline!" << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();

    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[SetClipTargetScreen] Invalid track index: " << m_trackIndex << std::endl;
        return false;
    }

    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->clips.size())) {
        std::cerr << "[SetClipTargetScreen] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->clips[m_clipIndex];
    auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cerr << "[SetClipTargetScreen] Clip component not found!" << std::endl;
        return false;
    }

    // Find screen by name
    if (m_screenName == "All Screens" || m_screenName.empty()) {
        clip->targetScreen = entt::null;
        std::cout << "[SetClipTargetScreen] Track " << m_trackIndex << ", Clip " << m_clipIndex
                  << " -> All Screens" << std::endl;
    } else {
        // Search for screen by name
        auto screenView = registry.view<Screen>();
        entt::entity foundScreen = entt::null;

        for (auto [screenEntity, screen] : screenView.each()) {
            if (screen.name == m_screenName) {
                foundScreen = screenEntity;
                break;
            }
        }

        if (foundScreen == entt::null) {
            std::cerr << "[SetClipTargetScreen] Screen not found: " << m_screenName << std::endl;
            return false;
        }

        clip->targetScreen = foundScreen;
        std::cout << "[SetClipTargetScreen] Track " << m_trackIndex << ", Clip " << m_clipIndex
                  << " -> " << m_screenName << " (entity=" << static_cast<uint32_t>(foundScreen) << ")" << std::endl;
    }

    return true;
}

nlohmann::json SetClipTargetScreenCommand::toJson() const {
    return {
        {"type", "SetClipTargetScreen"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex},
        {"screenName", m_screenName}
    };
}

std::string SetClipTargetScreenCommand::getDescription() const {
    return "Set target screen for track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex) + " to " + m_screenName;
}

CommandPtr SetClipTargetScreenCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string screenName = j.value("screenName", "All Screens");
    return std::make_unique<SetClipTargetScreenCommand>(trackIndex, clipIndex, screenName);
}

// ============================================================================
// AssertScreenExistsCommand
// ============================================================================

bool AssertScreenExistsCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    auto view = registry.view<Screen>();
    for (auto [entity, screen] : view.each()) {
        if (screen.name == m_name) {
            std::cout << "[AssertScreenExists] OK name=" << m_name << std::endl;
            return true;
        }
    }
    std::cerr << "[AssertScreenExists] FAIL: no screen named '" << m_name << "'" << std::endl;
    return false;
}

nlohmann::json AssertScreenExistsCommand::toJson() const {
    return {{"type", "AssertScreenExists"}, {"name", m_name}};
}

std::string AssertScreenExistsCommand::getDescription() const {
    return "Assert screen exists: " + m_name;
}

CommandPtr AssertScreenExistsCommand::fromJson(const nlohmann::json& j) {
    std::string name = j.value("name", "");
    return std::make_unique<AssertScreenExistsCommand>(name);
}

// ============================================================================
// AssertScreenCountCommand
// ============================================================================

bool AssertScreenCountCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    size_t actual = registry.view<Screen>().size();
    if (actual == m_count) {
        std::cout << "[AssertScreenCount] OK count=" << actual << std::endl;
        return true;
    }
    std::cerr << "[AssertScreenCount] FAIL: expected " << m_count
              << ", got " << actual << std::endl;
    return false;
}

nlohmann::json AssertScreenCountCommand::toJson() const {
    return {{"type", "AssertScreenCount"}, {"count", m_count}};
}

std::string AssertScreenCountCommand::getDescription() const {
    return "Assert screen count == " + std::to_string(m_count);
}

CommandPtr AssertScreenCountCommand::fromJson(const nlohmann::json& j) {
    size_t count = j.value("count", static_cast<size_t>(0));
    return std::make_unique<AssertScreenCountCommand>(count);
}

// ============================================================================
// SetClipPlaybackModeCommand
// ============================================================================

namespace {

Clip* lookupClipByTrack(Engine& engine, int trackIndex, int clipIndex, const char* who) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[" << who << "] No timeline" << std::endl;
        return nullptr;
    }
    const auto& tracks = timeline->getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[" << who << "] Invalid track index " << trackIndex << std::endl;
        return nullptr;
    }
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[trackIndex]);
    if (!track || clipIndex < 0 || clipIndex >= static_cast<int>(track->clips.size())) {
        std::cerr << "[" << who << "] Invalid clip index " << clipIndex << std::endl;
        return nullptr;
    }
    return registry.try_get<Clip>(track->clips[clipIndex]);
}

const char* playbackModeName(PlaybackMode mode) {
    switch (mode) {
        case PlaybackMode::Freeze:   return "Freeze";
        case PlaybackMode::Loop:     return "Loop";
        case PlaybackMode::PingPong: return "PingPong";
    }
    return "Freeze";
}

std::optional<PlaybackMode> parsePlaybackMode(const std::string& s) {
    if (s == "Freeze")   return PlaybackMode::Freeze;
    if (s == "Loop")     return PlaybackMode::Loop;
    if (s == "PingPong" || s == "Ping-Pong" || s == "pingpong")
        return PlaybackMode::PingPong;
    return std::nullopt;
}

} // namespace

bool SetClipPlaybackModeCommand::execute(Engine& engine) {
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipPlaybackMode");
    if (!clip) return false;
    if (!m_previousMode.has_value()) {
        m_previousMode = clip->playbackMode;
    }
    clip->playbackMode = m_mode;
    std::cout << "[SetClipPlaybackMode] Track " << m_trackIndex
              << ", Clip " << m_clipIndex
              << " -> " << playbackModeName(m_mode) << std::endl;
    return true;
}

bool SetClipPlaybackModeCommand::undo(Engine& engine) {
    if (!m_previousMode.has_value()) return false;
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipPlaybackMode");
    if (!clip) return false;
    clip->playbackMode = *m_previousMode;
    return true;
}

nlohmann::json SetClipPlaybackModeCommand::toJson() const {
    return {{"type", "SetClipPlaybackMode"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"mode", playbackModeName(m_mode)}};
}

std::string SetClipPlaybackModeCommand::getDescription() const {
    return std::string("Set playback mode -> ") + playbackModeName(m_mode) +
           " on track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr SetClipPlaybackModeCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string modeStr = j.value("mode", "Freeze");
    PlaybackMode mode = parsePlaybackMode(modeStr).value_or(PlaybackMode::Freeze);
    return std::make_unique<SetClipPlaybackModeCommand>(trackIndex, clipIndex, mode);
}

// ============================================================================
// SetClipFramerateCommand
// ============================================================================

bool SetClipFramerateCommand::execute(Engine& engine) {
    if (m_framerate <= 0.0) {
        std::cerr << "[SetClipFramerate] Framerate must be positive: " << m_framerate << std::endl;
        return false;
    }
    auto* timeline = engine.getTimeline();
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipFramerate");
    if (!clip || !timeline) return false;

    if (!m_previousFramerate.has_value()) {
        m_previousFramerate = clip->framerate;
        m_previousDuration = clip->duration;
    }

    clip->framerate = m_framerate;
    // Recompute duration from totalMediaFrames so the natural clip length on
    // the timeline matches the new rate. Matches Engine::loadClip /
    // ProjectSerializer::load semantics. See Engine.cpp:1213-1215.
    if (clip->totalMediaFrames > 0) {
        double tlFps = timeline->getFrameRate();
        clip->duration = std::max<FrameNumber>(1, static_cast<FrameNumber>(std::ceil(
            clip->totalMediaFrames * (tlFps / clip->framerate))));
    }
    std::cout << "[SetClipFramerate] Track " << m_trackIndex
              << ", Clip " << m_clipIndex
              << " -> " << clip->framerate << " fps"
              << " (duration now " << clip->duration << " frames)" << std::endl;
    return true;
}

bool SetClipFramerateCommand::undo(Engine& engine) {
    if (!m_previousFramerate.has_value() || !m_previousDuration.has_value()) return false;
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipFramerate");
    if (!clip) return false;
    clip->framerate = *m_previousFramerate;
    clip->duration = *m_previousDuration;
    return true;
}

nlohmann::json SetClipFramerateCommand::toJson() const {
    return {{"type", "SetClipFramerate"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"framerate", m_framerate}};
}

std::string SetClipFramerateCommand::getDescription() const {
    return "Set framerate -> " + std::to_string(m_framerate) +
           " on track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr SetClipFramerateCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    double framerate = j.value("framerate", 30.0);
    return std::make_unique<SetClipFramerateCommand>(trackIndex, clipIndex, framerate);
}

// ============================================================================
// SetClipDurationCommand
// ============================================================================

bool SetClipDurationCommand::execute(Engine& engine) {
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipDuration");
    if (!clip) return false;
    if (m_duration == 0) {
        std::cerr << "[SetClipDuration] Duration must be > 0" << std::endl;
        return false;
    }
    if (!m_previousDuration.has_value()) {
        m_previousDuration = clip->duration;
    }
    clip->duration = m_duration;
    std::cout << "[SetClipDuration] Track " << m_trackIndex
              << ", Clip " << m_clipIndex
              << " -> " << m_duration << " frames" << std::endl;
    return true;
}

bool SetClipDurationCommand::undo(Engine& engine) {
    if (!m_previousDuration.has_value()) return false;
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipDuration");
    if (!clip) return false;
    clip->duration = *m_previousDuration;
    return true;
}

nlohmann::json SetClipDurationCommand::toJson() const {
    return {{"type", "SetClipDuration"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"duration", m_duration}};
}

std::string SetClipDurationCommand::getDescription() const {
    return "Set duration -> " + std::to_string(m_duration) +
           " on track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr SetClipDurationCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    FrameNumber duration = j.value("duration", static_cast<FrameNumber>(0));
    return std::make_unique<SetClipDurationCommand>(trackIndex, clipIndex, duration);
}

} // namespace entity
