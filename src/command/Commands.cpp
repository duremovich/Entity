#include "entity/command/Commands.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/core/Engine.hpp"
#include "entity/director/CaptureBroker.hpp"
#include "entity/director/Director.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/director/SectionScheduler.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/ObjectAnimationOutput.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/media/ObjLoader.hpp"
#include <imgui.h>
#include <chrono>
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
    timeline->seekToFrame(m_frame);
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
                for (entt::entity ce : track->layers) {
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

        if (*m_clipIndex < 0 || *m_clipIndex >= static_cast<int>(track->layers.size())) {
            std::cerr << "[SelectClip] Invalid clip index: " << *m_clipIndex << std::endl;
            return false;
        }

        clipEntity = track->layers[*m_clipIndex];
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

    // Snapshot before destruction so undo can recreate the clip + decoder
    // + GPU slot. Bail if the snapshot is invalid -- happens when the
    // entity isn't in any track, which is not a recoverable state.
    auto snapshot = timeline->snapshotClipForDelete(target);
    if (!snapshot.valid()) {
        std::cerr << "[DeleteClip] Failed to snapshot clip entity="
                  << static_cast<uint32_t>(target) << " (not in any track?)" << std::endl;
        return false;
    }

    timeline->deleteClip(target);
    timeline->setSelectedClip(entt::null);

    m_snapshot = std::move(snapshot);
    m_captured = true;
    return true;
}

bool DeleteClipCommand::undo(Engine& engine) {
    if (!m_captured) {
        std::cerr << "[DeleteClip] undo called before successful execute" << std::endl;
        return false;
    }
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    entt::entity restored = timeline->restoreDeletedClip(m_snapshot);
    if (restored == entt::null) {
        return false;
    }

    // Retarget so a subsequent redo deletes the restored entity, not the
    // long-gone original ID.
    m_entityId = static_cast<uint32_t>(restored);
    return true;
}

bool DeleteClipCommand::redo(Engine& engine) {
    // Force re-capture against the restored entity (its component values
    // may have diverged from the original snapshot if other undoable
    // commands ran between undo and redo).
    m_captured = false;
    return execute(engine);
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

// ============================================================================
// WaitSecondsCommand
// ============================================================================

bool WaitSecondsCommand::execute(Engine& engine) {
    std::cout << "[WaitSeconds] Waiting " << m_count << " seconds" << std::endl;
    if (auto* dispatcher = engine.getCommandDispatcher()) {
        auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(m_count));
        dispatcher->setWaitUntil(std::chrono::steady_clock::now() + duration);
    }
    return true;
}

std::string WaitSecondsCommand::getDescription() const {
    return "Wait " + std::to_string(m_count) + " seconds";
}

CommandPtr WaitSecondsCommand::fromJson(const nlohmann::json& j) {
    double count = j.value("count", 1.0);
    return std::make_unique<WaitSecondsCommand>(count);
}

// ============================================================================
// AddTrackCommand
// ============================================================================

bool AddTrackCommand::execute(Engine& engine) {
    if (auto* timeline = engine.getTimeline()) {
        auto trackCount = timeline->getTrackCount();
        timeline->createTrack("Track " + std::to_string(trackCount + 1));
        std::cout << "[AddTrack] Created track " << trackCount
                  << " (total: " << (trackCount + 1) << ")" << std::endl;
        return true;
    }
    return false;
}

bool CaptureScreenshotCommand::execute(Engine& engine) {
    std::cout << "[CaptureScreenshot] Queuing capture to: " << m_filepath << std::endl;

    auto* director = engine.getDirector();
    auto* broker = director ? director->getCaptureBroker() : nullptr;
    if (!broker) {
        std::cerr << "[CaptureScreenshot] No capture broker available" << std::endl;
        return false;
    }
    return broker->requestScreenshotCapture(/*slot*/0, m_filepath,
                                            m_region == Region::FullWindow);
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
    std::cout << "[CaptureHash] Queuing hash of compose target " << m_composeSlot
              << " -> " << m_hashFilepath << std::endl;
    auto* director = engine.getDirector();
    auto* broker = director ? director->getCaptureBroker() : nullptr;
    if (!broker) {
        std::cerr << "[CaptureHash] No capture broker available" << std::endl;
        return false;
    }
    return broker->requestHashCapture(static_cast<int>(m_composeSlot),
                                      m_hashFilepath,
                                      m_goldenFilepath);
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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "SetClipBlendMode: Invalid clip index " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->layers[m_clipIndex];
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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;
    auto* layer = registry.try_get<MediaLayer>(track->layers[m_clipIndex]);
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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "SetClipOpacity: Invalid clip index " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->layers[m_clipIndex];
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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;
    auto* layer = registry.try_get<MediaLayer>(track->layers[m_clipIndex]);
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

        std::cout << "\nTrack " << ti << " (" << track->layers.size() << " clips):" << std::endl;

        for (size_t ci = 0; ci < track->layers.size(); ++ci) {
            if (m_clipIndex.has_value() && static_cast<int>(ci) != m_clipIndex.value()) continue;

            entt::entity clipEntity = track->layers[ci];
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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[SetClipRotation] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->layers[m_clipIndex];
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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;
    auto* transform = registry.try_get<Transform>(track->layers[m_clipIndex]);
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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[AddKeyframe] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->layers[m_clipIndex];

    // Get or create AnimatedProperties component
    auto& animProps = registry.get_or_emplace<AnimatedProperties>(clipEntity);

    // Parse property name
    AnimatableProperty property;
    if (m_property == "PositionX") property = AnimatableProperty::PositionX;
    else if (m_property == "PositionY") property = AnimatableProperty::PositionY;
    else if (m_property == "PositionZ") property = AnimatableProperty::PositionZ;
    else if (m_property == "Rotation") property = AnimatableProperty::Rotation;
    else if (m_property == "RotationX") property = AnimatableProperty::RotationX;
    else if (m_property == "RotationY") property = AnimatableProperty::RotationY;
    else if (m_property == "ScaleX") property = AnimatableProperty::ScaleX;
    else if (m_property == "ScaleY") property = AnimatableProperty::ScaleY;
    else if (m_property == "ScaleZ") property = AnimatableProperty::ScaleZ;
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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[ClearKeyframes] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->layers[m_clipIndex];

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
// UpsertKeyframeCommand
// ============================================================================

namespace {

const char* animatablePropertyName(AnimatableProperty p) {
    switch (p) {
        case AnimatableProperty::PositionX: return "PositionX";
        case AnimatableProperty::PositionY: return "PositionY";
        case AnimatableProperty::Rotation:  return "Rotation";
        case AnimatableProperty::ScaleX:    return "ScaleX";
        case AnimatableProperty::ScaleY:    return "ScaleY";
        case AnimatableProperty::Opacity:   return "Opacity";
    }
    return "Opacity";
}

std::optional<AnimatableProperty> parseAnimatableProperty(const std::string& s) {
    if (s == "PositionX") return AnimatableProperty::PositionX;
    if (s == "PositionY") return AnimatableProperty::PositionY;
    if (s == "Rotation")  return AnimatableProperty::Rotation;
    if (s == "ScaleX")    return AnimatableProperty::ScaleX;
    if (s == "ScaleY")    return AnimatableProperty::ScaleY;
    if (s == "Opacity")   return AnimatableProperty::Opacity;
    return std::nullopt;
}

} // namespace

bool UpsertKeyframeCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;

    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;

    entt::entity clipEntity = track->layers[m_clipIndex];
    auto& animProps = registry.get_or_emplace<AnimatedProperties>(clipEntity);

    // Auto-capture pre-edit state if the caller didn't provide it. Script
    // path doesn't set it; UI path should, since it captures on mouse-down
    // before any live mutation.
    if (!m_hasPreviousState) {
        const KeyframeTrack* kfTrack = animProps.getTrack(m_property);
        if (kfTrack) {
            const Keyframe* existing = kfTrack->getKeyframeAt(m_frame);
            m_previousValue = existing ? std::optional<float>(existing->value) : std::nullopt;
        } else {
            m_previousValue = std::nullopt;
        }
        m_hasPreviousState = true;
    }

    animProps.addKeyframe(m_property, m_frame, m_newValue, m_interp);
    return true;
}

bool UpsertKeyframeCommand::undo(Engine& engine) {
    if (!m_hasPreviousState) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;
    entt::entity clipEntity = track->layers[m_clipIndex];
    auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);
    if (!animProps) return false;

    KeyframeTrack& kfTrack = animProps->getOrCreateTrack(m_property);
    if (m_previousValue.has_value()) {
        // Overwrite with the prior value — addKeyframe replaces at the same frame.
        kfTrack.addKeyframe(m_frame, *m_previousValue);
    } else {
        // No keyframe existed before — remove the one we just added.
        kfTrack.removeKeyframe(m_frame);
    }
    return true;
}

nlohmann::json UpsertKeyframeCommand::toJson() const {
    return {
        {"type", "UpsertKeyframe"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex},
        {"property", animatablePropertyName(m_property)},
        {"frame", m_frame},
        {"value", m_newValue}
    };
}

std::string UpsertKeyframeCommand::getDescription() const {
    return std::string("Upsert keyframe ") + animatablePropertyName(m_property) +
           " @ frame " + std::to_string(m_frame) +
           " = " + std::to_string(m_newValue);
}

CommandPtr UpsertKeyframeCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string propStr = j.value("property", "Opacity");
    AnimatableProperty prop = parseAnimatableProperty(propStr).value_or(AnimatableProperty::Opacity);
    FrameNumber frame = j.value("frame", 0);
    float value = j.value("value", 0.0f);
    return std::make_unique<UpsertKeyframeCommand>(trackIndex, clipIndex, prop, frame, value);
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

namespace {

// Lookup screen name from entity. Returns "All Screens" for entt::null so
// the string value round-trips through the command's public API cleanly.
std::string screenNameForEntity(entt::registry& registry, entt::entity screen) {
    if (screen == entt::null) return "All Screens";
    auto* s = registry.try_get<Screen>(screen);
    if (!s) return "All Screens";
    return s->name;
}

} // namespace

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
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[SetClipTargetScreen] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }

    entt::entity clipEntity = track->layers[m_clipIndex];
    auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cerr << "[SetClipTargetScreen] Clip component not found!" << std::endl;
        return false;
    }

    if (!m_previousScreenName.has_value()) {
        m_previousScreenName = screenNameForEntity(registry, clip->targetScreen);
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

bool SetClipTargetScreenCommand::undo(Engine& engine) {
    if (!m_previousScreenName.has_value()) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;
    auto* clip = registry.try_get<Clip>(track->layers[m_clipIndex]);
    if (!clip) return false;

    const std::string& name = *m_previousScreenName;
    if (name == "All Screens" || name.empty()) {
        clip->targetScreen = entt::null;
    } else {
        entt::entity found = entt::null;
        for (auto [e, s] : registry.view<Screen>().each()) {
            if (s.name == name) { found = e; break; }
        }
        if (found == entt::null) return false;
        clip->targetScreen = found;
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
// AssertFrameCachedCommand
// ============================================================================

bool AssertFrameCachedCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[AssertFrameCached] No timeline" << std::endl;
        return false;
    }
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[AssertFrameCached] Invalid track index " << m_trackIndex << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[AssertFrameCached] Invalid clip index " << m_clipIndex << std::endl;
        return false;
    }
    entt::entity clipEntity = track->layers[m_clipIndex];

    auto* cache = engine.getFrameCache();
    if (!cache) {
        std::cerr << "[AssertFrameCached] FAIL: no FrameCache" << std::endl;
        return false;
    }
    if (!cache->has(clipEntity, m_sourceFrame)) {
        std::cerr << "[AssertFrameCached] FAIL: source frame " << m_sourceFrame
                  << " not in cache (track " << m_trackIndex << ", clip " << m_clipIndex
                  << "). cache bytes=" << cache->bytesUsed()
                  << " entries=" << cache->entryCount() << std::endl;
        return false;
    }
    std::cout << "[AssertFrameCached] OK frame=" << m_sourceFrame
              << " (track " << m_trackIndex << ", clip " << m_clipIndex
              << "); cache entries=" << cache->entryCount()
              << " bytes=" << cache->bytesUsed() << std::endl;
    return true;
}

nlohmann::json AssertFrameCachedCommand::toJson() const {
    return {
        {"type", "AssertFrameCached"},
        {"trackIndex",  m_trackIndex},
        {"clipIndex",   m_clipIndex},
        {"sourceFrame", m_sourceFrame},
    };
}

std::string AssertFrameCachedCommand::getDescription() const {
    return "Assert frame " + std::to_string(m_sourceFrame) + " cached for clip "
         + std::to_string(m_trackIndex) + "/" + std::to_string(m_clipIndex);
}

CommandPtr AssertFrameCachedCommand::fromJson(const nlohmann::json& j) {
    int trackIndex   = j.value("trackIndex", 0);
    int clipIndex    = j.value("clipIndex", 0);
    FrameNumber f    = j.value("sourceFrame", static_cast<FrameNumber>(0));
    return std::make_unique<AssertFrameCachedCommand>(trackIndex, clipIndex, f);
}

// ============================================================================
// SetFrameCacheBudgetCommand
// ============================================================================

bool SetFrameCacheBudgetCommand::execute(Engine& engine) {
    auto* cache = engine.getFrameCache();
    if (!cache) {
        std::cerr << "[SetFrameCacheBudget] FAIL: no FrameCache" << std::endl;
        return false;
    }
    cache->setMaxBytes(static_cast<size_t>(m_bytes));
    std::cout << "[SetFrameCacheBudget] OK budget=" << m_bytes
              << " (bytesUsed now=" << cache->bytesUsed()
              << ", entries=" << cache->entryCount() << ")" << std::endl;
    return true;
}

nlohmann::json SetFrameCacheBudgetCommand::toJson() const {
    return {{"type", "SetFrameCacheBudget"}, {"bytes", m_bytes}};
}

std::string SetFrameCacheBudgetCommand::getDescription() const {
    return "Set FrameCache budget = " + std::to_string(m_bytes) + " bytes";
}

CommandPtr SetFrameCacheBudgetCommand::fromJson(const nlohmann::json& j) {
    uint64_t bytes = j.value("bytes", static_cast<uint64_t>(0));
    return std::make_unique<SetFrameCacheBudgetCommand>(bytes);
}

// ============================================================================
// AssertFrameCacheBudgetOKCommand
// ============================================================================

bool AssertFrameCacheBudgetOKCommand::execute(Engine& engine) {
    auto* cache = engine.getFrameCache();
    if (!cache) {
        std::cerr << "[AssertFrameCacheBudgetOK] FAIL: no FrameCache" << std::endl;
        return false;
    }
    const size_t used    = cache->bytesUsed();
    const size_t budget  = cache->maxBytes();
    const size_t entries = cache->entryCount();

    // Invariant 1: cache must not exceed its budget. Holds at the level of
    // any single put/setMaxBytes call (those run evictUntilUnderBudget under
    // the mutex). A failure here means the eviction path regressed.
    if (used > budget) {
        std::cerr << "[AssertFrameCacheBudgetOK] FAIL: bytesUsed=" << used
                  << " > maxBytes=" << budget
                  << " (entries=" << entries << ")" << std::endl;
        return false;
    }
    // Invariant 2: cache must be alive. A trivially-empty cache satisfies
    // invariant 1 too — that's not what we want to gate. Production code
    // should always have at least the most-recent decoded frame held.
    if (entries == 0) {
        std::cerr << "[AssertFrameCacheBudgetOK] FAIL: cache empty"
                  << " (bytesUsed=" << used << ", maxBytes=" << budget
                  << "). Decoder isn't reaching the cache, or eviction is too aggressive." << std::endl;
        return false;
    }
    std::cout << "[AssertFrameCacheBudgetOK] OK used=" << used
              << "/" << budget << " entries=" << entries << std::endl;
    return true;
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
    if (!track || clipIndex < 0 || clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[" << who << "] Invalid clip index " << clipIndex << std::endl;
        return nullptr;
    }
    return registry.try_get<Clip>(track->layers[clipIndex]);
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

// ============================================================================
// SetClipMediaStartFrameCommand
// ============================================================================

bool SetClipMediaStartFrameCommand::execute(Engine& engine) {
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipMediaStartFrame");
    if (!clip) return false;
    if (m_mediaStartFrame < 0) {
        std::cerr << "[SetClipMediaStartFrame] Must be >= 0: " << m_mediaStartFrame << std::endl;
        return false;
    }
    if (clip->totalMediaFrames > 0 && m_mediaStartFrame >= clip->totalMediaFrames) {
        std::cerr << "[SetClipMediaStartFrame] Out of range ("
                  << m_mediaStartFrame << " >= " << clip->totalMediaFrames << ")" << std::endl;
        return false;
    }
    if (!m_previousMediaStartFrame.has_value()) {
        m_previousMediaStartFrame = clip->mediaStartFrame;
    }
    clip->mediaStartFrame = m_mediaStartFrame;
    std::cout << "[SetClipMediaStartFrame] Track " << m_trackIndex
              << ", Clip " << m_clipIndex
              << " -> " << m_mediaStartFrame << std::endl;
    return true;
}

bool SetClipMediaStartFrameCommand::undo(Engine& engine) {
    if (!m_previousMediaStartFrame.has_value()) return false;
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipMediaStartFrame");
    if (!clip) return false;
    clip->mediaStartFrame = *m_previousMediaStartFrame;
    return true;
}

nlohmann::json SetClipMediaStartFrameCommand::toJson() const {
    return {{"type", "SetClipMediaStartFrame"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"mediaStartFrame", m_mediaStartFrame}};
}

std::string SetClipMediaStartFrameCommand::getDescription() const {
    return "Set mediaStartFrame -> " + std::to_string(m_mediaStartFrame) +
           " on track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr SetClipMediaStartFrameCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    FrameNumber mediaStartFrame = j.value("mediaStartFrame", static_cast<FrameNumber>(0));
    return std::make_unique<SetClipMediaStartFrameCommand>(trackIndex, clipIndex, mediaStartFrame);
}

// ============================================================================
// SetClipMediaOutFrameCommand
// ============================================================================

bool SetClipMediaOutFrameCommand::execute(Engine& engine) {
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipMediaOutFrame");
    if (!clip) return false;
    // Inclusive out-point: must be >= mediaStartFrame (allowing 1-frame
    // playback windows) and < totalMediaFrames (last valid index is
    // totalMediaFrames - 1).
    if (m_mediaOutFrame < clip->mediaStartFrame) {
        std::cerr << "[SetClipMediaOutFrame] Must be >= mediaStartFrame ("
                  << m_mediaOutFrame << " < " << clip->mediaStartFrame << ")" << std::endl;
        return false;
    }
    if (clip->totalMediaFrames > 0 && m_mediaOutFrame >= clip->totalMediaFrames) {
        std::cerr << "[SetClipMediaOutFrame] Out of range ("
                  << m_mediaOutFrame << " >= " << clip->totalMediaFrames << ")" << std::endl;
        return false;
    }
    if (!m_previousMediaOutFrame.has_value()) {
        m_previousMediaOutFrame = clip->mediaOutFrame;
    }
    clip->mediaOutFrame = m_mediaOutFrame;
    std::cout << "[SetClipMediaOutFrame] Track " << m_trackIndex
              << ", Clip " << m_clipIndex
              << " -> " << m_mediaOutFrame << std::endl;
    return true;
}

bool SetClipMediaOutFrameCommand::undo(Engine& engine) {
    if (!m_previousMediaOutFrame.has_value()) return false;
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipMediaOutFrame");
    if (!clip) return false;
    clip->mediaOutFrame = *m_previousMediaOutFrame;
    return true;
}

nlohmann::json SetClipMediaOutFrameCommand::toJson() const {
    return {{"type", "SetClipMediaOutFrame"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"mediaOutFrame", m_mediaOutFrame}};
}

std::string SetClipMediaOutFrameCommand::getDescription() const {
    return "Set mediaOutFrame -> " + std::to_string(m_mediaOutFrame) +
           " on track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr SetClipMediaOutFrameCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    FrameNumber mediaOutFrame = j.value("mediaOutFrame", static_cast<FrameNumber>(0));
    return std::make_unique<SetClipMediaOutFrameCommand>(trackIndex, clipIndex, mediaOutFrame);
}

// ============================================================================
// Section break-point commands (Phase B)
// ============================================================================

bool AddSectionBreakCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    m_inserted = timeline->addSectionBreak(m_breakFrame, m_color, m_fadeSeconds);
    if (!m_inserted) {
        std::cerr << "[AddSectionBreak] FAIL: a break already exists at "
                  << m_breakFrame << std::endl;
        return false;
    }
    std::cout << "[AddSectionBreak] OK at " << m_breakFrame << std::endl;
    return true;
}

bool AddSectionBreakCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline || !m_inserted) return false;
    bool ok = timeline->removeSectionBreak(m_breakFrame);
    if (ok) m_inserted = false;
    return ok;
}

bool AddSectionBreakCommand::redo(Engine& engine) {
    return execute(engine);
}

nlohmann::json AddSectionBreakCommand::toJson() const {
    return {{"type", "AddSectionBreak"},
            {"breakFrame", m_breakFrame},
            {"color", m_color},
            {"fadeSeconds", m_fadeSeconds}};
}

std::string AddSectionBreakCommand::getDescription() const {
    return "Add section break at " + std::to_string(m_breakFrame);
}

CommandPtr AddSectionBreakCommand::fromJson(const nlohmann::json& j) {
    // Pre-Phase-4 payloads carried a "name" string; ignored on load.
    Timecode breakFrame = j.value("breakFrame", static_cast<Timecode>(0));
    uint32_t color = j.value("color", static_cast<uint32_t>(0xFF6090C8));
    double fadeSeconds = j.value("fadeSeconds", 0.0);
    return std::make_unique<AddSectionBreakCommand>(breakFrame, color, fadeSeconds);
}

bool RemoveSectionBreakCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    if (!m_captured) {
        // Capture pre-state by linear scan (sections vector is small).
        bool found = false;
        for (const auto& sec : timeline->getSections()) {
            if (sec.breakFrame == m_breakFrame) {
                m_previousState = sec;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "[RemoveSectionBreak] FAIL: no break at " << m_breakFrame << std::endl;
            return false;
        }
        m_captured = true;
    }
    bool ok = timeline->removeSectionBreak(m_breakFrame);
    if (!ok) {
        std::cerr << "[RemoveSectionBreak] FAIL: removeSectionBreak returned false for "
                  << m_breakFrame << std::endl;
    }
    return ok;
}

bool RemoveSectionBreakCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline || !m_captured) return false;
    return timeline->addSectionBreak(m_previousState.breakFrame,
                                     m_previousState.color,
                                     m_previousState.fadeSeconds);
}

bool RemoveSectionBreakCommand::redo(Engine& engine) {
    return execute(engine);
}

nlohmann::json RemoveSectionBreakCommand::toJson() const {
    return {{"type", "RemoveSectionBreak"}, {"breakFrame", m_breakFrame}};
}

std::string RemoveSectionBreakCommand::getDescription() const {
    return "Remove section break at " + std::to_string(m_breakFrame);
}

CommandPtr RemoveSectionBreakCommand::fromJson(const nlohmann::json& j) {
    Timecode breakFrame = j.value("breakFrame", static_cast<Timecode>(0));
    return std::make_unique<RemoveSectionBreakCommand>(breakFrame);
}

bool EditSectionBreakCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    if (!m_hasPreviousState) {
        for (const auto& sec : timeline->getSections()) {
            if (sec.breakFrame == m_oldBreakFrame) {
                m_previousState = sec;
                m_hasPreviousState = true;
                break;
            }
        }
        if (!m_hasPreviousState) {
            std::cerr << "[EditSectionBreak] FAIL: no break at " << m_oldBreakFrame << std::endl;
            return false;
        }
    }
    bool ok = timeline->editSectionBreak(m_oldBreakFrame, m_newBreakFrame,
                                         m_newColor, m_newFadeSeconds);
    if (!ok) {
        std::cerr << "[EditSectionBreak] FAIL: edit rejected (old=" << m_oldBreakFrame
                  << ", new=" << m_newBreakFrame << ")" << std::endl;
    }
    return ok;
}

bool EditSectionBreakCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline || !m_hasPreviousState) return false;
    // Restore by editing from the new state back to the captured pre-state.
    return timeline->editSectionBreak(m_newBreakFrame, m_previousState.breakFrame,
                                      m_previousState.color,
                                      m_previousState.fadeSeconds);
}

bool EditSectionBreakCommand::redo(Engine& engine) {
    return execute(engine);
}

nlohmann::json EditSectionBreakCommand::toJson() const {
    return {{"type", "EditSectionBreak"},
            {"oldBreakFrame", m_oldBreakFrame},
            {"newBreakFrame", m_newBreakFrame},
            {"newColor", m_newColor},
            {"newFadeSeconds", m_newFadeSeconds}};
}

std::string EditSectionBreakCommand::getDescription() const {
    return "Edit section break " + std::to_string(m_oldBreakFrame) +
           " -> " + std::to_string(m_newBreakFrame);
}

CommandPtr EditSectionBreakCommand::fromJson(const nlohmann::json& j) {
    // Pre-Phase-4 payloads carried a "newName" string; ignored on load.
    Timecode oldBreakFrame = j.value("oldBreakFrame", static_cast<Timecode>(0));
    Timecode newBreakFrame = j.value("newBreakFrame", oldBreakFrame);
    uint32_t newColor = j.value("newColor", static_cast<uint32_t>(0xFF6090C8));
    double newFadeSeconds = j.value("newFadeSeconds", 0.0);
    return std::make_unique<EditSectionBreakCommand>(oldBreakFrame, newBreakFrame,
                                                     newColor, newFadeSeconds);
}

bool SectionGoCommand::execute(Engine& engine) {
    Director* director = engine.getDirector();
    if (!director) {
        std::cerr << "[SectionGo] FAIL: no director" << std::endl;
        return false;
    }
    SectionScheduler* sched = director->getSectionScheduler();
    if (!sched) {
        std::cerr << "[SectionGo] FAIL: no scheduler" << std::endl;
        return false;
    }
    sched->go();
    std::cout << "[SectionGo] OK" << std::endl;
    return true;
}

bool SetClipSectionBehaviorCommand::execute(Engine& engine) {
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipSectionBehavior");
    if (!clip) return false;
    if (!m_previousBehavior.has_value()) m_previousBehavior = clip->sectionBehavior;
    clip->sectionBehavior = m_behavior;
    std::cout << "[SetClipSectionBehavior] OK behavior="
              << (m_behavior == SectionBehavior::Locked ? "Locked" : "Normal")
              << " on track " << m_trackIndex << ", clip " << m_clipIndex << std::endl;
    return true;
}

bool SetClipSectionBehaviorCommand::undo(Engine& engine) {
    if (!m_previousBehavior.has_value()) return false;
    Clip* clip = lookupClipByTrack(engine, m_trackIndex, m_clipIndex, "SetClipSectionBehavior");
    if (!clip) return false;
    clip->sectionBehavior = *m_previousBehavior;
    return true;
}

nlohmann::json SetClipSectionBehaviorCommand::toJson() const {
    return {{"type", "SetClipSectionBehavior"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"behavior", m_behavior == SectionBehavior::Locked ? "Locked" : "Normal"}};
}

std::string SetClipSectionBehaviorCommand::getDescription() const {
    return std::string("Set sectionBehavior -> ") +
           (m_behavior == SectionBehavior::Locked ? "Locked" : "Normal") +
           " on track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr SetClipSectionBehaviorCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string s = j.value("behavior", std::string{"Normal"});
    SectionBehavior b = (s == "Locked") ? SectionBehavior::Locked : SectionBehavior::Normal;
    return std::make_unique<SetClipSectionBehaviorCommand>(trackIndex, clipIndex, b);
}

// Legacy region-style alias. Emits two break points (start + end) so v8
// scripts and projects keep working without rewriting.
bool AddSectionCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    if (m_end <= m_start) {
        std::cerr << "[AddSection] FAIL: bad range [" << m_start << ", " << m_end << ")" << std::endl;
        return false;
    }
    bool okStart = timeline->addSectionBreak(m_start, m_color, 0.0);
    bool okEnd   = timeline->addSectionBreak(m_end, m_color, 0.0);
    if (!okStart && !okEnd) {
        std::cerr << "[AddSection] FAIL: both break points already existed" << std::endl;
        return false;
    }
    std::cout << "[AddSection] OK (legacy) '" << m_name << "' breaks at "
              << m_start << " + " << m_end << std::endl;
    return true;
}

nlohmann::json AddSectionCommand::toJson() const {
    return {{"type", "AddSection"}, {"name", m_name},
            {"start", m_start}, {"end", m_end}, {"color", m_color}};
}

std::string AddSectionCommand::getDescription() const {
    return "Add section (legacy) '" + m_name + "' [" + std::to_string(m_start) + ", " + std::to_string(m_end) + ")";
}

CommandPtr AddSectionCommand::fromJson(const nlohmann::json& j) {
    std::string name = j.value("name", "Section");
    Timecode start = j.value("start", static_cast<Timecode>(0));
    Timecode end = j.value("end", static_cast<Timecode>(0));
    uint32_t color = j.value("color", static_cast<uint32_t>(0xFF6090C8));
    return std::make_unique<AddSectionCommand>(std::move(name), start, end, color);
}

bool AssertSectionCountCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    size_t actual = timeline->getSections().size();
    if (actual == m_count) {
        std::cout << "[AssertSectionCount] OK count=" << actual << std::endl;
        return true;
    }
    std::cerr << "[AssertSectionCount] FAIL: expected " << m_count << ", got " << actual << std::endl;
    return false;
}

nlohmann::json AssertSectionCountCommand::toJson() const {
    return {{"type", "AssertSectionCount"}, {"count", m_count}};
}

std::string AssertSectionCountCommand::getDescription() const {
    return "Assert section count == " + std::to_string(m_count);
}

CommandPtr AssertSectionCountCommand::fromJson(const nlohmann::json& j) {
    size_t count = j.value("count", static_cast<size_t>(0));
    return std::make_unique<AssertSectionCountCommand>(count);
}

bool AssertPlayheadAtFrameCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    FrameNumber actual = timeline->getCurrentFrame();
    FrameNumber diff = actual >= m_frame ? (actual - m_frame) : (m_frame - actual);
    if (diff <= m_tolerance) {
        std::cout << "[AssertPlayheadAtFrame] OK frame=" << actual << std::endl;
        return true;
    }
    std::cerr << "[AssertPlayheadAtFrame] FAIL: expected " << m_frame
              << " (±" << m_tolerance << "), got " << actual << std::endl;
    return false;
}

nlohmann::json AssertPlayheadAtFrameCommand::toJson() const {
    return {{"type", "AssertPlayheadAtFrame"},
            {"frame", m_frame},
            {"tolerance", m_tolerance}};
}

std::string AssertPlayheadAtFrameCommand::getDescription() const {
    return "Assert playhead at frame " + std::to_string(m_frame);
}

CommandPtr AssertPlayheadAtFrameCommand::fromJson(const nlohmann::json& j) {
    FrameNumber frame = j.value("frame", static_cast<FrameNumber>(0));
    FrameNumber tolerance = j.value("tolerance", static_cast<FrameNumber>(0));
    return std::make_unique<AssertPlayheadAtFrameCommand>(frame, tolerance);
}

bool AssertPlaybackStateCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    PlaybackState actual = timeline->getPlaybackState();
    if (actual == m_state) {
        std::cout << "[AssertPlaybackState] OK" << std::endl;
        return true;
    }
    auto stateName = [](PlaybackState s) {
        switch (s) {
            case PlaybackState::Stopped: return "Stopped";
            case PlaybackState::Playing: return "Playing";
            case PlaybackState::Paused:  return "Paused";
        }
        return "?";
    };
    std::cerr << "[AssertPlaybackState] FAIL: expected " << stateName(m_state)
              << ", got " << stateName(actual) << std::endl;
    return false;
}

nlohmann::json AssertPlaybackStateCommand::toJson() const {
    const char* s = "Stopped";
    switch (m_state) {
        case PlaybackState::Stopped: s = "Stopped"; break;
        case PlaybackState::Playing: s = "Playing"; break;
        case PlaybackState::Paused:  s = "Paused";  break;
    }
    return {{"type", "AssertPlaybackState"}, {"state", s}};
}

std::string AssertPlaybackStateCommand::getDescription() const {
    return "Assert playback state";
}

CommandPtr AssertPlaybackStateCommand::fromJson(const nlohmann::json& j) {
    std::string s = j.value("state", std::string{"Stopped"});
    PlaybackState st = PlaybackState::Stopped;
    if (s == "Playing") st = PlaybackState::Playing;
    else if (s == "Paused") st = PlaybackState::Paused;
    return std::make_unique<AssertPlaybackStateCommand>(st);
}

bool AssertClipMediaFrameCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    auto* director = engine.getDirector();
    if (!timeline || !director) {
        std::cerr << "[AssertClipMediaFrame] FAIL: timeline/director unavailable" << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[AssertClipMediaFrame] FAIL: trackIndex " << m_trackIndex
                  << " out of range (tracks=" << tracks.size() << ")" << std::endl;
        return false;
    }
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 ||
        static_cast<size_t>(m_clipIndex) >= track->layers.size()) {
        std::cerr << "[AssertClipMediaFrame] FAIL: clipIndex " << m_clipIndex
                  << " out of range" << std::endl;
        return false;
    }
    entt::entity clipEntity = track->layers[m_clipIndex];
    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cerr << "[AssertClipMediaFrame] FAIL: no Clip component" << std::endl;
        return false;
    }

    auto* timeAuthority = director->getTimeAuthority();
    if (!timeAuthority) {
        std::cerr << "[AssertClipMediaFrame] FAIL: no PlaybackTimeAuthority" << std::endl;
        return false;
    }

    FrameNumber currentFrame = timeline->getCurrentFrame();
    FrameNumber actual = timeAuthority->mapToMediaFrame(clipEntity, *clip, currentFrame);
    FrameNumber diff = actual >= m_expected ? (actual - m_expected) : (m_expected - actual);
    const bool inBracket = (diff <= m_tolerance);
    const bool pass = (m_mode == Mode::Equal) ? inBracket : !inBracket;
    const char* op = (m_mode == Mode::Equal) ? "==" : "!=";
    if (pass) {
        std::cout << "[AssertClipMediaFrame] OK track=" << m_trackIndex
                  << " clip=" << m_clipIndex
                  << " mediaFrame=" << actual
                  << " (" << op << " " << m_expected << " +/-" << m_tolerance << ")"
                  << std::endl;
        return true;
    }
    std::cerr << "[AssertClipMediaFrame] FAIL: track=" << m_trackIndex
              << " clip=" << m_clipIndex
              << " expected " << op << " " << m_expected
              << " (+/-" << m_tolerance << "), got=" << actual << std::endl;
    return false;
}

nlohmann::json AssertClipMediaFrameCommand::toJson() const {
    return {{"type", "AssertClipMediaFrame"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"expected", m_expected},
            {"tolerance", m_tolerance},
            {"mode", (m_mode == Mode::Equal) ? "equal" : "notEqual"}};
}

std::string AssertClipMediaFrameCommand::getDescription() const {
    return "Assert clip media frame for track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr AssertClipMediaFrameCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex  = j.value("clipIndex", 0);
    FrameNumber expected  = j.value("expected", static_cast<FrameNumber>(0));
    FrameNumber tolerance = j.value("tolerance", static_cast<FrameNumber>(0));
    std::string modeStr = j.value("mode", std::string{"equal"});
    AssertClipMediaFrameCommand::Mode mode = (modeStr == "notEqual")
        ? AssertClipMediaFrameCommand::Mode::NotEqual
        : AssertClipMediaFrameCommand::Mode::Equal;
    return std::make_unique<AssertClipMediaFrameCommand>(trackIndex, clipIndex,
                                                          expected, tolerance, mode);
}

bool AssertClipFadeMultiplierCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    auto* director = engine.getDirector();
    if (!timeline || !director) {
        std::cerr << "[AssertClipFadeMultiplier] FAIL: timeline/director unavailable"
                  << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[AssertClipFadeMultiplier] FAIL: trackIndex " << m_trackIndex
                  << " out of range (tracks=" << tracks.size() << ")" << std::endl;
        return false;
    }
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 ||
        static_cast<size_t>(m_clipIndex) >= track->layers.size()) {
        std::cerr << "[AssertClipFadeMultiplier] FAIL: clipIndex " << m_clipIndex
                  << " out of range" << std::endl;
        return false;
    }
    entt::entity clipEntity = track->layers[m_clipIndex];
    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cerr << "[AssertClipFadeMultiplier] FAIL: no Clip component" << std::endl;
        return false;
    }

    auto* timeAuthority = director->getTimeAuthority();
    if (!timeAuthority) {
        std::cerr << "[AssertClipFadeMultiplier] FAIL: no PlaybackTimeAuthority"
                  << std::endl;
        return false;
    }

    const float actual = timeAuthority->computeSectionFadeMultiplier(*clip);
    const float diff = std::fabs(actual - m_expected);
    if (diff <= m_tolerance) {
        std::cout << "[AssertClipFadeMultiplier] OK track=" << m_trackIndex
                  << " clip=" << m_clipIndex
                  << " multiplier=" << actual
                  << " (== " << m_expected << " +/-" << m_tolerance << ")"
                  << std::endl;
        return true;
    }
    std::cerr << "[AssertClipFadeMultiplier] FAIL: track=" << m_trackIndex
              << " clip=" << m_clipIndex
              << " expected " << m_expected << " (+/-" << m_tolerance
              << "), got=" << actual << std::endl;
    return false;
}

nlohmann::json AssertClipFadeMultiplierCommand::toJson() const {
    return {{"type", "AssertClipFadeMultiplier"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"expected", m_expected},
            {"tolerance", m_tolerance}};
}

std::string AssertClipFadeMultiplierCommand::getDescription() const {
    return "Assert clip fade multiplier for track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr AssertClipFadeMultiplierCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex  = j.value("clipIndex", 0);
    float expected  = j.value("expected", 1.0f);
    float tolerance = j.value("tolerance", 0.01f);
    return std::make_unique<AssertClipFadeMultiplierCommand>(trackIndex, clipIndex,
                                                              expected, tolerance);
}

// ============================================================================
// Cue tag commands (Phase A)
// ============================================================================

bool FireCueCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const CueTag* cue = timeline->findCueTag(m_number);
    if (!cue) {
        std::cerr << "[FireCue] WARN: no cue with number " << m_number << std::endl;
        return false;
    }
    timeline->seek(cue->timestamp);
    timeline->play();
    std::cout << "[FireCue] OK number=" << m_number
              << " timestamp=" << cue->timestamp << std::endl;
    return true;
}

nlohmann::json FireCueCommand::toJson() const {
    return {{"type", "FireCue"}, {"number", m_number}};
}

std::string FireCueCommand::getDescription() const {
    return "Fire cue " + std::to_string(m_number);
}

CommandPtr FireCueCommand::fromJson(const nlohmann::json& j) {
    double number = j.value("number", 0.0);
    return std::make_unique<FireCueCommand>(number);
}

bool AddCueAtCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    CueTag tag;
    tag.number = m_number;
    tag.timestamp = m_timestamp;
    tag.label = m_label;
    m_inserted = timeline->addCueTag(std::move(tag));
    if (!m_inserted) {
        std::cerr << "[AddCueAt] FAIL: number " << m_number << " already exists" << std::endl;
        return false;
    }
    std::cout << "[AddCueAt] OK number=" << m_number
              << " timestamp=" << m_timestamp << std::endl;
    return true;
}

bool AddCueAtCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline || !m_inserted) return false;
    bool ok = timeline->removeCueTag(m_number);
    if (ok) m_inserted = false;
    return ok;
}

bool AddCueAtCommand::redo(Engine& engine) {
    return execute(engine);
}

nlohmann::json AddCueAtCommand::toJson() const {
    return {{"type", "AddCueAt"},
            {"number", m_number},
            {"timestamp", m_timestamp},
            {"label", m_label}};
}

std::string AddCueAtCommand::getDescription() const {
    return "Add cue " + std::to_string(m_number) +
           " at " + std::to_string(m_timestamp);
}

CommandPtr AddCueAtCommand::fromJson(const nlohmann::json& j) {
    double number = j.value("number", 0.0);
    Timecode timestamp = j.value("timestamp", static_cast<Timecode>(0));
    std::string label = j.value("label", std::string{});
    return std::make_unique<AddCueAtCommand>(number, timestamp, std::move(label));
}

bool RemoveCueCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    if (!m_captured) {
        const CueTag* live = timeline->findCueTag(m_number);
        if (!live) {
            std::cerr << "[RemoveCue] FAIL: no cue with number " << m_number << std::endl;
            return false;
        }
        m_previousState = *live;
        m_captured = true;
    }
    bool ok = timeline->removeCueTag(m_number);
    if (!ok) {
        std::cerr << "[RemoveCue] FAIL: removeCueTag returned false for "
                  << m_number << std::endl;
    }
    return ok;
}

bool RemoveCueCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline || !m_captured) return false;
    return timeline->addCueTag(m_previousState);
}

bool RemoveCueCommand::redo(Engine& engine) {
    return execute(engine);
}

nlohmann::json RemoveCueCommand::toJson() const {
    return {{"type", "RemoveCue"}, {"number", m_number}};
}

std::string RemoveCueCommand::getDescription() const {
    return "Remove cue " + std::to_string(m_number);
}

CommandPtr RemoveCueCommand::fromJson(const nlohmann::json& j) {
    double number = j.value("number", 0.0);
    return std::make_unique<RemoveCueCommand>(number);
}

bool EditCueCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    if (!m_hasPreviousState) {
        const CueTag* live = timeline->findCueTag(m_oldNumber);
        if (!live) {
            std::cerr << "[EditCue] FAIL: no cue with number " << m_oldNumber << std::endl;
            return false;
        }
        m_previousState = *live;
        m_hasPreviousState = true;
    }
    bool ok = timeline->editCueTag(m_oldNumber, m_newNumber, m_newTimestamp, m_newLabel);
    if (!ok) {
        std::cerr << "[EditCue] FAIL: editCueTag rejected (old=" << m_oldNumber
                  << ", new=" << m_newNumber << ")" << std::endl;
    }
    return ok;
}

bool EditCueCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline || !m_hasPreviousState) return false;
    // Restore by editing from the new state back to the captured pre-state.
    return timeline->editCueTag(m_newNumber, m_previousState.number,
                                m_previousState.timestamp, m_previousState.label);
}

bool EditCueCommand::redo(Engine& engine) {
    return execute(engine);
}

nlohmann::json EditCueCommand::toJson() const {
    return {{"type", "EditCue"},
            {"oldNumber", m_oldNumber},
            {"newNumber", m_newNumber},
            {"newTimestamp", m_newTimestamp},
            {"newLabel", m_newLabel}};
}

std::string EditCueCommand::getDescription() const {
    return "Edit cue " + std::to_string(m_oldNumber) +
           " -> " + std::to_string(m_newNumber);
}

CommandPtr EditCueCommand::fromJson(const nlohmann::json& j) {
    double oldNumber = j.value("oldNumber", 0.0);
    double newNumber = j.value("newNumber", oldNumber);
    Timecode newTimestamp = j.value("newTimestamp", static_cast<Timecode>(0));
    std::string newLabel = j.value("newLabel", std::string{});
    return std::make_unique<EditCueCommand>(oldNumber, newNumber, newTimestamp,
                                            std::move(newLabel));
}

bool AssertCueCountCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    size_t actual = timeline->getCueTags().size();
    if (actual == m_count) {
        std::cout << "[AssertCueCount] OK count=" << actual << std::endl;
        return true;
    }
    std::cerr << "[AssertCueCount] FAIL: expected " << m_count
              << ", got " << actual << std::endl;
    return false;
}

nlohmann::json AssertCueCountCommand::toJson() const {
    return {{"type", "AssertCueCount"}, {"count", m_count}};
}

std::string AssertCueCountCommand::getDescription() const {
    return "Assert cue count == " + std::to_string(m_count);
}

CommandPtr AssertCueCountCommand::fromJson(const nlohmann::json& j) {
    size_t count = j.value("count", static_cast<size_t>(0));
    return std::make_unique<AssertCueCountCommand>(count);
}

bool AssertCueExistsCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    if (timeline->findCueTag(m_number)) {
        std::cout << "[AssertCueExists] OK number=" << m_number << std::endl;
        return true;
    }
    std::cerr << "[AssertCueExists] FAIL: no cue with number " << m_number << std::endl;
    return false;
}

nlohmann::json AssertCueExistsCommand::toJson() const {
    return {{"type", "AssertCueExists"}, {"number", m_number}};
}

std::string AssertCueExistsCommand::getDescription() const {
    return "Assert cue exists: " + std::to_string(m_number);
}

CommandPtr AssertCueExistsCommand::fromJson(const nlohmann::json& j) {
    double number = j.value("number", 0.0);
    return std::make_unique<AssertCueExistsCommand>(number);
}

// ============================================================================
// Ripple time edits
// ============================================================================

bool RippleInsertTimeCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    m_record = timeline->rippleInsertTime(m_insertFrame, m_duration);
    return m_record.success;
}

bool RippleInsertTimeCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    timeline->undoRippleInsertTime(m_record);
    return true;
}

bool RippleInsertTimeCommand::redo(Engine& engine) {
    // Re-run from scratch — undo cleared the captured record, so execute()
    // will repopulate it. (Same approach SetClipOpacityCommand uses.)
    return execute(engine);
}

nlohmann::json RippleInsertTimeCommand::toJson() const {
    return {{"type", "RippleInsertTime"},
            {"insertFrame", m_insertFrame},
            {"durationFrames", m_duration}};
}

std::string RippleInsertTimeCommand::getDescription() const {
    return "Insert " + std::to_string(m_duration) + " frames at " + std::to_string(m_insertFrame);
}

CommandPtr RippleInsertTimeCommand::fromJson(const nlohmann::json& j) {
    FrameNumber insertFrame = j.value("insertFrame", static_cast<FrameNumber>(0));
    FrameNumber durationFrames = j.value("durationFrames", static_cast<FrameNumber>(0));
    return std::make_unique<RippleInsertTimeCommand>(insertFrame, durationFrames);
}

bool RippleDeleteTimeCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    m_record = timeline->rippleDeleteTime(m_rangeStart, m_rangeEnd);
    return m_record.success;
}

bool RippleDeleteTimeCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    timeline->undoRippleDeleteTime(m_record);
    return true;
}

bool RippleDeleteTimeCommand::redo(Engine& engine) {
    return execute(engine);
}

nlohmann::json RippleDeleteTimeCommand::toJson() const {
    return {{"type", "RippleDeleteTime"},
            {"rangeStart", m_rangeStart},
            {"rangeEnd", m_rangeEnd}};
}

std::string RippleDeleteTimeCommand::getDescription() const {
    return "Remove time [" + std::to_string(m_rangeStart) + ", " + std::to_string(m_rangeEnd) + ")";
}

CommandPtr RippleDeleteTimeCommand::fromJson(const nlohmann::json& j) {
    FrameNumber rangeStart = j.value("rangeStart", static_cast<FrameNumber>(0));
    FrameNumber rangeEnd = j.value("rangeEnd", static_cast<FrameNumber>(0));
    return std::make_unique<RippleDeleteTimeCommand>(rangeStart, rangeEnd);
}

bool SleepMsCommand::execute(Engine& engine) {
    std::cout << "[SleepMs] Pausing script for " << m_ms << " ms (editor thread unblocked)" << std::endl;
    if (auto* dispatcher = engine.getCommandDispatcher()) {
        dispatcher->setWaitUntil(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(m_ms));
    }
    return true;
}

nlohmann::json SleepMsCommand::toJson() const {
    return {{"type", "SleepMs"}, {"ms", m_ms}};
}

std::string SleepMsCommand::getDescription() const {
    return "Sleep editor " + std::to_string(m_ms) + " ms";
}

CommandPtr SleepMsCommand::fromJson(const nlohmann::json& j) {
    uint32_t ms = j.value("ms", uint32_t{0});
    return std::make_unique<SleepMsCommand>(ms);
}

bool AssertShowFrameCountAtLeastCommand::execute(Engine& engine) {
    const uint64_t actual = engine.getShowFrameCount();
    if (actual >= m_minCount) {
        std::cout << "[AssertShowFrameCountAtLeast] PASS: show frame count "
                  << actual << " >= " << m_minCount << std::endl;
        return true;
    }
    std::cerr << "[AssertShowFrameCountAtLeast] FAIL: show frame count "
              << actual << " < " << m_minCount << std::endl;
    return false;
}

nlohmann::json AssertShowFrameCountAtLeastCommand::toJson() const {
    return {{"type", "AssertShowFrameCountAtLeast"}, {"minCount", m_minCount}};
}

std::string AssertShowFrameCountAtLeastCommand::getDescription() const {
    return "Assert show frame count >= " + std::to_string(m_minCount);
}

CommandPtr AssertShowFrameCountAtLeastCommand::fromJson(const nlohmann::json& j) {
    uint64_t minCount = j.value("minCount", uint64_t{1});
    return std::make_unique<AssertShowFrameCountAtLeastCommand>(minCount);
}

bool AddSyntheticModelCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    entt::entity modelEntity = registry.create();
    Model& model = registry.emplace<Model>(modelEntity);
    model.name = m_name;
    model.filepath = "";
    model.mesh = createDefaultScreenMesh();
    std::cout << "[AddSyntheticModel] Created model '" << model.name << "' ("
              << model.mesh.vertices.size() << " verts)\n";
    return true;
}

nlohmann::json AddSyntheticModelCommand::toJson() const {
    return {{"type", "AddSyntheticModel"}, {"name", m_name}};
}

std::string AddSyntheticModelCommand::getDescription() const {
    return "Add synthetic model: " + m_name;
}

CommandPtr AddSyntheticModelCommand::fromJson(const nlohmann::json& j) {
    std::string name = j.value("name", std::string{"SyntheticModel"});
    return std::make_unique<AddSyntheticModelCommand>(std::move(name));
}

bool AssertMeshUploadCountCommand::execute(Engine& engine) {
    const uint64_t actual = engine.getMeshUploadCount();
    if (actual == m_expected) {
        std::cout << "[AssertMeshUploadCount] PASS: upload count == " << m_expected << std::endl;
        return true;
    }
    std::cerr << "[AssertMeshUploadCount] FAIL: upload count " << actual
              << " != expected " << m_expected << std::endl;
    return false;
}

nlohmann::json AssertMeshUploadCountCommand::toJson() const {
    return {{"type", "AssertMeshUploadCount"}, {"expected", m_expected}};
}

std::string AssertMeshUploadCountCommand::getDescription() const {
    return "Assert mesh upload count == " + std::to_string(m_expected);
}

CommandPtr AssertMeshUploadCountCommand::fromJson(const nlohmann::json& j) {
    uint64_t expected = j.value("expected", uint64_t{0});
    return std::make_unique<AssertMeshUploadCountCommand>(expected);
}

// ============================================================================
// CreateObjectAnimationLayerCommand (Phase 3.3)
// ============================================================================

bool CreateObjectAnimationLayerCommand::execute(Engine& engine) {
    // Resolve target: first Screen entity in the registry. The user can
    // reassign to any Screen/Prop via the Properties panel after creation.
    auto& registry = engine.getRegistry();
    entt::entity targetEntity = entt::null;
    auto screenView = registry.view<Screen>();
    if (!screenView.empty()) {
        targetEntity = *screenView.begin();
    }

    m_createdEntity = engine.createObjectAnimationLayer(targetEntity,
                                                        m_trackIndex,
                                                        m_startFrame,
                                                        m_duration);
    if (m_createdEntity == entt::null) {
        std::cerr << "[CreateObjectAnimationLayer] FAIL: createObjectAnimationLayer returned null"
                  << std::endl;
        return false;
    }
    std::cout << "[CreateObjectAnimationLayer] OK track=" << m_trackIndex
              << " start=" << m_startFrame
              << " duration=" << m_duration
              << " entity=" << static_cast<uint32_t>(m_createdEntity)
              << " target=" << (targetEntity != entt::null
                                  ? std::to_string(static_cast<uint32_t>(targetEntity))
                                  : "none")
              << std::endl;
    return true;
}

nlohmann::json CreateObjectAnimationLayerCommand::toJson() const {
    return {{"type", "CreateObjectAnimationLayer"},
            {"trackIndex", m_trackIndex},
            {"startFrame", m_startFrame},
            {"duration", m_duration}};
}

std::string CreateObjectAnimationLayerCommand::getDescription() const {
    return "Create OA layer on track " + std::to_string(m_trackIndex) +
           " at " + std::to_string(m_startFrame) +
           " dur=" + std::to_string(m_duration);
}

CommandPtr CreateObjectAnimationLayerCommand::fromJson(const nlohmann::json& j) {
    int trackIndex       = j.value("trackIndex", 0);
    FrameNumber start    = j.value("startFrame", static_cast<FrameNumber>(0));
    FrameNumber duration = j.value("duration", static_cast<FrameNumber>(30));
    return std::make_unique<CreateObjectAnimationLayerCommand>(trackIndex, start, duration);
}

// ============================================================================
// AssertObjectAnimationOutputCommand (Phase 3.3)
// ============================================================================

bool AssertObjectAnimationOutputCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[AssertObjectAnimationOutput] FAIL: no timeline" << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[AssertObjectAnimationOutput] FAIL: trackIndex "
                  << m_trackIndex << " out of range" << std::endl;
        return false;
    }
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_layerIndex < 0 ||
        static_cast<size_t>(m_layerIndex) >= track->layers.size()) {
        std::cerr << "[AssertObjectAnimationOutput] FAIL: layerIndex "
                  << m_layerIndex << " out of range" << std::endl;
        return false;
    }
    entt::entity layerEntity = track->layers[m_layerIndex];
    const auto* out = registry.try_get<ObjectAnimationOutput>(layerEntity);
    if (!out) {
        std::cerr << "[AssertObjectAnimationOutput] FAIL: entity "
                  << static_cast<uint32_t>(layerEntity)
                  << " has no ObjectAnimationOutput component" << std::endl;
        return false;
    }

    // Resolve field name to a float value
    float actual = 0.0f;
    bool  fieldKnown = true;
    if      (m_field == "positionX")   actual = out->positionOverride[0];
    else if (m_field == "positionY")   actual = out->positionOverride[1];
    else if (m_field == "positionZ")   actual = out->positionOverride[2];
    else if (m_field == "rotationX")   actual = out->rotationOverride[0];
    else if (m_field == "rotationY")   actual = out->rotationOverride[1];
    else if (m_field == "rotationZ")   actual = out->rotationOverride[2];
    else if (m_field == "sizeX")       actual = out->sizeOverride[0];
    else if (m_field == "sizeY")       actual = out->sizeOverride[1];
    else if (m_field == "sizeZ")       actual = out->sizeOverride[2];
    else if (m_field == "hasPosition") actual = out->hasPosition ? 1.0f : 0.0f;
    else if (m_field == "hasRotation") actual = out->hasRotation ? 1.0f : 0.0f;
    else if (m_field == "hasSize")     actual = out->hasSize     ? 1.0f : 0.0f;
    else {
        std::cerr << "[AssertObjectAnimationOutput] FAIL: unknown field '" << m_field << "'" << std::endl;
        fieldKnown = false;
    }
    if (!fieldKnown) return false;

    float diff = std::fabs(actual - m_expected);
    if (diff <= m_tolerance) {
        std::cout << "[AssertObjectAnimationOutput] OK track=" << m_trackIndex
                  << " layer=" << m_layerIndex
                  << " field=" << m_field
                  << " value=" << actual
                  << " (== " << m_expected << " +/-" << m_tolerance << ")"
                  << std::endl;
        return true;
    }
    std::cerr << "[AssertObjectAnimationOutput] FAIL: track=" << m_trackIndex
              << " layer=" << m_layerIndex
              << " field=" << m_field
              << " expected=" << m_expected
              << " (+/-" << m_tolerance << ") got=" << actual << std::endl;
    return false;
}

bool AssertScreenSnapshotCommand::execute(Engine& engine) {
    auto* director = engine.getDirector();
    if (!director) {
        std::cerr << "[AssertScreenSnapshot] FAIL: director unavailable" << std::endl;
        return false;
    }
    auto* timeAuthority = director->getTimeAuthority();
    if (!timeAuthority) {
        std::cerr << "[AssertScreenSnapshot] FAIL: no PlaybackTimeAuthority" << std::endl;
        return false;
    }

    bus::SceneSnapshot snapshot;
    timeAuthority->buildSceneSnapshot(snapshot);

    const bus::ScreenSnapshot* found = nullptr;
    for (const auto& ss : snapshot.screens) {
        if (ss.name == m_screenName) {
            found = &ss;
            break;
        }
    }
    if (!found) {
        std::cerr << "[AssertScreenSnapshot] FAIL: no screen named '" << m_screenName
                  << "' in snapshot (screens=" << snapshot.screens.size() << ")" << std::endl;
        return false;
    }

    float actual = 0.0f;
    bool  fieldKnown = true;
    if      (m_field == "positionX") actual = found->position[0];
    else if (m_field == "positionY") actual = found->position[1];
    else if (m_field == "positionZ") actual = found->position[2];
    else if (m_field == "rotationX") actual = found->rotation[0];
    else if (m_field == "rotationY") actual = found->rotation[1];
    else if (m_field == "rotationZ") actual = found->rotation[2];
    else if (m_field == "scaleX")    actual = found->scale[0];
    else if (m_field == "scaleY")    actual = found->scale[1];
    else if (m_field == "scaleZ")    actual = found->scale[2];
    else {
        std::cerr << "[AssertScreenSnapshot] FAIL: unknown field '" << m_field << "'" << std::endl;
        fieldKnown = false;
    }
    if (!fieldKnown) return false;

    float diff = std::fabs(actual - m_expected);
    if (diff <= m_tolerance) {
        std::cout << "[AssertScreenSnapshot] OK screen='" << m_screenName
                  << "' field=" << m_field
                  << " value=" << actual
                  << " (== " << m_expected << " +/-" << m_tolerance << ")"
                  << std::endl;
        return true;
    }
    std::cerr << "[AssertScreenSnapshot] FAIL: screen='" << m_screenName
              << "' field=" << m_field
              << " expected=" << m_expected
              << " (+/-" << m_tolerance << ") got=" << actual << std::endl;
    return false;
}

nlohmann::json AssertScreenSnapshotCommand::toJson() const {
    return {{"type", "AssertScreenSnapshot"},
            {"screenName", m_screenName},
            {"field", m_field},
            {"expected", m_expected},
            {"tolerance", m_tolerance}};
}

std::string AssertScreenSnapshotCommand::getDescription() const {
    return "Assert screen snapshot '" + m_screenName + "' " + m_field +
           "==" + std::to_string(m_expected);
}

CommandPtr AssertScreenSnapshotCommand::fromJson(const nlohmann::json& j) {
    std::string screenName = j.value("screenName", std::string{});
    std::string field      = j.value("field", std::string{"positionX"});
    float expected         = j.value("expected", 0.0f);
    float tolerance        = j.value("tolerance", 0.01f);
    return std::make_unique<AssertScreenSnapshotCommand>(
        std::move(screenName), std::move(field), expected, tolerance);
}

nlohmann::json AssertObjectAnimationOutputCommand::toJson() const {
    return {{"type", "AssertObjectAnimationOutput"},
            {"trackIndex", m_trackIndex},
            {"layerIndex", m_layerIndex},
            {"field", m_field},
            {"expected", m_expected},
            {"tolerance", m_tolerance}};
}

std::string AssertObjectAnimationOutputCommand::getDescription() const {
    return "Assert OA output track=" + std::to_string(m_trackIndex) +
           " layer=" + std::to_string(m_layerIndex) +
           " " + m_field + "==" + std::to_string(m_expected);
}

CommandPtr AssertObjectAnimationOutputCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int layerIndex = j.value("layerIndex", 0);
    std::string field = j.value("field", std::string{"positionX"});
    float expected  = j.value("expected", 0.0f);
    float tolerance = j.value("tolerance", 0.01f);
    return std::make_unique<AssertObjectAnimationOutputCommand>(
        trackIndex, layerIndex, std::move(field), expected, tolerance);
}

} // namespace entity
