/**
 * Timeline Implementation
 *
 * Manages timeline state, playback, and track organization.
 */

#include "entity/timeline/Timeline.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace entity {

Timeline::Timeline(entt::registry& registry)
    : m_registry(registry)
{
    std::cout << "[Timeline] Created" << std::endl;
}

void Timeline::update(double deltaTime) {
    ZoneScopedN("Timeline::update");
    if (m_playbackState.load() == PlaybackState::Playing) {
        // Advance current time based on deltaTime
        const Timecode deltaTimecode = static_cast<Timecode>(deltaTime * 1000000.0);
        const Timecode newTime = m_currentTime.fetch_add(deltaTimecode,
                                                         std::memory_order_relaxed)
                                 + deltaTimecode;

        // Clamp to duration. Tiny race window between fetch_add and the
        // clamp store: a concurrent seek() can land here and get overwritten,
        // but seek pauses playback so we won't be advancing the next tick.
        if (newTime > m_duration) {
            m_currentTime.store(m_duration, std::memory_order_relaxed);
            m_playbackState.store(PlaybackState::Stopped); // Stop at end
            std::cout << "[Timeline] Reached end, stopping playback" << std::endl;
        }
    }
}

void Timeline::play() {
    if (m_playbackState.load() != PlaybackState::Playing) {
        m_playbackState.store(PlaybackState::Playing);
        std::cout << "[Timeline] Playing from " << getCurrentTime() << std::endl;
    }
}

void Timeline::pause() {
    if (m_playbackState.load() == PlaybackState::Playing) {
        m_playbackState.store(PlaybackState::Paused);
        // Playhead is frame-quantized at rest. Snap on the play->pause
        // transition so the paused position lands exactly on a frame —
        // keyframe diamonds and the playhead line sit at the same pixel.
        const Timecode now = m_currentTime.load(std::memory_order_relaxed);
        const Timecode snapped = frameToTime(timeToFrame(now));
        m_currentTime.store(snapped, std::memory_order_relaxed);
        std::cout << "[Timeline] Paused at " << getCurrentTime() << std::endl;
    }
}

void Timeline::stop() {
    m_playbackState.store(PlaybackState::Stopped);
    m_currentTime.store(0, std::memory_order_relaxed);
    std::cout << "[Timeline] Stopped, reset to 0" << std::endl;
}

void Timeline::seek(Timecode time) {
    // Pause playback when seeking to prevent freezes
    // (ring buffer gets cleared on seek, main thread would block waiting for frames)
    if (m_playbackState.load() == PlaybackState::Playing) {
        m_playbackState.store(PlaybackState::Paused);
        std::cout << "[Timeline] Auto-paused for seek" << std::endl;
    }

    // Snap to nearest integer frame. The playhead has no sub-frame
    // semantics at rest — see feedback_playhead_frame_quantized_at_rest.
    // Per-tick playback advance in update() keeps sub-frame precision
    // for smooth motion; only user-driven rests are quantized.
    const Timecode snapped = frameToTime(timeToFrame(time));
    const Timecode clamped = std::max<Timecode>(0, std::min(snapped, m_duration));
    m_currentTime.store(clamped, std::memory_order_relaxed);

    // At-break state is owned by SectionScheduler's park flow, not by manual
    // seeks. Any user-driven seek (UI scrub, script SeekToFrame, command
    // playback) breaks the at-break invariant — clear it so the Phase 5
    // visibility gate doesn't misfire on the new playhead position.
    setSectionAtBreak(false);

    std::cout << "[Timeline] Seek to " << clamped << std::endl;
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
                // Delete all layers in this track
                for (entt::entity layerEntity : trackComponent->layers) {
                    if (m_registry.valid(layerEntity)) {
                        m_registry.destroy(layerEntity);
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
                // Delete all layers in this track
                for (entt::entity layerEntity : trackComponent->layers) {
                    if (m_registry.valid(layerEntity)) {
                        m_registry.destroy(layerEntity);
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
    m_selectedSectionBreakFrame.reset();

    // Reset timeline state
    m_currentTime.store(0, std::memory_order_relaxed);
    m_playbackState.store(PlaybackState::Stopped);
    m_atSectionBreak.store(false);

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
            track->removeLayer(clipEntity);
        }
    }

    // Destroy the clip entity
    m_registry.destroy(clipEntity);
    std::cout << "[Timeline] Deleted clip entity=" << static_cast<uint32_t>(clipEntity) << std::endl;
}

Timeline::DeletedClipSnapshot Timeline::snapshotClipForDelete(entt::entity clipEntity) const {
    DeletedClipSnapshot snap;
    if (!m_registry.valid(clipEntity)) {
        return snap;
    }

    // Resolve the containing track first — both archetypes go through this.
    // findTrackForClip is non-const; recreate its (small) lookup here so we
    // can keep snapshotClipForDelete const-qualified.
    auto trackView = m_registry.view<TimelineTrack>();
    for (auto trackEnt : trackView) {
        const auto& track = trackView.get<TimelineTrack>(trackEnt);
        if (std::find(track.layers.begin(), track.layers.end(), clipEntity) != track.layers.end()) {
            snap.trackEntity = trackEnt;
            break;
        }
    }
    if (snap.trackEntity == entt::null) {
        return snap;  // Entity isn't on any track — nothing to snapshot.
    }

    // Capture the Layer metadata first — both archetypes carry one and we
    // restore name/color from the snapshot so delete+undo round-trips them.
    if (const auto* lay = m_registry.try_get<Layer>(clipEntity)) {
        snap.layerName  = lay->name;
        snap.layerColor = lay->color;
    }

    const auto* clip = m_registry.try_get<Clip>(clipEntity);
    const auto* oal  = m_registry.try_get<ObjectAnimationLayer>(clipEntity);
    const auto* gen  = m_registry.try_get<GenerativeLayer>(clipEntity);

    if (clip) {
        snap.kind             = DeletedLayerKind::Clip;
        snap.filepath         = clip->filepath;
        snap.mediaType        = clip->mediaType;
        snap.startFrame       = clip->startFrame;
        snap.duration         = clip->duration;
        snap.mediaStartFrame  = clip->mediaStartFrame;
        snap.mediaOutFrame    = clip->mediaOutFrame;
        snap.totalMediaFrames = clip->totalMediaFrames;
        snap.framerate        = clip->framerate;
        snap.playbackMode     = clip->playbackMode;
        snap.sectionBehavior  = clip->sectionBehavior;
        snap.width            = clip->width;
        snap.height           = clip->height;
        snap.hasAlpha         = clip->hasAlpha;
        snap.frameBlending    = clip->frameBlending;
        snap.targetScreen     = clip->targetScreen;

        if (const auto* tr = m_registry.try_get<Transform>(clipEntity)) {
            snap.hadTransform = true;
            snap.transform    = *tr;
        }
        if (const auto* ml = m_registry.try_get<MediaLayer>(clipEntity)) {
            snap.hadMediaLayer = true;
            snap.mediaLayer    = *ml;
        }
        if (const auto* vt = m_registry.try_get<VideoTexture>(clipEntity)) {
            snap.hadVideoTexture = true;
            snap.videoTexWidth   = vt->width;
            snap.videoTexHeight  = vt->height;
        }
        snap.hadFrameBuffer = m_registry.all_of<FrameBuffer>(clipEntity);
    } else if (oal) {
        snap.kind              = DeletedLayerKind::ObjectAnimation;
        snap.oaTarget          = oal->target;
        snap.oaSectionBehavior = oal->sectionBehavior;
        snap.oaEndBehavior     = oal->endBehavior;
        if (const auto* lay = m_registry.try_get<Layer>(clipEntity)) {
            snap.startFrame = lay->startFrame;
            snap.duration   = lay->duration;
        }
    } else if (gen) {
        snap.kind    = DeletedLayerKind::Generative;
        snap.genLayer = *gen;
        if (const auto* lay = m_registry.try_get<Layer>(clipEntity)) {
            snap.startFrame = lay->startFrame;
            snap.duration   = lay->duration;
        }
        if (const auto* tr = m_registry.try_get<Transform>(clipEntity)) {
            snap.hadTransform = true;
            snap.transform    = *tr;
        }
        if (const auto* ml = m_registry.try_get<MediaLayer>(clipEntity)) {
            snap.hadMediaLayer = true;
            snap.mediaLayer    = *ml;
        }
        if (const auto* muncher = m_registry.try_get<MunchersGameState>(clipEntity)) {
            snap.hadMuncher    = true;
            snap.munchersState = *muncher;
        }
        if (const auto* tls = m_registry.try_get<TextLayerState>(clipEntity)) {
            snap.hadTextLayerState = true;
            snap.textLayerState    = *tls;
        }
    } else {
        // Unknown archetype — clear track entity to mark invalid.
        snap.trackEntity = entt::null;
        return snap;
    }

    if (const auto* ap = m_registry.try_get<AnimatedProperties>(clipEntity)) {
        snap.hadAnimatedProperties = true;
        snap.animatedProperties    = *ap;
    }
    return snap;
}

entt::entity Timeline::restoreDeletedClip(const DeletedClipSnapshot& snap) {
    if (!snap.valid() || !m_registry.valid(snap.trackEntity)) {
        std::cerr << "[Timeline] restoreDeletedClip: invalid snapshot or track gone" << std::endl;
        return entt::null;
    }
    auto* track = m_registry.try_get<TimelineTrack>(snap.trackEntity);
    if (!track) {
        std::cerr << "[Timeline] restoreDeletedClip: track has no TimelineTrack component" << std::endl;
        return entt::null;
    }

    entt::entity newEntity = m_registry.create();

    if (snap.kind == DeletedLayerKind::ObjectAnimation) {
        // --- OA layer restore --------------------------------------------
        // No Clip / Transform / MediaLayer / VideoTexture / FrameBuffer.
        // Layer placement comes directly from the snapshot (no Clip mirror).
        auto& lay = m_registry.emplace<Layer>(newEntity);
        lay.kind       = Layer::Kind::ObjectAnimation;
        lay.startFrame = snap.startFrame;
        lay.duration   = snap.duration;
        lay.name       = snap.layerName;
        lay.color      = snap.layerColor;
        if (auto* tt = m_registry.try_get<TimelineTrack>(snap.trackEntity)) {
            lay.trackIndex = tt->trackIndex;
        }

        auto& oal = m_registry.emplace<ObjectAnimationLayer>(newEntity);
        // Restored target entity stays valid within the same session — the
        // undo stack is cleared on project save/load (see DeleteClipCommand)
        // so we don't need to round-trip target by name here.
        oal.target          = m_registry.valid(snap.oaTarget) ? snap.oaTarget : entt::null;
        oal.sectionBehavior = snap.oaSectionBehavior;
        oal.endBehavior     = snap.oaEndBehavior;

        if (snap.hadAnimatedProperties) {
            m_registry.emplace<AnimatedProperties>(newEntity) = snap.animatedProperties;
        }

        track->layers.push_back(newEntity);
        track->sortLayers(m_registry);
        m_selectedClip = newEntity;

        std::cout << "[Timeline] Restored OA layer entity="
                  << static_cast<uint32_t>(newEntity)
                  << " on track=" << static_cast<uint32_t>(snap.trackEntity)
                  << " at frame=" << snap.startFrame << std::endl;
        // No clip-created callback for OA — there's no decoder / GPU slot to
        // provision. AnimationSystem picks up the entity on the next tick.
        return newEntity;
    }

    if (snap.kind == DeletedLayerKind::Generative) {
        // --- Generative layer restore ------------------------------------
        auto& lay = m_registry.emplace<Layer>(newEntity);
        lay.kind       = Layer::Kind::Generative;
        lay.startFrame = snap.startFrame;
        lay.duration   = snap.duration;
        lay.name       = snap.layerName;
        lay.color      = snap.layerColor;
        if (auto* tt = m_registry.try_get<TimelineTrack>(snap.trackEntity)) {
            lay.trackIndex = tt->trackIndex;
        }

        // GenerativeLayer — restore except renderTargetSlot which must be
        // re-allocated by CompositorSystem on the first show-thread tick.
        auto& gen = m_registry.emplace<GenerativeLayer>(newEntity);
        gen.targetScreen  = m_registry.valid(snap.genLayer.targetScreen)
                                ? snap.genLayer.targetScreen : entt::null;
        gen.renderWidth   = snap.genLayer.renderWidth;
        gen.renderHeight  = snap.genLayer.renderHeight;
        gen.renderTargetSlot = -1;  // fresh slot; show thread provisions it

        if (snap.hadTransform) {
            auto& t = m_registry.emplace<Transform>(newEntity);
            t       = snap.transform;
            t.dirty = true;
        }
        if (snap.hadMediaLayer) {
            m_registry.emplace<MediaLayer>(newEntity) = snap.mediaLayer;
        }

        if (snap.hadTextLayerState) {
            auto& tls = m_registry.emplace<TextLayerState>(newEntity);
            tls             = snap.textLayerState;
            tls.dirty       = true;  // force re-rasterize
            tls.textureSlot = -1;    // fresh slot
            tls.bakedWidth  = 0;
            tls.bakedHeight = 0;
        } else if (snap.hadMuncher) {
            // Restore game state but reset the tick counter — session restart.
            auto& gs = m_registry.emplace<MunchersGameState>(newEntity);
            gs.simFrame = 0;
        }

        if (snap.hadAnimatedProperties) {
            m_registry.emplace<AnimatedProperties>(newEntity) = snap.animatedProperties;
        }

        track->layers.push_back(newEntity);
        track->sortLayers(m_registry);
        m_selectedClip = newEntity;

        std::cout << "[Timeline] Restored generative layer entity="
                  << static_cast<uint32_t>(newEntity)
                  << " on track=" << static_cast<uint32_t>(snap.trackEntity)
                  << " at frame=" << snap.startFrame << std::endl;
        // No clip-created callback — GenerativeSystem picks up on next tick.
        return newEntity;
    }

    // --- Clip restore ----------------------------------------------------
    auto& clip = m_registry.emplace<Clip>(newEntity);
    clip.filepath         = snap.filepath;
    clip.mediaType        = snap.mediaType;
    clip.startFrame       = snap.startFrame;
    clip.duration         = snap.duration;
    clip.mediaStartFrame  = snap.mediaStartFrame;
    clip.mediaOutFrame    = snap.mediaOutFrame;
    clip.totalMediaFrames = snap.totalMediaFrames;
    clip.framerate        = snap.framerate;
    clip.playbackMode     = snap.playbackMode;
    clip.sectionBehavior  = snap.sectionBehavior;
    clip.width            = snap.width;
    clip.height           = snap.height;
    clip.hasAlpha         = snap.hasAlpha;
    clip.frameBlending    = snap.frameBlending;
    clip.targetScreen     = snap.targetScreen;
    clip.loaded           = false;   // The clip-created callback re-opens the decoder.
    clip.decoding         = false;

    if (snap.hadTransform) {
        auto& tr = m_registry.emplace<Transform>(newEntity);
        tr        = snap.transform;
        tr.dirty  = true;            // Force matrix recompute for the new entity.
    }
    if (snap.hadMediaLayer) {
        m_registry.emplace<MediaLayer>(newEntity) = snap.mediaLayer;
    }
    if (snap.hadVideoTexture) {
        auto& vt = m_registry.emplace<VideoTexture>(newEntity);
        vt.width  = snap.videoTexWidth;
        vt.height = snap.videoTexHeight;
        // descriptorSlot stays at UINT32_MAX so onClipCreated provisions a fresh slot.
    }
    if (snap.hadFrameBuffer) {
        m_registry.emplace<FrameBuffer>(newEntity);
    }
    if (snap.hadAnimatedProperties) {
        m_registry.emplace<AnimatedProperties>(newEntity) = snap.animatedProperties;
    }

    {
        auto& lay = m_registry.emplace<Layer>(newEntity);
        lay.kind  = Layer::Kind::Clip;
        // Restore Layer name/color too — previously lost on delete+undo
        // because the snapshot didn't carry them. layerName/layerColor are
        // always populated by snapshotClipForDelete now.
        lay.name  = snap.layerName;
        lay.color = snap.layerColor;
    }
    syncLayerFromClip(m_registry, newEntity);
    track->layers.push_back(newEntity);
    track->sortLayers(m_registry);

    m_selectedClip = newEntity;

    std::cout << "[Timeline] Restored clip entity=" << static_cast<uint32_t>(newEntity)
              << " on track=" << static_cast<uint32_t>(snap.trackEntity)
              << " at frame=" << snap.startFrame << std::endl;

    if (m_clipCreatedCallback) {
        m_clipCreatedCallback(newEntity, clip.filepath);
    }
    return newEntity;
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
    newClip.mediaOutFrame = clip->mediaOutFrame;        // Same source out-point
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

    // Layer companion — right-half inherits Clip kind.
    {
        auto& lay = m_registry.emplace<Layer>(newClipEntity);
        lay.kind = Layer::Kind::Clip;
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
    syncLayerFromClip(m_registry, clipEntity);

    // Add new clip to the same track
    syncLayerFromClip(m_registry, newClipEntity);
    track->layers.push_back(newClipEntity);
    track->sortLayers(m_registry);

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
    newClip.mediaOutFrame = clip->mediaOutFrame;
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

    // Layer companion — duplicate inherits Clip kind.
    {
        auto& lay = m_registry.emplace<Layer>(newClipEntity);
        lay.kind = Layer::Kind::Clip;
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
    syncLayerFromClip(m_registry, newClipEntity);
    track->layers.push_back(newClipEntity);
    track->sortLayers(m_registry);

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
            auto it = std::find(track->layers.begin(), track->layers.end(), clipEntity);
            if (it != track->layers.end()) {
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
    auto it = std::find(currentTrack->layers.begin(), currentTrack->layers.end(), clipEntity);
    if (it != currentTrack->layers.end()) {
        currentTrack->layers.erase(it);
    }

    // Add to target track
    targetTrack->layers.push_back(clipEntity);

    // Update zOrder to match new track
    // Track 0 (top of timeline UI) should render on top = highest z-order
    auto* layer = m_registry.try_get<MediaLayer>(clipEntity);
    if (layer) {
        layer->zOrder = 1000 - newTrackIndex;
        std::cout << "[Timeline] Updated clip zOrder to " << layer->zOrder << " (track " << newTrackIndex << ")" << std::endl;
    }
    if (auto* lay = m_registry.try_get<Layer>(clipEntity)) {
        lay->trackIndex = static_cast<uint32_t>(newTrackIndex);
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
    // Snapshot the entity list first because splitClip mutates track->layers.
    std::vector<entt::entity> toSplit;
    for (entt::entity trackEntity : m_tracks) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;
        for (entt::entity layerEntity : track->layers) {
            auto* clip = m_registry.try_get<Clip>(layerEntity);
            if (!clip) continue;
            FrameNumber endF = clip->startFrame + clip->duration;
            if (clip->startFrame < insertFrame && endF > insertFrame) {
                toSplit.push_back(layerEntity);
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
        for (entt::entity layerEntity : track->layers) {
            auto* clip = m_registry.try_get<Clip>(layerEntity);
            if (!clip) continue;
            if (clip->startFrame >= insertFrame) {
                result.shifted.push_back({layerEntity, clip->startFrame});
                clip->startFrame += durationFrames;
                syncLayerFromClip(m_registry, layerEntity);
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
            syncLayerFromClip(m_registry, s.entity);
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
        for (entt::entity layerEntity : track->layers) {
            auto* clip = m_registry.try_get<Clip>(layerEntity);
            if (!clip) continue;
            const FrameNumber endF = clip->startFrame + clip->duration;
            const bool overlaps = (clip->startFrame < rangeEnd) && (endF > rangeStart);
            if (overlaps) {
                std::cerr << "[Timeline] rippleDeleteTime: aborted — clip entity="
                          << static_cast<uint32_t>(layerEntity)
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
        for (entt::entity layerEntity : track->layers) {
            auto* clip = m_registry.try_get<Clip>(layerEntity);
            if (!clip) continue;
            if (clip->startFrame >= rangeEnd) {
                result.shifted.push_back({layerEntity, clip->startFrame});
                clip->startFrame -= removeDur;
                syncLayerFromClip(m_registry, layerEntity);
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
            syncLayerFromClip(m_registry, s.entity);
        }
    }
    record.shifted.clear();
    record.success = false;
}

// ============================================================================
// Section break points (Phase B refactor of the original region-style API).
// Vector kept sorted ascending by `breakFrame`; lookups are binary-search.
// ============================================================================

bool Timeline::addSectionBreak(Timecode breakFrame,
                               uint32_t color, double fadeSeconds) {
    auto it = std::lower_bound(m_sections.begin(), m_sections.end(), breakFrame,
        [](const Section& s, Timecode v) { return s.breakFrame < v; });
    if (it != m_sections.end() && it->breakFrame == breakFrame) {
        std::cerr << "[Timeline] addSectionBreak: rejected duplicate break at "
                  << breakFrame << std::endl;
        return false;
    }
    Section sec;
    sec.breakFrame  = breakFrame;
    sec.color       = color;
    sec.fadeSeconds = fadeSeconds;
    m_sections.insert(it, std::move(sec));
    return true;
}

bool Timeline::removeSectionBreak(Timecode breakFrame) {
    auto it = std::lower_bound(m_sections.begin(), m_sections.end(), breakFrame,
        [](const Section& s, Timecode v) { return s.breakFrame < v; });
    if (it == m_sections.end() || it->breakFrame != breakFrame) {
        return false;
    }
    m_sections.erase(it);
    if (m_selectedSectionBreakFrame == breakFrame) {
        m_selectedSectionBreakFrame.reset();
    }
    return true;
}

bool Timeline::editSectionBreak(Timecode oldBreakFrame, Timecode newBreakFrame,
                                uint32_t newColor, double newFadeSeconds) {
    auto oldIt = std::lower_bound(m_sections.begin(), m_sections.end(), oldBreakFrame,
        [](const Section& s, Timecode v) { return s.breakFrame < v; });
    if (oldIt == m_sections.end() || oldIt->breakFrame != oldBreakFrame) {
        std::cerr << "[Timeline] editSectionBreak: no break at "
                  << oldBreakFrame << std::endl;
        return false;
    }

    if (newBreakFrame != oldBreakFrame) {
        auto collide = std::lower_bound(m_sections.begin(), m_sections.end(), newBreakFrame,
            [](const Section& s, Timecode v) { return s.breakFrame < v; });
        if (collide != m_sections.end() && collide->breakFrame == newBreakFrame) {
            std::cerr << "[Timeline] editSectionBreak: a break already exists at "
                      << newBreakFrame << std::endl;
            return false;
        }
    }

    if (newBreakFrame == oldBreakFrame) {
        oldIt->color       = newColor;
        oldIt->fadeSeconds = newFadeSeconds;
    } else {
        Section updated;
        updated.breakFrame  = newBreakFrame;
        updated.color       = newColor;
        updated.fadeSeconds = newFadeSeconds;
        m_sections.erase(oldIt);
        auto insertIt = std::lower_bound(m_sections.begin(), m_sections.end(), updated.breakFrame,
            [](const Section& s, Timecode v) { return s.breakFrame < v; });
        m_sections.insert(insertIt, std::move(updated));
        if (m_selectedSectionBreakFrame == oldBreakFrame) {
            m_selectedSectionBreakFrame = newBreakFrame;
        }
    }
    return true;
}

const Timeline::Section* Timeline::findNextBreakAfter(Timecode time) const {
    auto it = std::upper_bound(m_sections.begin(), m_sections.end(), time,
        [](Timecode v, const Section& s) { return v < s.breakFrame; });
    if (it == m_sections.end()) return nullptr;
    return &(*it);
}

const Timeline::Section* Timeline::findSectionBreakNear(Timecode time, Timecode tolerance) const {
    if (m_sections.empty()) return nullptr;
    auto it = std::lower_bound(m_sections.begin(), m_sections.end(), time,
        [](const Section& s, Timecode v) { return s.breakFrame < v; });
    const Section* best = nullptr;
    Timecode bestDelta = std::numeric_limits<Timecode>::max();
    auto consider = [&](std::vector<Section>::const_iterator candidate) {
        if (candidate == m_sections.end()) return;
        Timecode delta = std::llabs(static_cast<long long>(candidate->breakFrame - time));
        if (delta <= tolerance && delta < bestDelta) {
            bestDelta = delta;
            best = &(*candidate);
        }
    };
    consider(it);
    if (it != m_sections.begin()) consider(std::prev(it));
    return best;
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

// static
void Timeline::syncLayerFromClip(entt::registry& registry, entt::entity entity) {
    const auto* clip = registry.try_get<Clip>(entity);
    auto* layer = registry.try_get<Layer>(entity);
    if (!clip || !layer) return;
    layer->startFrame = clip->startFrame;
    layer->duration   = clip->duration;
}

} // namespace entity
