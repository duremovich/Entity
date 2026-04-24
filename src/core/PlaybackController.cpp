#include "entity/core/PlaybackController.hpp"

#include "entity/render/IRenderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/systems/DecodeSystem.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/FrameRingBuffer.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/FrameBuffer.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace entity {

PlaybackController::PlaybackController(entt::registry& registry, Timeline* timeline, IRenderer* renderer)
    : m_registry(registry)
    , m_timeline(timeline)
    , m_renderer(renderer)
{
}

PlaybackController::~PlaybackController() = default;

void PlaybackController::startTiming() {
    m_startTime = Clock::now();
    m_lastFrameTime = m_startTime;
    m_deltaTime = 0.0;
    m_elapsedTime = 0.0;
    m_frameCount = 0;
}

void PlaybackController::updateTiming() {
    TimePoint currentTime = Clock::now();

    std::chrono::duration<double> delta = currentTime - m_lastFrameTime;
    m_deltaTime = delta.count();

    std::chrono::duration<double> elapsed = currentTime - m_startTime;
    m_elapsedTime = elapsed.count();

    m_lastFrameTime = currentTime;
}

bool PlaybackController::isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const {
    // Check if the timeline frame falls within the clip's range
    return frame >= clip.startFrame && frame < (clip.startFrame + clip.duration);
}

FrameNumber PlaybackController::mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const {
    // Calculate frame position relative to clip start (in timeline frames)
    FrameNumber localFrame = timelineFrame - clip.startFrame;

    // Convert timeline frames to source media frames using frame rate ratio
    // E.g., for 24fps video on 30fps timeline: sourceFrame = localFrame * (24/30) = localFrame * 0.8
    double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    double frameRateRatio = clip.framerate / timelineFrameRate;
    FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));

    // Get source media length
    FrameNumber sourceLength = clip.totalMediaFrames > 0 ? clip.totalMediaFrames :
        static_cast<FrameNumber>(clip.duration * frameRateRatio);

    // If within source media range, direct mapping
    if (sourceLocalFrame < sourceLength) {
        return clip.mediaStartFrame + sourceLocalFrame;
    }

    // Clip extends beyond source media - apply playback mode
    switch (clip.playbackMode) {
        case PlaybackMode::Freeze:
            // Hold on last frame
            return clip.mediaStartFrame + sourceLength - 1;

        case PlaybackMode::Loop:
            // Restart from beginning
            return clip.mediaStartFrame + (sourceLocalFrame % sourceLength);

        case PlaybackMode::PingPong: {
            // Play forward then backward (palindrome)
            FrameNumber cycle = sourceLocalFrame / sourceLength;
            FrameNumber pos = sourceLocalFrame % sourceLength;
            // Even cycles play forward, odd cycles play backward
            if (cycle % 2 == 0) {
                return clip.mediaStartFrame + pos;
            } else {
                return clip.mediaStartFrame + (sourceLength - 1 - pos);
            }
        }
    }

    // Fallback (should never reach)
    return clip.mediaStartFrame + sourceLength - 1;
}

const DecodedFrame* PlaybackController::getCurrentVideoFrame() const {
    // First check multi-clip frames (new system)
    if (m_timeline) {
        FrameNumber currentFrame = m_timeline->getCurrentFrame();

        // Find first active clip with a valid frame
        auto view = m_registry.view<Clip, VideoTexture>();
        for (auto [entity, clip, videoTex] : view.each()) {
            if (isClipActiveAtFrame(clip, currentFrame)) {
                const auto* state = m_registry.try_get<ClipDecodeState>(entity);
                if (state && state->frame && state->frame->valid) {
                    // Verify frame number is close to expected (avoid stale frame flash)
                    FrameNumber expectedMediaFrame = mapToMediaFrame(clip, currentFrame);
                    FrameNumber cachedFrameNum = state->frame->frameNumber;

                    // Allow some tolerance for decode-ahead, but reject obviously stale frames
                    int64_t frameDelta = static_cast<int64_t>(cachedFrameNum) - static_cast<int64_t>(expectedMediaFrame);
                    if (std::abs(frameDelta) > 16) {
                        // Stale frame from a different position - don't display it
                        continue;
                    }

                    return state->frame.get();
                }
            }
        }
    }

    return nullptr;
}

void PlaybackController::updateClipVideos() {
    if (!m_renderer || !m_timeline) {
        return;
    }

    auto funcStart = std::chrono::high_resolution_clock::now();
    int clipCount = 0;
    int activeCount = 0;
    int ringBufferCount = 0;
    int syncDecodeCount = 0;

    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    PlaybackState playState = m_timeline->getPlaybackState();

    // Iterate over all entities with Clip and VideoTexture components
    auto view = m_registry.view<Clip, VideoTexture>();

    for (auto [entity, clip, videoTex] : view.each()) {
        clipCount++;
        // Skip if no texture slot allocated
        if (!videoTex.isAllocated()) {
            continue;
        }

        // Check if clip is active at current frame
        if (!isClipActiveAtFrame(clip, currentFrame)) {
            continue;
        }
        activeCount++;

        // Map timeline frame to media frame
        FrameNumber mediaFrame = mapToMediaFrame(clip, currentFrame);

        // Only process if frame number changed
        auto* state = m_registry.try_get<ClipDecodeState>(entity);
        FrameNumber lastFrame = state ? state->lastDecodedFrame : UINT32_MAX;

        if (mediaFrame == lastFrame) {
            continue;  // Frame hasn't changed, skip
        }

        // Try to get frame from FrameRingBuffer first (threaded decode path)
        auto* frameBuffer = m_registry.try_get<FrameBuffer>(entity);
        if (!frameBuffer) {
            // No FrameBuffer - this clip wasn't set up for threaded decode
            // During playback, skip this clip entirely to avoid blocking
            if (playState == PlaybackState::Playing) {
                std::cout << "[SKIP] No FrameBuffer for entity " << static_cast<uint32_t>(entity)
                          << " during playback - skipping" << std::endl;
                continue;
            }
        } else if (!frameBuffer->ringBuffer) {
            // FrameBuffer exists but no ring buffer
            if (playState == PlaybackState::Playing) {
                std::cout << "[SKIP] No ringBuffer for entity " << static_cast<uint32_t>(entity)
                          << " during playback - skipping" << std::endl;
                continue;
            }
        }
        if (frameBuffer && frameBuffer->ringBuffer) {
            // Check if seek is pending - buffer contains stale frames until decode thread processes seek
            const DecodeWorker* worker = m_decodeSystem ? m_decodeSystem->getWorker(entity) : nullptr;
            if (worker && worker->seekPending.load()) {
                // Skip frame retrieval - buffer has stale data
                // Keep showing previous frame until fresh frames arrive
                continue;
            }

            DecodedFrame ringFrame;
            bool gotFrame = false;

            // During playback, consume frames to keep buffer flowing
            // During scrubbing/paused, just peek without consuming
            // EXCEPTION: For ping-pong mode, NEVER consume frames - we need them for both directions
            auto ringStart = std::chrono::high_resolution_clock::now();

            // For ping-pong, don't consume frames at all - we need them in both directions
            bool isPingPong = (clip.playbackMode == PlaybackMode::PingPong);

            if (playState == PlaybackState::Playing && !isPingPong) {
                // Normal forward playback (non-ping-pong) - consume frames to keep buffer flowing
                gotFrame = frameBuffer->ringBuffer->consumeUpTo(mediaFrame, ringFrame);
            } else {
                // Scrubbing, paused, OR ping-pong mode - don't consume, just get
                // This preserves frames in buffer for ping-pong bidirectional access
                gotFrame = frameBuffer->ringBuffer->getFrame(mediaFrame, ringFrame);
            }

            auto ringMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - ringStart).count();
            if (ringMs > 50) {
                std::cout << "[SLOW RING] getFrame/consumeUpTo took " << ringMs << "ms for entity "
                          << static_cast<uint32_t>(entity) << " frame " << mediaFrame << std::endl;
            }
            ringBufferCount++;

            if (gotFrame) {
                // Got frame from ring buffer - upload to GPU (RGBA or pre-compressed BC).
                bool uploadSuccess = m_renderer->uploadVideoFrameToSlot(
                    videoTex.descriptorSlot,
                    ringFrame.data.data(),
                    ringFrame.width,
                    ringFrame.height,
                    ringFrame.format
                );

                if (uploadSuccess) {
                    videoTex.width = ringFrame.width;
                    videoTex.height = ringFrame.height;
                    if (state) {
                        state->lastDecodedFrame = mediaFrame;

                        // Also update frame so getCurrentVideoFrame() returns this frame
                        if (state->frame) {
                            *state->frame = ringFrame;
                        }
                    }
                }
                continue;  // Frame processed from ring buffer, skip legacy path
            }

            // Frame not in ring buffer - try to get nearest available frame
            // This prevents visual "freeze" when decode thread is catching up
            if (playState == PlaybackState::Playing) {
                // During playback, try to get any frame that's in the buffer
                // This is better than showing nothing (freeze)
                DecodedFrame nearestFrame;
                if (frameBuffer->ringBuffer->getNearestFrame(mediaFrame, nearestFrame)) {
                    // Got a frame (might not be exact target, but close enough)
                    bool uploadSuccess = m_renderer->uploadVideoFrameToSlot(
                        videoTex.descriptorSlot,
                        nearestFrame.data.data(),
                        nearestFrame.width,
                        nearestFrame.height,
                        nearestFrame.format
                    );
                    if (uploadSuccess) {
                        videoTex.width = nearestFrame.width;
                        videoTex.height = nearestFrame.height;
                    }
                }
                // Either way, don't fall through to sync decode during playback
                continue;
            }
            // During paused/scrubbing - fall through to legacy decode (blocking is OK)
        }

        // Legacy path: synchronous decode on main thread (only used when paused/scrubbing)
        // CRITICAL SAFETY CHECK: Never do sync decode during playback - it causes freezes!
        // If we somehow got here during playback, skip this frame.
        // Check CURRENT playState from timeline (not cached) to catch any state changes
        if (m_timeline->getPlaybackState() == PlaybackState::Playing) {
            // Should not happen - but if it does, skip to avoid freeze
            continue;
        }

        if (!state) {
            continue;
        }

        Decoder* decoder = state->decoder.get();
        DecodedFrame* frame = state->frame.get();

        if (!decoder || !decoder->isOpen() || !frame) {
            continue;
        }

        syncDecodeCount++;
        // Log that we're doing synchronous decode
        auto decodeStart = std::chrono::high_resolution_clock::now();
        std::cout << "[SYNC DECODE] Entity " << static_cast<uint32_t>(entity)
                  << " frame " << mediaFrame << " playState=" << static_cast<int>(playState) << std::endl;

        // Decode the frame synchronously (blocking - only for scrub/pause)
        Result result = decoder->decodeFrame(mediaFrame, *frame);

        auto decodeEnd = std::chrono::high_resolution_clock::now();
        auto decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();

        if (decodeMs > 50) {
            std::cout << "[SYNC DECODE] Took " << decodeMs << "ms for entity " << static_cast<uint32_t>(entity) << std::endl;
        }

        if (result == Result::Success) {
            frame->valid = true;
            state->lastDecodedFrame = mediaFrame;

            // Upload frame to GPU texture slot (sync-decode path — RGBA or BC).
            bool uploadSuccess = m_renderer->uploadVideoFrameToSlot(
                videoTex.descriptorSlot,
                frame->data.data(),
                frame->width,
                frame->height,
                frame->format
            );

            if (uploadSuccess) {
                videoTex.width = frame->width;
                videoTex.height = frame->height;
            }
        } else {
            frame->valid = false;
        }
    }

    // Log summary if update took significant time
    auto funcMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - funcStart).count();
    if (funcMs > 100) {
        std::cout << "[CLIP UPDATE] Total=" << funcMs << "ms"
                  << " clips=" << clipCount
                  << " active=" << activeCount
                  << " ringBuf=" << ringBufferCount
                  << " syncDec=" << syncDecodeCount
                  << " frame=" << currentFrame
                  << " state=" << static_cast<int>(playState) << std::endl;
    }
}

} // namespace entity
