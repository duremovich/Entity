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
#include <chrono>
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
    if (m_playbackState.load() == PlaybackState::Playing &&
        !m_seekSyncGate.load(std::memory_order_acquire)) {
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
        // Engage the seek-sync gate before transitioning to Playing so
        // Timeline::update() holds the playhead parked until
        // SeekSyncController confirms all active decoders are prerolled.
        // pause(), stop(), seek(), and clear() clear the gate on any
        // non-play transition so a stale gate can never lock the timeline.
        m_seekSyncEngageTimeNs.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_release);
        m_seekSyncGate.store(true, std::memory_order_release);
        m_playbackState.store(PlaybackState::Playing);
        std::cout << "[Timeline] Playing from " << getCurrentTime() << std::endl;
    }
}

void Timeline::pause() {
    m_seekSyncGate.store(false, std::memory_order_release);
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
    m_seekSyncGate.store(false, std::memory_order_release);
    m_playbackState.store(PlaybackState::Stopped);
    m_currentTime.store(0, std::memory_order_relaxed);
    std::cout << "[Timeline] Stopped, reset to 0" << std::endl;
}

void Timeline::seek(Timecode time) {
    m_seekSyncGate.store(false, std::memory_order_release);
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

        // Reindex remaining tracks, updating Layer::trackIndex and
        // MediaLayer::zOrder on every layer entity in each track.
        reindexTracks();
    }
}

void Timeline::reindexTracks() {
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        entt::entity trackEnt = m_tracks[i];
        if (!m_registry.valid(trackEnt)) continue;

        auto* trackComp = m_registry.try_get<TimelineTrack>(trackEnt);
        if (!trackComp) continue;

        trackComp->trackIndex = static_cast<uint32_t>(i);

        for (entt::entity layerEnt : trackComp->layers) {
            if (!m_registry.valid(layerEnt)) continue;

            if (auto* lay = m_registry.try_get<Layer>(layerEnt)) {
                lay->trackIndex = static_cast<uint32_t>(i);
            }
            if (auto* ml = m_registry.try_get<MediaLayer>(layerEnt)) {
                ml->zOrder = 1000u - static_cast<uint32_t>(i);
            }
        }
    }
}

entt::entity Timeline::insertTrackAt(size_t index) {
    // Clamp to valid insertion range.
    if (index > m_tracks.size()) {
        index = m_tracks.size();
    }

    entt::entity trackEntity = m_registry.create();
    auto& track = m_registry.emplace<TimelineTrack>(trackEntity);
    track.trackIndex = static_cast<uint32_t>(index);  // will be corrected by reindexTracks()

    m_tracks.insert(m_tracks.begin() + static_cast<std::ptrdiff_t>(index), trackEntity);

    reindexTracks();

    std::cout << "[Timeline] Inserted track at index " << index
              << ", entity=" << static_cast<uint32_t>(trackEntity) << std::endl;

    return trackEntity;
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
    // Use clearSections() to take the exclusive lock (plugin worker threads
    // may concurrently call snapshotSectionsAndRate).
    clearSections();

    // Cue tags belong to the project; drop them with the timeline.
    m_cueTags.clear();

    // Reset selection (Phase 12: drop the multi-select set too — its entity
    // ids point at clips this clear() just destroyed).
    m_selectedClip = entt::null;
    m_selectedClips.clear();
    m_selectedCueNumber.reset();
    m_selectedSectionBreakFrame.reset();

    // Reset timeline state
    m_seekSyncGate.store(false, std::memory_order_release);
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

    // Drop the entity from the multi-select set + fix the primary so the
    // selection invariant holds regardless of caller (Phase 12/13 fix B).
    // Without this, an entt id recycled into a new clip could inherit a stale
    // "selected" highlight. We do NOT clear m_selectedClip wholesale — other
    // callers (e.g. group delete) manage the primary explicitly — but we must
    // not leave a dangling id in the set or as the primary.
    {
        auto it = std::find(m_selectedClips.begin(), m_selectedClips.end(), clipEntity);
        if (it != m_selectedClips.end()) {
            m_selectedClips.erase(it);
        }
        if (m_selectedClip == clipEntity) {
            m_selectedClip = m_selectedClips.empty() ? entt::null
                                                     : m_selectedClips.back();
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
        if (const auto* as = m_registry.try_get<AudioSource>(clipEntity)) {
            snap.hadAudioSource = true;
            snap.audioSource    = *as;
        }
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
    if (snap.hadAudioSource) {
        m_registry.emplace<AudioSource>(newEntity) = snap.audioSource;
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

std::pair<int, int> Timeline::clipIndices(entt::entity clip) const {
    for (int t = 0; t < static_cast<int>(m_tracks.size()); ++t) {
        const auto* track = m_registry.try_get<TimelineTrack>(m_tracks[t]);
        if (!track) continue;
        for (int l = 0; l < static_cast<int>(track->layers.size()); ++l) {
            if (track->layers[l] == clip) return {t, l};
        }
    }
    return {-1, -1};
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
//
// Both ops are archetype-generic: they read/write placement through the two
// file-local helpers below so Clip-backed and Layer-only (OA / Generative /
// Text / Signal) entities all participate. Clip is the source of truth for
// Clip-backed entities (the Layer mirror is re-synced via syncLayerFromClip);
// Layer is the source of truth for Layer-only entities.
// ============================================================================

namespace {

struct RipplePlacement {
    FrameNumber startFrame{0};
    FrameNumber duration{0};
    bool        isClip{false};   // true → Clip is source of truth (media slip applies)
    bool        valid{false};
};

RipplePlacement readRipplePlacement(entt::registry& reg, entt::entity e) {
    if (const auto* clip = reg.try_get<Clip>(e)) {
        return {clip->startFrame, clip->duration, true, true};
    }
    if (const auto* lay = reg.try_get<Layer>(e)) {
        return {lay->startFrame, lay->duration, false, true};
    }
    return {};
}

// Write start frame + duration back to whichever component owns placement.
// Clip-backed entities re-sync the Layer mirror.
void writeRipplePlacement(entt::registry& reg, entt::entity e,
                          FrameNumber startFrame, FrameNumber duration) {
    if (auto* clip = reg.try_get<Clip>(e)) {
        clip->startFrame = startFrame;
        clip->duration   = duration;
        Timeline::syncLayerFromClip(reg, e);
        return;
    }
    if (auto* lay = reg.try_get<Layer>(e)) {
        lay->startFrame = startFrame;
        lay->duration   = duration;
    }
}

// Shift every keyframe at clip-relative frame >= pivot by `delta` on every
// track of the entity's AnimatedProperties. Returns false if the entity had
// no AnimatedProperties (nothing to record/restore).
bool shiftKeyframesFrom(AnimatedProperties& ap, FrameNumber pivot, FrameNumber delta) {
    bool moved = false;
    for (auto& track : ap.tracks) {
        for (auto& kf : track.keyframes) {
            if (kf.frame >= pivot) {
                kf.frame += delta;
                moved = true;
            }
        }
    }
    return moved;
}

} // namespace

Timeline::RippleInsertResult Timeline::rippleInsertTime(FrameNumber insertFrame, FrameNumber durationFrames) {
    RippleInsertResult result{};
    if (insertFrame < 0 || durationFrames <= 0) {
        std::cerr << "[Timeline] rippleInsertTime: bad args (insertFrame=" << insertFrame
                  << ", durationFrames=" << durationFrames << ")" << std::endl;
        return result;
    }

    // Entities: lengthen spanning clips (and shift their later keyframes),
    // shift everything at/after the insert point. All layer kinds.
    for (entt::entity trackEntity : m_tracks) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;
        for (entt::entity e : track->layers) {
            RipplePlacement p = readRipplePlacement(m_registry, e);
            if (!p.valid) continue;
            const FrameNumber endF = p.startFrame + p.duration;

            if (p.startFrame >= insertFrame) {
                // Entirely at/after the insert point — shift right.
                result.shifted.push_back({e, p.startFrame});
                writeRipplePlacement(m_registry, e, p.startFrame + durationFrames, p.duration);
            } else if (endF > insertFrame) {
                // Spans the insert point — lengthen rather than split.
                ClipResizeRecord rr;
                rr.entity        = e;
                rr.oldStartFrame = p.startFrame;
                rr.oldDuration   = p.duration;
                result.lengthened.push_back(rr);
                writeRipplePlacement(m_registry, e, p.startFrame, p.duration + durationFrames);

                // Shift keyframes at/after the (clip-relative) insert point so
                // the animation tail stays glued to the content after it.
                if (auto* ap = m_registry.try_get<AnimatedProperties>(e)) {
                    AnimPropsRecord kr;
                    kr.entity       = e;
                    kr.hadAnimProps = true;
                    kr.oldAnimProps = *ap;
                    const FrameNumber pivot = insertFrame - p.startFrame;
                    if (shiftKeyframesFrom(*ap, pivot, durationFrames)) {
                        result.keyframes.push_back(std::move(kr));
                    }
                }
            }
            // else: entirely before insertFrame — untouched.
        }
    }

    // Cues at/after the insert point shift +D.
    // THREADING: m_cueTags is mutated here WITHOUT a lock, unlike m_sections
    // (m_sectionsMutex). Safe ONLY because every cue reader today is on the
    // editor thread. If a show-thread cue reader is ever added (e.g. the
    // show-control GO-direction work on the roadmap), m_cueTags needs a guard
    // like m_sectionsMutex — racing this in-place edit is a crash-class bug.
    for (auto& cue : m_cueTags) {
        if (cue.frame >= insertFrame) {
            result.cues.push_back({cue.number, cue.frame});
            cue.frame += durationFrames;
        }
    }

    // Section breaks at/after the insert point shift +D. m_sections is sorted
    // ascending by breakFrame; a uniform +D on a contiguous tail preserves
    // order, so an in-place edit is safe.
    {
        std::unique_lock lock(m_sectionsMutex);
        for (auto& sec : m_sections) {
            if (sec.breakFrame >= insertFrame) {
                result.sections.push_back({sec.breakFrame, sec.breakFrame + durationFrames});
                sec.breakFrame += durationFrames;
            }
        }
    }

    result.success = true;
    std::cout << "[Timeline] rippleInsertTime: inserted " << durationFrames
              << " frames at " << insertFrame
              << " (lengthened " << result.lengthened.size()
              << ", shifted " << result.shifted.size()
              << ", cues " << result.cues.size()
              << ", sections " << result.sections.size() << ")" << std::endl;
    return result;
}

void Timeline::undoRippleInsertTime(RippleInsertResult& record) {
    if (!record.success) return;

    // Restore section breaks (reverse the +D).
    {
        std::unique_lock lock(m_sectionsMutex);
        for (auto& sr : record.sections) {
            auto it = std::lower_bound(m_sections.begin(), m_sections.end(), sr.newBreakFrame,
                [](const Section& s, FrameNumber v) { return s.breakFrame < v; });
            if (it != m_sections.end() && it->breakFrame == sr.newBreakFrame) {
                it->breakFrame = sr.oldBreakFrame;
            }
        }
        std::sort(m_sections.begin(), m_sections.end(),
                  [](const Section& a, const Section& b) { return a.breakFrame < b.breakFrame; });
    }
    record.sections.clear();

    // Restore cue frames. THREADING: lock-free m_cueTags mutation — see the
    // note at the rippleInsertTime cue-shift site (editor-thread-only readers).
    for (auto& cr : record.cues) {
        for (auto& cue : m_cueTags) {
            if (cue.number == cr.number) { cue.frame = cr.oldFrame; break; }
        }
    }
    record.cues.clear();

    // Restore keyframes (whole-component snapshots taken pre-mutation).
    for (auto& kr : record.keyframes) {
        if (!m_registry.valid(kr.entity)) continue;
        if (auto* ap = m_registry.try_get<AnimatedProperties>(kr.entity)) {
            *ap = kr.oldAnimProps;
        } else {
            m_registry.emplace<AnimatedProperties>(kr.entity, kr.oldAnimProps);
        }
    }
    record.keyframes.clear();

    // Restore lengthened (spanning) clips' duration.
    for (auto& rr : record.lengthened) {
        if (!m_registry.valid(rr.entity)) continue;
        writeRipplePlacement(m_registry, rr.entity, rr.oldStartFrame, rr.oldDuration);
    }
    record.lengthened.clear();

    // Restore shifted start frames.
    for (auto& s : record.shifted) {
        if (!m_registry.valid(s.entity)) continue;
        RipplePlacement p = readRipplePlacement(m_registry, s.entity);
        writeRipplePlacement(m_registry, s.entity, s.oldStartFrame, p.duration);
    }
    record.shifted.clear();
    record.success = false;
}

Timeline::RippleDeleteResult Timeline::rippleDeleteTime(FrameNumber rangeStart, FrameNumber rangeEnd) {
    RippleDeleteResult result{};
    if (rangeStart < 0 || rangeEnd <= rangeStart) {
        std::cerr << "[Timeline] rippleDeleteTime: bad range [" << rangeStart << ", " << rangeEnd << ")" << std::endl;
        return result;
    }
    const FrameNumber removeDur = rangeEnd - rangeStart;

    // Pre-flight: any entity FULLY contained in the range will be
    // snapshot-deleted. If one can't be snapshotted (Signal layers have no
    // DeletedLayerKind entry → snapshotClipForDelete returns invalid), abort
    // the whole op without mutating anything.
    for (entt::entity trackEntity : m_tracks) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;
        for (entt::entity e : track->layers) {
            RipplePlacement p = readRipplePlacement(m_registry, e);
            if (!p.valid) continue;
            const FrameNumber endF = p.startFrame + p.duration;
            const bool fullyInside = (p.startFrame >= rangeStart && endF <= rangeEnd);
            if (fullyInside && !snapshotClipForDelete(e).valid()) {
                std::cerr << "[Timeline] rippleDeleteTime: aborted — entity="
                          << static_cast<uint32_t>(e)
                          << " is fully inside [" << rangeStart << ", " << rangeEnd << ")"
                          << " but can't be snapshotted (unsupported layer kind, e.g. Signal)."
                          << " Move it out of the range, then retry." << std::endl;
                return result;
            }
        }
    }

    // Collect the entities to act on first, because snapshot-delete mutates
    // track->layers underneath the iteration.
    std::vector<entt::entity> entities;
    for (entt::entity trackEntity : m_tracks) {
        auto* track = m_registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;
        for (entt::entity e : track->layers) {
            if (readRipplePlacement(m_registry, e).valid) entities.push_back(e);
        }
    }

    for (entt::entity e : entities) {
        RipplePlacement p = readRipplePlacement(m_registry, e);
        if (!p.valid) continue;
        const FrameNumber startF = p.startFrame;
        const FrameNumber endF   = startF + p.duration;

        const bool fullyInside    = (startF >= rangeStart && endF <= rangeEnd);
        const bool straddlesStart = (startF < rangeStart && endF > rangeStart && endF <= rangeEnd);
        const bool straddlesEnd   = (startF >= rangeStart && startF < rangeEnd && endF > rangeEnd);
        const bool spansRange     = (startF < rangeStart && endF > rangeEnd);
        const bool entirelyAfter  = (startF >= rangeEnd);

        if (fullyInside) {
            // Snapshot-delete the whole entity (pre-flight guaranteed snapshot ok).
            DeletedClipSnapshot snap = snapshotClipForDelete(e);
            if (snap.valid()) {
                result.deleted.push_back(std::move(snap));
                deleteClip(e);
            }
        } else if (straddlesStart) {
            // Range cuts off this entity's tail — keep only the head up to
            // rangeStart.
            result.resized.push_back({e, startF, p.duration, -1});
            writeRipplePlacement(m_registry, e, startF, rangeStart - startF);
        } else if (straddlesEnd) {
            // Range cuts off this entity's head — survivor starts at rangeStart
            // (post-shift) and keeps the [rangeEnd, endF) tail. For Clip layers
            // the media in-point slips so the same content shows.
            FrameNumber oldMediaStart = -1;
            if (p.isClip) {
                if (auto* clip = m_registry.try_get<Clip>(e)) {
                    oldMediaStart = clip->mediaStartFrame;
                    clip->mediaStartFrame += (rangeEnd - startF);
                }
            }
            result.resized.push_back({e, startF, p.duration, oldMediaStart});
            writeRipplePlacement(m_registry, e, rangeStart, endF - rangeEnd);
        } else if (spansRange) {
            // Range carved out of the middle — shorten by removeDur, and on the
            // keyframe track drop the deleted clip-relative window and pull the
            // tail keyframes back.
            if (auto* ap = m_registry.try_get<AnimatedProperties>(e)) {
                AnimPropsRecord kr;
                kr.entity = e; kr.hadAnimProps = true; kr.oldAnimProps = *ap;
                const FrameNumber relStart = rangeStart - startF;
                const FrameNumber relEnd   = rangeEnd   - startF;
                bool touched = false;
                for (auto& track : ap->tracks) {
                    auto& kfs = track.keyframes;
                    const std::size_t before = kfs.size();
                    kfs.erase(std::remove_if(kfs.begin(), kfs.end(),
                        [&](const Keyframe& k){ return k.frame >= relStart && k.frame < relEnd; }),
                        kfs.end());
                    if (kfs.size() != before) touched = true;
                    for (auto& k : kfs) {
                        if (k.frame >= relEnd) { k.frame -= removeDur; touched = true; }
                    }
                }
                if (touched) result.keyframes.push_back(std::move(kr));
            }
            result.resized.push_back({e, startF, p.duration, -1});
            writeRipplePlacement(m_registry, e, startF, p.duration - removeDur);
        } else if (entirelyAfter) {
            result.shifted.push_back({e, startF});
            writeRipplePlacement(m_registry, e, startF - removeDur, p.duration);
        }
        // else: entirely before rangeStart — untouched.
    }

    // Cues: inside the range → deleted; after → shift left.
    // THREADING: lock-free m_cueTags rebuild — see the note at the
    // rippleInsertTime cue-shift site (editor-thread-only readers; a future
    // show-thread cue reader must add a guard like m_sectionsMutex).
    {
        std::vector<CueTag> survivors;
        survivors.reserve(m_cueTags.size());
        for (auto& cue : m_cueTags) {
            if (cue.frame >= rangeStart && cue.frame < rangeEnd) {
                result.cuesDeleted.push_back(cue);
            } else if (cue.frame >= rangeEnd) {
                result.cuesShifted.push_back({cue.number, cue.frame});
                cue.frame -= removeDur;
                survivors.push_back(cue);
            } else {
                survivors.push_back(cue);
            }
        }
        m_cueTags = std::move(survivors);
    }

    // Sections: inside the range → deleted; after → shift left.
    {
        std::unique_lock lock(m_sectionsMutex);
        std::vector<Section> survivors;
        survivors.reserve(m_sections.size());
        for (auto& sec : m_sections) {
            if (sec.breakFrame >= rangeStart && sec.breakFrame < rangeEnd) {
                result.sectionsDeleted.push_back(sec);
            } else if (sec.breakFrame >= rangeEnd) {
                result.sectionsShifted.push_back({sec.breakFrame, sec.breakFrame - removeDur});
                sec.breakFrame -= removeDur;
                survivors.push_back(sec);
            } else {
                survivors.push_back(sec);
            }
        }
        m_sections = std::move(survivors);
        std::sort(m_sections.begin(), m_sections.end(),
                  [](const Section& a, const Section& b) { return a.breakFrame < b.breakFrame; });
    }

    result.success = true;
    std::cout << "[Timeline] rippleDeleteTime: removed [" << rangeStart << ", " << rangeEnd
              << ") (deleted " << result.deleted.size()
              << ", resized " << result.resized.size()
              << ", shifted " << result.shifted.size()
              << ", cues -" << result.cuesDeleted.size()
              << ", sections -" << result.sectionsDeleted.size() << ")" << std::endl;
    return result;
}

void Timeline::undoRippleDeleteTime(RippleDeleteResult& record) {
    if (!record.success) return;

    // Restore sections: un-shift survivors, re-insert deleted.
    {
        std::unique_lock lock(m_sectionsMutex);
        for (auto& sr : record.sectionsShifted) {
            auto it = std::lower_bound(m_sections.begin(), m_sections.end(), sr.newBreakFrame,
                [](const Section& s, FrameNumber v) { return s.breakFrame < v; });
            if (it != m_sections.end() && it->breakFrame == sr.newBreakFrame) {
                it->breakFrame = sr.oldBreakFrame;
            }
        }
        for (auto& sec : record.sectionsDeleted) {
            m_sections.push_back(sec);
        }
        std::sort(m_sections.begin(), m_sections.end(),
                  [](const Section& a, const Section& b) { return a.breakFrame < b.breakFrame; });
    }
    record.sectionsShifted.clear();
    record.sectionsDeleted.clear();

    // Restore cues: un-shift survivors, re-insert deleted. m_cueTags is sorted
    // by number; re-sort after re-inserting. THREADING: lock-free m_cueTags
    // mutation — see the note at the rippleInsertTime cue-shift site
    // (editor-thread-only readers; add a guard like m_sectionsMutex if a
    // show-thread cue reader is ever introduced).
    for (auto& cr : record.cuesShifted) {
        for (auto& cue : m_cueTags) {
            if (cue.number == cr.number) { cue.frame = cr.oldFrame; break; }
        }
    }
    for (auto& cue : record.cuesDeleted) {
        m_cueTags.push_back(cue);
    }
    std::sort(m_cueTags.begin(), m_cueTags.end(),
              [](const CueTag& a, const CueTag& b) { return a.number < b.number; });
    record.cuesShifted.clear();
    record.cuesDeleted.clear();

    // Restore shifted start frames.
    for (auto& s : record.shifted) {
        if (!m_registry.valid(s.entity)) continue;
        RipplePlacement p = readRipplePlacement(m_registry, s.entity);
        writeRipplePlacement(m_registry, s.entity, s.oldStartFrame, p.duration);
    }
    record.shifted.clear();

    // Restore resized (head/tail trim, spanning shorten) placements + media slip.
    for (auto& rr : record.resized) {
        if (!m_registry.valid(rr.entity)) continue;
        writeRipplePlacement(m_registry, rr.entity, rr.oldStartFrame, rr.oldDuration);
        if (rr.oldMediaStartFrame >= 0) {
            if (auto* clip = m_registry.try_get<Clip>(rr.entity)) {
                clip->mediaStartFrame = rr.oldMediaStartFrame;
            }
        }
    }
    record.resized.clear();

    // Restore keyframes (whole-component snapshots).
    for (auto& kr : record.keyframes) {
        if (!m_registry.valid(kr.entity)) continue;
        if (auto* ap = m_registry.try_get<AnimatedProperties>(kr.entity)) {
            *ap = kr.oldAnimProps;
        } else {
            m_registry.emplace<AnimatedProperties>(kr.entity, kr.oldAnimProps);
        }
    }
    record.keyframes.clear();

    // Re-create fully-deleted entities last (fresh entity IDs; undo stack is
    // cleared on project load so the stale IDs in other records don't matter
    // — those records were already applied above).
    for (auto& snap : record.deleted) {
        restoreDeletedClip(snap);
    }
    record.deleted.clear();
    record.success = false;
}

// ============================================================================
// Section break points (Phase B refactor of the original region-style API).
// Vector kept sorted ascending by `breakFrame`; lookups are binary-search.
// ============================================================================

void Timeline::setFrameRate(double frameRate) {
    std::unique_lock lock(m_sectionsMutex);
    m_frameRate = frameRate;
}

void Timeline::clearSections() {
    std::unique_lock lock(m_sectionsMutex);
    m_sections.clear();
}

bool Timeline::addSectionBreak(FrameNumber breakFrame,
                               uint32_t color, double fadeSeconds) {
    std::unique_lock lock(m_sectionsMutex);
    auto it = std::lower_bound(m_sections.begin(), m_sections.end(), breakFrame,
        [](const Section& s, FrameNumber v) { return s.breakFrame < v; });
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

bool Timeline::removeSectionBreak(FrameNumber breakFrame) {
    std::unique_lock lock(m_sectionsMutex);
    auto it = std::lower_bound(m_sections.begin(), m_sections.end(), breakFrame,
        [](const Section& s, FrameNumber v) { return s.breakFrame < v; });
    if (it == m_sections.end() || it->breakFrame != breakFrame) {
        return false;
    }
    m_sections.erase(it);
    if (m_selectedSectionBreakFrame == breakFrame) {
        m_selectedSectionBreakFrame.reset();
    }
    return true;
}

bool Timeline::editSectionBreak(FrameNumber oldBreakFrame, FrameNumber newBreakFrame,
                                uint32_t newColor, double newFadeSeconds) {
    std::unique_lock lock(m_sectionsMutex);
    auto oldIt = std::lower_bound(m_sections.begin(), m_sections.end(), oldBreakFrame,
        [](const Section& s, FrameNumber v) { return s.breakFrame < v; });
    if (oldIt == m_sections.end() || oldIt->breakFrame != oldBreakFrame) {
        std::cerr << "[Timeline] editSectionBreak: no break at "
                  << oldBreakFrame << std::endl;
        return false;
    }

    if (newBreakFrame != oldBreakFrame) {
        auto collide = std::lower_bound(m_sections.begin(), m_sections.end(), newBreakFrame,
            [](const Section& s, FrameNumber v) { return s.breakFrame < v; });
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
            [](const Section& s, FrameNumber v) { return s.breakFrame < v; });
        m_sections.insert(insertIt, std::move(updated));
        if (m_selectedSectionBreakFrame == oldBreakFrame) {
            m_selectedSectionBreakFrame = newBreakFrame;
        }
    }
    return true;
}

const Timeline::Section* Timeline::findNextBreakAfter(FrameNumber afterFrame) const {
    // Editor-thread-only caller; no lock needed here.
    auto it = std::upper_bound(m_sections.begin(), m_sections.end(), afterFrame,
        [](FrameNumber v, const Section& s) { return v < s.breakFrame; });
    if (it == m_sections.end()) return nullptr;
    return &(*it);
}

const Timeline::Section* Timeline::findSectionBreakNear(FrameNumber frame, FrameNumber tolerance) const {
    // Editor-thread-only caller; no lock needed here.
    if (m_sections.empty()) return nullptr;
    auto it = std::lower_bound(m_sections.begin(), m_sections.end(), frame,
        [](const Section& s, FrameNumber v) { return s.breakFrame < v; });
    const Section* best = nullptr;
    FrameNumber bestDelta = std::numeric_limits<FrameNumber>::max();
    auto consider = [&](std::vector<Section>::const_iterator candidate) {
        if (candidate == m_sections.end()) return;
        FrameNumber delta = std::llabs(static_cast<long long>(candidate->breakFrame - frame));
        if (delta <= tolerance && delta < bestDelta) {
            bestDelta = delta;
            best = &(*candidate);
        }
    };
    consider(it);
    if (it != m_sections.begin()) consider(std::prev(it));
    return best;
}

std::pair<std::vector<Timeline::Section>, double>
Timeline::copySectionsAndRate() const {
    std::shared_lock lock(m_sectionsMutex);
    return { m_sections, m_frameRate };
}

void Timeline::snapshotSectionsAndRate(FrameNumber currentFrame,
                                       double&   outFrameRate,
                                       int&      outActiveIdx,
                                       int64_t&  outActiveFrame,
                                       int&      outNextIdx,
                                       int64_t&  outNextFrame) const {
    std::shared_lock lock(m_sectionsMutex);
    outFrameRate  = m_frameRate;
    outActiveIdx  = -1;
    outNextIdx    = -1;
    outActiveFrame = 0;
    outNextFrame   = 0;
    for (int i = 0; i < static_cast<int>(m_sections.size()); ++i) {
        // breakFrame is already an integer timeline frame.
        auto bf = static_cast<int64_t>(m_sections[i].breakFrame);
        if (bf <= static_cast<int64_t>(currentFrame)) {
            outActiveIdx   = i;
            outActiveFrame = bf;
        } else if (outNextIdx == -1) {
            outNextIdx   = i;
            outNextFrame = bf;
        }
    }
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
                          FrameNumber newFrame, std::string newLabel) {
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
        oldIt->frame = newFrame;
        oldIt->label = std::move(newLabel);
    } else {
        CueTag updated{newNumber, newFrame, std::move(newLabel)};
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
