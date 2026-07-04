#include "entity/command/Commands.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/core/Engine.hpp"
#include "entity/render/OutputManager.hpp"
#include "entity/components/OutputDisplay.hpp"
#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/dmx/OutboundBridge.hpp"
#include "entity/director/CaptureBroker.hpp"
#include "entity/director/Director.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/director/SectionScheduler.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingAssetOps.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/ObjectAnimationOutput.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/TextLayerState.hpp"
#include "entity/components/RemotePatch.hpp"
#include "entity/components/AudioSource.hpp"
#include "entity/components/TrackUiState.hpp"
#include "entity/remote/RemoteControlStore.hpp"
#include "entity/remote/RemotePatchUtil.hpp"
#include "entity/audio/AudioEngine.hpp"
#include "entity/audio/LoopbackDevice.hpp"
#include "entity/systems/AudioSystem.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/input/InputBus.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/media/ObjLoader.hpp"
#include <imgui.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <optional>
#include <thread>

// Placement read/write helpers promoted for unit-test access (no Engine dep).
#include "entity/command/LayerPlacementOps.hpp"

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
        engine.requestSignalReset();
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
    engine.requestSignalReset();
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
        engine.requestSignalReset();
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
        engine.requestSignalReset();
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

    auto& reg = engine.getRegistry();
    const auto* clip = reg.try_get<Clip>(target);
    if (!clip) {
        std::cerr << "[DuplicateClip] Target has no Clip component" << std::endl;
        return false;
    }
    // Match the legacy duplicateClip layout: place the duplicate
    // immediately after the original on the same track.
    const FrameNumber dstStartFrame = clip->startFrame + clip->duration;

    int srcTrackIdx = -1;
    {
        entt::entity trackEnt = timeline->findTrackForClip(target);
        if (trackEnt != entt::null) {
            const auto& tracks = timeline->getTracks();
            for (size_t i = 0; i < tracks.size(); ++i) {
                if (tracks[i] == trackEnt) { srcTrackIdx = static_cast<int>(i); break; }
            }
        }
    }

    entt::entity newClip = engine.cloneClipEntity(target, dstStartFrame, srcTrackIdx);
    if (newClip == entt::null) return false;
    timeline->setSelectedClip(newClip);
    m_createdEntity = newClip;
    m_executed = true;
    return true;
}

bool DuplicateClipCommand::undo(Engine& engine) {
    if (!m_executed || m_createdEntity == entt::null) {
        std::cerr << "[DuplicateClip] undo called before successful execute" << std::endl;
        return false;
    }
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    if (!engine.getRegistry().valid(m_createdEntity)) {
        // Entity already gone (another command destroyed it). Treat as
        // success — undo is idempotent.
        m_executed = false;
        return true;
    }
    timeline->deleteClip(m_createdEntity);
    timeline->setSelectedClip(entt::null);
    m_executed = false;
    m_createdEntity = entt::null;
    return true;
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

namespace {
// Resolve a (trackIndex, layerIndex) pair to its entity, or entt::null if out
// of range. Mirrors SelectClipCommand's resolution.
entt::entity resolveTrackClip(Timeline& timeline, int trackIndex, int layerIndex) {
    const auto& tracks = timeline.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) return entt::null;
    auto& reg = timeline.getRegistry();
    const auto* track = reg.try_get<TimelineTrack>(tracks[trackIndex]);
    if (!track) return entt::null;
    if (layerIndex < 0 || layerIndex >= static_cast<int>(track->layers.size())) return entt::null;
    return track->layers[layerIndex];
}

// Reverse: the (trackIndex, layerIndex) of an entity, or {-1,-1} if not on any
// track.
std::pair<int, int> findTrackClip(Timeline& timeline, entt::entity e) {
    const auto& tracks = timeline.getTracks();
    auto& reg = timeline.getRegistry();
    for (int t = 0; t < static_cast<int>(tracks.size()); ++t) {
        const auto* track = reg.try_get<TimelineTrack>(tracks[t]);
        if (!track) continue;
        for (int l = 0; l < static_cast<int>(track->layers.size()); ++l) {
            if (track->layers[l] == e) return {t, l};
        }
    }
    return {-1, -1};
}
}  // namespace

bool DeleteClipsCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    // Resolve targets to entities. With explicit (track,layer) pairs, resolve
    // each; otherwise pull the live multi-selection and RECORD its (track,layer)
    // pairs into m_targets so toJson + redo are deterministic.
    std::vector<entt::entity> targets;
    if (!m_targets.empty()) {
        for (const auto& tc : m_targets) {
            entt::entity e = resolveTrackClip(*timeline, tc.first, tc.second);
            if (e != entt::null) targets.push_back(e);
        }
    } else {
        targets = timeline->getSelectedClips();
        m_targets.clear();
        for (entt::entity e : targets) {
            auto tc = findTrackClip(*timeline, e);
            if (tc.first >= 0) m_targets.push_back(tc);
        }
    }
    if (targets.empty()) {
        std::cerr << "[DeleteClips] No clips selected" << std::endl;
        return false;
    }

    // Snapshot every target BEFORE destroying anything (deleting one shifts
    // track->layers, but snapshots capture by value so order doesn't matter).
    // A clip that can't be snapshotted (Signal — no DeletedLayerKind entry) is
    // skipped with a log; the rest still delete.
    m_snapshots.clear();
    std::vector<entt::entity> toDelete;
    for (entt::entity e : targets) {
        auto snap = timeline->snapshotClipForDelete(e);
        if (!snap.valid()) {
            std::cerr << "[DeleteClips] Skipping entity=" << static_cast<uint32_t>(e)
                      << " — not snapshottable (unsupported layer kind, e.g. Signal)"
                      << std::endl;
            continue;
        }
        m_snapshots.push_back(std::move(snap));
        toDelete.push_back(e);
    }
    if (toDelete.empty()) {
        std::cerr << "[DeleteClips] No deletable clips among the selection" << std::endl;
        return false;
    }

    for (entt::entity e : toDelete) {
        timeline->deleteClip(e);
    }
    timeline->setSelectedClip(entt::null);

    m_captured = true;
    return true;
}

bool DeleteClipsCommand::undo(Engine& engine) {
    if (!m_captured) {
        std::cerr << "[DeleteClips] undo called before successful execute" << std::endl;
        return false;
    }
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    // Restore all snapshots as one logical step. Restored entities get fresh
    // ids but land back in their original tracks in start-frame order, so the
    // recorded (track,layer) targets stay valid for a subsequent redo.
    for (const auto& snap : m_snapshots) {
        timeline->restoreDeletedClip(snap);
    }
    return true;
}

bool DeleteClipsCommand::redo(Engine& engine) {
    // Re-run against the recorded (track,layer) targets (stable across the
    // undo's restore). Same re-capture approach as DeleteClipCommand::redo.
    m_captured = false;
    return execute(engine);
}

nlohmann::json DeleteClipsCommand::toJson() const {
    nlohmann::json pairs = nlohmann::json::array();
    for (const auto& tc : m_targets) {
        pairs.push_back({{"track", tc.first}, {"layer", tc.second}});
    }
    return {{"type", "DeleteClips"}, {"clips", pairs}};
}

std::string DeleteClipsCommand::getDescription() const {
    const size_t n = m_targets.empty() ? m_snapshots.size() : m_targets.size();
    return "Delete " + std::to_string(n) + " clips";
}

CommandPtr DeleteClipsCommand::fromJson(const nlohmann::json& j) {
    std::vector<DeleteClipsCommand::TrackClip> targets;
    if (j.contains("clips") && j["clips"].is_array()) {
        for (const auto& c : j["clips"]) {
            const int t = c.value("track", -1);
            const int l = c.value("layer", -1);
            if (t >= 0 && l >= 0) targets.emplace_back(t, l);
        }
    }
    return std::make_unique<DeleteClipsCommand>(std::move(targets));
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

// ============================================================================
// Remote-control patch commands (ADR-0028)
// ============================================================================

// Resolves a (trackIndex, layerIndex) pair to the layer entity, or
// entt::null with a stderr line. Mirrors SetClipOpacityCommand's walk.
static entt::entity resolveLayerEntity(Engine& engine, int trackIndex,
                                       int layerIndex, const char* who) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return entt::null;
    const auto& tracks = timeline->getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << who << ": invalid track index " << trackIndex << "\n";
        return entt::null;
    }
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[static_cast<std::size_t>(trackIndex)]);
    if (!track || layerIndex < 0 || layerIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << who << ": invalid layer index " << layerIndex << "\n";
        return entt::null;
    }
    return track->layers[static_cast<std::size_t>(layerIndex)];
}

bool PatchLayerRemoteCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    const auto e = resolveLayerEntity(engine, m_trackIndex, m_layerIndex,
                                      "PatchLayerRemote");
    if (e == entt::null) return false;
    if (registry.all_of<RemotePatch>(e)) {
        std::cerr << "PatchLayerRemote: layer already patched\n";
        return false;
    }
    auto* store = engine.getRemoteControlStore();
    if (!store) return false;

    std::string id = m_assignedId.empty()
        ? (m_explicitId.empty() ? remote::makeAutoPatchId(registry, e)
                                : m_explicitId)
        : m_assignedId;  // redo path reuses the original id
    if (!remote::isValidPatchId(id) || remote::patchIdInUse(registry, id)) {
        std::cerr << "PatchLayerRemote: invalid or duplicate id '"
                  << id << "'\n";
        return false;
    }
    const int slot = store->allocateSlot(id);
    if (slot < 0) {
        std::cerr << "PatchLayerRemote: store full ("
                  << remote::kMaxRemotePatches << " patches max)\n";
        return false;
    }
    auto& rp = registry.emplace<RemotePatch>(e);
    rp.patchId = id;
    rp.storeSlot = slot;
    rp.armedByDefault = false;
    m_assignedId = id;
    std::cout << "[PatchLayerRemote] '" << id << "' -> slot " << slot << "\n";
    return true;
}

bool PatchLayerRemoteCommand::undo(Engine& engine) {
    auto& registry = engine.getRegistry();
    const auto e = resolveLayerEntity(engine, m_trackIndex, m_layerIndex,
                                      "PatchLayerRemote.undo");
    if (e == entt::null) return false;
    auto* rp = registry.try_get<RemotePatch>(e);
    if (!rp) return false;
    if (auto* store = engine.getRemoteControlStore()) {
        store->freeSlot(rp->storeSlot);
    }
    registry.remove<RemotePatch>(e);
    return true;
}

nlohmann::json PatchLayerRemoteCommand::toJson() const {
    return {{"type", "PatchLayerRemote"}, {"trackIndex", m_trackIndex},
            {"layerIndex", m_layerIndex}, {"patchId", m_explicitId}};
}

std::string PatchLayerRemoteCommand::getDescription() const {
    return "Patch layer to remote control";
}

CommandPtr PatchLayerRemoteCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<PatchLayerRemoteCommand>(
        j.value("trackIndex", 0), j.value("layerIndex", 0),
        j.value("patchId", std::string{}));
}

bool UnpatchLayerRemoteCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    const auto e = resolveLayerEntity(engine, m_trackIndex, m_layerIndex,
                                      "UnpatchLayerRemote");
    if (e == entt::null) return false;
    auto* rp = registry.try_get<RemotePatch>(e);
    if (!rp) {
        std::cerr << "UnpatchLayerRemote: layer is not patched\n";
        return false;
    }
    m_previousId   = rp->patchId;
    m_previousArmed = rp->armedByDefault;
    if (auto* store = engine.getRemoteControlStore()) {
        store->freeSlot(rp->storeSlot);
    }
    registry.remove<RemotePatch>(e);
    std::cout << "[UnpatchLayerRemote] removed patch '" << m_previousId << "'\n";
    return true;
}

bool UnpatchLayerRemoteCommand::undo(Engine& engine) {
    auto& registry = engine.getRegistry();
    const auto e = resolveLayerEntity(engine, m_trackIndex, m_layerIndex,
                                      "UnpatchLayerRemote.undo");
    if (e == entt::null) return false;
    if (registry.all_of<RemotePatch>(e)) return false;  // already patched somehow
    auto* store = engine.getRemoteControlStore();
    if (!store) return false;
    const int slot = store->allocateSlot(m_previousId);
    if (slot < 0) return false;
    auto& rp = registry.emplace<RemotePatch>(e);
    rp.patchId        = m_previousId;
    rp.storeSlot      = slot;
    rp.armedByDefault = m_previousArmed;
    return true;
}

nlohmann::json UnpatchLayerRemoteCommand::toJson() const {
    return {{"type", "UnpatchLayerRemote"}, {"trackIndex", m_trackIndex},
            {"layerIndex", m_layerIndex}};
}

std::string UnpatchLayerRemoteCommand::getDescription() const {
    return "Unpatch layer from remote control";
}

CommandPtr UnpatchLayerRemoteCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<UnpatchLayerRemoteCommand>(
        j.value("trackIndex", 0), j.value("layerIndex", 0));
}

bool RenameRemotePatchCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    const auto e = resolveLayerEntity(engine, m_trackIndex, m_layerIndex,
                                      "RenameRemotePatch");
    if (e == entt::null) return false;
    auto* rp = registry.try_get<RemotePatch>(e);
    if (!rp) {
        std::cerr << "RenameRemotePatch: layer is not patched\n";
        return false;
    }
    if (!remote::isValidPatchId(m_newId) || remote::patchIdInUse(registry, m_newId)) {
        std::cerr << "RenameRemotePatch: invalid or duplicate id '" << m_newId << "'\n";
        return false;
    }
    auto* store = engine.getRemoteControlStore();
    if (!store) return false;
    if (!store->renameSlot(rp->storeSlot, m_newId)) return false;
    m_previousId = rp->patchId;
    rp->patchId  = m_newId;
    std::cout << "[RenameRemotePatch] '" << m_previousId << "' -> '" << m_newId << "'\n";
    return true;
}

bool RenameRemotePatchCommand::undo(Engine& engine) {
    auto& registry = engine.getRegistry();
    const auto e = resolveLayerEntity(engine, m_trackIndex, m_layerIndex,
                                      "RenameRemotePatch.undo");
    if (e == entt::null) return false;
    auto* rp = registry.try_get<RemotePatch>(e);
    if (!rp) return false;
    auto* store = engine.getRemoteControlStore();
    if (!store) return false;
    if (!store->renameSlot(rp->storeSlot, m_previousId)) return false;
    rp->patchId = m_previousId;
    return true;
}

nlohmann::json RenameRemotePatchCommand::toJson() const {
    return {{"type", "RenameRemotePatch"}, {"trackIndex", m_trackIndex},
            {"layerIndex", m_layerIndex}, {"newId", m_newId}};
}

std::string RenameRemotePatchCommand::getDescription() const {
    return "Rename remote patch to '" + m_newId + "'";
}

CommandPtr RenameRemotePatchCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<RenameRemotePatchCommand>(
        j.value("trackIndex", 0), j.value("layerIndex", 0),
        j.value("newId", std::string{}));
}

bool SetRemotePatchArmedCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    const auto e = resolveLayerEntity(engine, m_trackIndex, m_layerIndex,
                                      "SetRemotePatchArmed");
    if (e == entt::null) return false;
    auto* rp = registry.try_get<RemotePatch>(e);
    if (!rp) {
        std::cerr << "SetRemotePatchArmed: layer is not patched\n";
        return false;
    }
    m_previousArmed    = rp->armedByDefault;
    rp->armedByDefault = m_armed;
    if (auto* store = engine.getRemoteControlStore()) {
        store->setEngaged(rp->storeSlot, m_armed);
    }
    return true;
}

bool SetRemotePatchArmedCommand::undo(Engine& engine) {
    auto& registry = engine.getRegistry();
    const auto e = resolveLayerEntity(engine, m_trackIndex, m_layerIndex,
                                      "SetRemotePatchArmed.undo");
    if (e == entt::null) return false;
    auto* rp = registry.try_get<RemotePatch>(e);
    if (!rp) return false;
    rp->armedByDefault = m_previousArmed;
    if (auto* store = engine.getRemoteControlStore()) {
        store->setEngaged(rp->storeSlot, m_previousArmed);
    }
    return true;
}

nlohmann::json SetRemotePatchArmedCommand::toJson() const {
    return {{"type", "SetRemotePatchArmed"}, {"trackIndex", m_trackIndex},
            {"layerIndex", m_layerIndex}, {"armed", m_armed}};
}

std::string SetRemotePatchArmedCommand::getDescription() const {
    return m_armed ? "Arm remote patch" : "Disarm remote patch";
}

CommandPtr SetRemotePatchArmedCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<SetRemotePatchArmedCommand>(
        j.value("trackIndex", 0), j.value("layerIndex", 0),
        j.value("armed", false));
}

// ============================================================================
// RenameTrackCommand
// ============================================================================

bool RenameTrackCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "RenameTrack: invalid track index " << m_trackIndex << "\n";
        return false;
    }
    auto& registry = engine.getRegistry();
    entt::entity trackEntity = tracks[static_cast<std::size_t>(m_trackIndex)];
    auto* uiState = registry.try_get<TrackUiState>(trackEntity);
    m_previousName = uiState ? uiState->name : std::string{};
    if (!uiState) {
        registry.emplace<TrackUiState>(trackEntity, TrackUiState{m_newName, false});
    } else {
        uiState->name = m_newName;
    }
    std::cout << "[RenameTrack] track " << m_trackIndex
              << " '" << m_previousName << "' -> '" << m_newName << "'\n";
    return true;
}

bool RenameTrackCommand::undo(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& registry = engine.getRegistry();
    entt::entity trackEntity = tracks[static_cast<std::size_t>(m_trackIndex)];
    auto* uiState = registry.try_get<TrackUiState>(trackEntity);
    if (!uiState) {
        registry.emplace<TrackUiState>(trackEntity, TrackUiState{m_previousName, false});
    } else {
        uiState->name = m_previousName;
    }
    return true;
}

nlohmann::json RenameTrackCommand::toJson() const {
    return {{"type", "RenameTrack"}, {"trackIndex", m_trackIndex},
            {"newName", m_newName}};
}

std::string RenameTrackCommand::getDescription() const {
    return "Rename track " + std::to_string(m_trackIndex + 1) +
           " to '" + m_newName + "'";
}

CommandPtr RenameTrackCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<RenameTrackCommand>(
        j.value("trackIndex", 0), j.value("newName", std::string{}));
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
        case AnimatableProperty::PositionZ: return "PositionZ";
        case AnimatableProperty::Rotation:  return "Rotation";
        case AnimatableProperty::RotationX: return "RotationX";
        case AnimatableProperty::RotationY: return "RotationY";
        case AnimatableProperty::RotationZ: return "RotationZ";
        case AnimatableProperty::ScaleX:    return "ScaleX";
        case AnimatableProperty::ScaleY:    return "ScaleY";
        case AnimatableProperty::ScaleZ:    return "ScaleZ";
        case AnimatableProperty::Opacity:   return "Opacity";
    }
    return "Opacity";
}

std::optional<AnimatableProperty> parseAnimatableProperty(const std::string& s) {
    if (s == "PositionX") return AnimatableProperty::PositionX;
    if (s == "PositionY") return AnimatableProperty::PositionY;
    if (s == "PositionZ") return AnimatableProperty::PositionZ;
    if (s == "Rotation")  return AnimatableProperty::Rotation;
    if (s == "RotationX") return AnimatableProperty::RotationX;
    if (s == "RotationY") return AnimatableProperty::RotationY;
    if (s == "RotationZ") return AnimatableProperty::RotationZ;
    if (s == "ScaleX")    return AnimatableProperty::ScaleX;
    if (s == "ScaleY")    return AnimatableProperty::ScaleY;
    if (s == "ScaleZ")    return AnimatableProperty::ScaleZ;
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
// SetKeyframeInterpolationCommand
// ============================================================================

namespace {

const char* interpName(InterpolationType t) {
    switch (t) {
        case InterpolationType::Linear:    return "linear";
        case InterpolationType::Step:      return "step";
        case InterpolationType::EaseIn:    return "ease_in";
        case InterpolationType::EaseOut:   return "ease_out";
        case InterpolationType::EaseInOut: return "ease_in_out";
    }
    return "linear";
}

InterpolationType parseInterp(const std::string& s) {
    if (s == "step")        return InterpolationType::Step;
    if (s == "ease_in")     return InterpolationType::EaseIn;
    if (s == "ease_out")    return InterpolationType::EaseOut;
    if (s == "ease_in_out") return InterpolationType::EaseInOut;
    return InterpolationType::Linear;
}

} // namespace

bool SetKeyframeInterpolationCommand::execute(Engine& engine) {
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
    KeyframeTrack* kfTrack = animProps->getTrack(m_property);
    if (!kfTrack) return false;
    Keyframe* kf = kfTrack->getKeyframeAt(m_frame);
    if (!kf) return false;

    // Auto-capture pre-edit state if not provided by the UI path.
    if (!m_hasPreviousState) {
        m_prevInterp  = kf->interpolation;
        m_prevEaseIn  = kf->easeIn;
        m_prevEaseOut = kf->easeOut;
        m_hasPreviousState = true;
    }

    kf->interpolation = m_newInterp;
    kf->easeIn        = m_newEaseIn;
    kf->easeOut       = m_newEaseOut;
    return true;
}

bool SetKeyframeInterpolationCommand::undo(Engine& engine) {
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
    KeyframeTrack* kfTrack = animProps->getTrack(m_property);
    if (!kfTrack) return false;
    Keyframe* kf = kfTrack->getKeyframeAt(m_frame);
    if (!kf) return false;

    kf->interpolation = m_prevInterp;
    kf->easeIn        = m_prevEaseIn;
    kf->easeOut       = m_prevEaseOut;
    return true;
}

nlohmann::json SetKeyframeInterpolationCommand::toJson() const {
    return {
        {"type", "SetKeyframeInterpolation"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex},
        {"property", animatablePropertyName(m_property)},
        {"frame", m_frame},
        {"interpolation", interpName(m_newInterp)},
        {"easeIn", m_newEaseIn},
        {"easeOut", m_newEaseOut}
    };
}

std::string SetKeyframeInterpolationCommand::getDescription() const {
    return std::string("Set keyframe interp ") + animatablePropertyName(m_property) +
           " @ frame " + std::to_string(m_frame) + " -> " + interpName(m_newInterp);
}

CommandPtr SetKeyframeInterpolationCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string propStr = j.value("property", "Opacity");
    AnimatableProperty prop = parseAnimatableProperty(propStr).value_or(AnimatableProperty::Opacity);
    FrameNumber frame = j.value("frame", 0);
    InterpolationType interp = parseInterp(j.value("interpolation", "linear"));
    float easeIn  = j.value("easeIn",  0.42f);
    float easeOut = j.value("easeOut", 0.58f);
    return std::make_unique<SetKeyframeInterpolationCommand>(
        trackIndex, clipIndex, prop, frame, interp, easeIn, easeOut);
}

// ============================================================================
// MoveKeyframeCommand
// ============================================================================

bool MoveKeyframeCommand::execute(Engine& engine) {
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
    KeyframeTrack* kfTrack = animProps->getTrack(m_property);
    if (!kfTrack) return false;

    Keyframe* src = kfTrack->getKeyframeAt(m_oldFrame);
    if (!src) return false;

    // Snapshot before any mutation — also carries the keyframe across the move.
    if (!m_hasPreviousState) {
        m_movedValue   = src->value;
        m_movedInterp  = src->interpolation;
        m_movedEaseIn  = src->easeIn;
        m_movedEaseOut = src->easeOut;
        m_hasPreviousState = true;
    }

    if (m_oldFrame == m_newFrame) return true;
    // Refuse to silently overwrite a distinct keyframe at the destination.
    if (kfTrack->getKeyframeAt(m_newFrame)) {
        std::cerr << "[MoveKeyframe] Destination frame " << m_newFrame
                  << " already occupied" << std::endl;
        return false;
    }

    kfTrack->removeKeyframe(m_oldFrame);
    kfTrack->addKeyframe(m_newFrame, m_movedValue, m_movedInterp);
    // addKeyframe drops easeIn/easeOut — restore them on the inserted keyframe.
    if (Keyframe* moved = kfTrack->getKeyframeAt(m_newFrame)) {
        moved->easeIn  = m_movedEaseIn;
        moved->easeOut = m_movedEaseOut;
    }
    return true;
}

bool MoveKeyframeCommand::undo(Engine& engine) {
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
    KeyframeTrack* kfTrack = animProps->getTrack(m_property);
    if (!kfTrack) return false;

    kfTrack->removeKeyframe(m_newFrame);
    kfTrack->addKeyframe(m_oldFrame, m_movedValue, m_movedInterp);
    if (Keyframe* restored = kfTrack->getKeyframeAt(m_oldFrame)) {
        restored->easeIn  = m_movedEaseIn;
        restored->easeOut = m_movedEaseOut;
    }
    return true;
}

nlohmann::json MoveKeyframeCommand::toJson() const {
    return {
        {"type", "MoveKeyframe"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex},
        {"property", animatablePropertyName(m_property)},
        {"oldFrame", m_oldFrame},
        {"newFrame", m_newFrame}
    };
}

std::string MoveKeyframeCommand::getDescription() const {
    return std::string("Move keyframe ") + animatablePropertyName(m_property) +
           " frame " + std::to_string(m_oldFrame) + " -> " + std::to_string(m_newFrame);
}

CommandPtr MoveKeyframeCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string propStr = j.value("property", "Opacity");
    AnimatableProperty prop = parseAnimatableProperty(propStr).value_or(AnimatableProperty::Opacity);
    FrameNumber oldFrame = j.value("oldFrame", 0);
    FrameNumber newFrame = j.value("newFrame", 0);
    return std::make_unique<MoveKeyframeCommand>(trackIndex, clipIndex, prop, oldFrame, newFrame);
}

// ============================================================================
// RemoveKeyframeCommand
// ============================================================================

bool RemoveKeyframeCommand::execute(Engine& engine) {
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
    KeyframeTrack* kfTrack = animProps->getTrack(m_property);
    if (!kfTrack) return false;
    Keyframe* kf = kfTrack->getKeyframeAt(m_frame);
    if (!kf) return false;

    if (!m_hasPreviousState) {
        m_removedValue   = kf->value;
        m_removedInterp  = kf->interpolation;
        m_removedEaseIn  = kf->easeIn;
        m_removedEaseOut = kf->easeOut;
        m_hasPreviousState = true;
    }

    kfTrack->removeKeyframe(m_frame);
    return true;
}

bool RemoveKeyframeCommand::undo(Engine& engine) {
    if (!m_hasPreviousState) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;
    entt::entity clipEntity = track->layers[m_clipIndex];
    auto& animProps = registry.get_or_emplace<AnimatedProperties>(clipEntity);
    KeyframeTrack& kfTrack = animProps.getOrCreateTrack(m_property);
    kfTrack.addKeyframe(m_frame, m_removedValue, m_removedInterp);
    if (Keyframe* restored = kfTrack.getKeyframeAt(m_frame)) {
        restored->easeIn  = m_removedEaseIn;
        restored->easeOut = m_removedEaseOut;
    }
    return true;
}

nlohmann::json RemoveKeyframeCommand::toJson() const {
    return {
        {"type", "RemoveKeyframe"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex},
        {"property", animatablePropertyName(m_property)},
        {"frame", m_frame}
    };
}

std::string RemoveKeyframeCommand::getDescription() const {
    return std::string("Remove keyframe ") + animatablePropertyName(m_property) +
           " @ frame " + std::to_string(m_frame);
}

CommandPtr RemoveKeyframeCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string propStr = j.value("property", "Opacity");
    AnimatableProperty prop = parseAnimatableProperty(propStr).value_or(AnimatableProperty::Opacity);
    FrameNumber frame = j.value("frame", 0);
    return std::make_unique<RemoveKeyframeCommand>(trackIndex, clipIndex, prop, frame);
}

// ============================================================================
// UpsertEffectKeyframeCommand / RemoveEffectKeyframeCommand
//
// Hash-keyed effect-param keyframe commands (Phase 3, 2026-05-25).
// Operate on EffectAnimatedParameters::tracks, keyed by FNV-1a hash of
// the param name. paramName goes on the wire as a string so the JSON
// stays human-readable and wire-stable across builds.
// ============================================================================

namespace {

EffectAnimatedParameters::NamedTrack*
findEffectTrack(EffectAnimatedParameters& anim, std::uint32_t hash) {
    for (auto& t : anim.tracks) {
        if (t.paramKeyHash == hash) return &t;
    }
    return nullptr;
}

EffectAnimatedParameters::NamedTrack&
getOrCreateEffectTrack(EffectAnimatedParameters& anim, std::uint32_t hash) {
    if (auto* t = findEffectTrack(anim, hash)) return *t;
    anim.tracks.push_back({});
    anim.tracks.back().paramKeyHash = hash;
    return anim.tracks.back();
}

void upsertKeyframeInTrack(EffectAnimatedParameters::NamedTrack& track,
                           FrameNumber frame, float value,
                           InterpolationType interp) {
    // Mirrors KeyframeTrack::addKeyframe: binary-search insert / replace.
    auto it = std::lower_bound(track.keyframes.begin(), track.keyframes.end(), frame,
        [](const Keyframe& k, FrameNumber f) { return k.frame < f; });
    if (it != track.keyframes.end() && it->frame == frame) {
        it->value = value;
        it->interpolation = interp;
        return;
    }
    track.keyframes.insert(it, Keyframe{frame, value, interp});
}

bool removeKeyframeFromTrack(EffectAnimatedParameters::NamedTrack& track,
                             FrameNumber frame) {
    auto it = std::lower_bound(track.keyframes.begin(), track.keyframes.end(), frame,
        [](const Keyframe& k, FrameNumber f) { return k.frame < f; });
    if (it != track.keyframes.end() && it->frame == frame) {
        track.keyframes.erase(it);
        return true;
    }
    return false;
}

const Keyframe* findKeyframeAt(const EffectAnimatedParameters::NamedTrack& track,
                               FrameNumber frame) {
    for (const auto& kf : track.keyframes) {
        if (kf.frame == frame) return &kf;
    }
    return nullptr;
}

} // namespace

bool UpsertEffectKeyframeCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(m_effectEntity) || !registry.all_of<Effect>(m_effectEntity)) {
        std::cerr << "[UpsertEffectKeyframe] invalid effect entity" << std::endl;
        return false;
    }
    auto& anim = registry.get_or_emplace<EffectAnimatedParameters>(m_effectEntity);
    const std::uint32_t hash = effects::fnv1a32(m_paramName);

    // Auto-capture pre-edit state if the caller didn't (script path).
    if (!m_hasPreviousState) {
        if (const auto* t = findEffectTrack(anim, hash)) {
            const Keyframe* existing = findKeyframeAt(*t, m_frame);
            m_previousValue = existing
                ? std::optional<float>(existing->value)
                : std::nullopt;
        } else {
            m_previousValue = std::nullopt;
        }
        m_hasPreviousState = true;
    }

    auto& track = getOrCreateEffectTrack(anim, hash);
    upsertKeyframeInTrack(track, m_frame, m_newValue, m_interp);
    return true;
}

bool UpsertEffectKeyframeCommand::undo(Engine& engine) {
    if (!m_hasPreviousState) return false;
    auto& registry = engine.getRegistry();
    auto* anim = registry.try_get<EffectAnimatedParameters>(m_effectEntity);
    if (!anim) return false;
    const std::uint32_t hash = effects::fnv1a32(m_paramName);
    auto* track = findEffectTrack(*anim, hash);
    if (!track) return false;

    if (m_previousValue.has_value()) {
        // Overwrite with the prior value.
        upsertKeyframeInTrack(*track, m_frame, *m_previousValue, m_interp);
    } else {
        // No keyframe existed before — remove the one we just added.
        removeKeyframeFromTrack(*track, m_frame);
        // If the track is now empty, drop it so EffectAnimatedParameters
        // stays minimal (mirrors what an explicit Remove would have done).
        if (track->keyframes.empty()) {
            anim->tracks.erase(std::remove_if(anim->tracks.begin(), anim->tracks.end(),
                [hash](const EffectAnimatedParameters::NamedTrack& t) {
                    return t.paramKeyHash == hash && t.keyframes.empty();
                }), anim->tracks.end());
        }
    }
    return true;
}

nlohmann::json UpsertEffectKeyframeCommand::toJson() const {
    return {
        {"type", "UpsertEffectKeyframe"},
        {"effectEntity", static_cast<std::uint32_t>(m_effectEntity)},
        {"paramName", m_paramName},
        {"frame", m_frame},
        {"value", m_newValue}
    };
}

std::string UpsertEffectKeyframeCommand::getDescription() const {
    return std::string("Upsert effect keyframe ") + m_paramName +
           " @ frame " + std::to_string(m_frame) +
           " = " + std::to_string(m_newValue);
}

CommandPtr UpsertEffectKeyframeCommand::fromJson(const nlohmann::json& j) {
    const auto effEnt = static_cast<entt::entity>(
        j.value("effectEntity", std::uint32_t{0}));
    auto paramName = j.value("paramName", std::string{});
    FrameNumber frame = j.value("frame", 0);
    float value = j.value("value", 0.0f);
    return std::make_unique<UpsertEffectKeyframeCommand>(effEnt, std::move(paramName), frame, value);
}

bool RemoveEffectKeyframeCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(m_effectEntity)) return false;
    auto* anim = registry.try_get<EffectAnimatedParameters>(m_effectEntity);
    if (!anim) return false;
    const std::uint32_t hash = effects::fnv1a32(m_paramName);
    auto* track = findEffectTrack(*anim, hash);
    if (!track) return false;
    const Keyframe* kf = findKeyframeAt(*track, m_frame);
    if (!kf) return false;

    if (!m_hasPreviousState) {
        m_removedValue    = kf->value;
        m_removedInterp   = kf->interpolation;
        m_removedEaseIn   = kf->easeIn;
        m_removedEaseOut  = kf->easeOut;
        m_hasPreviousState = true;
    }
    removeKeyframeFromTrack(*track, m_frame);
    // Drop empty tracks so the component stays minimal.
    if (track->keyframes.empty()) {
        anim->tracks.erase(std::remove_if(anim->tracks.begin(), anim->tracks.end(),
            [hash](const EffectAnimatedParameters::NamedTrack& t) {
                return t.paramKeyHash == hash && t.keyframes.empty();
            }), anim->tracks.end());
    }
    return true;
}

bool RemoveEffectKeyframeCommand::undo(Engine& engine) {
    if (!m_hasPreviousState) return false;
    auto& registry = engine.getRegistry();
    auto& anim = registry.get_or_emplace<EffectAnimatedParameters>(m_effectEntity);
    const std::uint32_t hash = effects::fnv1a32(m_paramName);
    auto& track = getOrCreateEffectTrack(anim, hash);
    upsertKeyframeInTrack(track, m_frame, m_removedValue, m_removedInterp);
    // Restore bezier tangents — upsertKeyframeInTrack only carries
    // value + interp (matches KeyframeTrack::addKeyframe contract).
    for (auto& kf : track.keyframes) {
        if (kf.frame == m_frame) {
            kf.easeIn  = m_removedEaseIn;
            kf.easeOut = m_removedEaseOut;
            break;
        }
    }
    return true;
}

nlohmann::json RemoveEffectKeyframeCommand::toJson() const {
    return {
        {"type", "RemoveEffectKeyframe"},
        {"effectEntity", static_cast<std::uint32_t>(m_effectEntity)},
        {"paramName", m_paramName},
        {"frame", m_frame}
    };
}

std::string RemoveEffectKeyframeCommand::getDescription() const {
    return std::string("Remove effect keyframe ") + m_paramName +
           " @ frame " + std::to_string(m_frame);
}

CommandPtr RemoveEffectKeyframeCommand::fromJson(const nlohmann::json& j) {
    const auto effEnt = static_cast<entt::entity>(
        j.value("effectEntity", std::uint32_t{0}));
    auto paramName = j.value("paramName", std::string{});
    FrameNumber frame = j.value("frame", 0);
    return std::make_unique<RemoveEffectKeyframeCommand>(effEnt, std::move(paramName), frame);
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
    // Sit on floor by default — position.y is quad center.
    screen.position[1] = screen.size[1] * 0.5f;

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

// Apply a single-screen Direct target to a clip's ContentRoutingRef +
// legacy alias. ADR-0022: ContentRoutingRef points at a library
// ContentRoutingAsset; for a single-screen target we resolve to that
// Screen's auto-direct asset (creating it on demand if the per-tick
// RoutingLibrarySystem reconcile hasn't run yet). The legacy
// `Clip::targetScreen` field stays in sync for one project-format
// version. `screen == entt::null` clears the ref (renders on all
// visible screens).
void applyClipTargetScreen(entt::registry& registry, entt::entity clipEntity,
                            Clip& clip, entt::entity screen) {
    clip.targetScreen = screen;
    routing::setLayerTargetScreen(registry, clipEntity, screen);
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
        applyClipTargetScreen(registry, clipEntity, *clip, entt::null);
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

        applyClipTargetScreen(registry, clipEntity, *clip, foundScreen);
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
    entt::entity clipEntity = track->layers[m_clipIndex];
    if (name == "All Screens" || name.empty()) {
        applyClipTargetScreen(registry, clipEntity, *clip, entt::null);
    } else {
        entt::entity found = entt::null;
        for (auto [e, s] : registry.view<Screen>().each()) {
            if (s.name == name) { found = e; break; }
        }
        if (found == entt::null) return false;
        applyClipTargetScreen(registry, clipEntity, *clip, found);
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
// SetContentRoutingCommand (ADR-0021 M3)
// ============================================================================

namespace {

// Resolve a Screen entity from its name. Returns entt::null if absent —
// caller decides whether to reject or fall back. Mirrors the lookup in
// SetClipTargetScreenCommand for cross-session stability.
entt::entity findScreenByName(entt::registry& registry, const std::string& name) {
    if (name.empty()) return entt::null;
    auto view = registry.view<Screen>();
    for (auto [e, s] : view.each()) {
        if (s.name == name) return e;
    }
    return entt::null;
}

// Capture a clip's current routing into the command's "previous"
// snapshot fields by resolving its ContentRoutingRef → ContentRoutingAsset
// (ADR-0022). Screen entities are read out by name so undo survives
// project reloads. No ref / null asset is captured as an empty-targets
// Direct routing (the legacy "render on all" semantic).
void captureCurrentRouting(entt::registry& registry, entt::entity clipEntity,
                            std::string& outMode,
                            std::vector<SetContentRoutingCommand::TargetSpec>& outTargets) {
    outTargets.clear();
    outMode = "Direct";
    const auto* asset = routing::tryGetAsset(registry, clipEntity);
    if (!asset) return;
    switch (asset->kind) {
        case RouteMode::Direct:  outMode = "Direct";  break;
        case RouteMode::Tiled:   outMode = "Tiled";   break;
        case RouteMode::FeedMap: outMode = "FeedMap"; break;
    }
    for (const auto& t : asset->targets) {
        SetContentRoutingCommand::TargetSpec spec;
        spec.uvRect = t.uvRect;
        if (t.screen != entt::null &&
            registry.valid(t.screen) &&
            registry.all_of<Screen>(t.screen)) {
            spec.screenName = registry.get<Screen>(t.screen).name;
        }
        outTargets.push_back(std::move(spec));
    }
}

// Apply a `(mode, targets)` spec to a clip (ADR-0022). Single-screen
// Direct routings point the layer's ContentRoutingRef at the auto-direct
// asset for that Screen. Anything else (Tiled, multi-target) creates a
// fresh user-named "Custom Routing N" asset and points the ref at it.
// Looks up screens by name; the legacy `Clip::targetScreen` alias stays
// in sync (sole target for Direct+single, entt::null otherwise). Returns
// false if any named screen can't be resolved.
bool applyContentRoutingSpec(entt::registry& registry, entt::entity clipEntity, Clip& clip,
                              const std::string& mode,
                              const std::vector<SetContentRoutingCommand::TargetSpec>& targets) {
    RouteMode kindMode = RouteMode::Direct;
    if (mode == "Tiled")        kindMode = RouteMode::Tiled;
    else if (mode == "FeedMap") kindMode = RouteMode::FeedMap;

    // Resolve each TargetSpec → RouteTarget by looking up screen names.
    std::vector<RouteTarget> resolved;
    resolved.reserve(targets.size());
    for (const auto& spec : targets) {
        RouteTarget t;
        t.uvRect = spec.uvRect;
        if (!spec.screenName.empty()) {
            t.screen = findScreenByName(registry, spec.screenName);
            if (t.screen == entt::null) {
                std::cerr << "[SetContentRouting] Screen not found: "
                          << spec.screenName << std::endl;
                return false;
            }
        }
        resolved.push_back(t);
    }

    // Direct + single screen target with identity uvRect = point at the
    // Screen's auto-direct asset, identical to applyClipTargetScreen.
    const bool routesToAutoDirect =
        kindMode == RouteMode::Direct &&
        resolved.size() == 1 &&
        resolved[0].screen != entt::null &&
        resolved[0].uvRect == std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};

    if (routesToAutoDirect) {
        routing::setLayerTargetScreen(registry, clipEntity, resolved[0].screen);
        clip.targetScreen = resolved[0].screen;
    } else if (resolved.empty()) {
        // Empty targets = legacy "render on all visible".
        auto& ref = registry.get_or_emplace<ContentRoutingRef>(clipEntity);
        ref.asset = entt::null;
        clip.targetScreen = entt::null;
    } else {
        // Multi-target or non-identity uvRect: spin up a custom asset.
        // If the layer already references a user-created asset, reuse it
        // in place instead of leaking new "Custom Routing N" entries on
        // every edit.
        const auto* existingRef = registry.try_get<ContentRoutingRef>(clipEntity);
        bool mutatedExisting = false;
        if (existingRef && existingRef->asset != entt::null &&
            registry.valid(existingRef->asset)) {
            auto* existing = registry.try_get<ContentRoutingAsset>(existingRef->asset);
            if (existing && existing->autoBoundScreen == entt::null) {
                existing->kind = kindMode;
                existing->targets = resolved;
                mutatedExisting = true;
            }
        }
        if (!mutatedExisting) {
            routing::setLayerCustomRouting(registry, clipEntity, kindMode,
                                            std::move(resolved));
        }
        // Legacy alias collapses to "all" for multi-target.
        clip.targetScreen = entt::null;
    }
    return true;
}

} // namespace

bool SetContentRoutingCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[SetContentRouting] No timeline!" << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[SetContentRouting] Invalid track index: " << m_trackIndex << std::endl;
        return false;
    }
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[SetContentRouting] Invalid clip index: " << m_clipIndex << std::endl;
        return false;
    }
    entt::entity clipEntity = track->layers[m_clipIndex];
    auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cerr << "[SetContentRouting] Clip component not found!" << std::endl;
        return false;
    }
    if (!m_previousCaptured) {
        captureCurrentRouting(registry, clipEntity, m_previousMode, m_previousTargets);
        m_previousCaptured = true;
    }
    return applyContentRoutingSpec(registry, clipEntity, *clip, m_mode, m_targets);
}

bool SetContentRoutingCommand::undo(Engine& engine) {
    if (!m_previousCaptured) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& registry = engine.getRegistry();
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;
    auto* clip = registry.try_get<Clip>(track->layers[m_clipIndex]);
    if (!clip) return false;
    return applyContentRoutingSpec(registry, track->layers[m_clipIndex], *clip,
                                    m_previousMode, m_previousTargets);
}

nlohmann::json SetContentRoutingCommand::toJson() const {
    nlohmann::json targetsArr = nlohmann::json::array();
    for (const auto& t : m_targets) {
        targetsArr.push_back({
            {"screenName", t.screenName},
            {"uvRect", {t.uvRect[0], t.uvRect[1], t.uvRect[2], t.uvRect[3]}}
        });
    }
    return {
        {"type", "SetContentRouting"},
        {"trackIndex", m_trackIndex},
        {"clipIndex", m_clipIndex},
        {"mode", m_mode},
        {"targets", std::move(targetsArr)}
    };
}

std::string SetContentRoutingCommand::getDescription() const {
    return "Set content routing for track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex) +
           " to " + m_mode + " with " + std::to_string(m_targets.size()) + " target(s)";
}

CommandPtr SetContentRoutingCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex = j.value("clipIndex", 0);
    std::string mode = j.value("mode", std::string{"Direct"});
    std::vector<SetContentRoutingCommand::TargetSpec> targets;
    if (j.contains("targets") && j["targets"].is_array()) {
        for (const auto& tj : j["targets"]) {
            SetContentRoutingCommand::TargetSpec spec;
            spec.screenName = tj.value("screenName", std::string{});
            if (tj.contains("uvRect") && tj["uvRect"].is_array()) {
                const auto& uv = tj["uvRect"];
                for (std::size_t i = 0; i < spec.uvRect.size() && i < uv.size(); ++i)
                    spec.uvRect[i] = uv[i].get<float>();
            }
            targets.push_back(std::move(spec));
        }
    }
    return std::make_unique<SetContentRoutingCommand>(trackIndex, clipIndex, mode, std::move(targets));
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
// AssertVideoTextureSlotsAllocatedCommand
// ============================================================================

bool AssertVideoTextureSlotsAllocatedCommand::execute(Engine& engine) {
    auto* renderer = engine.getRenderer();
    if (!renderer) {
        std::cerr << "[AssertVideoTextureSlotsAllocated] No renderer" << std::endl;
        return false;
    }
    uint32_t actual = renderer->getVideoTextureSlotsAllocated();
    if (actual == m_count) {
        std::cout << "[AssertVideoTextureSlotsAllocated] OK count=" << actual << std::endl;
        return true;
    }
    std::cerr << "[AssertVideoTextureSlotsAllocated] FAIL: expected " << m_count
              << ", got " << actual << std::endl;
    return false;
}

nlohmann::json AssertVideoTextureSlotsAllocatedCommand::toJson() const {
    return {{"type", "AssertVideoTextureSlotsAllocated"}, {"count", m_count}};
}

std::string AssertVideoTextureSlotsAllocatedCommand::getDescription() const {
    return "Assert video texture slots allocated == " + std::to_string(m_count);
}

CommandPtr AssertVideoTextureSlotsAllocatedCommand::fromJson(const nlohmann::json& j) {
    uint32_t count = j.value("count", static_cast<uint32_t>(0));
    return std::make_unique<AssertVideoTextureSlotsAllocatedCommand>(count);
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
// SetLayerStartFrameCommand / SetLayerDurationCommand
// ============================================================================

namespace {

// Look up a layer entity by (trackIndex, layerIndex). Returns entt::null and
// logs an error on any invalid index. Works for all entity kinds on a track —
// Clip-backed and Layer-only alike. Stays file-local: requires Engine.
entt::entity lookupLayerByTrack(Engine& engine, int trackIndex, int layerIndex, const char* who) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[" << who << "] No timeline" << std::endl;
        return entt::null;
    }
    const auto& tracks = timeline->getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[" << who << "] Invalid track index " << trackIndex << std::endl;
        return entt::null;
    }
    auto& registry = engine.getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(tracks[trackIndex]);
    if (!track || layerIndex < 0 || layerIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[" << who << "] Invalid layer index " << layerIndex << std::endl;
        return entt::null;
    }
    return track->layers[layerIndex];
}

} // anonymous namespace

bool SetLayerStartFrameCommand::execute(Engine& engine) {
    entt::entity e = lookupLayerByTrack(engine, m_trackIndex, m_layerIndex, "SetLayerStartFrame");
    if (e == entt::null) return false;
    if (m_startFrame < 0) {
        std::cerr << "[SetLayerStartFrame] start must be >= 0: " << m_startFrame << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    if (!m_previousStartFrame.has_value()) {
        m_previousStartFrame = command::readLayerStartFrame(registry, e);
    }
    if (!command::writeLayerStartFrame(registry, e, m_startFrame)) return false;
    std::cout << "[SetLayerStartFrame] Track " << m_trackIndex
              << ", Layer " << m_layerIndex
              << " -> " << m_startFrame << std::endl;
    return true;
}

bool SetLayerStartFrameCommand::undo(Engine& engine) {
    if (!m_previousStartFrame.has_value()) return false;
    entt::entity e = lookupLayerByTrack(engine, m_trackIndex, m_layerIndex, "SetLayerStartFrame");
    if (e == entt::null) return false;
    return command::writeLayerStartFrame(engine.getRegistry(), e, *m_previousStartFrame);
}

nlohmann::json SetLayerStartFrameCommand::toJson() const {
    return {{"type",       "SetLayerStartFrame"},
            {"trackIndex", m_trackIndex},
            {"layerIndex", m_layerIndex},
            {"startFrame", m_startFrame}};
}

std::string SetLayerStartFrameCommand::getDescription() const {
    return "Set startFrame -> " + std::to_string(m_startFrame) +
           " on track " + std::to_string(m_trackIndex) +
           ", layer " + std::to_string(m_layerIndex);
}

CommandPtr SetLayerStartFrameCommand::fromJson(const nlohmann::json& j) {
    int trackIndex  = j.value("trackIndex", 0);
    int layerIndex  = j.value("layerIndex", 0);
    FrameNumber sf  = j.value("startFrame", static_cast<FrameNumber>(0));
    return std::make_unique<SetLayerStartFrameCommand>(trackIndex, layerIndex, sf);
}

bool SetLayerDurationCommand::execute(Engine& engine) {
    entt::entity e = lookupLayerByTrack(engine, m_trackIndex, m_layerIndex, "SetLayerDuration");
    if (e == entt::null) return false;
    if (m_duration < 1) {
        std::cerr << "[SetLayerDuration] Duration must be >= 1: " << m_duration << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    if (!m_previousDuration.has_value()) {
        m_previousDuration = command::readLayerDuration(registry, e);
    }
    if (!command::writeLayerDuration(registry, e, m_duration)) return false;
    std::cout << "[SetLayerDuration] Track " << m_trackIndex
              << ", Layer " << m_layerIndex
              << " -> " << m_duration << " frames" << std::endl;
    return true;
}

bool SetLayerDurationCommand::undo(Engine& engine) {
    if (!m_previousDuration.has_value()) return false;
    entt::entity e = lookupLayerByTrack(engine, m_trackIndex, m_layerIndex, "SetLayerDuration");
    if (e == entt::null) return false;
    return command::writeLayerDuration(engine.getRegistry(), e, *m_previousDuration);
}

nlohmann::json SetLayerDurationCommand::toJson() const {
    return {{"type",       "SetLayerDuration"},
            {"trackIndex", m_trackIndex},
            {"layerIndex", m_layerIndex},
            {"duration",   m_duration}};
}

std::string SetLayerDurationCommand::getDescription() const {
    return "Set duration -> " + std::to_string(m_duration) +
           " on track " + std::to_string(m_trackIndex) +
           ", layer " + std::to_string(m_layerIndex);
}

CommandPtr SetLayerDurationCommand::fromJson(const nlohmann::json& j) {
    int trackIndex   = j.value("trackIndex", 0);
    int layerIndex   = j.value("layerIndex", 0);
    FrameNumber dur  = j.value("duration", static_cast<FrameNumber>(1));
    return std::make_unique<SetLayerDurationCommand>(trackIndex, layerIndex, dur);
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
    // breakFrame is an integer timeline frame (v24+ frame-native markers).
    // Pre-Phase-4 payloads carried a "name" string; ignored on load.
    FrameNumber breakFrame = j.value("breakFrame", FrameNumber{0});
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
    FrameNumber breakFrame = j.value("breakFrame", FrameNumber{0});
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
    // breakFrame values are integer timeline frames (v24+ frame-native).
    // Pre-Phase-4 payloads carried a "newName" string; ignored on load.
    FrameNumber oldBreakFrame = j.value("oldBreakFrame", FrameNumber{0});
    FrameNumber newBreakFrame = j.value("newBreakFrame", oldBreakFrame);
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
    // Legacy command: m_start/m_end are microsecond positions. Convert to
    // integer timeline frames (the frame-native section-break storage).
    const FrameNumber startFrame = timeline->timeToFrame(m_start);
    const FrameNumber endFrame   = timeline->timeToFrame(m_end);
    bool okStart = timeline->addSectionBreak(startFrame, m_color, 0.0);
    bool okEnd   = timeline->addSectionBreak(endFrame, m_color, 0.0);
    if (!okStart && !okEnd) {
        std::cerr << "[AddSection] FAIL: both break points already existed" << std::endl;
        return false;
    }
    std::cout << "[AddSection] OK (legacy) '" << m_name << "' breaks at "
              << startFrame << " + " << endFrame << std::endl;
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
// AssertRemoteRenderOpacityCommand (ADR-0028 Phase 9)
// ============================================================================

bool AssertRemoteRenderOpacityCommand::execute(Engine& engine) {
    auto* director = engine.getDirector();
    if (!director) {
        std::cerr << "[AssertRemoteRenderOpacity] FAIL: director unavailable\n";
        return false;
    }
    auto* timeAuthority = director->getTimeAuthority();
    if (!timeAuthority) {
        std::cerr << "[AssertRemoteRenderOpacity] FAIL: no PlaybackTimeAuthority\n";
        return false;
    }

    auto& registry = engine.getRegistry();
    int slot = -1;
    for (auto [e, rp] : registry.view<RemotePatch>().each()) {
        if (rp.patchId == m_patchId) { slot = rp.storeSlot; break; }
    }
    if (slot < 0) {
        std::cerr << "[AssertRemoteRenderOpacity] FAIL: no patch '"
                  << m_patchId << "'\n";
        return false;
    }

    // Diagnostic: log the raw store state so timing/wiring issues are visible.
    if (auto* store = engine.getRemoteControlStore()) {
        const auto s = store->sample(slot);
        std::cout << "[AssertRemoteRenderOpacity] store slot=" << slot
                  << " engaged=" << s.engaged
                  << " presentMask=" << s.presentMask
                  << " opacity=" << s.get(remote::RemoteParam::Opacity) << "\n";
    }

    bus::SceneSnapshot snap;
    timeAuthority->buildSceneSnapshot(snap);
    bus::RenderFrame rf;
    timeAuthority->buildRenderFrame(rf, snap);

    for (const auto& cl : rf.contentLayers) {
        if (cl.remoteSlot != slot) continue;
        const float got = cl.opacity;
        if (std::fabs(got - m_expected) <= m_tolerance) {
            std::cout << "[AssertRemoteRenderOpacity] OK: " << m_patchId
                      << " opacity " << got << "\n";
            return true;
        }
        std::cerr << "[AssertRemoteRenderOpacity] FAIL: " << m_patchId
                  << " opacity " << got << " expected " << m_expected
                  << " +/- " << m_tolerance << "\n";
        return false;
    }
    std::cerr << "[AssertRemoteRenderOpacity] FAIL: no contentLayer for '"
              << m_patchId << "'\n";
    return false;
}

nlohmann::json AssertRemoteRenderOpacityCommand::toJson() const {
    return {{"type", "AssertRemoteRenderOpacity"},
            {"patchId", m_patchId},
            {"expected", m_expected},
            {"tolerance", m_tolerance}};
}

std::string AssertRemoteRenderOpacityCommand::getDescription() const {
    return "Assert remote render opacity for patch '" + m_patchId + "'";
}

CommandPtr AssertRemoteRenderOpacityCommand::fromJson(const nlohmann::json& j) {
    std::string patchId   = j.value("patchId", std::string{});
    float expected        = j.value("expected", 1.0f);
    float tolerance       = j.value("tolerance", 0.01f);
    return std::make_unique<AssertRemoteRenderOpacityCommand>(patchId, expected,
                                                              tolerance);
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
    timeline->seekToFrame(cue->frame);
    timeline->play();
    std::cout << "[FireCue] OK number=" << m_number
              << " frame=" << cue->frame << std::endl;
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

bool ArmCueCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const CueTag* cue = timeline->findCueTag(m_number);
    if (!cue) {
        std::cerr << "[ArmCue] WARN: no cue with number " << m_number << std::endl;
        return false;
    }
    // Arming = selecting: DecodeSystem/AudioSystem prewarm the selected
    // cue's clips (warm-set armed window). No seek, no transport change.
    timeline->setSelectedCueNumber(m_number);
    std::cout << "[ArmCue] OK number=" << m_number
              << " frame=" << cue->frame << std::endl;
    return true;
}

nlohmann::json ArmCueCommand::toJson() const {
    return {{"type", "ArmCue"}, {"number", m_number}};
}

std::string ArmCueCommand::getDescription() const {
    return "Arm cue " + std::to_string(m_number);
}

CommandPtr ArmCueCommand::fromJson(const nlohmann::json& j) {
    double number = j.value("number", 0.0);
    return std::make_unique<ArmCueCommand>(number);
}

// ============================================================================
// DMX commands (#13 — Phase D: DMX/Art-Net plugin)
// ============================================================================

bool SetDmxOutCommand::execute(Engine& /*engine*/) {
    entity::dmx::OutboundBridge::instance().setChannel(
        m_universe, m_channel, m_value, /*priority*/100);
    std::cout << "[SetDmxOut] universe=" << m_universe
              << " channel=" << m_channel
              << " value=" << static_cast<int>(m_value) << std::endl;
    return true;
}

nlohmann::json SetDmxOutCommand::toJson() const {
    return {{"type", "SetDmxOut"},
            {"universe", m_universe},
            {"channel",  m_channel},
            {"value",    m_value}};
}

std::string SetDmxOutCommand::getDescription() const {
    return "Set DMX out u=" + std::to_string(m_universe) +
           " ch=" + std::to_string(m_channel) +
           " v=" + std::to_string(m_value);
}

CommandPtr SetDmxOutCommand::fromJson(const nlohmann::json& j) {
    const auto universe = static_cast<std::uint16_t>(j.value("universe", 0));
    const auto channel  = static_cast<std::uint16_t>(j.value("channel",  1));
    const auto value    = static_cast<std::uint8_t>(
        std::clamp(j.value("value", 0), 0, 255));
    return std::make_unique<SetDmxOutCommand>(universe, channel, value);
}

bool SetDmxMappingsJsonCommand::execute(Engine& engine) {
    auto* pm = engine.getProjectManager();
    if (!pm) return false;
    if (!m_previousCaptured) {
        m_previousJson = pm->getDmxMappingsJson();
        m_previousCaptured = true;
    }
    pm->setDmxMappingsJson(m_newJson);
    return true;
}

bool SetDmxMappingsJsonCommand::undo(Engine& engine) {
    auto* pm = engine.getProjectManager();
    if (!pm || !m_previousCaptured) return false;
    pm->setDmxMappingsJson(m_previousJson);
    return true;
}

bool SetDmxMappingsJsonCommand::redo(Engine& engine) {
    auto* pm = engine.getProjectManager();
    if (!pm) return false;
    pm->setDmxMappingsJson(m_newJson);
    return true;
}

nlohmann::json SetDmxMappingsJsonCommand::toJson() const {
    return {{"type", "SetDmxMappingsJson"}, {"json", m_newJson}};
}

CommandPtr SetDmxMappingsJsonCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<SetDmxMappingsJsonCommand>(
        j.value("json", std::string{}));
}

bool AddCueAtCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    CueTag tag;
    tag.number = m_number;
    tag.frame = m_frame;
    tag.label = m_label;
    m_inserted = timeline->addCueTag(std::move(tag));
    if (!m_inserted) {
        std::cerr << "[AddCueAt] FAIL: number " << m_number << " already exists" << std::endl;
        return false;
    }
    std::cout << "[AddCueAt] OK number=" << m_number
              << " frame=" << m_frame << std::endl;
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
            {"frame", m_frame},
            {"label", m_label}};
}

std::string AddCueAtCommand::getDescription() const {
    return "Add cue " + std::to_string(m_number) +
           " at frame " + std::to_string(m_frame);
}

CommandPtr AddCueAtCommand::fromJson(const nlohmann::json& j) {
    double number = j.value("number", 0.0);
    FrameNumber frame = j.value("frame", FrameNumber{0});
    std::string label = j.value("label", std::string{});
    return std::make_unique<AddCueAtCommand>(number, frame, std::move(label));
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
    bool ok = timeline->editCueTag(m_oldNumber, m_newNumber, m_newFrame, m_newLabel);
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
                                m_previousState.frame, m_previousState.label);
}

bool EditCueCommand::redo(Engine& engine) {
    return execute(engine);
}

nlohmann::json EditCueCommand::toJson() const {
    return {{"type", "EditCue"},
            {"oldNumber", m_oldNumber},
            {"newNumber", m_newNumber},
            {"newFrame", m_newFrame},
            {"newLabel", m_newLabel}};
}

std::string EditCueCommand::getDescription() const {
    return "Edit cue " + std::to_string(m_oldNumber) +
           " -> " + std::to_string(m_newNumber);
}

CommandPtr EditCueCommand::fromJson(const nlohmann::json& j) {
    double oldNumber = j.value("oldNumber", 0.0);
    double newNumber = j.value("newNumber", oldNumber);
    FrameNumber newFrame = j.value("newFrame", FrameNumber{0});
    std::string newLabel = j.value("newLabel", std::string{});
    return std::make_unique<EditCueCommand>(oldNumber, newNumber, newFrame,
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

bool StallEditorCommand::execute(Engine& engine) {
    (void)engine;
    std::cout << "[StallEditor] Blocking editor thread for " << m_ms
              << " ms (heartbeat goes stale; show-thread stall fallback engages)"
              << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(m_ms));
    return true;
}

nlohmann::json StallEditorCommand::toJson() const {
    return {{"type", "StallEditor"}, {"ms", m_ms}};
}

std::string StallEditorCommand::getDescription() const {
    return "Stall editor thread " + std::to_string(m_ms) + " ms";
}

CommandPtr StallEditorCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<StallEditorCommand>(j.value("ms", 500u));
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
    // Apply optional sectionBehavior (default Normal — no registry touch needed).
    if (m_sectionBehavior != SectionBehavior::Normal) {
        auto& oal = engine.getRegistry().get<ObjectAnimationLayer>(m_createdEntity);
        oal.sectionBehavior = m_sectionBehavior;
    }
    std::cout << "[CreateObjectAnimationLayer] OK track=" << m_trackIndex
              << " start=" << m_startFrame
              << " duration=" << m_duration
              << " behavior=" << (m_sectionBehavior == SectionBehavior::Locked ? "Locked" : "Normal")
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
            {"duration", m_duration},
            {"sectionBehavior", m_sectionBehavior == SectionBehavior::Locked ? "Locked" : "Normal"}};
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
    std::string behStr   = j.value("sectionBehavior", std::string{"Normal"});
    SectionBehavior beh  = (behStr == "Locked") ? SectionBehavior::Locked : SectionBehavior::Normal;
    return std::make_unique<CreateObjectAnimationLayerCommand>(trackIndex, start, duration, beh);
}

// ============================================================================
// CreateMuncherLayerCommand (first generative layer kind)
// ============================================================================

bool CreateMuncherLayerCommand::execute(Engine& engine) {
    // Resolve target: first Screen entity in the registry. Same policy as
    // CreateObjectAnimationLayerCommand — the user retargets via Properties.
    auto& registry = engine.getRegistry();
    entt::entity targetEntity = entt::null;
    auto screenView = registry.view<Screen>();
    if (!screenView.empty()) {
        targetEntity = *screenView.begin();
    }

    m_createdEntity = engine.createMuncherLayer(targetEntity,
                                                m_trackIndex,
                                                m_startFrame,
                                                m_duration);
    if (m_createdEntity == entt::null) {
        std::cerr << "[CreateMuncherLayer] FAIL: createMuncherLayer returned null" << std::endl;
        return false;
    }
    std::cout << "[CreateMuncherLayer] OK track=" << m_trackIndex
              << " start=" << m_startFrame
              << " duration=" << m_duration
              << " entity=" << static_cast<uint32_t>(m_createdEntity)
              << " target=" << (targetEntity != entt::null
                                  ? std::to_string(static_cast<uint32_t>(targetEntity))
                                  : "none")
              << std::endl;
    return true;
}

nlohmann::json CreateMuncherLayerCommand::toJson() const {
    return {{"type", "CreateMuncherLayer"},
            {"trackIndex", m_trackIndex},
            {"startFrame", m_startFrame},
            {"duration", m_duration}};
}

std::string CreateMuncherLayerCommand::getDescription() const {
    return "Create Muncher generative layer on track " + std::to_string(m_trackIndex) +
           " at " + std::to_string(m_startFrame) +
           " dur=" + std::to_string(m_duration);
}

CommandPtr CreateMuncherLayerCommand::fromJson(const nlohmann::json& j) {
    int trackIndex       = j.value("trackIndex", 0);
    FrameNumber start    = j.value("startFrame", static_cast<FrameNumber>(0));
    FrameNumber duration = j.value("duration", static_cast<FrameNumber>(300));
    return std::make_unique<CreateMuncherLayerCommand>(trackIndex, start, duration);
}

// ============================================================================
// CreateTextLayerCommand
// ============================================================================

bool CreateTextLayerCommand::execute(Engine& engine) {
    // Resolve target: first Screen entity in the registry. Same policy as
    // CreateMuncherLayerCommand — the user retargets via Properties.
    auto& registry = engine.getRegistry();
    entt::entity targetEntity = entt::null;
    auto screenView = registry.view<Screen>();
    if (!screenView.empty()) {
        targetEntity = *screenView.begin();
    }

    m_createdEntity = engine.createTextLayer(targetEntity,
                                             m_trackIndex,
                                             m_startFrame,
                                             m_duration);
    if (m_createdEntity == entt::null) {
        std::cerr << "[CreateTextLayer] FAIL: createTextLayer returned null" << std::endl;
        return false;
    }
    std::cout << "[CreateTextLayer] OK track=" << m_trackIndex
              << " start=" << m_startFrame
              << " duration=" << m_duration
              << " entity=" << static_cast<uint32_t>(m_createdEntity)
              << " target=" << (targetEntity != entt::null
                                  ? std::to_string(static_cast<uint32_t>(targetEntity))
                                  : "none")
              << std::endl;
    return true;
}

nlohmann::json CreateTextLayerCommand::toJson() const {
    return {{"type", "CreateTextLayer"},
            {"trackIndex", m_trackIndex},
            {"startFrame", m_startFrame},
            {"duration", m_duration}};
}

std::string CreateTextLayerCommand::getDescription() const {
    return "Create Text generative layer on track " + std::to_string(m_trackIndex) +
           " at " + std::to_string(m_startFrame) +
           " dur=" + std::to_string(m_duration);
}

CommandPtr CreateTextLayerCommand::fromJson(const nlohmann::json& j) {
    int trackIndex       = j.value("trackIndex", 0);
    FrameNumber start    = j.value("startFrame", static_cast<FrameNumber>(0));
    FrameNumber duration = j.value("duration", static_cast<FrameNumber>(300));
    return std::make_unique<CreateTextLayerCommand>(trackIndex, start, duration);
}

// ============================================================================
// SetInputChannelCommand
// ============================================================================

bool SetInputChannelCommand::execute(Engine& engine) {
    auto* bus = engine.getInputBus();
    if (!bus) {
        std::cerr << "[SetInputChannel] FAIL: no InputBus on engine" << std::endl;
        return false;
    }
    bus->setFloat(m_channel, m_value);
    std::cout << "[SetInputChannel] " << m_channel << " = " << m_value << std::endl;
    return true;
}

nlohmann::json SetInputChannelCommand::toJson() const {
    return {{"type", "SetInputChannel"},
            {"channel", m_channel},
            {"value", m_value}};
}

std::string SetInputChannelCommand::getDescription() const {
    return "Set input channel " + m_channel + " = " + std::to_string(m_value);
}

CommandPtr SetInputChannelCommand::fromJson(const nlohmann::json& j) {
    std::string channel = j.value("channel", std::string{});
    float       value   = j.value("value",   0.0f);
    return std::make_unique<SetInputChannelCommand>(std::move(channel), value);
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

// ============================================================================
// Per-layer effects (issue #54)
// ============================================================================

namespace {

// Resolve a paramName to its slot index in the kind's positional schema.
// Returns -1 if no match — caller treats that as a soft skip.
int findParamSlotByName(const effects::EffectKind& kind, const std::string& name) {
    for (std::size_t i = 0; i < kind.params.size(); ++i) {
        if (kind.params[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

bool AddEffectCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(m_layerEntity)) {
        std::cerr << "[AddEffect] layerEntity invalid" << std::endl;
        return false;
    }

    auto* kindRegistry = engine.getEffectKindRegistry();
    if (!kindRegistry) {
        std::cerr << "[AddEffect] EffectKindRegistry not bound on engine" << std::endl;
        return false;
    }
    const std::uint32_t kindHash = effects::fnv1a32(m_kindStableId);
    const effects::EffectKind* kind = kindRegistry->find(kindHash);
    if (!kind) {
        std::cerr << "[AddEffect] Unknown effect kind: " << m_kindStableId << std::endl;
        return false;
    }

    // Create the effect entity and attach the standard component bundle.
    entt::entity fxEnt = registry.create();
    Effect& fx = registry.emplace<Effect>(fxEnt);
    fx.kindId     = kindHash;
    fx.enabled    = true;
    fx.graphX     = 0.0f;
    fx.graphY     = 0.0f;
    fx.ownerLayer = m_layerEntity;

    EffectParameters& params = registry.emplace<EffectParameters>(fxEnt);
    params.values.reserve(kind->params.size());
    for (const auto& schema : kind->params) {
        params.values.push_back(schema.defaultValue);
    }

    registry.emplace<EffectAnimatedParameters>(fxEnt);

    // Append to the layer's EffectChain (create the component if absent).
    auto& chain = registry.get_or_emplace<EffectChain>(m_layerEntity);
    chain.nodes.push_back(fxEnt);
    if (chain.outputNode == entt::null) {
        chain.outputNode = fxEnt;
    }

    m_createdEffectEntity = fxEnt;
    return true;
}

bool AddEffectCommand::undo(Engine& engine) {
    if (!m_createdEffectEntity.has_value()) return false;
    auto& registry = engine.getRegistry();
    if (auto* chain = registry.try_get<EffectChain>(m_layerEntity)) {
        chain->nodes.erase(
            std::remove(chain->nodes.begin(), chain->nodes.end(), *m_createdEffectEntity),
            chain->nodes.end());
        if (chain->outputNode == *m_createdEffectEntity) {
            chain->outputNode = chain->nodes.empty() ? entt::null : chain->nodes.back();
        }
        if (chain->nodes.empty()) {
            registry.remove<EffectChain>(m_layerEntity);
        }
    }
    if (registry.valid(*m_createdEffectEntity)) {
        registry.destroy(*m_createdEffectEntity);
    }
    m_createdEffectEntity.reset();
    return true;
}

nlohmann::json AddEffectCommand::toJson() const {
    return {{"type", "AddEffect"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"kindStableId", m_kindStableId}};
}

std::string AddEffectCommand::getDescription() const {
    return "Add effect " + m_kindStableId;
}

CommandPtr AddEffectCommand::fromJson(const nlohmann::json& j) {
    const auto layerEntity = static_cast<entt::entity>(
        j.value("layerEntity", std::uint32_t{0}));
    auto kind = j.value("kindStableId", std::string{});
    return std::make_unique<AddEffectCommand>(layerEntity, std::move(kind));
}

bool RemoveEffectCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(m_layerEntity) || !registry.valid(m_effectEntity)) {
        return false;
    }
    auto* chain = registry.try_get<EffectChain>(m_layerEntity);
    if (!chain) return false;

    // Capture state for undo.
    auto it = std::find(chain->nodes.begin(), chain->nodes.end(), m_effectEntity);
    if (it == chain->nodes.end()) return false;
    m_savedPositionInChain = static_cast<std::size_t>(it - chain->nodes.begin());

    if (auto* fx = registry.try_get<Effect>(m_effectEntity)) {
        m_savedKindId  = fx->kindId;
        m_savedEnabled = fx->enabled;
        m_savedGraphX  = fx->graphX;
        m_savedGraphY  = fx->graphY;
    }
    if (auto* params = registry.try_get<EffectParameters>(m_effectEntity)) {
        m_savedParams = params->values;
    }

    chain->nodes.erase(it);
    if (chain->outputNode == m_effectEntity) {
        chain->outputNode = chain->nodes.empty() ? entt::null : chain->nodes.back();
    }
    registry.destroy(m_effectEntity);
    if (chain->nodes.empty()) {
        registry.remove<EffectChain>(m_layerEntity);
    }
    m_savedExecuted = true;
    return true;
}

bool RemoveEffectCommand::undo(Engine& engine) {
    if (!m_savedExecuted) return false;
    auto& registry = engine.getRegistry();
    if (!registry.valid(m_layerEntity)) return false;

    // Re-create the effect entity. EnTT may not give us the same numeric ID
    // as before — that's fine for undo since the chain refers to the new
    // entity. JSON-recorded references to the old ID would dangle though.
    entt::entity fxEnt = registry.create();
    auto& fx = registry.emplace<Effect>(fxEnt);
    fx.kindId     = m_savedKindId;
    fx.enabled    = m_savedEnabled;
    fx.graphX     = m_savedGraphX;
    fx.graphY     = m_savedGraphY;
    fx.ownerLayer = m_layerEntity;
    auto& params = registry.emplace<EffectParameters>(fxEnt);
    params.values = m_savedParams;
    registry.emplace<EffectAnimatedParameters>(fxEnt);

    auto& chain = registry.get_or_emplace<EffectChain>(m_layerEntity);
    const std::size_t insertAt = std::min(m_savedPositionInChain, chain.nodes.size());
    chain.nodes.insert(chain.nodes.begin() + insertAt, fxEnt);
    if (chain.outputNode == entt::null) chain.outputNode = fxEnt;
    return true;
}

nlohmann::json RemoveEffectCommand::toJson() const {
    return {{"type", "RemoveEffect"},
            {"layerEntity",  static_cast<std::uint32_t>(m_layerEntity)},
            {"effectEntity", static_cast<std::uint32_t>(m_effectEntity)}};
}

std::string RemoveEffectCommand::getDescription() const {
    return "Remove effect";
}

CommandPtr RemoveEffectCommand::fromJson(const nlohmann::json& j) {
    const auto layerEntity  = static_cast<entt::entity>(j.value("layerEntity",  std::uint32_t{0}));
    const auto effectEntity = static_cast<entt::entity>(j.value("effectEntity", std::uint32_t{0}));
    return std::make_unique<RemoveEffectCommand>(layerEntity, effectEntity);
}

bool SetEffectEnabledCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(m_effectEntity)) return false;
    auto* fx = registry.try_get<Effect>(m_effectEntity);
    if (!fx) return false;
    if (!m_previousEnabled.has_value()) m_previousEnabled = fx->enabled;
    fx->enabled = m_enabled;
    return true;
}

bool SetEffectEnabledCommand::undo(Engine& engine) {
    if (!m_previousEnabled.has_value()) return false;
    auto& registry = engine.getRegistry();
    auto* fx = registry.try_get<Effect>(m_effectEntity);
    if (!fx) return false;
    fx->enabled = *m_previousEnabled;
    return true;
}

nlohmann::json SetEffectEnabledCommand::toJson() const {
    return {{"type", "SetEffectEnabled"},
            {"effectEntity", static_cast<std::uint32_t>(m_effectEntity)},
            {"enabled", m_enabled}};
}

std::string SetEffectEnabledCommand::getDescription() const {
    return m_enabled ? "Enable effect" : "Disable effect";
}

CommandPtr SetEffectEnabledCommand::fromJson(const nlohmann::json& j) {
    const auto effectEntity = static_cast<entt::entity>(j.value("effectEntity", std::uint32_t{0}));
    const bool enabled = j.value("enabled", true);
    return std::make_unique<SetEffectEnabledCommand>(effectEntity, enabled);
}

bool SetEffectFloatParamCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(m_effectEntity)) return false;
    auto* fx = registry.try_get<Effect>(m_effectEntity);
    auto* params = registry.try_get<EffectParameters>(m_effectEntity);
    if (!fx || !params) return false;

    auto* kindRegistry = engine.getEffectKindRegistry();
    if (!kindRegistry) return false;
    const effects::EffectKind* kind = kindRegistry->find(fx->kindId);
    if (!kind) return false;

    const int slot = findParamSlotByName(*kind, m_paramName);
    if (slot < 0) return false;
    if (static_cast<std::size_t>(slot) >= params->values.size()) {
        // Schema grew since the effect was created — extend with defaults.
        const std::size_t oldSize = params->values.size();
        params->values.resize(kind->params.size());
        for (std::size_t i = oldSize; i < params->values.size(); ++i) {
            params->values[i] = kind->params[i].defaultValue;
        }
    }
    if (!m_previousValue.has_value()) {
        m_previousValue = params->values[slot].f4[0];
    }
    params->values[slot] = ParamValue::makeFloat(m_value);
    return true;
}

bool SetEffectFloatParamCommand::undo(Engine& engine) {
    if (!m_previousValue.has_value()) return false;
    auto& registry = engine.getRegistry();
    auto* fx = registry.try_get<Effect>(m_effectEntity);
    auto* params = registry.try_get<EffectParameters>(m_effectEntity);
    if (!fx || !params) return false;
    auto* kindRegistry = engine.getEffectKindRegistry();
    if (!kindRegistry) return false;
    const effects::EffectKind* kind = kindRegistry->find(fx->kindId);
    if (!kind) return false;
    const int slot = findParamSlotByName(*kind, m_paramName);
    if (slot < 0 || static_cast<std::size_t>(slot) >= params->values.size()) return false;
    params->values[slot] = ParamValue::makeFloat(*m_previousValue);
    return true;
}

nlohmann::json SetEffectFloatParamCommand::toJson() const {
    return {{"type", "SetEffectFloatParam"},
            {"effectEntity", static_cast<std::uint32_t>(m_effectEntity)},
            {"paramName", m_paramName},
            {"value", m_value}};
}

std::string SetEffectFloatParamCommand::getDescription() const {
    return "Set effect param " + m_paramName;
}

CommandPtr SetEffectFloatParamCommand::fromJson(const nlohmann::json& j) {
    const auto effectEntity = static_cast<entt::entity>(j.value("effectEntity", std::uint32_t{0}));
    auto paramName = j.value("paramName", std::string{});
    float value = j.value("value", 0.0f);
    return std::make_unique<SetEffectFloatParamCommand>(
        effectEntity, std::move(paramName), value);
}

// ============================================================================
// Clip Clipboard — Copy / Cut / Paste
// ============================================================================

namespace {

entt::entity resolveClipForClipboard(Engine& engine, const std::optional<uint32_t>& entityId) {
    if (entityId.has_value()) {
        auto e = static_cast<entt::entity>(*entityId);
        return engine.getRegistry().valid(e) ? e : entt::null;
    }
    if (auto* timeline = engine.getTimeline()) {
        return timeline->getSelectedClip();
    }
    return entt::null;
}

}  // namespace

namespace {
// Build a clipboard group from a set of source entities. Snapshots each
// copyable entity, skips un-copyable kinds (OA / Signal) with a log, and sets
// relativeStartOffset on every entry relative to the group's minimum start so
// paste preserves the relative layout. Returns an empty vector if nothing was
// copyable.
std::vector<ClipClipboardSnapshot>
buildClipboardGroup(Engine& engine, const std::vector<entt::entity>& sources) {
    std::vector<ClipClipboardSnapshot> group;
    auto* timeline = engine.getTimeline();
    if (!timeline) return group;

    FrameNumber minStart = std::numeric_limits<FrameNumber>::max();
    for (entt::entity e : sources) {
        auto snap = engine.snapshotClipForClipboard(e);
        if (!snap.valid) {
            std::cerr << "[CopyClip] Skipping entity=" << static_cast<uint32_t>(e)
                      << " - not copyable (Signal / not on a track)" << std::endl;
            continue;
        }
        minStart = std::min(minStart, snap.base.startFrame);
        group.push_back(std::move(snap));
    }
    if (group.empty()) return group;
    for (auto& snap : group) {
        snap.relativeStartOffset = snap.base.startFrame - minStart;
    }
    return group;
}
}  // namespace

bool CopyClipCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;

    // Group copy when the multi-selection has >1 AND no explicit single
    // entity was requested; otherwise the single-clip path.
    std::vector<entt::entity> sources;
    if (!m_entityId.has_value() && timeline->getSelectedClips().size() > 1) {
        sources = timeline->getSelectedClips();
    } else {
        entt::entity target = resolveClipForClipboard(engine, m_entityId);
        if (target == entt::null) {
            std::cerr << "[CopyClip] No clip to copy" << std::endl;
            return false;
        }
        sources.push_back(target);
    }

    auto group = buildClipboardGroup(engine, sources);
    if (group.empty()) {
        std::cerr << "[CopyClip] Nothing copyable in the selection" << std::endl;
        return false;
    }
    const size_t n = group.size();
    engine.setClipClipboard(std::move(group));
    std::cout << "[CopyClip] Clipboard updated with " << n << " clip(s)" << std::endl;
    return true;
}

nlohmann::json CopyClipCommand::toJson() const {
    nlohmann::json j = {{"type", "CopyClip"}};
    if (m_entityId.has_value()) j["entityId"] = *m_entityId;
    return j;
}

CommandPtr CopyClipCommand::fromJson(const nlohmann::json& j) {
    if (j.contains("entityId")) {
        return std::make_unique<CopyClipCommand>(j["entityId"].get<uint32_t>());
    }
    return std::make_unique<CopyClipCommand>();
}

bool CutClipCommand::execute(Engine& engine) {
    entt::entity target = resolveClipForClipboard(engine, m_entityId);
    if (target == entt::null) {
        std::cerr << "[CutClip] No clip to cut" << std::endl;
        return false;
    }
    auto snap = engine.snapshotClipForClipboard(target);
    if (!snap.valid) {
        std::cerr << "[CutClip] Failed to snapshot clip entity="
                  << static_cast<uint32_t>(target) << std::endl;
        return false;
    }
    engine.setClipClipboard(std::move(snap));

    // Enqueue an undoable DeleteClipCommand so the Cut composes cleanly
    // with Ctrl+Z. The clipboard write itself is OOB for undo.
    auto* dispatcher = engine.getCommandDispatcher();
    if (dispatcher) {
        dispatcher->enqueue(std::make_unique<DeleteClipCommand>(
            static_cast<uint32_t>(target)));
    } else {
        // No dispatcher (test harness?) — perform the delete inline.
        if (auto* timeline = engine.getTimeline()) {
            timeline->deleteClip(target);
            timeline->setSelectedClip(entt::null);
        }
    }
    std::cout << "[CutClip] Cut entity=" << static_cast<uint32_t>(target) << std::endl;
    return true;
}

nlohmann::json CutClipCommand::toJson() const {
    nlohmann::json j = {{"type", "CutClip"}};
    if (m_entityId.has_value()) j["entityId"] = *m_entityId;
    return j;
}

CommandPtr CutClipCommand::fromJson(const nlohmann::json& j) {
    if (j.contains("entityId")) {
        return std::make_unique<CutClipCommand>(j["entityId"].get<uint32_t>());
    }
    return std::make_unique<CutClipCommand>();
}

bool PasteClipCommand::execute(Engine& engine) {
    const auto& group = engine.clipClipboard();
    if (group.empty()) {
        std::cerr << "[PasteClip] Clipboard empty" << std::endl;
        return false;
    }
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    auto& reg = engine.getRegistry();
    const int trackCount = static_cast<int>(timeline->getTracks().size());

    // Resolve the anchor frame. Explicit wins (right-click menu), else playhead.
    const FrameNumber anchorFrame = m_targetFrame.has_value()
        ? *m_targetFrame
        : timeline->getCurrentFrame();

    // Resolve a base destination track for the FIRST entry. Explicit
    // constructor argument wins; otherwise fall back to the selected clip's
    // track, then the group's first source track, then track 0. Each entry's
    // own track is then computed relative to the group's first source track so
    // a multi-track group keeps its relative track spread.
    int baseTrackIdx = -1;
    if (m_targetTrack.has_value()) {
        baseTrackIdx = *m_targetTrack;
    } else {
        entt::entity sel = timeline->getSelectedClip();
        if (sel != entt::null && reg.valid(sel)) {
            entt::entity selTrack = timeline->findTrackForClip(sel);
            if (selTrack != entt::null) {
                const auto& tracks = timeline->getTracks();
                for (size_t i = 0; i < tracks.size(); ++i) {
                    if (tracks[i] == selTrack) { baseTrackIdx = static_cast<int>(i); break; }
                }
            }
        }
        if (baseTrackIdx < 0) baseTrackIdx = group.front().sourceTrackIndex;
    }
    if (baseTrackIdx < 0) baseTrackIdx = 0;

    // The group's first source track is the reference for per-entry track
    // offsets (preserves a multi-track group's vertical spread on paste).
    const int refSourceTrack = group.front().sourceTrackIndex;

    m_createdEntities.clear();
    entt::entity lastCreated = entt::null;
    for (const auto& snap : group) {
        const FrameNumber dstFrame = anchorFrame + snap.relativeStartOffset;
        int dstTrack = baseTrackIdx;
        if (refSourceTrack >= 0 && snap.sourceTrackIndex >= 0) {
            dstTrack = baseTrackIdx + (snap.sourceTrackIndex - refSourceTrack);
        }
        // Clamp to the track range so an out-of-range offset doesn't drop the
        // entry (materialize also clamps, but clamp here for the log clarity).
        if (dstTrack < 0) dstTrack = 0;
        if (dstTrack >= trackCount) dstTrack = trackCount - 1;

        entt::entity created = engine.materializeClipFromSnapshot(snap, dstFrame, dstTrack);
        if (created == entt::null) {
            std::cerr << "[PasteClip] Skipped one entry (materialize failed) at frame="
                      << dstFrame << " track=" << dstTrack << std::endl;
            continue;
        }
        m_createdEntities.push_back(created);
        lastCreated = created;
    }

    if (m_createdEntities.empty()) {
        std::cerr << "[PasteClip] Nothing pasted (all entries failed)" << std::endl;
        return false;
    }

    // Select the whole pasted group; primary = last created.
    timeline->setSelection(m_createdEntities);
    (void)lastCreated;
    m_executed = true;
    std::cout << "[PasteClip] pasted " << m_createdEntities.size()
              << " clip(s) at anchor frame=" << anchorFrame << std::endl;
    return true;
}

nlohmann::json PasteClipCommand::toJson() const {
    nlohmann::json j = {{"type", "PasteClip"}};
    if (m_targetFrame.has_value()) j["targetFrame"] = *m_targetFrame;
    if (m_targetTrack.has_value()) j["trackIndex"] = *m_targetTrack;
    return j;
}

CommandPtr PasteClipCommand::fromJson(const nlohmann::json& j) {
    if (j.contains("targetFrame") && j.contains("trackIndex")) {
        return std::make_unique<PasteClipCommand>(
            j["targetFrame"].get<FrameNumber>(),
            j["trackIndex"].get<int>());
    }
    return std::make_unique<PasteClipCommand>();
}

bool PasteClipCommand::undo(Engine& engine) {
    if (!m_executed || m_createdEntities.empty()) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    // Remove every pasted entity as one step.
    for (entt::entity e : m_createdEntities) {
        if (engine.getRegistry().valid(e)) {
            timeline->deleteClip(e);
        }
    }
    timeline->setSelectedClip(entt::null);
    m_executed = false;
    m_createdEntities.clear();
    return true;
}

// ============================================================================
// SetClipMedia — atomic media swap (filepath + media metadata)
// ============================================================================

bool SetClipMediaCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[SetClipMedia] Track index out of range: " << m_trackIndex << std::endl;
        return false;
    }
    auto& reg = engine.getRegistry();
    auto* track = reg.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) {
        std::cerr << "[SetClipMedia] Clip index out of range: " << m_clipIndex << std::endl;
        return false;
    }
    entt::entity clipEnt = track->layers[m_clipIndex];
    auto* clip = reg.try_get<Clip>(clipEnt);
    if (!clip) {
        std::cerr << "[SetClipMedia] Entity has no Clip component" << std::endl;
        return false;
    }

    // Detect + open the new media. Done synchronously on the editor
    // thread; takes ~30-60ms on a cold-cache open. Acceptable cost for
    // an explicit user action.
    const std::string openPath = engine.resolveMediaPath(m_filepath);
    MediaType mediaType = detectMediaType(openPath);
    if (mediaType == MediaType::Unknown) {
        std::cerr << "[SetClipMedia] Unsupported media type for: " << openPath << std::endl;
        return false;
    }
    auto newDecoder = createDecoder(mediaType);
    if (!newDecoder) {
        std::cerr << "[SetClipMedia] Failed to create decoder for: " << openPath << std::endl;
        return false;
    }
    Result openResult = newDecoder->open(openPath);
    if (openResult != Result::Success) {
        std::cerr << "[SetClipMedia] Failed to open: " << openPath << std::endl;
        return false;
    }

    // Snapshot prev state on first execute (so undo restores faithfully
    // even after subsequent edits in this command's redo chain).
    if (!m_captured) {
        m_prevFilepath         = clip->filepath;
        m_prevMediaType        = clip->mediaType;
        m_prevWidth            = clip->width;
        m_prevHeight           = clip->height;
        m_prevFramerate        = clip->framerate;
        m_prevTotalMediaFrames = clip->totalMediaFrames;
        m_prevMediaStartFrame  = clip->mediaStartFrame;
        m_prevMediaOutFrame    = clip->mediaOutFrame;
        m_prevHasAlpha         = clip->hasAlpha;
        m_captured             = true;
    }

    // Tear down the existing worker — joins its thread so the new
    // filepath is the only file the next worker can see. Synchronous
    // on the editor thread; can take 50+ms for a 4K ProRes mid-decode
    // join, acceptable for explicit user action.
    engine.destroyClipWorker(clipEnt);
    clip->loaded = false;

    // Update Clip atomically with the new media's metadata. mediaStart
    // resets to 0 (indexed into previous source). mediaOutFrame is set
    // to the new source's last frame — matches the onMediaDroppedOnTimeline
    // pattern (Engine.cpp:3662). Leaving -1 (the "end of source" sentinel)
    // is a latent bug on the show side: ClipCatalogEntry doesn't carry
    // totalMediaFrames, so clipFromCatalog reconstructs a Clip with
    // totalMediaFrames=0, and effectivePlaybackLength falls back to 1 —
    // every frame maps to source frame 0 ("frozen on first frame").
    clip->filepath         = m_filepath;
    clip->mediaType        = mediaType;
    clip->width            = newDecoder->getWidth();
    clip->height           = newDecoder->getHeight();
    clip->framerate        = newDecoder->getFrameRate();
    clip->totalMediaFrames = newDecoder->getDuration();
    clip->hasAlpha         = newDecoder->hasAlpha();
    clip->mediaStartFrame  = 0;
    clip->mediaOutFrame    = clip->totalMediaFrames > 0
        ? clip->totalMediaFrames - 1 : -1;
    clip->decoding         = false;

    // Hand the freshly-opened decoder to ClipDecodeState. The old
    // ClipDecodeState's decoder is destroyed by emplace_or_replace —
    // safe because the DecodeWorker holds its own decoder, not this one.
    auto& state = reg.emplace_or_replace<ClipDecodeState>(clipEnt);
    state.decoder = std::move(newDecoder);
    state.frame   = std::make_unique<DecodedFrame>();
    state.frame->allocate(clip->width, clip->height);
    state.lastDecodedFrame = UINT32_MAX;

    // Mark loaded so DecodeSystem creates a fresh worker next tick
    // (which will spin up its own decoder against the new filepath).
    clip->loaded = true;

    // VideoTexture size may have changed — refresh markers and null
    // the in-flight frame pointer because destroyClipWorker just evicted
    // the FrameCache, so any AVFrame* currentFrame was pointing into is
    // gone. Renderer will pick up new frames once the fresh worker
    // produces them.
    if (auto* vt = reg.try_get<VideoTexture>(clipEnt)) {
        vt->width        = clip->width;
        vt->height       = clip->height;
        vt->currentFrame = nullptr;
        vt->needsUpload  = false;
        // Keep the existing descriptor slot — the renderer's video pool
        // entry continues to back this clip; the upload path resizes the
        // GPU texture on the first frame from the new decoder.
    }

    std::cout << "[SetClipMedia] entity=" << static_cast<uint32_t>(clipEnt)
              << " '" << m_prevFilepath << "' -> '" << m_filepath
              << "' (" << clip->width << "x" << clip->height
              << " @ " << clip->framerate << ")" << std::endl;
    return true;
}

bool SetClipMediaCommand::undo(Engine& engine) {
    if (!m_captured) return false;
    auto* timeline = engine.getTimeline();
    if (!timeline) return false;
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || m_trackIndex >= static_cast<int>(tracks.size())) return false;
    auto& reg = engine.getRegistry();
    auto* track = reg.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 || m_clipIndex >= static_cast<int>(track->layers.size())) return false;
    entt::entity clipEnt = track->layers[m_clipIndex];
    auto* clip = reg.try_get<Clip>(clipEnt);
    if (!clip) return false;

    // Reopen the previous media. If it's gone we still revert the Clip
    // fields so subsequent ops see a coherent state — the user is the
    // one who has to fix the missing file.
    const std::string openPath = engine.resolveMediaPath(m_prevFilepath);
    auto newDecoder = createDecoder(m_prevMediaType);
    bool reopenOk = false;
    if (newDecoder && newDecoder->open(openPath) == Result::Success) {
        reopenOk = true;
    }

    // Same teardown as execute(): join the current worker so the next
    // worker opens against the restored filepath.
    engine.destroyClipWorker(clipEnt);
    clip->loaded   = false;
    clip->filepath = m_prevFilepath;
    clip->mediaType = m_prevMediaType;
    clip->width  = m_prevWidth;
    clip->height = m_prevHeight;
    clip->framerate        = m_prevFramerate;
    clip->totalMediaFrames = m_prevTotalMediaFrames;
    clip->mediaStartFrame  = m_prevMediaStartFrame;
    clip->mediaOutFrame    = m_prevMediaOutFrame;
    clip->hasAlpha         = m_prevHasAlpha;
    clip->decoding         = false;

    if (reopenOk) {
        auto& state = reg.emplace_or_replace<ClipDecodeState>(clipEnt);
        state.decoder = std::move(newDecoder);
        state.frame   = std::make_unique<DecodedFrame>();
        state.frame->allocate(clip->width, clip->height);
        state.lastDecodedFrame = UINT32_MAX;
        clip->loaded = true;
        if (auto* vt = reg.try_get<VideoTexture>(clipEnt)) {
            vt->width        = clip->width;
            vt->height       = clip->height;
            vt->currentFrame = nullptr;
            vt->needsUpload  = false;
        }
        std::cout << "[SetClipMedia] undo: restored '" << m_prevFilepath << "'" << std::endl;
    } else {
        std::cerr << "[SetClipMedia] undo: could not reopen previous media '"
                  << m_prevFilepath << "'" << std::endl;
    }
    return true;
}

nlohmann::json SetClipMediaCommand::toJson() const {
    return {{"type", "SetClipMedia"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"filepath", m_filepath}};
}

std::string SetClipMediaCommand::getDescription() const {
    return "Set clip media: " + m_filepath;
}

CommandPtr SetClipMediaCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<SetClipMediaCommand>(
        j.value("trackIndex", 0),
        j.value("clipIndex", 0),
        j.value("filepath", std::string{}));
}

// ============================================================================
// Generative Layer Property Commands
// ============================================================================

bool SetGenerativeRenderSizeCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(m_layerEntity)) return false;
    auto* gen = registry.try_get<GenerativeLayer>(m_layerEntity);
    if (!gen) return false;
    if (!m_prevWidth.has_value()) {
        m_prevWidth  = gen->renderWidth;
        m_prevHeight = gen->renderHeight;
    }
    gen->renderWidth  = m_width;
    gen->renderHeight = m_height;
    // For Text layers, re-bake at new resolution.
    if (auto* tls = registry.try_get<TextLayerState>(m_layerEntity)) {
        tls->dirty = true;
    }
    return true;
}

bool SetGenerativeRenderSizeCommand::undo(Engine& engine) {
    if (!m_prevWidth.has_value()) return false;
    auto& registry = engine.getRegistry();
    auto* gen = registry.try_get<GenerativeLayer>(m_layerEntity);
    if (!gen) return false;
    gen->renderWidth  = *m_prevWidth;
    gen->renderHeight = *m_prevHeight;
    if (auto* tls = registry.try_get<TextLayerState>(m_layerEntity)) {
        tls->dirty = true;
    }
    return true;
}

nlohmann::json SetGenerativeRenderSizeCommand::toJson() const {
    return {{"type", "SetGenerativeRenderSize"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"width",  m_width},
            {"height", m_height}};
}

std::string SetGenerativeRenderSizeCommand::getDescription() const {
    return "Set generative layer render size";
}

CommandPtr SetGenerativeRenderSizeCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    const auto w = j.value("width",  std::uint32_t{1920});
    const auto h = j.value("height", std::uint32_t{1080});
    return std::make_unique<SetGenerativeRenderSizeCommand>(e, w, h);
}

// ============================================================================
// Text Layer Property Commands
// ============================================================================

static TextLayerState* getTextLayerState(Engine& engine, entt::entity e) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(e)) return nullptr;
    return registry.try_get<TextLayerState>(e);
}

bool SetTextContentCommand::execute(Engine& engine) {
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    if (!m_previousText.has_value()) m_previousText = s->text;
    s->text  = m_text;
    s->dirty = true;
    return true;
}

bool SetTextContentCommand::undo(Engine& engine) {
    if (!m_previousText.has_value()) return false;
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    s->text  = *m_previousText;
    s->dirty = true;
    return true;
}

nlohmann::json SetTextContentCommand::toJson() const {
    return {{"type", "SetTextContent"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"text", m_text}};
}

std::string SetTextContentCommand::getDescription() const { return "Set text content"; }

CommandPtr SetTextContentCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    return std::make_unique<SetTextContentCommand>(e, j.value("text", std::string{}));
}

// ----------------------------------------------------------------------------

bool SetTextFontCommand::execute(Engine& engine) {
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    if (!m_previousFont.has_value()) m_previousFont = s->fontFamily;
    s->fontFamily = m_fontFamily;
    s->dirty      = true;
    return true;
}

bool SetTextFontCommand::undo(Engine& engine) {
    if (!m_previousFont.has_value()) return false;
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    s->fontFamily = *m_previousFont;
    s->dirty      = true;
    return true;
}

nlohmann::json SetTextFontCommand::toJson() const {
    return {{"type", "SetTextFont"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"fontFamily", m_fontFamily}};
}

std::string SetTextFontCommand::getDescription() const { return "Set text font"; }

CommandPtr SetTextFontCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    return std::make_unique<SetTextFontCommand>(e, j.value("fontFamily", std::string{"Segoe UI"}));
}

// ----------------------------------------------------------------------------

bool SetTextFontSizeCommand::execute(Engine& engine) {
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    if (!m_previousFontSize.has_value()) m_previousFontSize = s->fontSize;
    s->fontSize = m_fontSize;
    s->dirty    = true;
    return true;
}

bool SetTextFontSizeCommand::undo(Engine& engine) {
    if (!m_previousFontSize.has_value()) return false;
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    s->fontSize = *m_previousFontSize;
    s->dirty    = true;
    return true;
}

nlohmann::json SetTextFontSizeCommand::toJson() const {
    return {{"type", "SetTextFontSize"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"fontSize", m_fontSize}};
}

std::string SetTextFontSizeCommand::getDescription() const { return "Set text font size"; }

CommandPtr SetTextFontSizeCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    return std::make_unique<SetTextFontSizeCommand>(e, j.value("fontSize", 96.0f));
}

// ----------------------------------------------------------------------------

bool SetTextColorCommand::execute(Engine& engine) {
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    if (!m_prevR.has_value()) {
        m_prevR = s->color.r; m_prevG = s->color.g;
        m_prevB = s->color.b; m_prevA = s->color.a;
    }
    s->color = {m_r, m_g, m_b, m_a};
    s->dirty = true;
    return true;
}

bool SetTextColorCommand::undo(Engine& engine) {
    if (!m_prevR.has_value()) return false;
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    s->color = {*m_prevR, *m_prevG, *m_prevB, *m_prevA};
    s->dirty = true;
    return true;
}

nlohmann::json SetTextColorCommand::toJson() const {
    return {{"type", "SetTextColor"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"r", m_r}, {"g", m_g}, {"b", m_b}, {"a", m_a}};
}

std::string SetTextColorCommand::getDescription() const { return "Set text color"; }

CommandPtr SetTextColorCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    return std::make_unique<SetTextColorCommand>(
        e,
        j.value("r", 1.0f), j.value("g", 1.0f), j.value("b", 1.0f), j.value("a", 1.0f));
}

// ----------------------------------------------------------------------------

bool SetTextAlignmentCommand::execute(Engine& engine) {
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    if (!m_previousAlignment.has_value())
        m_previousAlignment = static_cast<uint8_t>(s->alignment);
    s->alignment = static_cast<TextAlignment>(m_alignment);
    s->dirty     = true;
    return true;
}

bool SetTextAlignmentCommand::undo(Engine& engine) {
    if (!m_previousAlignment.has_value()) return false;
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    s->alignment = static_cast<TextAlignment>(*m_previousAlignment);
    s->dirty     = true;
    return true;
}

nlohmann::json SetTextAlignmentCommand::toJson() const {
    return {{"type", "SetTextAlignment"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"alignment", m_alignment}};
}

std::string SetTextAlignmentCommand::getDescription() const { return "Set text alignment"; }

CommandPtr SetTextAlignmentCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    return std::make_unique<SetTextAlignmentCommand>(
        e, static_cast<uint8_t>(j.value("alignment", 1)));
}

// ----------------------------------------------------------------------------

bool SetTextBoldCommand::execute(Engine& engine) {
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    if (!m_previousBold.has_value()) m_previousBold = s->bold;
    s->bold  = m_bold;
    s->dirty = true;
    return true;
}

bool SetTextBoldCommand::undo(Engine& engine) {
    if (!m_previousBold.has_value()) return false;
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    s->bold  = *m_previousBold;
    s->dirty = true;
    return true;
}

nlohmann::json SetTextBoldCommand::toJson() const {
    return {{"type", "SetTextBold"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"bold", m_bold}};
}

std::string SetTextBoldCommand::getDescription() const {
    return m_bold ? "Set text bold" : "Unset text bold";
}

CommandPtr SetTextBoldCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    return std::make_unique<SetTextBoldCommand>(e, j.value("bold", false));
}

// ----------------------------------------------------------------------------

bool SetTextItalicCommand::execute(Engine& engine) {
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    if (!m_previousItalic.has_value()) m_previousItalic = s->italic;
    s->italic = m_italic;
    s->dirty  = true;
    return true;
}

bool SetTextItalicCommand::undo(Engine& engine) {
    if (!m_previousItalic.has_value()) return false;
    auto* s = getTextLayerState(engine, m_layerEntity);
    if (!s) return false;
    s->italic = *m_previousItalic;
    s->dirty  = true;
    return true;
}

nlohmann::json SetTextItalicCommand::toJson() const {
    return {{"type", "SetTextItalic"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"italic", m_italic}};
}

std::string SetTextItalicCommand::getDescription() const {
    return m_italic ? "Set text italic" : "Unset text italic";
}

CommandPtr SetTextItalicCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    return std::make_unique<SetTextItalicCommand>(e, j.value("italic", false));
}

// ============================================================================
// SetTextLayerPropertiesCommand
// ============================================================================

bool SetTextLayerPropertiesCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[SetTextLayerProperties] FAIL: no timeline" << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[SetTextLayerProperties] FAIL: trackIndex " << m_trackIndex
                  << " out of range" << std::endl;
        return false;
    }
    const auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_layerIndex < 0 ||
        static_cast<size_t>(m_layerIndex) >= track->layers.size()) {
        std::cerr << "[SetTextLayerProperties] FAIL: layerIndex " << m_layerIndex
                  << " out of range" << std::endl;
        return false;
    }
    entt::entity layerEntity = track->layers[m_layerIndex];
    auto* tls = registry.try_get<TextLayerState>(layerEntity);
    if (!tls) {
        std::cerr << "[SetTextLayerProperties] FAIL: entity "
                  << static_cast<uint32_t>(layerEntity)
                  << " has no TextLayerState component" << std::endl;
        return false;
    }

    if (m_props.contains("text"))       tls->text       = m_props["text"].get<std::string>();
    if (m_props.contains("fontFamily")) tls->fontFamily = m_props["fontFamily"].get<std::string>();
    if (m_props.contains("fontSize"))   tls->fontSize   = m_props["fontSize"].get<float>();
    if (m_props.contains("colorR"))     tls->color.r    = m_props["colorR"].get<float>();
    if (m_props.contains("colorG"))     tls->color.g    = m_props["colorG"].get<float>();
    if (m_props.contains("colorB"))     tls->color.b    = m_props["colorB"].get<float>();
    if (m_props.contains("colorA"))     tls->color.a    = m_props["colorA"].get<float>();
    if (m_props.contains("bold"))       tls->bold       = m_props["bold"].get<bool>();
    if (m_props.contains("italic"))     tls->italic     = m_props["italic"].get<bool>();
    if (m_props.contains("alignment")) {
        std::string a = m_props["alignment"].get<std::string>();
        if      (a == "Left")   tls->alignment = TextAlignment::Left;
        else if (a == "Center") tls->alignment = TextAlignment::Center;
        else if (a == "Right")  tls->alignment = TextAlignment::Right;
    }
    tls->dirty = true;

    std::cout << "[SetTextLayerProperties] OK entity="
              << static_cast<uint32_t>(layerEntity) << std::endl;
    return true;
}

nlohmann::json SetTextLayerPropertiesCommand::toJson() const {
    nlohmann::json j = m_props;
    j["type"]        = "SetTextLayerProperties";
    j["trackIndex"]  = m_trackIndex;
    j["layerIndex"]  = m_layerIndex;
    return j;
}

std::string SetTextLayerPropertiesCommand::getDescription() const {
    return "Set text layer properties track=" + std::to_string(m_trackIndex)
         + " layer=" + std::to_string(m_layerIndex);
}

CommandPtr SetTextLayerPropertiesCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int layerIndex = j.value("layerIndex", 0);
    return std::make_unique<SetTextLayerPropertiesCommand>(trackIndex, layerIndex, j);
}

// ============================================================================
// AssertTextLayerStateCommand
// ============================================================================

bool AssertTextLayerStateCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[AssertTextLayerState] FAIL: no timeline" << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[AssertTextLayerState] FAIL: trackIndex " << m_trackIndex
                  << " out of range (tracks=" << tracks.size() << ")" << std::endl;
        return false;
    }
    const auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_layerIndex < 0 ||
        static_cast<size_t>(m_layerIndex) >= track->layers.size()) {
        std::cerr << "[AssertTextLayerState] FAIL: layerIndex " << m_layerIndex
                  << " out of range" << std::endl;
        return false;
    }
    entt::entity layerEntity = track->layers[m_layerIndex];
    const auto* tls = registry.try_get<TextLayerState>(layerEntity);
    if (!tls) {
        std::cerr << "[AssertTextLayerState] FAIL: entity "
                  << static_cast<uint32_t>(layerEntity)
                  << " has no TextLayerState component" << std::endl;
        return false;
    }

    if (!m_isFloat) {
        // String / enum assertion
        std::string actual;
        if      (m_field == "text")       actual = tls->text;
        else if (m_field == "fontFamily") actual = tls->fontFamily;
        else if (m_field == "alignment") {
            switch (tls->alignment) {
                case TextAlignment::Left:   actual = "Left";   break;
                case TextAlignment::Center: actual = "Center"; break;
                case TextAlignment::Right:  actual = "Right";  break;
            }
        }
        else {
            std::cerr << "[AssertTextLayerState] FAIL: unknown string field '"
                      << m_field << "'" << std::endl;
            return false;
        }
        if (actual == m_expectedStr) {
            std::cout << "[AssertTextLayerState] OK " << m_field
                      << "='" << actual << "'" << std::endl;
            return true;
        }
        std::cerr << "[AssertTextLayerState] FAIL: " << m_field
                  << " expected='" << m_expectedStr << "' got='" << actual << "'" << std::endl;
        return false;
    }

    // Float / bool assertion
    float actual = 0.f;
    if      (m_field == "fontSize") actual = tls->fontSize;
    else if (m_field == "colorR")   actual = tls->color.r;
    else if (m_field == "colorG")   actual = tls->color.g;
    else if (m_field == "colorB")   actual = tls->color.b;
    else if (m_field == "colorA")   actual = tls->color.a;
    else if (m_field == "bold")     actual = tls->bold   ? 1.f : 0.f;
    else if (m_field == "italic")   actual = tls->italic ? 1.f : 0.f;
    else {
        std::cerr << "[AssertTextLayerState] FAIL: unknown float field '"
                  << m_field << "'" << std::endl;
        return false;
    }
    float diff = std::fabs(actual - m_expectedFloat);
    if (diff <= m_tolerance) {
        std::cout << "[AssertTextLayerState] OK " << m_field << "=" << actual << std::endl;
        return true;
    }
    std::cerr << "[AssertTextLayerState] FAIL: " << m_field
              << " expected=" << m_expectedFloat
              << " got=" << actual
              << " diff=" << diff
              << " tol=" << m_tolerance << std::endl;
    return false;
}

nlohmann::json AssertTextLayerStateCommand::toJson() const {
    if (m_isFloat) {
        return {{"type", "AssertTextLayerState"},
                {"trackIndex", m_trackIndex},
                {"layerIndex", m_layerIndex},
                {"field", m_field},
                {"expectedFloat", m_expectedFloat},
                {"tolerance", m_tolerance}};
    }
    return {{"type", "AssertTextLayerState"},
            {"trackIndex", m_trackIndex},
            {"layerIndex", m_layerIndex},
            {"field", m_field},
            {"expected", m_expectedStr}};
}

std::string AssertTextLayerStateCommand::getDescription() const {
    return "Assert TextLayerState track=" + std::to_string(m_trackIndex)
         + " layer=" + std::to_string(m_layerIndex)
         + " " + m_field;
}

CommandPtr AssertTextLayerStateCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int layerIndex = j.value("layerIndex", 0);
    std::string field = j.value("field", std::string{});
    if (j.contains("expectedFloat") || j.contains("tolerance")) {
        float expected  = j.value("expectedFloat", 0.f);
        float tolerance = j.value("tolerance", 0.01f);
        return std::make_unique<AssertTextLayerStateCommand>(
            trackIndex, layerIndex, std::move(field), expected, tolerance);
    }
    std::string expected = j.value("expected", std::string{});
    return std::make_unique<AssertTextLayerStateCommand>(
        trackIndex, layerIndex, std::move(field), std::move(expected));
}

// ============================================================================
// UndoCommand
// ============================================================================

bool UndoCommand::execute(Engine& engine) {
    auto* dispatcher = engine.getCommandDispatcher();
    if (!dispatcher) {
        std::cerr << "[Undo] FAIL: no CommandDispatcher on engine" << std::endl;
        return false;
    }
    if (dispatcher->getUndoDepth() == 0) {
        std::cerr << "[Undo] FAIL: undo stack is empty" << std::endl;
        return false;
    }
    bool ok = dispatcher->undo(engine);
    if (ok) {
        std::cout << "[Undo] OK, depth now=" << dispatcher->getUndoDepth() << std::endl;
    } else {
        std::cerr << "[Undo] FAIL: undo() returned false" << std::endl;
    }
    return ok;
}

CommandPtr UndoCommand::fromJson(const nlohmann::json& /*j*/) {
    return std::make_unique<UndoCommand>();
}

// ============================================================================
// AssertTrackLayerCountCommand
// ============================================================================

bool AssertTrackLayerCountCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[AssertTrackLayerCount] FAIL: no timeline" << std::endl;
        return false;
    }
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[AssertTrackLayerCount] FAIL: trackIndex " << m_trackIndex
                  << " out of range (tracks=" << tracks.size() << ")" << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track) {
        std::cerr << "[AssertTrackLayerCount] FAIL: track entity has no TimelineTrack component" << std::endl;
        return false;
    }
    const size_t actual = track->layers.size();
    if (actual == m_count) {
        std::cout << "[AssertTrackLayerCount] OK track=" << m_trackIndex
                  << " count=" << actual << std::endl;
        return true;
    }
    std::cerr << "[AssertTrackLayerCount] FAIL: track=" << m_trackIndex
              << " expected=" << m_count << " got=" << actual << std::endl;
    return false;
}

nlohmann::json AssertTrackLayerCountCommand::toJson() const {
    return {{"type", "AssertTrackLayerCount"},
            {"trackIndex", m_trackIndex},
            {"count", m_count}};
}

std::string AssertTrackLayerCountCommand::getDescription() const {
    return "Assert track " + std::to_string(m_trackIndex)
         + " has " + std::to_string(m_count) + " layer(s)";
}

CommandPtr AssertTrackLayerCountCommand::fromJson(const nlohmann::json& j) {
    int    trackIndex = j.value("trackIndex", 0);
    size_t count      = j.value("count", static_cast<size_t>(0));
    return std::make_unique<AssertTrackLayerCountCommand>(trackIndex, count);
}

// ============================================================================
// AssertKeyframeCountCommand
// ============================================================================

bool AssertKeyframeCountCommand::execute(Engine& engine) {
    auto* timeline = engine.getTimeline();
    if (!timeline) {
        std::cerr << "[AssertKeyframeCount] FAIL: no timeline" << std::endl;
        return false;
    }
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[AssertKeyframeCount] FAIL: trackIndex " << m_trackIndex
                  << " out of range (tracks=" << tracks.size() << ")" << std::endl;
        return false;
    }
    auto& registry = engine.getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 ||
        static_cast<size_t>(m_clipIndex) >= track->layers.size()) {
        std::cerr << "[AssertKeyframeCount] FAIL: clipIndex " << m_clipIndex
                  << " out of range" << std::endl;
        return false;
    }
    const auto prop = parseAnimatableProperty(m_property);
    if (!prop) {
        std::cerr << "[AssertKeyframeCount] FAIL: unknown property '"
                  << m_property << "'" << std::endl;
        return false;
    }
    entt::entity layerEntity = track->layers[m_clipIndex];
    const auto* animProps = registry.try_get<AnimatedProperties>(layerEntity);
    const KeyframeTrack* kfTrack = animProps ? animProps->getTrack(*prop) : nullptr;
    const size_t actual = kfTrack ? kfTrack->keyframes.size() : 0;
    if (actual == m_count) {
        std::cout << "[AssertKeyframeCount] OK track=" << m_trackIndex
                  << " clip=" << m_clipIndex << " " << m_property
                  << " count=" << actual << std::endl;
        return true;
    }
    std::cerr << "[AssertKeyframeCount] FAIL: track=" << m_trackIndex
              << " clip=" << m_clipIndex << " " << m_property
              << " expected=" << m_count << " got=" << actual << std::endl;
    return false;
}

nlohmann::json AssertKeyframeCountCommand::toJson() const {
    return {{"type", "AssertKeyframeCount"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"property", m_property},
            {"count", m_count}};
}

std::string AssertKeyframeCountCommand::getDescription() const {
    return "Assert track " + std::to_string(m_trackIndex) + " clip "
         + std::to_string(m_clipIndex) + " " + m_property + " has "
         + std::to_string(m_count) + " keyframe(s)";
}

CommandPtr AssertKeyframeCountCommand::fromJson(const nlohmann::json& j) {
    int trackIndex = j.value("trackIndex", 0);
    int clipIndex  = j.value("clipIndex", 0);
    std::string property = j.value("property", "Opacity");
    size_t count = j.value("count", static_cast<size_t>(0));
    return std::make_unique<AssertKeyframeCountCommand>(
        trackIndex, clipIndex, property, count);
}

// ============================================================================
// Audio source commands
// ============================================================================

namespace {
AudioSource* getAudioSource(Engine& engine, entt::entity clipEntity) {
    auto& reg = engine.getRegistry();
    return reg.try_get<AudioSource>(clipEntity);
}
} // anonymous namespace

// ----------------------------------------------------------------------------
// SetAudioSourceGainCommand
// ----------------------------------------------------------------------------

bool SetAudioSourceGainCommand::execute(Engine& engine) {
    auto* src = getAudioSource(engine, m_clipEntity);
    if (!src) return false;
    if (!m_previousGain.has_value()) m_previousGain = src->gain;
    src->gain = m_gain;
    return true;
}

bool SetAudioSourceGainCommand::undo(Engine& engine) {
    if (!m_previousGain.has_value()) return false;
    auto* src = getAudioSource(engine, m_clipEntity);
    if (!src) return false;
    src->gain = *m_previousGain;
    return true;
}

nlohmann::json SetAudioSourceGainCommand::toJson() const {
    return {{"type", "SetAudioSourceGain"},
            {"clipEntity", static_cast<std::uint32_t>(m_clipEntity)},
            {"gain", m_gain}};
}

std::string SetAudioSourceGainCommand::getDescription() const {
    return "Set clip audio gain";
}

CommandPtr SetAudioSourceGainCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("clipEntity", std::uint32_t{0}));
    return std::make_unique<SetAudioSourceGainCommand>(e, j.value("gain", 1.0f));
}

// ----------------------------------------------------------------------------
// SetAudioSourceMuteCommand
// ----------------------------------------------------------------------------

bool SetAudioSourceMuteCommand::execute(Engine& engine) {
    auto* src = getAudioSource(engine, m_clipEntity);
    if (!src) return false;
    if (!m_previousMute.has_value()) m_previousMute = src->mute;
    src->mute = m_mute;
    return true;
}

bool SetAudioSourceMuteCommand::undo(Engine& engine) {
    if (!m_previousMute.has_value()) return false;
    auto* src = getAudioSource(engine, m_clipEntity);
    if (!src) return false;
    src->mute = *m_previousMute;
    return true;
}

nlohmann::json SetAudioSourceMuteCommand::toJson() const {
    return {{"type", "SetAudioSourceMute"},
            {"clipEntity", static_cast<std::uint32_t>(m_clipEntity)},
            {"mute", m_mute}};
}

std::string SetAudioSourceMuteCommand::getDescription() const {
    return m_mute ? "Mute clip audio" : "Unmute clip audio";
}

CommandPtr SetAudioSourceMuteCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("clipEntity", std::uint32_t{0}));
    return std::make_unique<SetAudioSourceMuteCommand>(e, j.value("mute", false));
}

// ----------------------------------------------------------------------------
// SetAudioSourceSoloCommand
// ----------------------------------------------------------------------------

bool SetAudioSourceSoloCommand::execute(Engine& engine) {
    auto* src = getAudioSource(engine, m_clipEntity);
    if (!src) return false;
    if (!m_previousSolo.has_value()) m_previousSolo = src->solo;
    src->solo = m_solo;
    return true;
}

bool SetAudioSourceSoloCommand::undo(Engine& engine) {
    if (!m_previousSolo.has_value()) return false;
    auto* src = getAudioSource(engine, m_clipEntity);
    if (!src) return false;
    src->solo = *m_previousSolo;
    return true;
}

nlohmann::json SetAudioSourceSoloCommand::toJson() const {
    return {{"type", "SetAudioSourceSolo"},
            {"clipEntity", static_cast<std::uint32_t>(m_clipEntity)},
            {"solo", m_solo}};
}

std::string SetAudioSourceSoloCommand::getDescription() const {
    return m_solo ? "Solo clip audio" : "Unsolo clip audio";
}

CommandPtr SetAudioSourceSoloCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("clipEntity", std::uint32_t{0}));
    return std::make_unique<SetAudioSourceSoloCommand>(e, j.value("solo", false));
}

// ----------------------------------------------------------------------------
// SetMasterGainCommand
// ----------------------------------------------------------------------------

bool SetMasterGainCommand::execute(Engine& engine) {
    auto* ae = engine.getAudioEngine();
    if (!ae) return false;
    if (!m_previousGain.has_value()) m_previousGain = ae->masterGain();
    ae->setMasterGain(m_gain);
    return true;
}

bool SetMasterGainCommand::undo(Engine& engine) {
    if (!m_previousGain.has_value()) return false;
    auto* ae = engine.getAudioEngine();
    if (!ae) return false;
    ae->setMasterGain(*m_previousGain);
    return true;
}

nlohmann::json SetMasterGainCommand::toJson() const {
    return {{"type", "SetMasterGain"}, {"gain", m_gain}};
}

std::string SetMasterGainCommand::getDescription() const {
    return "Set master audio gain";
}

CommandPtr SetMasterGainCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<SetMasterGainCommand>(j.value("gain", 1.0f));
}

// ----------------------------------------------------------------------------
// SetMasterMuteCommand
// ----------------------------------------------------------------------------

bool SetMasterMuteCommand::execute(Engine& engine) {
    auto* ae = engine.getAudioEngine();
    if (!ae) return false;
    if (!m_previousMute.has_value()) m_previousMute = ae->masterMute();
    ae->setMasterMute(m_mute);
    return true;
}

bool SetMasterMuteCommand::undo(Engine& engine) {
    if (!m_previousMute.has_value()) return false;
    auto* ae = engine.getAudioEngine();
    if (!ae) return false;
    ae->setMasterMute(*m_previousMute);
    return true;
}

nlohmann::json SetMasterMuteCommand::toJson() const {
    return {{"type", "SetMasterMute"}, {"mute", m_mute}};
}

std::string SetMasterMuteCommand::getDescription() const {
    return m_mute ? "Mute master audio" : "Unmute master audio";
}

CommandPtr SetMasterMuteCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<SetMasterMuteCommand>(j.value("mute", false));
}

// ============================================================================
// Audio capture assertions
// ============================================================================

namespace {

// Downmix interleaved stereo float buffer to mono, return RMS.
float captureRms(const std::vector<float>& buf) {
    if (buf.empty()) return 0.0f;
    double acc = 0.0;
    for (float s : buf) acc += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(acc / buf.size()));
}

// Goertzel filter energy at `hz` for a mono-mixed float buffer sampled at `sr`.
float goertzel(const std::vector<float>& buf, float hz, int sr) {
    if (buf.empty() || sr <= 0) return 0.0f;
    constexpr double PI = 3.14159265358979323846;
    const double k  = static_cast<double>(buf.size()) * hz / sr;
    const double w  = 2.0 * PI * k / buf.size();
    const double c  = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (float x : buf) {
        s0 = x + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(s1 * s1 + s2 * s2 - c * s1 * s2);
}

// Return the LoopbackDevice* from an Engine's AudioEngine, or nullptr.
entity::LoopbackDevice* getLoopback(Engine& engine) {
    auto* ae = engine.getAudioEngine();
    if (!ae) return nullptr;
    return dynamic_cast<entity::LoopbackDevice*>(ae->device());
}

} // namespace

bool ClearAudioCaptureCommand::execute(Engine& engine) {
    auto* lb = getLoopback(engine);
    if (lb) lb->clearCapture();
    return true;
}

CommandPtr ClearAudioCaptureCommand::fromJson(const nlohmann::json&) {
    return std::make_unique<ClearAudioCaptureCommand>();
}

bool AssertAudioCaptureRmsCommand::execute(Engine& engine) {
    auto* lb = getLoopback(engine);
    if (!lb) {
        std::cerr << "[AssertAudioCaptureRms] No LoopbackDevice available\n";
        return false;
    }
    const auto buf = lb->capturedCopy();
    const float rms = captureRms(buf);
    if (rms <= m_minRms) {
        std::cerr << "[AssertAudioCaptureRms] RMS " << rms
                  << " <= required " << m_minRms
                  << " (" << buf.size() << " samples captured)\n";
        return false;
    }
    return true;
}

nlohmann::json AssertAudioCaptureRmsCommand::toJson() const {
    return {{"type", "AssertAudioCaptureRms"}, {"minRms", m_minRms}};
}

std::string AssertAudioCaptureRmsCommand::getDescription() const {
    return "Assert audio capture RMS > " + std::to_string(m_minRms);
}

CommandPtr AssertAudioCaptureRmsCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<AssertAudioCaptureRmsCommand>(
        j.value("minRms", 0.01f));
}

bool AssertAudioCaptureGoertzelCommand::execute(Engine& engine) {
    auto* lb = getLoopback(engine);
    if (!lb) {
        std::cerr << "[AssertAudioCaptureGoertzel] No LoopbackDevice available\n";
        return false;
    }
    auto* ae = engine.getAudioEngine();
    const int sr = ae ? ae->sampleRate() : 48000;
    const auto buf = lb->capturedCopy();
    if (buf.empty()) {
        std::cerr << "[AssertAudioCaptureGoertzel] Capture buffer is empty\n";
        return false;
    }
    // Downmix to mono for the filter.
    const int ch = ae ? ae->device()->channelCount() : 2;
    std::vector<float> mono;
    mono.reserve(buf.size() / ch);
    for (size_t i = 0; i < buf.size(); i += ch) {
        float sum = 0.0f;
        for (int c = 0; c < ch && (i + c) < buf.size(); ++c) sum += buf[i + c];
        mono.push_back(sum / ch);
    }
    const float targetE    = goertzel(mono, m_targetHz,    sr);
    const float referenceE = goertzel(mono, m_referenceHz, sr);
    if (referenceE <= 0.0f || (targetE / referenceE) < m_minRatio) {
        std::cerr << "[AssertAudioCaptureGoertzel] target(" << m_targetHz << "Hz)="
                  << targetE << " reference(" << m_referenceHz << "Hz)="
                  << referenceE << " ratio=" << (referenceE > 0 ? targetE/referenceE : 0.f)
                  << " < required " << m_minRatio << "\n";
        return false;
    }
    return true;
}

nlohmann::json AssertAudioCaptureGoertzelCommand::toJson() const {
    return {{"type", "AssertAudioCaptureGoertzel"},
            {"targetHz", m_targetHz},
            {"referenceHz", m_referenceHz},
            {"minRatio", m_minRatio}};
}

std::string AssertAudioCaptureGoertzelCommand::getDescription() const {
    return "Assert Goertzel(" + std::to_string(m_targetHz) + "Hz) / ("
         + std::to_string(m_referenceHz) + "Hz) >= " + std::to_string(m_minRatio);
}

CommandPtr AssertAudioCaptureGoertzelCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<AssertAudioCaptureGoertzelCommand>(
        j.value("targetHz",    1000.0f),
        j.value("referenceHz",  500.0f),
        j.value("minRatio",      3.0f));
}

bool AssertAudioWorkerSeekFrameCommand::execute(Engine& engine) {
    auto* timeline  = engine.getTimeline();
    auto* director  = engine.getDirector();
    if (!timeline || !director) {
        std::cerr << "[AssertAudioWorkerSeekFrame] FAIL: timeline/director unavailable"
                  << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[AssertAudioWorkerSeekFrame] FAIL: trackIndex " << m_trackIndex
                  << " out of range (tracks=" << tracks.size() << ")" << std::endl;
        return false;
    }
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 ||
        static_cast<size_t>(m_clipIndex) >= track->layers.size()) {
        std::cerr << "[AssertAudioWorkerSeekFrame] FAIL: clipIndex " << m_clipIndex
                  << " out of range" << std::endl;
        return false;
    }
    entt::entity clipEntity = track->layers[m_clipIndex];
    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cerr << "[AssertAudioWorkerSeekFrame] FAIL: no Clip component" << std::endl;
        return false;
    }
    if (!registry.try_get<AudioSource>(clipEntity)) {
        std::cerr << "[AssertAudioWorkerSeekFrame] FAIL: clip has no AudioSource "
                     "(media has no audio stream)" << std::endl;
        return false;
    }

    auto* audioSystem = director->getAudioSystem();
    if (!audioSystem) {
        std::cerr << "[AssertAudioWorkerSeekFrame] FAIL: no AudioSystem" << std::endl;
        return false;
    }

    const int64_t actual = audioSystem->getWorkerSeekTargetFrame(
        clipEntity, clip->framerate);
    if (actual < 0) {
        std::cerr << "[AssertAudioWorkerSeekFrame] FAIL: worker not initialized yet "
                     "(returned -1)" << std::endl;
        return false;
    }

    const int64_t diff = std::abs(actual - m_expected);
    if (diff <= m_tolerance) {
        std::cout << "[AssertAudioWorkerSeekFrame] OK track=" << m_trackIndex
                  << " clip=" << m_clipIndex
                  << " seekFrame=" << actual
                  << " (== " << m_expected << " +/-" << m_tolerance << ")" << std::endl;
        return true;
    }
    std::cerr << "[AssertAudioWorkerSeekFrame] FAIL: track=" << m_trackIndex
              << " clip=" << m_clipIndex
              << " expected " << m_expected << " (+/-" << m_tolerance
              << "), got=" << actual << std::endl;
    return false;
}

nlohmann::json AssertAudioWorkerSeekFrameCommand::toJson() const {
    return {{"type", "AssertAudioWorkerSeekFrame"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"expected", m_expected},
            {"tolerance", m_tolerance}};
}

std::string AssertAudioWorkerSeekFrameCommand::getDescription() const {
    return "Assert audio worker seek frame for track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr AssertAudioWorkerSeekFrameCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<AssertAudioWorkerSeekFrameCommand>(
        j.value("trackIndex", 0),
        j.value("clipIndex",  0),
        j.value("expected",  int64_t{0}),
        j.value("tolerance", int64_t{5}));
}

bool AssertAudioWorkerSeekCountAtMostCommand::execute(Engine& engine) {
    auto* timeline  = engine.getTimeline();
    auto* director  = engine.getDirector();
    if (!timeline || !director) {
        std::cerr << "[AssertAudioWorkerSeekCountAtMost] FAIL: timeline/director unavailable"
                  << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();
    const auto& tracks = timeline->getTracks();
    if (m_trackIndex < 0 || static_cast<size_t>(m_trackIndex) >= tracks.size()) {
        std::cerr << "[AssertAudioWorkerSeekCountAtMost] FAIL: trackIndex " << m_trackIndex
                  << " out of range (tracks=" << tracks.size() << ")" << std::endl;
        return false;
    }
    auto* track = registry.try_get<TimelineTrack>(tracks[m_trackIndex]);
    if (!track || m_clipIndex < 0 ||
        static_cast<size_t>(m_clipIndex) >= track->layers.size()) {
        std::cerr << "[AssertAudioWorkerSeekCountAtMost] FAIL: clipIndex " << m_clipIndex
                  << " out of range" << std::endl;
        return false;
    }
    entt::entity clipEntity = track->layers[m_clipIndex];
    if (!registry.try_get<AudioSource>(clipEntity)) {
        std::cerr << "[AssertAudioWorkerSeekCountAtMost] FAIL: clip has no AudioSource "
                     "(media has no audio stream)" << std::endl;
        return false;
    }

    auto* audioSystem = director->getAudioSystem();
    if (!audioSystem) {
        std::cerr << "[AssertAudioWorkerSeekCountAtMost] FAIL: no AudioSystem" << std::endl;
        return false;
    }

    const int64_t actual = audioSystem->getWorkerSeekCount(clipEntity);
    if (actual < 0) {
        std::cerr << "[AssertAudioWorkerSeekCountAtMost] FAIL: no audio worker"
                  << std::endl;
        return false;
    }
    if (actual <= m_maxCount) {
        std::cout << "[AssertAudioWorkerSeekCountAtMost] OK track=" << m_trackIndex
                  << " clip=" << m_clipIndex
                  << " seekCount=" << actual
                  << " (<= " << m_maxCount << ")" << std::endl;
        return true;
    }
    std::cerr << "[AssertAudioWorkerSeekCountAtMost] FAIL: track=" << m_trackIndex
              << " clip=" << m_clipIndex
              << " seekCount=" << actual
              << " exceeds max " << m_maxCount
              << " (seek storm — each seek is an audible ring-clear)" << std::endl;
    return false;
}

nlohmann::json AssertAudioWorkerSeekCountAtMostCommand::toJson() const {
    return {{"type", "AssertAudioWorkerSeekCountAtMost"},
            {"trackIndex", m_trackIndex},
            {"clipIndex", m_clipIndex},
            {"maxCount", m_maxCount}};
}

std::string AssertAudioWorkerSeekCountAtMostCommand::getDescription() const {
    return "Assert audio worker seek count <= " + std::to_string(m_maxCount) +
           " for track " + std::to_string(m_trackIndex) +
           ", clip " + std::to_string(m_clipIndex);
}

CommandPtr AssertAudioWorkerSeekCountAtMostCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<AssertAudioWorkerSeekCountAtMostCommand>(
        j.value("trackIndex", 0),
        j.value("clipIndex",  0),
        j.value("maxCount",  int64_t{3}));
}

// ============================================================================
// EnumerateDisplaysCommand
// ============================================================================

bool EnumerateDisplaysCommand::execute(Engine& engine) {
    auto* om = engine.getOutputManager();
    if (!om) {
        std::cerr << "[EnumerateDisplays] OutputManager not available" << std::endl;
        return false;
    }

    const auto& displays = om->getAvailableDisplays();

    nlohmann::json result;
    result["count"] = displays.size();
    result["displays"] = nlohmann::json::array();
    for (const auto& d : displays) {
        nlohmann::json entry;
        entry["index"]       = d.index;
        entry["name"]        = d.displayName;
        entry["device"]      = d.deviceName;
        entry["width"]       = d.width;
        entry["height"]      = d.height;
        entry["refreshRate"] = static_cast<int>(d.refreshRate);
        entry["primary"]     = d.isPrimary;
        result["displays"].push_back(std::move(entry));
    }

    std::cout << "[EnumerateDisplays] " << result.dump() << std::endl;
    return true;
}

CommandPtr EnumerateDisplaysCommand::fromJson(const nlohmann::json& /*j*/) {
    return std::make_unique<EnumerateDisplaysCommand>();
}

// ============================================================================
// AssignScreenToDisplayCommand
// ============================================================================

bool AssignScreenToDisplayCommand::execute(Engine& engine) {
    auto* om = engine.getOutputManager();
    if (!om) {
        std::cerr << "[AssignScreenToDisplay] OutputManager not available" << std::endl;
        return false;
    }

    auto& registry = engine.getRegistry();

    // Find screen by name.
    entt::entity screenEntity = entt::null;
    for (auto [e, s] : registry.view<Screen>().each()) {
        if (s.name == m_screenName) {
            screenEntity = e;
            break;
        }
    }
    if (screenEntity == entt::null) {
        std::cerr << "[AssignScreenToDisplay] Screen not found: " << m_screenName << std::endl;
        return false;
    }

    // Check display index — skip gracefully when out of range so the same
    // stress-test script runs on any machine regardless of display count.
    const auto& displays = om->getAvailableDisplays();
    if (m_displayIndex < 0 || m_displayIndex >= static_cast<int32_t>(displays.size())) {
        std::cout << "[AssignScreenToDisplay] Skipping " << m_screenName
                  << ": displayIndex " << m_displayIndex << " out of range ("
                  << displays.size() << " displays available)" << std::endl;
        // Return true — not a script failure, just skipped on this machine.
        return true;
    }

    // Find an existing OutputDisplay that already sources this screen, or
    // create a fresh one named "Output_<screenName>".
    entt::entity outputEntity = entt::null;
    for (auto [e, od] : registry.view<OutputDisplay>().each()) {
        if (od.sourceScreen == screenEntity) {
            outputEntity = e;
            break;
        }
    }

    bool createdNow = false;
    if (outputEntity == entt::null) {
        outputEntity = om->createOutput("Output_" + m_screenName, OutputType::Physical);
        createdNow = true;
    }

    // Capture undo state before mutating (only on first execute).
    if (!m_executed) {
        m_outputEntity  = outputEntity;
        m_createdOutput = createdNow;
        if (!createdNow) {
            const auto& od          = registry.get<OutputDisplay>(outputEntity);
            m_prevEnabled           = od.enabled;
            m_prevSourceScreen      = od.sourceScreen;
            m_prevPhysicalDisplayIndex = od.physicalDisplayIndex;
        }
        m_executed = true;
    }

    // Set sourceScreen and enable before assigning the display, so that
    // assignDisplay's transport path picks up the correct enabled state.
    {
        auto& od        = registry.get<OutputDisplay>(outputEntity);
        od.sourceScreen = screenEntity;
        od.enabled      = true;
    }

    om->assignDisplay(outputEntity, m_displayIndex);

    std::cout << "[AssignScreenToDisplay] Screen '" << m_screenName
              << "' -> display " << m_displayIndex
              << " (output entity=" << static_cast<uint32_t>(outputEntity)
              << (createdNow ? ", created" : ", reused") << ")" << std::endl;
    return true;
}

bool AssignScreenToDisplayCommand::undo(Engine& engine) {
    if (!m_executed || m_outputEntity == entt::null) return false;

    auto* om = engine.getOutputManager();
    if (!om) return false;

    auto& registry = engine.getRegistry();
    if (!registry.valid(m_outputEntity)) return false;

    if (m_createdOutput) {
        // Output didn't exist before — destroy it.
        om->removeOutput(m_outputEntity);
        std::cout << "[AssignScreenToDisplay] Undo: destroyed output for '"
                  << m_screenName << "'" << std::endl;
    } else {
        // Restore prior state.
        auto& od = registry.get<OutputDisplay>(m_outputEntity);
        od.sourceScreen = m_prevSourceScreen;
        od.enabled      = m_prevEnabled;
        if (m_prevPhysicalDisplayIndex >= 0) {
            om->assignDisplay(m_outputEntity, m_prevPhysicalDisplayIndex);
        }
        // Sync enabled state through the bus so the show thread sees it.
        bus::SetOutputEnabled msg{
            static_cast<std::uint64_t>(m_outputEntity),
            m_prevEnabled
        };
        engine.publishSetOutputEnabled(msg);
        std::cout << "[AssignScreenToDisplay] Undo: restored output for '"
                  << m_screenName << "'" << std::endl;
    }
    return true;
}

nlohmann::json AssignScreenToDisplayCommand::toJson() const {
    return {{"type", "AssignScreenToDisplay"},
            {"screenName", m_screenName},
            {"displayIndex", m_displayIndex}};
}

std::string AssignScreenToDisplayCommand::getDescription() const {
    return "Assign screen '" + m_screenName + "' to display " + std::to_string(m_displayIndex);
}

CommandPtr AssignScreenToDisplayCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<AssignScreenToDisplayCommand>(
        j.value("screenName", ""),
        j.value("displayIndex", 0));
}

// ============================================================================
// SetOutputEnabledCommand
// ============================================================================

bool SetOutputEnabledCommand::execute(Engine& engine) {
    auto& registry = engine.getRegistry();

    // Find OutputDisplay by name.
    entt::entity found = entt::null;
    for (auto [e, od] : registry.view<OutputDisplay>().each()) {
        if (od.name == m_outputName) {
            found = e;
            break;
        }
    }
    if (found == entt::null) {
        std::cerr << "[SetOutputEnabled] OutputDisplay not found: " << m_outputName << std::endl;
        return false;
    }

    // Capture previous state on first execute for undo.
    if (!m_previousEnabled.has_value()) {
        m_previousEnabled = registry.get<OutputDisplay>(found).enabled;
        m_outputEntity    = found;
    }

    // publishSetOutputEnabled writes the registry `enabled` (editor thread,
    // single chokepoint per issue #76) and routes the message through the
    // bus so the show thread handles swap-chain lifecycle safely (ADR-0014).
    engine.publishSetOutputEnabled(bus::SetOutputEnabled{
        static_cast<std::uint64_t>(found),
        m_enabled
    });

    std::cout << "[SetOutputEnabled] '" << m_outputName << "' -> "
              << (m_enabled ? "enabled" : "disabled") << std::endl;
    return true;
}

bool SetOutputEnabledCommand::undo(Engine& engine) {
    if (!m_previousEnabled.has_value()) return false;
    if (m_outputEntity == entt::null) return false;

    auto& registry = engine.getRegistry();
    if (!registry.valid(m_outputEntity)) return false;

    const bool prev = *m_previousEnabled;
    engine.publishSetOutputEnabled(bus::SetOutputEnabled{
        static_cast<std::uint64_t>(m_outputEntity),
        prev
    });

    std::cout << "[SetOutputEnabled] Undo: '" << m_outputName << "' -> "
              << (prev ? "enabled" : "disabled") << std::endl;
    return true;
}

nlohmann::json SetOutputEnabledCommand::toJson() const {
    return {{"type", "SetOutputEnabled"},
            {"outputName", m_outputName},
            {"enabled", m_enabled}};
}

std::string SetOutputEnabledCommand::getDescription() const {
    return std::string("Set output '") + m_outputName + "' " +
           (m_enabled ? "enabled" : "disabled");
}

CommandPtr SetOutputEnabledCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<SetOutputEnabledCommand>(
        j.value("outputName", ""),
        j.value("enabled", true));
}

// ============================================================================
// CreateSignalLayerCommand (Phase 3)
// ============================================================================

bool CreateSignalLayerCommand::execute(Engine& engine) {
    m_createdEntity = engine.createSignalLayer(m_trackIndex, m_startFrame, m_duration);
    if (m_createdEntity == entt::null) {
        std::cerr << "[CreateSignalLayer] FAIL: createSignalLayer returned null"
                  << std::endl;
        return false;
    }
    std::cout << "[CreateSignalLayer] OK track=" << m_trackIndex
              << " start=" << m_startFrame
              << " duration=" << m_duration
              << " entity=" << static_cast<uint32_t>(m_createdEntity)
              << std::endl;
    return true;
}

nlohmann::json CreateSignalLayerCommand::toJson() const {
    return {{"type", "CreateSignalLayer"},
            {"trackIndex", m_trackIndex},
            {"startFrame", m_startFrame},
            {"duration",   m_duration}};
}

std::string CreateSignalLayerCommand::getDescription() const {
    return "Create Signal layer on track " + std::to_string(m_trackIndex) +
           " at " + std::to_string(m_startFrame) +
           " dur=" + std::to_string(m_duration);
}

CommandPtr CreateSignalLayerCommand::fromJson(const nlohmann::json& j) {
    return std::make_unique<CreateSignalLayerCommand>(
        j.value("trackIndex", 0),
        j.value("startFrame", FrameNumber{0}),
        j.value("duration",   FrameNumber{60}));
}

// ============================================================================
// TestSignalEmitCommand
// ============================================================================

bool TestSignalEmitCommand::execute(Engine& engine) {
    entity::plugin::SignalEmitPod pod;

    // Copy address into the fixed buffer, clamped to kMaxAddressLen.
    const std::size_t addrLen =
        std::min(m_address.size(), entity::plugin::SignalEmitPod::kMaxAddressLen);
    std::memcpy(pod.address, m_address.c_str(), addrLen);
    pod.address[addrLen] = '\0';

    std::size_t argIdx = 0;
    if (m_hasI && argIdx < entity::plugin::SignalEmitPod::kMaxArgs) {
        auto& a = pod.args[argIdx++];
        a.type = entity::plugin::SignalArgPod::Type::Int;
        a.i    = m_i;
    }
    if (m_hasF && argIdx < entity::plugin::SignalEmitPod::kMaxArgs) {
        auto& a = pod.args[argIdx++];
        a.type = entity::plugin::SignalArgPod::Type::Float;
        a.f    = m_f;
    }
    if (m_hasS && argIdx < entity::plugin::SignalEmitPod::kMaxArgs) {
        auto& a = pod.args[argIdx++];
        a.type = entity::plugin::SignalArgPod::Type::String;
        const std::size_t sLen =
            std::min(m_s.size(), entity::plugin::SignalArgPod::kMaxStringLen);
        std::memcpy(a.s, m_s.c_str(), sLen);
        a.s[sLen] = '\0';
    }
    pod.argCount = argIdx;

    engine.postSignalEmit(pod);
    return true;
}

nlohmann::json TestSignalEmitCommand::toJson() const {
    nlohmann::json j = {{"type", "TestSignalEmit"}, {"address", m_address}};
    if (m_hasI) j["iArg"] = m_i;
    if (m_hasF) j["fArg"] = m_f;
    if (m_hasS) j["sArg"] = m_s;
    return j;
}

std::string TestSignalEmitCommand::getDescription() const {
    return std::string("TestSignalEmit ") + m_address;
}

CommandPtr TestSignalEmitCommand::fromJson(const nlohmann::json& j) {
    const std::string address = j.value("address", "/entity/signal/test");
    const bool hasI = j.contains("iArg") && j["iArg"].is_number_integer();
    const bool hasF = j.contains("fArg") && j["fArg"].is_number();
    const bool hasS = j.contains("sArg") && j["sArg"].is_string();
    return std::make_unique<TestSignalEmitCommand>(
        address,
        hasI, hasI ? j["iArg"].get<int32_t>() : int32_t{0},
        hasF, hasF ? j["fArg"].get<float>()    : 0.0f,
        hasS, hasS ? j["sArg"].get<std::string>() : std::string{});
}

// ============================================================================
// SetSignal* commands (Phase 8)
// ============================================================================

namespace {
static SignalLayer* getSignalLayer(Engine& engine, entt::entity e) {
    auto& registry = engine.getRegistry();
    if (!registry.valid(e)) return nullptr;
    return registry.try_get<SignalLayer>(e);
}
} // namespace

// -- SetSignalModeCommand --

bool SetSignalModeCommand::execute(Engine& engine) {
    auto* sig = getSignalLayer(engine, m_layerEntity);
    if (!sig) return false;
    sig->mode = static_cast<SignalLayer::Mode>(m_mode);
    return true;
}

nlohmann::json SetSignalModeCommand::toJson() const {
    return {{"type", "SetSignalMode"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"mode", m_mode}};
}

std::string SetSignalModeCommand::getDescription() const {
    return std::string("Set signal mode to ") + (m_mode == 0 ? "Momentary" : "Continuous");
}

CommandPtr SetSignalModeCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    // Accept int or string mode
    int mode = 0;
    if (j.contains("mode")) {
        if (j["mode"].is_string()) {
            mode = (j["mode"].get<std::string>() == "Continuous") ? 1 : 0;
        } else {
            mode = j["mode"].get<int>();
        }
    }
    return std::make_unique<SetSignalModeCommand>(e, mode);
}

// -- SetSignalAddressCommand --

bool SetSignalAddressCommand::execute(Engine& engine) {
    auto* sig = getSignalLayer(engine, m_layerEntity);
    if (!sig) return false;
    sig->address = m_address;
    return true;
}

nlohmann::json SetSignalAddressCommand::toJson() const {
    return {{"type", "SetSignalAddress"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"address", m_address}};
}

std::string SetSignalAddressCommand::getDescription() const {
    return "Set signal address to " + m_address;
}

CommandPtr SetSignalAddressCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    return std::make_unique<SetSignalAddressCommand>(
        e, j.value("address", std::string{"/entity/signal"}));
}

// -- SetSignalArgsCommand --

bool SetSignalArgsCommand::execute(Engine& engine) {
    auto* sig = getSignalLayer(engine, m_layerEntity);
    if (!sig) return false;

    // Parse the args JSON string defensively.
    nlohmann::json jArgs;
    try {
        jArgs = nlohmann::json::parse(m_argsJson);
    } catch (const std::exception& ex) {
        std::cerr << "[SetSignalArgs] malformed args JSON: " << ex.what() << std::endl;
        return false;
    }
    if (!jArgs.is_array()) {
        std::cerr << "[SetSignalArgs] args must be a JSON array" << std::endl;
        return false;
    }

    std::vector<SignalLayer::Arg> args;
    for (const auto& aj : jArgs) {
        SignalLayer::Arg a;
        // type field maps "int"/"float"/"string" to enum
        const std::string typeStr = aj.value("type", std::string{"int"});
        if (typeStr == "float")       a.type = SignalLayer::Arg::Type::Float;
        else if (typeStr == "string") a.type = SignalLayer::Arg::Type::String;
        else                          a.type = SignalLayer::Arg::Type::Int;
        a.i           = aj.value("i",            std::int32_t{0});
        a.f           = aj.value("f",            0.0f);
        a.s           = aj.value("s",            std::string{});
        a.isValueBound = aj.value("isValueBound", false);
        args.push_back(std::move(a));
    }
    sig->args = std::move(args);
    return true;
}

nlohmann::json SetSignalArgsCommand::toJson() const {
    return {{"type", "SetSignalArgs"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"args", m_argsJson}};
}

std::string SetSignalArgsCommand::getDescription() const {
    return "Set signal args";
}

CommandPtr SetSignalArgsCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    std::string argsJson = "[]";
    if (j.contains("args")) {
        if (j["args"].is_string()) {
            argsJson = j["args"].get<std::string>();
        } else if (j["args"].is_array()) {
            argsJson = j["args"].dump();
        }
    }
    return std::make_unique<SetSignalArgsCommand>(e, std::move(argsJson));
}

// -- SetSignalValueSourceCommand --

bool SetSignalValueSourceCommand::execute(Engine& engine) {
    auto* sig = getSignalLayer(engine, m_layerEntity);
    if (!sig) return false;
    sig->valueSource = static_cast<SignalLayer::ValueSource>(m_valueSource);
    return true;
}

nlohmann::json SetSignalValueSourceCommand::toJson() const {
    return {{"type", "SetSignalValueSource"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"valueSource", m_valueSource}};
}

std::string SetSignalValueSourceCommand::getDescription() const {
    return std::string("Set signal value source to ") +
           (m_valueSource == 0 ? "Progress" : "Keyframe");
}

CommandPtr SetSignalValueSourceCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    int vs = 0;
    if (j.contains("valueSource")) {
        if (j["valueSource"].is_string()) {
            vs = (j["valueSource"].get<std::string>() == "Keyframe") ? 1 : 0;
        } else {
            vs = j["valueSource"].get<int>();
        }
    }
    return std::make_unique<SetSignalValueSourceCommand>(e, vs);
}

// -- SetSignalMappingCommand --

bool SetSignalMappingCommand::execute(Engine& engine) {
    auto* sig = getSignalLayer(engine, m_layerEntity);
    if (!sig) return false;
    sig->srcMin  = m_srcMin;
    sig->srcMax  = m_srcMax;
    sig->outMin  = m_outMin;
    sig->outMax  = m_outMax;
    sig->outType = static_cast<SignalLayer::Arg::Type>(m_outType);
    return true;
}

nlohmann::json SetSignalMappingCommand::toJson() const {
    return {{"type", "SetSignalMapping"},
            {"layerEntity", static_cast<std::uint32_t>(m_layerEntity)},
            {"srcMin", m_srcMin}, {"srcMax", m_srcMax},
            {"outMin", m_outMin}, {"outMax", m_outMax},
            {"outType", m_outType}};
}

std::string SetSignalMappingCommand::getDescription() const {
    return "Set signal mapping";
}

CommandPtr SetSignalMappingCommand::fromJson(const nlohmann::json& j) {
    const auto e = static_cast<entt::entity>(j.value("layerEntity", std::uint32_t{0}));
    int outType = 0;
    if (j.contains("outType")) {
        if (j["outType"].is_string()) {
            const std::string s = j["outType"].get<std::string>();
            if (s == "Float")  outType = 1;
            else if (s == "String") outType = 2;
            else               outType = 0; // Int default
        } else {
            outType = j["outType"].get<int>();
        }
    }
    return std::make_unique<SetSignalMappingCommand>(
        e,
        j.value("srcMin", 0.0f), j.value("srcMax", 1.0f),
        j.value("outMin", 0.0f), j.value("outMax", 1.0f),
        outType);
}

} // namespace entity
