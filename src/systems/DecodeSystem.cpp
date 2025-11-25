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

    // Handle playback state changes
    PlaybackState playbackState = m_timeline->getPlaybackState();
    bool shouldPause = (playbackState == PlaybackState::Paused || playbackState == PlaybackState::Stopped);

    // Update global pause state based on timeline
    if (shouldPause && !m_globalPaused.load()) {
        pauseAll();
    } else if (!shouldPause && m_globalPaused.load()) {
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
            // Create worker for new clip
            createWorker(entity, registry);
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

            // Check if we need to seek (e.g., timeline jumped or scrubbing)
            FrameNumber lastDecoded = worker->currentFrame.load();
            FrameNumber bufferStart = lastDecoded > DECODE_AHEAD_FRAMES ? lastDecoded - DECODE_AHEAD_FRAMES : 0;

            // Seek if:
            // 1. Requested frame is before buffer start (scrubbed backwards)
            // 2. Requested frame is too far ahead (scrubbed far forward)
            // 3. Buffer is empty and we need frames
            bool needsSeek = (mediaFrame < bufferStart) ||
                            (mediaFrame > lastDecoded + DECODE_AHEAD_FRAMES * 2) ||
                            (frameBuffer.ringBuffer && frameBuffer.ringBuffer->isEmpty() && lastDecoded != mediaFrame);

            if (needsSeek) {
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

    // Signal seek to decode thread
    worker->seekTarget.store(frame);
    worker->seekPending.store(true);
    worker->cv.notify_all();

    std::cout << "Seek requested for clip to frame " << frame << std::endl;
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

void DecodeSystem::createWorker(entt::entity entity, entt::registry& registry) {
    auto* clip = registry.try_get<Clip>(entity);
    auto* frameBuffer = registry.try_get<FrameBuffer>(entity);

    if (!clip || !frameBuffer) {
        std::cerr << "Cannot create decode worker: missing Clip or FrameBuffer component" << std::endl;
        return;
    }

    // Create decoder for this clip's media type
    auto decoder = createDecoder(clip->mediaType);
    if (!decoder) {
        std::cerr << "Failed to create decoder for media type: "
                  << static_cast<int>(clip->mediaType) << std::endl;
        return;
    }

    // Open the media file
    Result result = decoder->open(clip->filepath);
    if (result != Result::Success) {
        std::cerr << "Failed to open media file: " << clip->filepath << std::endl;
        return;
    }

    // Create ring buffer if not already present
    if (!frameBuffer->ringBuffer) {
        frameBuffer->ringBuffer = std::make_shared<FrameRingBuffer>();
    }

    // Create worker
    auto worker = std::make_unique<DecodeWorker>();
    worker->decoder = std::move(decoder);
    worker->ringBuffer = frameBuffer->ringBuffer;
    worker->running.store(true);
    worker->currentFrame.store(0);
    worker->targetFrame.store(DECODE_AHEAD_FRAMES);

    // Start decode thread
    DecodeWorker* workerPtr = worker.get();
    worker->thread = std::thread(&DecodeSystem::decodeThreadFunc, workerPtr, entity);

    m_workers[entity] = std::move(worker);

    std::cout << "Created decode worker for clip: " << clip->filepath << std::endl;
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

void DecodeSystem::decodeThreadFunc(DecodeWorker* worker, entt::entity entity) {
    if (!worker || !worker->decoder || !worker->ringBuffer) {
        std::cerr << "Decode thread started with invalid worker state" << std::endl;
        return;
    }

    std::cout << "Decode thread started for entity" << std::endl;

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

        if (result == Result::Success && frame.valid) {
            frame.frameNumber = nextFrame;

            // Push to ring buffer (move semantics)
            DecodedFrame frameCopy;
            frameCopy.data = frame.data;  // Copy data
            frameCopy.frameNumber = frame.frameNumber;
            frameCopy.width = frame.width;
            frameCopy.height = frame.height;
            frameCopy.pts = frame.pts;
            frameCopy.valid = frame.valid;

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

FrameNumber DecodeSystem::calculateDecodeAhead(FrameNumber currentFrame, FrameNumber bufferCount) const {
    // Simple strategy: always try to keep DECODE_AHEAD_FRAMES in buffer
    return currentFrame + DECODE_AHEAD_FRAMES;
}

} // namespace entity
