/**
 * DecodeSystem Implementation
 *
 * Manages background decode threads for video playback.
 */

#include "entity/systems/DecodeSystem.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/VideoTexture.hpp"
#include <iostream>
#include <chrono>

namespace entity {

DecodeSystem::DecodeSystem() = default;

DecodeSystem::~DecodeSystem() {
    // Ensure all workers are stopped
    for (auto& [entity, worker] : m_workers) {
        if (worker && worker->running.load()) {
            worker->running.store(false);
            worker->cv.notify_all();
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
    }
    m_workers.clear();
}

void DecodeSystem::initialize(entt::registry& registry) {
    std::cout << "DecodeSystem initialized" << std::endl;
}

void DecodeSystem::update(entt::registry& registry, float deltaTime) {
    if (!m_timeline) return;

    // Detect scrubbing end - when scrubbing stops, force seeks for all active clips
    bool currentlyScrubbing = m_timeline->isScrubbing();
    bool scrubbingJustEnded = m_wasScrubbing && !currentlyScrubbing;
    m_wasScrubbing = currentlyScrubbing;

    // ALWAYS-WARM BUFFERING: Never pause decode workers based on timeline state!
    // This ensures buffers are always ready at the playhead position for zero-latency playback.
    // Decode workers only pause when the ring buffer is full, not when timeline is paused.
    //
    // Old behavior (caused playback delay):
    //   Timeline paused → pause workers → buffers go cold → Play pressed → delay while buffering
    //
    // New behavior (zero-latency):
    //   Timeline paused → workers keep running → buffers stay warm → Play pressed → instant start

    // Ensure all workers are running (in case they were previously paused)
    if (m_globalPaused.load()) {
        resumeAll();
    }

    FrameNumber currentTimelineFrame = m_timeline->getCurrentFrame();

    // Iterate over all entities with Clip and FrameBuffer components
    auto view = registry.view<Clip, FrameBuffer>();

    for (auto [entity, clip, frameBuffer] : view.each()) {
        // Check if clip is loaded
        if (!clip.loaded) continue;

        // Check if worker exists for this clip
        auto workerIt = m_workers.find(entity);

        if (workerIt == m_workers.end()) {
            // Create worker for new clip - pass the initial media frame based on current timeline position
            FrameNumber initialMediaFrame = 0;
            if (currentTimelineFrame >= clip.startFrame &&
                currentTimelineFrame < clip.startFrame + clip.duration) {
                // Timeline is within clip - start at current position
                // Convert timeline frames to source frames using frame rate ratio
                double timelineFrameRate = m_timeline->getFrameRate();
                double frameRateRatio = clip.framerate / timelineFrameRate;
                FrameNumber localFrame = currentTimelineFrame - clip.startFrame;
                FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));
                initialMediaFrame = clip.mediaStartFrame + sourceLocalFrame;
            } else if (currentTimelineFrame < clip.startFrame) {
                // Timeline is before clip - start at clip's media start
                initialMediaFrame = clip.mediaStartFrame;
            } else {
                // Timeline is after clip - start at end (but this shouldn't happen for active clips)
                initialMediaFrame = clip.mediaStartFrame;
            }
            createWorker(entity, registry, initialMediaFrame);
            workerIt = m_workers.find(entity);
            if (workerIt == m_workers.end()) continue;
        }

        DecodeWorker* worker = workerIt->second.get();
        if (!worker) continue;

        // Update playback mode in case user changed it
        worker->playbackMode = clip.playbackMode;
        // totalMediaFrames should be in source frames (not timeline frames)
        // clip.totalMediaFrames is set from decoder and is correct
        // clip.duration is in timeline frames, so convert if using as fallback
        if (clip.totalMediaFrames > 0) {
            worker->totalMediaFrames = clip.totalMediaFrames;
        } else {
            double frameRateRatio = clip.framerate / m_timeline->getFrameRate();
            worker->totalMediaFrames = static_cast<FrameNumber>(clip.duration * frameRateRatio);
        }

        // Calculate which media frame this clip needs based on timeline position
        if (currentTimelineFrame >= clip.startFrame &&
            currentTimelineFrame < clip.startFrame + clip.duration) {
            // Clip is active - calculate target media frame using playback mode
            FrameNumber localFrame = currentTimelineFrame - clip.startFrame;

            // Convert timeline frames to source media frames using frame rate ratio
            // E.g., for 24fps video on 30fps timeline: sourceFrame = localFrame * (24/30) = localFrame * 0.8
            double timelineFrameRate = m_timeline->getFrameRate();
            double frameRateRatio = clip.framerate / timelineFrameRate;
            FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));

            // Source length is always in source frames (from decoder)
            FrameNumber sourceLength = clip.totalMediaFrames > 0 ? clip.totalMediaFrames :
                static_cast<FrameNumber>(clip.duration * frameRateRatio);
            FrameNumber mediaFrame = clip.mediaStartFrame;  // Default value

            if (sourceLocalFrame < sourceLength) {
                // Within source media range
                mediaFrame = clip.mediaStartFrame + sourceLocalFrame;
            } else {
                // Beyond source media - apply playback mode
                switch (clip.playbackMode) {
                    case PlaybackMode::Freeze:
                        mediaFrame = clip.mediaStartFrame + sourceLength - 1;
                        break;
                    case PlaybackMode::Loop:
                        mediaFrame = clip.mediaStartFrame + (sourceLocalFrame % sourceLength);
                        break;
                    case PlaybackMode::PingPong: {
                        FrameNumber cycle = sourceLocalFrame / sourceLength;
                        FrameNumber pos = sourceLocalFrame % sourceLength;
                        if (cycle % 2 == 0) {
                            mediaFrame = clip.mediaStartFrame + pos;
                        } else {
                            mediaFrame = clip.mediaStartFrame + (sourceLength - 1 - pos);
                        }
                        break;
                    }
                }
            }

            // Detect ping-pong reverse phase
            bool isReverse = false;
            if (clip.playbackMode == PlaybackMode::PingPong && sourceLength > 0) {
                FrameNumber cycle = sourceLocalFrame / sourceLength;
                isReverse = (cycle % 2 == 1);
            }

            // Track reverse state
            worker->pingPongReverse.store(isReverse);

            // Get actual ring buffer capacity (may be larger for ping-pong clips)
            uint32_t ringBufferCapacity = frameBuffer.ringBuffer ?
                frameBuffer.ringBuffer->getCapacity() : FrameRingBuffer::DEFAULT_CAPACITY;

            // Update target frame for decode-ahead
            if (clip.playbackMode == PlaybackMode::PingPong && sourceLength > 0) {
                if (sourceLength <= ringBufferCapacity) {
                    // Small source: buffer can hold all frames
                    // Target is end of media so all frames get decoded once
                    worker->targetFrame.store(clip.mediaStartFrame + sourceLength - 1);
                } else {
                    // Large source: need chunk-based buffering
                    // During forward: decode ahead of current position
                    // During reverse: need to seek backward when entering reverse
                    if (isReverse) {
                        // In reverse: target is current frame (we need frames leading up to here)
                        worker->targetFrame.store(mediaFrame);
                    } else {
                        // In forward: decode ahead
                        worker->targetFrame.store(mediaFrame + DECODE_AHEAD_FRAMES);
                    }
                }
            } else {
                worker->targetFrame.store(mediaFrame + DECODE_AHEAD_FRAMES);
            }

            // Check if we need to seek
            // Skip seek detection during scrubbing to prevent buffer thrashing
            FrameNumber lastRequested = worker->lastRequestedFrame.load();
            bool needsSeek = false;

            if (!m_timeline->isScrubbing() && !worker->seekPending.load() && lastRequested != DecodeWorker::INVALID_FRAME) {
                int64_t frameDelta = static_cast<int64_t>(mediaFrame) - static_cast<int64_t>(lastRequested);

                if (clip.playbackMode == PlaybackMode::PingPong) {
                    // Ping-pong seek logic depends on source size vs buffer capacity
                    if (sourceLength <= ringBufferCapacity) {
                        // Small source: all frames fit in buffer, only seek on large user jumps
                        if (std::abs(frameDelta) > static_cast<int64_t>(sourceLength)) {
                            needsSeek = true;
                        }
                    } else {
                        // Large source: need to seek when direction changes
                        // or when frame is outside estimated buffer range
                        FrameNumber currentDecoded = worker->currentFrame.load();

                        // Estimate what's in buffer: last ringBufferCapacity frames decoded
                        FrameNumber bufferStart = (currentDecoded > ringBufferCapacity) ?
                                                   currentDecoded - ringBufferCapacity : 0;
                        FrameNumber bufferEnd = currentDecoded;

                        // Seek if requested frame is outside buffer range
                        if (mediaFrame < bufferStart || mediaFrame > bufferEnd + DECODE_AHEAD_FRAMES) {
                            needsSeek = true;
                            std::cout << "Ping-pong seek: frame " << mediaFrame
                                      << " outside buffer range [" << bufferStart << "-" << bufferEnd << "]" << std::endl;
                        }
                    }
                } else {
                    // Normal seek logic for non-ping-pong modes
                    constexpr int SEEK_HYSTERESIS = 8;
                    if (frameDelta < -SEEK_HYSTERESIS) {
                        needsSeek = true;
                    } else if (frameDelta > DECODE_AHEAD_FRAMES + SEEK_HYSTERESIS) {
                        needsSeek = true;
                    } else {
                        // Decoder too slow to keep up (CPU-bound): seek forward to target
                        // so playback holds wall-clock pacing by dropping intermediate frames
                        // instead of playing in slow-motion.
                        FrameNumber currentDecoded = worker->currentFrame.load();
                        if (mediaFrame > currentDecoded &&
                            (mediaFrame - currentDecoded) > DECODE_AHEAD_FRAMES + SEEK_HYSTERESIS) {
                            needsSeek = true;
                        }
                    }
                }
            }

            // Update last requested frame
            worker->lastRequestedFrame.store(mediaFrame);

            // Force seek when scrubbing just ended to ensure buffer is at correct position
            if (scrubbingJustEnded) {
                needsSeek = true;
            }

            if (needsSeek) {
                std::cout << "Seek: jump from " << lastRequested << " to " << mediaFrame << std::endl;
                seekClip(entity, mediaFrame);
            }

            // Update FrameBuffer component state
            if (frameBuffer.ringBuffer) {
                frameBuffer.bufferedFrames.store(frameBuffer.ringBuffer->getCount());
                frameBuffer.targetFrame.store(mediaFrame);
                frameBuffer.currentPTS.store(static_cast<Timestamp>(mediaFrame));
            }
        } else if (currentTimelineFrame < clip.startFrame) {
            // Timeline is BEFORE clip start - proactively prepare for clip entry
            // This prevents stale frame flash when re-entering the clip
            FrameNumber lastRequested = worker->lastRequestedFrame.load();

            // If we were previously showing a later frame, seek to start
            if (lastRequested != DecodeWorker::INVALID_FRAME &&
                lastRequested != clip.mediaStartFrame &&
                !worker->seekPending.load()) {
                // Reset to clip start so we're ready when timeline enters clip
                seekClip(entity, clip.mediaStartFrame);
                worker->lastRequestedFrame.store(DecodeWorker::INVALID_FRAME);
            }
        }
    }

    // Clean up workers for removed entities
    std::vector<entt::entity> toRemove;
    for (auto& [entity, worker] : m_workers) {
        if (!registry.valid(entity) || !registry.all_of<Clip, FrameBuffer>(entity)) {
            toRemove.push_back(entity);
        }
    }
    for (entt::entity entity : toRemove) {
        destroyWorker(entity);
    }
}

void DecodeSystem::shutdown(entt::registry& registry) {
    std::cout << "DecodeSystem shutting down..." << std::endl;

    // Stop all workers
    for (auto& [entity, worker] : m_workers) {
        if (worker && worker->running.load()) {
            worker->running.store(false);
            worker->cv.notify_all();
        }
    }

    // Join all threads
    for (auto& [entity, worker] : m_workers) {
        if (worker && worker->thread.joinable()) {
            worker->thread.join();
        }
    }

    m_workers.clear();
    std::cout << "DecodeSystem shutdown complete" << std::endl;
}

void DecodeSystem::seekClip(entt::entity clipEntity, FrameNumber frame) {
    auto workerIt = m_workers.find(clipEntity);
    if (workerIt == m_workers.end()) return;

    DecodeWorker* worker = workerIt->second.get();
    if (!worker) return;

    // If worker is still initializing, just update the target for when it's ready
    // The decode thread will pick up the seek after initialization completes
    if (worker->initFailed.load()) {
        return;  // Worker failed to initialize, skip
    }

    // Update targetFrame BEFORE signaling seek
    // This ensures decode thread has valid target immediately after processing seek
    // Fixes race condition where decode thread would stall due to stale targetFrame
    worker->targetFrame.store(frame + DECODE_AHEAD_FRAMES);

    // Signal seek to decode thread
    worker->seekTarget.store(frame);
    worker->seekPending.store(true);
    worker->cv.notify_all();
}

void DecodeSystem::pauseAll() {
    m_globalPaused.store(true);
    for (auto& [entity, worker] : m_workers) {
        if (worker) {
            worker->paused.store(true);
        }
    }
}

void DecodeSystem::resumeAll() {
    m_globalPaused.store(false);
    for (auto& [entity, worker] : m_workers) {
        if (worker) {
            worker->paused.store(false);
            worker->cv.notify_all();
        }
    }
}

const DecodeWorker* DecodeSystem::getWorker(entt::entity clipEntity) const {
    auto it = m_workers.find(clipEntity);
    if (it != m_workers.end()) {
        return it->second.get();
    }
    return nullptr;
}

void DecodeSystem::createWorker(entt::entity entity, entt::registry& registry, FrameNumber initialFrame) {
    auto totalStart = std::chrono::high_resolution_clock::now();

    auto* clip = registry.try_get<Clip>(entity);
    auto* frameBuffer = registry.try_get<FrameBuffer>(entity);

    if (!clip || !frameBuffer) {
        std::cerr << "Cannot create decode worker: missing Clip or FrameBuffer component" << std::endl;
        return;
    }

    // Create ring buffer if not already present
    // For ping-pong mode, use a larger buffer to hold all source frames
    auto ringBufferStart = std::chrono::high_resolution_clock::now();
    if (!frameBuffer->ringBuffer) {
        uint32_t bufferCapacity = FrameRingBuffer::DEFAULT_CAPACITY;  // 32 frames default

        if (clip->playbackMode == PlaybackMode::PingPong && clip->totalMediaFrames > 0) {
            // For ping-pong, buffer should hold ALL source frames for smooth bidirectional playback
            // Cap at 256 frames (~8 seconds @ 30fps) to avoid excessive memory use
            constexpr uint32_t MAX_PINGPONG_BUFFER = 256;
            bufferCapacity = std::min(static_cast<uint32_t>(clip->totalMediaFrames) + 8, MAX_PINGPONG_BUFFER);
            std::cout << "Ping-pong clip: using buffer capacity " << bufferCapacity
                      << " for " << clip->totalMediaFrames << " source frames" << std::endl;
        }

        frameBuffer->ringBuffer = std::make_shared<FrameRingBuffer>(bufferCapacity);
    }
    auto ringBufferElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - ringBufferStart).count();

    // Create worker with DEFERRED initialization
    // Decoder is opened in the worker thread to avoid blocking the main thread
    // This prevents freezes when clips become active during playback
    auto workerCreateStart = std::chrono::high_resolution_clock::now();
    auto worker = std::make_shared<DecodeWorker>();
    worker->ringBuffer = frameBuffer->ringBuffer;
    worker->running.store(true);
    worker->currentFrame.store(initialFrame);
    worker->targetFrame.store(initialFrame + DECODE_AHEAD_FRAMES);

    // Store info for deferred decoder initialization in worker thread
    worker->filepath = clip->filepath;
    worker->mediaType = clip->mediaType;
    worker->initialized.store(false);
    worker->initFailed.store(false);

    // Copy playback mode info for loop/ping-pong support
    worker->playbackMode = clip->playbackMode;
    worker->totalMediaFrames = clip->totalMediaFrames > 0 ? clip->totalMediaFrames : clip->duration;

    // Set up initial seek to the correct position
    worker->seekTarget.store(initialFrame);
    worker->seekPending.store(true);
    auto workerCreateElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - workerCreateStart).count();

    // Start decode thread (decoder will be opened there, not here)
    // Pass shared_ptr to ensure worker lifetime extends beyond thread execution
    auto threadStart = std::chrono::high_resolution_clock::now();
    worker->thread = std::thread(&DecodeSystem::decodeThreadFunc, worker, entity);
    auto threadElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - threadStart).count();

    m_workers[entity] = worker;

    auto totalElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - totalStart).count();

    std::cout << "[TIMING] createWorker total=" << totalElapsed << "us"
              << " (ringBuffer=" << ringBufferElapsed << "us"
              << ", workerCreate=" << workerCreateElapsed << "us"
              << ", thread=" << threadElapsed << "us)"
              << " for " << clip->filepath << std::endl;
}

void DecodeSystem::destroyWorker(entt::entity entity) {
    auto it = m_workers.find(entity);
    if (it == m_workers.end()) return;

    DecodeWorker* worker = it->second.get();
    if (worker) {
        // Signal thread to stop
        worker->running.store(false);
        worker->cv.notify_all();

        // Wait for thread to finish
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }

    m_workers.erase(it);
    std::cout << "Destroyed decode worker for entity" << std::endl;
}

void DecodeSystem::decodeThreadFunc(std::shared_ptr<DecodeWorker> worker, entt::entity entity) {
    if (!worker || !worker->ringBuffer) {
        std::cerr << "Decode thread started with invalid worker state" << std::endl;
        return;
    }

    // Wrap the whole body in try/catch so a decoder bug (bad file, corrupted stream,
    // bad_alloc under memory pressure, unexpected FFmpeg return) doesn't tear down
    // the whole process. The clip is marked failed and playback continues without it.
    try {

    std::cout << "Decode thread started for entity (initializing decoder...)" << std::endl;

    // DEFERRED INITIALIZATION: Open decoder in worker thread, not main thread
    // This prevents blocking the main render loop when clips become active
    auto decoder = createDecoder(worker->mediaType);
    if (!decoder) {
        std::cerr << "Decode thread: Failed to create decoder for media type: "
                  << static_cast<int>(worker->mediaType) << std::endl;
        worker->initFailed.store(true);
        return;
    }

    Result openResult = decoder->open(worker->filepath);
    if (openResult != Result::Success) {
        std::cerr << "Decode thread: Failed to open media file: " << worker->filepath << std::endl;
        worker->initFailed.store(true);
        return;
    }

    // Store the decoder and mark as initialized
    worker->decoder = std::move(decoder);
    worker->initialized.store(true);

    std::cout << "Decode thread: Decoder initialized for " << worker->filepath << std::endl;

    // Allocate frame buffer based on decoder dimensions
    DecodedFrame frame;
    frame.allocate(worker->decoder->getWidth(), worker->decoder->getHeight());

    FrameNumber nextFrame = 0;

    while (worker->running.load()) {
        // Check for pause
        if (worker->paused.load()) {
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->cv.wait(lock, [worker]() {
                return !worker->paused.load() || !worker->running.load();
            });
            if (!worker->running.load()) break;
        }

        // Check for seek request
        if (worker->seekPending.load()) {
            FrameNumber seekFrame = worker->seekTarget.load();

            // Check if the sought frame is already in the buffer
            DecodedFrame testFrame;
            bool frameInBuffer = worker->ringBuffer->getFrame(seekFrame, testFrame);

            if (frameInBuffer) {
                // Frame already buffered - no need to clear, just update decode position
                // This prevents buffer thrashing during rapid scrubbing
                nextFrame = seekFrame + 1;  // Continue decoding from after the sought frame
                worker->currentFrame.store(seekFrame);
            } else {
                // Frame not in buffer - need to clear and seek
                worker->ringBuffer->clear();

                // Seek the decoder
                Result result = worker->decoder->seek(seekFrame);
                if (result == Result::Success) {
                    nextFrame = seekFrame;
                    worker->currentFrame.store(seekFrame);
                } else {
                    std::cerr << "Decode seek failed for frame " << seekFrame << std::endl;
                }
            }

            // Safety: Ensure targetFrame is valid for immediate decoding
            // This handles edge cases where main thread hasn't updated targetFrame yet
            FrameNumber currentTarget = worker->targetFrame.load();
            if (nextFrame > currentTarget) {
                worker->targetFrame.store(nextFrame + DECODE_AHEAD_FRAMES);
            }

            worker->seekPending.store(false);
        }

        // Check if we should decode more frames
        FrameNumber target = worker->targetFrame.load();
        uint32_t bufferCount = worker->ringBuffer->getCount();

        // Stop decoding if buffer is full or we've reached target
        if (worker->ringBuffer->isFull() || nextFrame > target) {
            // Wait a bit before checking again
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Don't decode past media duration
        // For loop/ping-pong modes, the DecodeSystem::update() will trigger seeks
        // when the target frame wraps around
        if (nextFrame >= worker->decoder->getDuration()) {
            // Reached end of media - wait for seek from update() or user action
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Ensure frame.data is sized for decode. After a successful push, the ring-buffer
        // move-assignment swaps our vector with the slot's previous (cleared) vector —
        // size becomes 0 but capacity is preserved (std::vector::clear contract).
        // resize() back to full is free at steady state; one-time alloc per ring slot.
        const size_t expectedBytes = static_cast<size_t>(
            worker->decoder->getWidth()) * worker->decoder->getHeight() * 4;
        if (frame.data.size() < expectedBytes) {
            frame.data.resize(expectedBytes);
        }

        // Decode the next frame
        Result result = worker->decoder->decodeFrame(nextFrame, frame);

        if (result == Result::Success && frame.valid.load(std::memory_order_acquire)) {
            frame.frameNumber = nextFrame;

            // Move frame directly into ring buffer (O(1) vector pointer swap, no 8.3 MB copy).
            // Ring buffer's move-assignment swaps frame.data with the slot's old (empty) buffer;
            // the resize check at the top of the next iteration restores size for the next decode.
            if (worker->ringBuffer->push(std::move(frame))) {
                worker->currentFrame.store(nextFrame);
                nextFrame++;
            } else {
                // Buffer full - wait and retry. frame.data is unchanged (push was no-op on isFull).
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } else {
            // Decode failed - skip frame
            std::cerr << "Failed to decode frame " << nextFrame << std::endl;
            nextFrame++;
        }
    }

    std::cout << "Decode thread exiting" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[DecodeThread] Unhandled exception for " << worker->filepath
                  << ": " << e.what() << " — marking clip failed, playback continues" << std::endl;
        worker->initFailed.store(true);
        worker->running.store(false);
    } catch (...) {
        std::cerr << "[DecodeThread] Unknown exception for " << worker->filepath
                  << " — marking clip failed, playback continues" << std::endl;
        worker->initFailed.store(true);
        worker->running.store(false);
    }
    (void)entity;  // Suppress unused-parameter warning in unlikely catch paths
}

} // namespace entity
