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
                initialMediaFrame = clip.mediaStartFrame + (currentTimelineFrame - clip.startFrame);
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

        // Calculate which media frame this clip needs based on timeline position
        if (currentTimelineFrame >= clip.startFrame &&
            currentTimelineFrame < clip.startFrame + clip.duration) {
            // Clip is active - calculate target media frame
            FrameNumber mediaFrame = clip.mediaStartFrame + (currentTimelineFrame - clip.startFrame);

            // Update target frame for decode-ahead
            worker->targetFrame.store(mediaFrame + DECODE_AHEAD_FRAMES);

            // Check if we need to seek based on DISCONTINUITY detection
            // Only seek when the user actually jumped to a different position (scrub/click)
            // NOT during normal sequential playback (frame N to frame N+1)
            FrameNumber lastRequested = worker->lastRequestedFrame.load();
            bool needsSeek = false;

            if (!worker->seekPending.load() && lastRequested != UINT32_MAX) {
                // Calculate the jump distance
                int64_t frameDelta = static_cast<int64_t>(mediaFrame) - static_cast<int64_t>(lastRequested);

                // Only seek on actual discontinuities:
                // - Jumped backwards (any amount, user scrubbed back)
                // - Jumped forward more than a reasonable decode-ahead window
                if (frameDelta < 0) {
                    // Jumped backwards - need to seek
                    needsSeek = true;
                } else if (frameDelta > DECODE_AHEAD_FRAMES + 8) {
                    // Jumped way forward (beyond what decode-ahead can cover)
                    needsSeek = true;
                }
                // frameDelta of 0, 1, or small positive = normal playback, let decode catch up
            }

            // Update last requested frame BEFORE seeking (so next frame sees correct delta)
            worker->lastRequestedFrame.store(mediaFrame);

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
    auto ringBufferStart = std::chrono::high_resolution_clock::now();
    if (!frameBuffer->ringBuffer) {
        frameBuffer->ringBuffer = std::make_shared<FrameRingBuffer>();
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

            // Clear the ring buffer
            worker->ringBuffer->clear();

            // Seek the decoder
            Result result = worker->decoder->seek(seekFrame);
            if (result == Result::Success) {
                nextFrame = seekFrame;
                worker->currentFrame.store(seekFrame);
                std::cout << "Decode thread seeked to frame " << seekFrame << std::endl;
            } else {
                std::cerr << "Decode seek failed" << std::endl;
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
        if (nextFrame >= worker->decoder->getDuration()) {
            // Reached end of media - wait for seek or stop
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Decode the next frame
        Result result = worker->decoder->decodeFrame(nextFrame, frame);

        if (result == Result::Success && frame.valid.load(std::memory_order_acquire)) {
            frame.frameNumber = nextFrame;

            // Push to ring buffer (move semantics)
            DecodedFrame frameCopy;
            frameCopy.data = frame.data;  // Copy data
            frameCopy.frameNumber = frame.frameNumber;
            frameCopy.width = frame.width;
            frameCopy.height = frame.height;
            frameCopy.pts = frame.pts;
            frameCopy.valid.store(frame.valid.load(std::memory_order_acquire), std::memory_order_release);

            if (worker->ringBuffer->push(std::move(frameCopy))) {
                worker->currentFrame.store(nextFrame);
                nextFrame++;
            } else {
                // Buffer full - wait and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } else {
            // Decode failed - skip frame
            std::cerr << "Failed to decode frame " << nextFrame << std::endl;
            nextFrame++;
        }
    }

    std::cout << "Decode thread exiting" << std::endl;
}

} // namespace entity
