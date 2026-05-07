/**
 * Timeline Implementation
 *
 * Manages timeline state, playback, and track organization.
 */

#include "entity/timeline/Timeline.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace entity {

Timeline::Timeline(entt::registry& registry)
    : m_registry(registry)
{
    std::cout << "[Timeline] Created" << std::endl;
}

void Timeline::update(double deltaTime) {
    if (m_playbackState.load() == PlaybackState::Playing) {
        // Advance current time based on deltaTime
        Timecode deltaTimecode = static_cast<Timecode>(deltaTime * 1000000.0); // Convert seconds to microseconds
        m_currentTime += deltaTimecode;

        // Clamp to duration
        if (m_currentTime > m_duration) {
            m_currentTime = m_duration;
            m_playbackState.store(PlaybackState::Stopped); // Stop at end
            std::cout << "[Timeline] Reached end, stopping playback" << std::endl;
        }
    }
}

void Timeline::play() {
    if (m_playbackState.load() != PlaybackState::Playing) {
        m_playbackState.store(PlaybackState::Playing);
        std::cout << "[Timeline] Playing from " << m_currentTime << std::endl;
    }
}

void Timeline::pause() {
    if (m_playbackState.load() == PlaybackState::Playing) {
        m_playbackState.store(PlaybackState::Paused);
        std::cout << "[Timeline] Paused at " << m_currentTime << std::endl;
    }
}

void Timeline::stop() {
    m_playbackState.store(PlaybackState::Stopped);
    m_currentTime = 0;
    std::cout << "[Timeline] Stopped, reset to 0" << std::endl;
}

void Timeline::seek(Timecode time) {
    // Pause playback when seeking to prevent freezes
    // (ring buffer gets cleared on seek, main thread would block waiting for frames)
    if (m_playbackState.load() == PlaybackState::Playing) {
        m_playbackState.store(PlaybackState::Paused);
        std::cout << "[Timeline] Auto-paused for seek" << std::endl;
    }

    m_currentTime = time;

    // Clamp to valid range
    if (m_currentTime < 0) {
        m_currentTime = 0;
    }
    if (m_currentTime > m_duration) {
        m_currentTime = m_duration;
    }

    std::cout << "[Timeline] Seek to " << m_currentTime << std::endl;
}

entt::entity Timeline::createTrack(const std::string& name) {
    // Create track entity
    entt::entity trackEntity = m_registry.create();

    // Add TimelineTrack component
    auto& track = m_registry.emplace<TimelineTrack>(trackEntity);
    track.trackIndex = static_cast<uint32_t>(m_tracks.size());

    // Add to tracks list
    m_tracks.push_back(trackEntity);

    std::cout << "[Timeline] Created track " << track.trackIndex
              << " (" << name << "), entity=" << static_cast<uint32_t>(trackEntity) << std::endl;

    return trackEntity;
}

void Timeline::deleteTrack(entt::entity track) {
    // Remove from tracks list
    auto it = std::find(m_tracks.begin(), m_tracks.end(), track);
    if (it != m_tracks.end()) {
        m_tracks.erase(it);

        // Get track component to access clips
        if (m_registry.valid(track)) {
            auto* trackComponent = m_registry.try_get<TimelineTrack>(track);
            if (trackComponent) {
                // Delete all clips in this track
                for (entt::entity clipEntity : trackComponent->clips) {
                    if (m_registry.valid(clipEntity)) {
                        m_registry.destroy(clipEntity);
                    }
                }
            }

            // Delete track entity
            m_registry.destroy(track);

            std::cout << "[Timeline] Deleted track entity=" << static_cast<uint32_t>(track) << std::endl;
        }

        // Reindex remaining tracks
        for (size_t i = 0; i < m_tracks.size(); ++i) {
            if (m_registry.valid(m_tracks[i])) {
                auto* trackComponent = m_registry.try_get<TimelineTrack>(m_tracks[i]);
                if (trackComponent) {
                    trackComponent->trackIndex = static_cast<uint32_t>(i);
                }
            }
        }
    }
}

void Timeline::clear() {
    std::cout << "[Timeline] Clearing all tracks and clips..." << std::endl;

    // Delete all tracks and their clips
    for (entt::entity trackEntity : m_tracks) {
        if (m_registry.valid(trackEntity)) {
            auto* trackComponent = m_registry.try_get<TimelineTrack>(trackEntity);
            if (trackComponent) {
                // Delete all clips in this track
                for (entt::entity clipEntity : trackComponent->clips) {
                    if (m_registry.valid(clipEntity)) {
                        m_registry.destroy(clipEntity);
                    }
                }
            }
            // Delete track entity
            m_registry.destroy(trackEntity);
        }
    }

    // Clear tracks list
    m_tracks.clear();

    // Clear named sections too — they were tied to the project being unloaded.
    m_sections.clear();

    // Cue tags belong to the project; drop them with the timeline.
    m_cueTags.clear();

    // Reset selection
    m_selectedClip = entt::null;
    m_selectedCueNumber.reset();

    // Reset timeline state
    m_currentTime = 0;
    m_playbackState.store(PlaybackState::Stopped);

    std::cout << "[Timeline] Cleared" << std::endl;
}

void Timeline::deleteClip(entt::entity clipEntity) {
    if (!m_registry.valid(clipEntity)) {
        return;
    }

    // Find the track containing this clip
    entt::entity trackEntity = findTrackForClip(clipEntity);
    if (trackEntity != entt::null) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (track) {
            track->removeClip(clipEntity);
        }
    }

    // Destroy the clip entity
    m_registry.destroy(clipEntity);
    std::cout << "[Timeline] Deleted clip entity=" << static_cast<uint32_t>(clipEntity) << std::endl;
}

entt::entity Timeline::splitClip(entt::entity clipEntity, FrameNumber splitFrame) {
    // Validate clip entity
    if (!m_registry.valid(clipEntity)) {
        std::cout << "[Timeline] splitClip: Invalid clip entity" << std::endl;
        return entt::null;
    }

    auto* clip = m_registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cout << "[Timeline] splitClip: Clip component not found" << std::endl;
        return entt::null;
    }

    // Check if split point is within the clip
    FrameNumber clipEnd = clip->startFrame + clip->duration;
    if (splitFrame <= clip->startFrame || splitFrame >= clipEnd) {
        std::cout << "[Timeline] splitClip: Split frame " << splitFrame
                  << " is outside clip range [" << clip->startFrame << ", " << clipEnd << ")" << std::endl;
        return entt::null;
    }

    // Find the track containing this clip
    entt::entity trackEntity = findTrackForClip(clipEntity);
    if (trackEntity == entt::null) {
        std::cout << "[Timeline] splitClip: Clip not found in any track" << std::endl;
        return entt::null;
    }

    auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
    if (!track) {
        return entt::null;
    }

    // Calculate timing for both halves
    FrameNumber leftDuration = splitFrame - clip->startFrame;
    FrameNumber rightStart = splitFrame;
    FrameNumber rightDuration = clipEnd - splitFrame;

    // Convert leftDuration from timeline frames to source frames for mediaStartFrame calculation
    // mediaStartFrame is in source frames, so we need to convert timeline frames properly
    double frameRateRatio = clip->framerate / m_frameRate;
    FrameNumber leftDurationInSourceFrames = static_cast<FrameNumber>(std::floor(leftDuration * frameRateRatio));
    FrameNumber rightMediaStart = clip->mediaStartFrame + leftDurationInSourceFrames;

    std::cout << "[Timeline] Splitting clip at frame " << splitFrame
              << ": left=[" << clip->startFrame << ", dur=" << leftDuration << "]"
              << ", right=[" << rightStart << ", dur=" << rightDuration << ", mediaStart=" << rightMediaStart << "]"
              << std::endl;

    // Create new clip entity for the right half
    entt::entity newClipEntity = m_registry.create();

    // Copy Clip component for right half
    auto& newClip = m_registry.emplace<Clip>(newClipEntity);
    newClip.filepath = clip->filepath;
    newClip.mediaType = clip->mediaType;
    newClip.startFrame = rightStart;
    newClip.duration = rightDuration;
    newClip.mediaStartFrame = rightMediaStart;
    newClip.totalMediaFrames = clip->totalMediaFrames;  // Same source media
    newClip.framerate = clip->framerate;
    newClip.playbackMode = clip->playbackMode;
    newClip.width = clip->width;
    newClip.height = clip->height;
    newClip.hasAlpha = clip->hasAlpha;
    newClip.frameBlending = clip->frameBlending;
    newClip.targetScreen = clip->targetScreen;
    // Note: FFmpeg contexts (formatContext, codecContext, etc.) are NOT copied
    // They will be initialized when the decoder is created for this clip
    newClip.loaded = false;
    newClip.decoding = false;

    // Copy Transform if exists
    auto* srcTransform = m_registry.try_get<Transform>(clipEntity);
    if (srcTransform) {
        auto& newTransform = m_registry.emplace<Transform>(newClipEntity);
        newTransform.position = srcTransform->position;
        newTransform.rotation = srcTransform->rotation;
        newTransform.scale = srcTransform->scale;
        newTransform.dirty = true;
    }

    // Copy MediaLayer if exists
    auto* srcLayer = m_registry.try_get<MediaLayer>(clipEntity);
    if (srcLayer) {
        auto& newLayer = m_registry.emplace<MediaLayer>(newClipEntity);
        newLayer.zOrder = srcLayer->zOrder;
        newLayer.opacity = srcLayer->opacity;
        newLayer.blendMode = srcLayer->blendMode;
        newLayer.visible = srcLayer->visible;
    }

    // Create new VideoTexture (blank - will be populated by decoder)
    auto* srcVideoTex = m_registry.try_get<VideoTexture>(clipEntity);
    if (srcVideoTex) {
        auto& newVideoTex = m_registry.emplace<VideoTexture>(newClipEntity);
        newVideoTex.width = srcVideoTex->width;
        newVideoTex.height = srcVideoTex->height;
        // Note: texture, uploadBuffer, and srvHandle are NOT copied
        // They will be created fresh by the decoder
    }

    // FrameBuffer is just a marker; the engine-global FrameCache holds frames.
    if (m_registry.all_of<FrameBuffer>(clipEntity)) {
        m_registry.emplace<FrameBuffer>(newClipEntity);
    }

    // Copy and adjust AnimatedProperties for split
    auto* srcAnimProps = m_registry.try_get<AnimatedProperties>(clipEntity);
    if (srcAnimProps) {
        // Calculate split offset relative to clip start
        FrameNumber splitOffset = splitFrame - clip->startFrame;

        // Copy AnimatedProperties to right clip
        auto& newAnimProps = m_registry.emplace<AnimatedProperties>(newClipEntity);
        newAnimProps = *srcAnimProps;

        // Adjust keyframe times for right portion (remove keyframes before split, adjust remaining)
        for (auto& kfTrack : newAnimProps.tracks) {
            // Remove keyframes before split point
            kfTrack.keyframes.erase(
                std::remove_if(kfTrack.keyframes.begin(), kfTrack.keyframes.end(),
                    [splitOffset](const Keyframe& kf) { return kf.frame < splitOffset; }),
                kfTrack.keyframes.end());

            // Adjust remaining keyframe times (shift to 0-based for new clip)
            for (auto& kf : kfTrack.keyframes) {
                kf.frame -= splitOffset;
            }
        }

        // Truncate keyframes on original (left) clip to before split point
        for (auto& kfTrack : srcAnimProps->tracks) {
            kfTrack.keyframes.erase(
                std::remove_if(kfTrack.keyframes.begin(), kfTrack.keyframes.end(),
                    [splitOffset](const Keyframe& kf) { return kf.frame >= splitOffset; }),
                kfTrack.keyframes.end());
        }

        std::cout << "[Timeline] Split AnimatedProperties: left has "
                  << srcAnimProps->getTotalKeyframeCount() << " keyframes, right has "
                  << newAnimProps.getTotalKeyframeCount() << " keyframes" << std::endl;
    }

    // Modify original clip to be the left half
    clip->duration = leftDuration;

    // Add new clip to the same track
    track->clips.push_back(newClipEntity);

    // Clear selection to avoid confusion
    m_selectedClip = entt::null;

    std::cout << "[Timeline] Split complete: original entity=" << static_cast<uint32_t>(clipEntity)
              << ", new entity=" << static_cast<uint32_t>(newClipEntity) << std::endl;

    // Notify Engine to create decoder/resources for new clip
    if (m_clipCreatedCallback) {
        m_clipCreatedCallback(newClipEntity, newClip.filepath);
    }

    return newClipEntity;
}

entt::entity Timeline::duplicateClip(entt::entity clipEntity) {
    // Validate clip entity
    if (!m_registry.valid(clipEntity)) {
        std::cout << "[Timeline] duplicateClip: Invalid clip entity" << std::endl;
        return entt::null;
    }

    auto* clip = m_registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cout << "[Timeline] duplicateClip: Clip component not found" << std::endl;
        return entt::null;
    }

    // Find the track containing this clip
    entt::entity trackEntity = findTrackForClip(clipEntity);
    if (trackEntity == entt::null) {
        std::cout << "[Timeline] duplicateClip: Clip not found in any track" << std::endl;
        return entt::null;
    }

    auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
    if (!track) {
        return entt::null;
    }

    // Calculate position for duplicate (immediately after original)
    FrameNumber newStartFrame = clip->startFrame + clip->duration;

    std::cout << "[Timeline] Duplicating clip at frame " << clip->startFrame
              << " to frame " << newStartFrame << std::endl;

    // Create new clip entity
    entt::entity newClipEntity = m_registry.create();

    // Copy Clip component
    auto& newClip = m_registry.emplace<Clip>(newClipEntity);
    newClip.filepath = clip->filepath;
    newClip.mediaType = clip->mediaType;
    newClip.startFrame = newStartFrame;
    newClip.duration = clip->duration;
    newClip.mediaStartFrame = clip->mediaStartFrame;  // Same media start
    newClip.totalMediaFrames = clip->totalMediaFrames;
    newClip.framerate = clip->framerate;
    newClip.playbackMode = clip->playbackMode;
    newClip.width = clip->width;
    newClip.height = clip->height;
    newClip.hasAlpha = clip->hasAlpha;
    newClip.frameBlending = clip->frameBlending;
    newClip.targetScreen = clip->targetScreen;
    newClip.loaded = false;
    newClip.decoding = false;

    // Copy Transform if exists
    auto* srcTransform = m_registry.try_get<Transform>(clipEntity);
    if (srcTransform) {
        auto& newTransform = m_registry.emplace<Transform>(newClipEntity);
        newTransform.position = srcTransform->position;
        newTransform.rotation = srcTransform->rotation;
        newTransform.scale = srcTransform->scale;
        newTransform.dirty = true;
    }

    // Copy MediaLayer if exists
    auto* srcLayer = m_registry.try_get<MediaLayer>(clipEntity);
    if (srcLayer) {
        auto& newLayer = m_registry.emplace<MediaLayer>(newClipEntity);
        newLayer.zOrder = srcLayer->zOrder;
        newLayer.opacity = srcLayer->opacity;
        newLayer.blendMode = srcLayer->blendMode;
        newLayer.visible = srcLayer->visible;
    }

    // Create new VideoTexture
    auto* srcVideoTex = m_registry.try_get<VideoTexture>(clipEntity);
    if (srcVideoTex) {
        auto& newVideoTex = m_registry.emplace<VideoTexture>(newClipEntity);
        newVideoTex.width = srcVideoTex->width;
        newVideoTex.height = srcVideoTex->height;
    }

    // FrameBuffer is just a marker; the engine-global FrameCache holds frames.
    if (m_registry.all_of<FrameBuffer>(clipEntity)) {
        m_registry.emplace<FrameBuffer>(newClipEntity);
    }

    // Copy AnimatedProperties if exists (duplicate gets identical keyframes)
    auto* srcAnimProps = m_registry.try_get<AnimatedProperties>(clipEntity);
    if (srcAnimProps) {
        auto& newAnimProps = m_registry.emplace<AnimatedProperties>(newClipEntity);
        newAnimProps = *srcAnimProps;

        std::cout << "[Timeline] Duplicated AnimatedProperties with "
                  << newAnimProps.getTotalKeyframeCount() << " keyframes" << std::endl;
    }

    // Add new clip to the same track
    track->clips.push_back(newClipEntity);

    // Select the new clip
    m_selectedClip = newClipEntity;

    std::cout << "[Timeline] Duplicate complete: new entity=" << static_cast<uint32_t>(newClipEntity) << std::endl;

    // Notify Engine to create decoder/resources for new clip
    if (m_clipCreatedCallback) {
        m_clipCreatedCallback(newClipEntity, newClip.filepath);
    }

    return newClipEntity;
}

entt::entity Timeline::findTrackForClip(entt::entity clipEntity) const {
    for (entt::entity trackEntity : m_tracks) {
        const auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (track) {
            auto it = std::find(track->clips.begin(), track->clips.end(), clipEntity);
            if (it != track->clips.end()) {
                return trackEntity;
            }
        }
    }
    return entt::null;
}

bool Timeline::moveClipToTrack(entt::entity clipEntity, int newTrackIndex) {
    // Validate target track index
    if (newTrackIndex < 0 || newTrackIndex >= static_cast<int>(m_tracks.size())) {
        return false;
    }

    // Find current track containing the clip
    entt::entity currentTrackEntity = findTrackForClip(clipEntity);
    if (currentTrackEntity == entt::null) {
        return false;
    }

    // Get current and target track components
    auto* currentTrack = m_registry.try_get<TimelineTrack>(currentTrackEntity);
    auto* targetTrack = m_registry.try_get<TimelineTrack>(m_tracks[newTrackIndex]);

    if (!currentTrack || !targetTrack) {
        return false;
    }

    // Check if already on target track
    if (currentTrackEntity == m_tracks[newTrackIndex]) {
        return true;  // Already on correct track
    }

    // Remove from current track
    auto it = std::find(currentTrack->clips.begin(), currentTrack->clips.end(), clipEntity);
    if (it != currentTrack->clips.end()) {
        currentTrack->clips.erase(it);
    }

    // Add to target track
    targetTrack->clips.push_back(clipEntity);

    // Update zOrder to match new track
    // Track 0 (top of timeline UI) should render on top = highest z-order
    auto* layer = m_registry.try_get<MediaLayer>(clipEntity);
    if (layer) {
        layer->zOrder = 1000 - newTrackIndex;
        std::cout << "[Timeline] Updated clip zOrder to " << layer->zOrder << " (track " << newTrackIndex << ")" << std::endl;
    }

    std::cout << "[Timeline] Moved clip entity=" << static_cast<uint32_t>(clipEntity)
              << " from track " << currentTrack->trackIndex
              << " to track " << targetTrack->trackIndex << std::endl;

    return true;
}

// ============================================================================
// Ripple time edits
// ============================================================================

Timeline::RippleInsertResult Timeline::rippleInsertTime(FrameNumber insertFrame, FrameNumber durationFrames) {
    RippleInsertResult result{};
    if (insertFrame < 0 || durationFrames <= 0) {
        std::cerr << "[Timeline] rippleInsertTime: bad args (insertFrame=" << insertFrame
                  << ", durationFrames=" << durationFrames << ")" << std::endl;
        return result;
    }

    // Phase 1: split clips that span insertFrame. Capture original duration
    // and AnimatedProperties so undo can merge cleanly.
    // Snapshot the entity list first because splitClip mutates track->clips.
    std::vector<entt::entity> toSplit;
    for (entt::entity trackEntity : m_tracks) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;
        for (entt::entity clipEntity : track->clips) {
            auto* clip = m_registry.try_get<Clip>(clipEntity);
            if (!clip) continue;
            FrameNumber endF = clip->startFrame + clip->duration;
            if (clip->startFrame < insertFrame && endF > insertFrame) {
                toSplit.push_back(clipEntity);
            }
        }
    }
    for (entt::entity e : toSplit) {
        auto* clip = m_registry.try_get<Clip>(e);
        if (!clip) continue;
        ClipSplitRecord rec;
        rec.originalEntity = e;
        rec.oldDuration = clip->duration;
        if (auto* ap = m_registry.try_get<AnimatedProperties>(e)) {
            rec.hadAnimProps = true;
            rec.oldAnimProps = *ap;
        }
        rec.newRightEntity = splitClip(e, insertFrame);
        if (rec.newRightEntity == entt::null) {
            std::cerr << "[Timeline] rippleInsertTime: splitClip failed mid-op; aborting." << std::endl;
            // Roll back any splits we already did so we don't leave the timeline half-modified.
            for (auto it = result.splits.rbegin(); it != result.splits.rend(); ++it) {
                if (m_registry.valid(it->newRightEntity)) {
                    deleteClip(it->newRightEntity);
                }
                if (auto* origClip = m_registry.try_get<Clip>(it->originalEntity)) {
                    origClip->duration = it->oldDuration;
                }
                if (it->hadAnimProps) {
                    if (auto* ap = m_registry.try_get<AnimatedProperties>(it->originalEntity)) {
                        *ap = it->oldAnimProps;
                    }
                }
            }
            return result;
        }
        result.splits.push_back(std::move(rec));
    }

    // Phase 2: shift everything starting at or after insertFrame.
    for (entt::entity trackEntity : m_tracks) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;
        for (entt::entity clipEntity : track->clips) {
            auto* clip = m_registry.try_get<Clip>(clipEntity);
            if (!clip) continue;
            if (clip->startFrame >= insertFrame) {
                result.shifted.push_back({clipEntity, clip->startFrame});
                clip->startFrame += durationFrames;
            }
        }
    }

    result.success = true;
    std::cout << "[Timeline] rippleInsertTime: inserted " << durationFrames
              << " frames at " << insertFrame
              << " (split " << result.splits.size()
              << ", shifted " << result.shifted.size() << ")" << std::endl;
    return result;
}

void Timeline::undoRippleInsertTime(RippleInsertResult& record) {
    if (!record.success) return;

    // Reverse Phase 2: restore shifted startFrames.
    for (auto& s : record.shifted) {
        if (auto* clip = m_registry.try_get<Clip>(s.entity)) {
            clip->startFrame = s.oldStartFrame;
        }
    }
    record.shifted.clear();

    // Reverse Phase 1: undo splits in reverse order. Delete the right halves
    // and restore the left halves' duration + AnimatedProperties.
    for (auto it = record.splits.rbegin(); it != record.splits.rend(); ++it) {
        if (m_registry.valid(it->newRightEntity)) {
            deleteClip(it->newRightEntity);
        }
        if (auto* clip = m_registry.try_get<Clip>(it->originalEntity)) {
            clip->duration = it->oldDuration;
        }
        if (it->hadAnimProps) {
            if (auto* ap = m_registry.try_get<AnimatedProperties>(it->originalEntity)) {
                *ap = it->oldAnimProps;
            } else {
                m_registry.emplace<AnimatedProperties>(it->originalEntity, it->oldAnimProps);
            }
        }
    }
    record.splits.clear();
    record.success = false;
}

Timeline::RippleDeleteResult Timeline::rippleDeleteTime(FrameNumber rangeStart, FrameNumber rangeEnd) {
    RippleDeleteResult result{};
    if (rangeStart < 0 || rangeEnd <= rangeStart) {
        std::cerr << "[Timeline] rippleDeleteTime: bad range [" << rangeStart << ", " << rangeEnd << ")" << std::endl;
        return result;
    }
    const FrameNumber removeDur = rangeEnd - rangeStart;

    // Pre-flight: refuse if any clip overlaps the range. Splitting + recreating
    // deleted clips needs a different undo path than a simple shift snapshot;
    // saving that for v2.
    for (entt::entity trackEntity : m_tracks) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;
        for (entt::entity clipEntity : track->clips) {
            auto* clip = m_registry.try_get<Clip>(clipEntity);
            if (!clip) continue;
            const FrameNumber endF = clip->startFrame + clip->duration;
            const bool overlaps = (clip->startFrame < rangeEnd) && (endF > rangeStart);
            if (overlaps) {
                std::cerr << "[Timeline] rippleDeleteTime: aborted — clip entity="
                          << static_cast<uint32_t>(clipEntity)
                          << " (frames [" << clip->startFrame << ", " << endF << "))"
                          << " overlaps [" << rangeStart << ", " << rangeEnd << "). "
                          << "Split or move overlapping clips, then retry." << std::endl;
                return result;
            }
        }
    }

    // Shift everything entirely after rangeEnd left by removeDur.
    for (entt::entity trackEntity : m_tracks) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;
        for (entt::entity clipEntity : track->clips) {
            auto* clip = m_registry.try_get<Clip>(clipEntity);
            if (!clip) continue;
            if (clip->startFrame >= rangeEnd) {
                result.shifted.push_back({clipEntity, clip->startFrame});
                clip->startFrame -= removeDur;
            }
        }
    }

    result.success = true;
    std::cout << "[Timeline] rippleDeleteTime: removed [" << rangeStart << ", " << rangeEnd
              << ") (shifted " << result.shifted.size() << " clips)" << std::endl;
    return result;
}

void Timeline::undoRippleDeleteTime(RippleDeleteResult& record) {
    if (!record.success) return;
    for (auto& s : record.shifted) {
        if (auto* clip = m_registry.try_get<Clip>(s.entity)) {
            clip->startFrame = s.oldStartFrame;
        }
    }
    record.shifted.clear();
    record.success = false;
}

// ============================================================================
// Cue tags
// ============================================================================

bool Timeline::addCueTag(CueTag tag) {
    auto it = std::lower_bound(m_cueTags.begin(), m_cueTags.end(), tag.number,
        [](const CueTag& c, double v) { return c.number < v; });
    if (it != m_cueTags.end() && it->number == tag.number) {
        std::cerr << "[Timeline] addCueTag: rejected duplicate cue number "
                  << tag.number << std::endl;
        return false;
    }
    m_cueTags.insert(it, std::move(tag));
    return true;
}

bool Timeline::removeCueTag(double number) {
    auto it = std::lower_bound(m_cueTags.begin(), m_cueTags.end(), number,
        [](const CueTag& c, double v) { return c.number < v; });
    if (it == m_cueTags.end() || it->number != number) {
        return false;
    }
    m_cueTags.erase(it);
    if (m_selectedCueNumber == number) m_selectedCueNumber.reset();
    return true;
}

const CueTag* Timeline::findCueTag(double number) const {
    auto it = std::lower_bound(m_cueTags.begin(), m_cueTags.end(), number,
        [](const CueTag& c, double v) { return c.number < v; });
    if (it == m_cueTags.end() || it->number != number) return nullptr;
    return &(*it);
}

bool Timeline::editCueTag(double oldNumber, double newNumber,
                          Timecode newTimestamp, std::string newLabel) {
    auto oldIt = std::lower_bound(m_cueTags.begin(), m_cueTags.end(), oldNumber,
        [](const CueTag& c, double v) { return c.number < v; });
    if (oldIt == m_cueTags.end() || oldIt->number != oldNumber) {
        std::cerr << "[Timeline] editCueTag: no cue with number " << oldNumber << std::endl;
        return false;
    }

    if (newNumber != oldNumber) {
        // Reject collisions with other cues. Self-match is fine because we'll
        // erase the old slot before inserting at the new one.
        auto collide = std::lower_bound(m_cueTags.begin(), m_cueTags.end(), newNumber,
            [](const CueTag& c, double v) { return c.number < v; });
        if (collide != m_cueTags.end() && collide->number == newNumber) {
            std::cerr << "[Timeline] editCueTag: cue number " << newNumber
                      << " already exists" << std::endl;
            return false;
        }
    }

    // Mutate in place when number didn't change; otherwise erase + sorted-insert.
    if (newNumber == oldNumber) {
        oldIt->timestamp = newTimestamp;
        oldIt->label = std::move(newLabel);
    } else {
        CueTag updated{newNumber, newTimestamp, std::move(newLabel)};
        m_cueTags.erase(oldIt);
        auto insertIt = std::lower_bound(m_cueTags.begin(), m_cueTags.end(), updated.number,
            [](const CueTag& c, double v) { return c.number < v; });
        m_cueTags.insert(insertIt, std::move(updated));
        if (m_selectedCueNumber == oldNumber) m_selectedCueNumber = newNumber;
    }
    return true;
}

} // namespace entity
