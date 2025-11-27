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
#include "entity/media/FrameRingBuffer.hpp"
#include <iostream>
#include <algorithm>

namespace entity {

Timeline::Timeline(entt::registry& registry)
    : m_registry(registry)
{
    std::cout << "[Timeline] Created" << std::endl;
}

void Timeline::update(double deltaTime) {
    if (m_playbackState == PlaybackState::Playing) {
        // Advance current time based on deltaTime
        Timecode deltaTimecode = static_cast<Timecode>(deltaTime * 1000000.0); // Convert seconds to microseconds
        m_currentTime += deltaTimecode;

        // Clamp to duration
        if (m_currentTime > m_duration) {
            m_currentTime = m_duration;
            m_playbackState = PlaybackState::Stopped; // Stop at end
            std::cout << "[Timeline] Reached end, stopping playback" << std::endl;
        }
    }
}

void Timeline::play() {
    if (m_playbackState != PlaybackState::Playing) {
        m_playbackState = PlaybackState::Playing;
        std::cout << "[Timeline] Playing from " << m_currentTime << std::endl;
    }
}

void Timeline::pause() {
    if (m_playbackState == PlaybackState::Playing) {
        m_playbackState = PlaybackState::Paused;
        std::cout << "[Timeline] Paused at " << m_currentTime << std::endl;
    }
}

void Timeline::stop() {
    m_playbackState = PlaybackState::Stopped;
    m_currentTime = 0;
    std::cout << "[Timeline] Stopped, reset to 0" << std::endl;
}

void Timeline::seek(Timecode time) {
    // Pause playback when seeking to prevent freezes
    // (ring buffer gets cleared on seek, main thread would block waiting for frames)
    if (m_playbackState == PlaybackState::Playing) {
        m_playbackState = PlaybackState::Paused;
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

    // Reset selection
    m_selectedClip = entt::null;

    // Reset timeline state
    m_currentTime = 0;
    m_playbackState = PlaybackState::Stopped;

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
    FrameNumber rightMediaStart = clip->mediaStartFrame + leftDuration;

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
    newClip.framerate = clip->framerate;
    newClip.width = clip->width;
    newClip.height = clip->height;
    newClip.hasAlpha = clip->hasAlpha;
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

    // Create new FrameBuffer (with fresh ring buffer)
    auto* srcFrameBuffer = m_registry.try_get<FrameBuffer>(clipEntity);
    if (srcFrameBuffer) {
        auto& newFrameBuffer = m_registry.emplace<FrameBuffer>(newClipEntity);
        newFrameBuffer.ringBuffer = std::make_shared<FrameRingBuffer>(32);
        newFrameBuffer.currentPTS.store(0);
        newFrameBuffer.targetFrame.store(0);
        newFrameBuffer.isBuffering.store(true);
        newFrameBuffer.bufferedFrames.store(0);
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
    newClip.framerate = clip->framerate;
    newClip.width = clip->width;
    newClip.height = clip->height;
    newClip.hasAlpha = clip->hasAlpha;
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

    // Create new FrameBuffer with fresh ring buffer
    auto* srcFrameBuffer = m_registry.try_get<FrameBuffer>(clipEntity);
    if (srcFrameBuffer) {
        auto& newFrameBuffer = m_registry.emplace<FrameBuffer>(newClipEntity);
        newFrameBuffer.ringBuffer = std::make_shared<FrameRingBuffer>(32);
        newFrameBuffer.currentPTS.store(0);
        newFrameBuffer.targetFrame.store(0);
        newFrameBuffer.isBuffering.store(true);
        newFrameBuffer.bufferedFrames.store(0);
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

} // namespace entity
