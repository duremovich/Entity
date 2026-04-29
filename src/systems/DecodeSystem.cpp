/**
 * DecodeSystem Implementation
 *
 * Manages background decode threads. Each clip with a FrameBuffer marker
 * gets a worker that pumps decoded frames into the engine-global FrameCache.
 */

#include "entity/systems/DecodeSystem.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodeBufferPool.hpp"
#include "entity/media/FrameCache.hpp"
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
    (void)registry;
    std::cout << "DecodeSystem initialized" << std::endl;
}

void DecodeSystem::update(entt::registry& registry, float deltaTime) {
    (void)deltaTime;
    if (!m_timeline) return;

    // Detect scrubbing end - when scrubbing stops, force seeks for all active clips
    bool currentlyScrubbing = m_timeline->isScrubbing();
    bool scrubbingJustEnded = m_wasScrubbing && !currentlyScrubbing;
    m_wasScrubbing = currentlyScrubbing;

    // ALWAYS-WARM BUFFERING — workers don't pause on timeline state, only on
    // explicit globalPaused (off by default). Frames sit hot at the playhead
    // so Play starts with zero buffering latency.
    if (m_globalPaused.load()) {
        resumeAll();
    }

    FrameNumber currentTimelineFrame = m_timeline->getCurrentFrame();

    auto view = registry.view<Clip, FrameBuffer>();
    // FrameBuffer is an empty marker type; entt elides empty components from
    // view::each's tuple, so the binding here is (entity, clip) only.
    for (auto [entity, clip] : view.each()) {
        if (!clip.loaded) continue;

        auto workerIt = m_workers.find(entity);
        if (workerIt == m_workers.end()) {
            // Bootstrap a worker at the current playhead-mapped media frame
            FrameNumber initialMediaFrame = clip.mediaStartFrame;
            if (currentTimelineFrame >= clip.startFrame &&
                currentTimelineFrame < clip.startFrame + clip.duration) {
                double timelineFrameRate = m_timeline->getFrameRate();
                double frameRateRatio = clip.framerate / timelineFrameRate;
                FrameNumber localFrame = currentTimelineFrame - clip.startFrame;
                FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));
                initialMediaFrame = clip.mediaStartFrame + sourceLocalFrame;
            }
            createWorker(entity, registry, initialMediaFrame);
            workerIt = m_workers.find(entity);
            if (workerIt == m_workers.end()) continue;
        }

        DecodeWorker* worker = workerIt->second.get();
        if (!worker) continue;

        // Keep worker's playback-mode + duration views in sync with the clip
        worker->playbackMode = clip.playbackMode;
        if (clip.totalMediaFrames > 0) {
            worker->totalMediaFrames = clip.totalMediaFrames;
        } else {
            double frameRateRatio = clip.framerate / m_timeline->getFrameRate();
            worker->totalMediaFrames = static_cast<FrameNumber>(clip.duration * frameRateRatio);
        }

        if (currentTimelineFrame >= clip.startFrame &&
            currentTimelineFrame < clip.startFrame + clip.duration) {
            // Active clip: compute target media frame using playback mode
            FrameNumber localFrame = currentTimelineFrame - clip.startFrame;

            double timelineFrameRate = m_timeline->getFrameRate();
            double frameRateRatio = clip.framerate / timelineFrameRate;
            FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));

            FrameNumber sourceLength = clip.totalMediaFrames > 0 ? clip.totalMediaFrames :
                static_cast<FrameNumber>(clip.duration * frameRateRatio);
            FrameNumber mediaFrame = clip.mediaStartFrame;

            if (sourceLocalFrame < sourceLength) {
                mediaFrame = clip.mediaStartFrame + sourceLocalFrame;
            } else {
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

            bool isReverse = false;
            if (clip.playbackMode == PlaybackMode::PingPong && sourceLength > 0) {
                FrameNumber cycle = sourceLocalFrame / sourceLength;
                isReverse = (cycle % 2 == 1);
            }
            worker->pingPongReverse.store(isReverse);

            // Decode-ahead target: in ping-pong reverse, target the current
            // mediaFrame (we need frames at and below this one); otherwise
            // run ahead by DECODE_AHEAD_FRAMES.
            if (clip.playbackMode == PlaybackMode::PingPong && isReverse) {
                worker->targetFrame.store(mediaFrame);
            } else {
                worker->targetFrame.store(mediaFrame + DECODE_AHEAD_FRAMES);
            }

            // Seek-on-discontinuity. Skip during scrubbing — seeks fire on
            // scrub-release instead, to avoid thrashing the cache while the
            // user is dragging.
            FrameNumber lastRequested = worker->lastRequestedFrame.load();
            bool needsSeek = false;

            if (!m_timeline->isScrubbing() && !worker->seekPending.load() && lastRequested != DecodeWorker::INVALID_FRAME) {
                int64_t frameDelta = static_cast<int64_t>(mediaFrame) - static_cast<int64_t>(lastRequested);

                if (clip.playbackMode == PlaybackMode::PingPong) {
                    // Ping-pong: only seek on huge jumps (the cache holds the
                    // working set; small back-and-forth motion is hits, not
                    // seeks). The "jump > full source length" heuristic
                    // catches user-initiated mid-cycle scrubs.
                    if (std::abs(frameDelta) > static_cast<int64_t>(sourceLength)) {
                        needsSeek = true;
                    }
                } else {
                    constexpr int SEEK_HYSTERESIS = 8;
                    if (frameDelta < -SEEK_HYSTERESIS) {
                        needsSeek = true;  // backward scrub
                    } else if (frameDelta > DECODE_AHEAD_FRAMES + SEEK_HYSTERESIS) {
                        needsSeek = true;  // user-driven big forward jump
                    }
                    // CPU-bound lag: NO force-seek. The worker self-paces by
                    // jumping `nextFrame` to the current playhead each
                    // iteration (see decodeThreadFunc); ProResDecoder's
                    // decodeFrame uses cheap packet-skipping for those
                    // small forward jumps. User preference: smooth realtime
                    // > getting all source frames. Force-seek with
                    // avcodec_flush_buffers produced 76 ms freezes every
                    // ~300 ms; adaptive pacing produces a steady frame-drop
                    // pattern instead.
                }
            }

            worker->lastRequestedFrame.store(mediaFrame);

            if (scrubbingJustEnded) {
                needsSeek = true;
            }

            if (needsSeek) {
                std::cout << "Seek: jump from " << lastRequested << " to " << mediaFrame << std::endl;
                seekClip(entity, mediaFrame);
            }
        } else if (currentTimelineFrame < clip.startFrame) {
            // Timeline before clip start - prep for re-entry to avoid stale-frame flash
            FrameNumber lastRequested = worker->lastRequestedFrame.load();
            if (lastRequested != DecodeWorker::INVALID_FRAME &&
                lastRequested != clip.mediaStartFrame &&
                !worker->seekPending.load()) {
                seekClip(entity, clip.mediaStartFrame);
                worker->lastRequestedFrame.store(DecodeWorker::INVALID_FRAME);
            }
        }
    }

    // Tear down workers for entities that were destroyed
    std::vector<entt::entity> toRemove;
    for (auto& [entity, worker] : m_workers) {
        if (!registry.valid(entity) || !registry.all_of<Clip, FrameBuffer>(entity)) {
            toRemove.push_back(entity);
        }
    }
    for (entt::entity entity : toRemove) {
        // Drop this clip's frames from the cache so they don't squat budget
        if (m_frameCache) m_frameCache->evictClip(entity);
        destroyWorker(entity);
    }
}

void DecodeSystem::shutdown(entt::registry& registry) {
    (void)registry;
    std::cout << "DecodeSystem shutting down..." << std::endl;

    for (auto& [entity, worker] : m_workers) {
        if (worker && worker->running.load()) {
            worker->running.store(false);
            worker->cv.notify_all();
        }
    }
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

    if (worker->initFailed.load()) return;

    // Update target BEFORE signaling seek so the decode thread sees a valid
    // target the moment it processes the seek (no stall on stale target).
    worker->targetFrame.store(frame + DECODE_AHEAD_FRAMES);

    worker->seekTarget.store(frame);
    worker->seekPending.store(true);
    worker->cv.notify_all();
}

void DecodeSystem::pauseAll() {
    m_globalPaused.store(true);
    for (auto& [entity, worker] : m_workers) {
        if (worker) worker->paused.store(true);
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
    return it != m_workers.end() ? it->second.get() : nullptr;
}

void DecodeSystem::createWorker(entt::entity entity, entt::registry& registry, FrameNumber initialFrame) {
    auto totalStart = std::chrono::high_resolution_clock::now();

    auto* clip = registry.try_get<Clip>(entity);
    if (!clip) {
        std::cerr << "Cannot create decode worker: missing Clip component" << std::endl;
        return;
    }
    if (!m_frameCache) {
        std::cerr << "Cannot create decode worker: FrameCache not injected (Engine::initialize wiring bug)" << std::endl;
        return;
    }

    auto workerCreateStart = std::chrono::high_resolution_clock::now();
    auto worker = std::make_shared<DecodeWorker>();
    worker->cache  = m_frameCache;
    worker->pool   = m_bufferPool;  // may be nullptr; worker falls back to per-frame malloc
    worker->entity = entity;
    worker->running.store(true);
    worker->currentFrame.store(initialFrame);
    worker->targetFrame.store(initialFrame + DECODE_AHEAD_FRAMES);

    // If the main thread already opened a decoder on this entity (via
    // Engine::onMediaDroppedOnTimeline or ProjectManager::load's callback),
    // prefer the path + media type from THAT decoder — those values went
    // through ProjectManager::decoderPathFor and reflect whatever transcoded
    // HAP file the library entry points at. Falling back to clip->filepath/
    // mediaType would open the ORIGINAL (e.g. ProRes) path with a HAPDecoder,
    // which fails at the codec_id check.
    const ClipDecodeState* state = registry.try_get<ClipDecodeState>(entity);
    if (state && state->decoder && state->decoder->isOpen()) {
        worker->filepath  = state->decoder->getFilePath();
        worker->mediaType = state->decoder->getMediaType();
    } else {
        worker->filepath  = clip->filepath;
        worker->mediaType = clip->mediaType;
    }
    worker->initialized.store(false);
    worker->initFailed.store(false);

    worker->playbackMode = clip->playbackMode;
    worker->totalMediaFrames = clip->totalMediaFrames > 0 ? clip->totalMediaFrames : clip->duration;

    worker->seekTarget.store(initialFrame);
    worker->seekPending.store(true);
    auto workerCreateElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - workerCreateStart).count();

    auto threadStart = std::chrono::high_resolution_clock::now();
    worker->thread = std::thread(&DecodeSystem::decodeThreadFunc, worker);
    auto threadElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - threadStart).count();

    m_workers[entity] = worker;

    auto totalElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - totalStart).count();

    std::cout << "[TIMING] createWorker total=" << totalElapsed << "us"
              << " (workerCreate=" << workerCreateElapsed << "us"
              << ", thread=" << threadElapsed << "us)"
              << " for " << clip->filepath << std::endl;
}

void DecodeSystem::destroyWorker(entt::entity entity) {
    auto it = m_workers.find(entity);
    if (it == m_workers.end()) return;

    DecodeWorker* worker = it->second.get();
    if (worker) {
        worker->running.store(false);
        worker->cv.notify_all();
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    m_workers.erase(it);
    std::cout << "Destroyed decode worker for entity" << std::endl;
}

void DecodeSystem::decodeThreadFunc(std::shared_ptr<DecodeWorker> worker) {
    if (!worker || !worker->cache) {
        std::cerr << "Decode thread started with invalid worker state" << std::endl;
        return;
    }

    // Wrap the body in try/catch so a decoder bug (bad file, corrupted stream,
    // bad_alloc, unexpected FFmpeg return) doesn't tear down the whole process.
    // The clip is marked failed and playback continues without it.
    try {

    std::cout << "Decode thread started for entity (initializing decoder...)" << std::endl;

    // Deferred decoder open in the worker thread (not main) so the main loop
    // never blocks on FFmpeg's open path when a clip becomes active.
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

    worker->decoder = std::move(decoder);
    worker->initialized.store(true);
    std::cout << "Decode thread: Decoder initialized for " << worker->filepath << std::endl;

    DecodedFrame frame;
    frame.allocate(worker->decoder->getWidth(), worker->decoder->getHeight());

    FrameNumber nextFrame = 0;

    while (worker->running.load()) {
        if (worker->paused.load()) {
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->cv.wait(lock, [worker]() {
                return !worker->paused.load() || !worker->running.load();
            });
            if (!worker->running.load()) break;
        }

        if (worker->seekPending.load()) {
            FrameNumber seekFrame = worker->seekTarget.load();

            // If the cache already has the seek target, no decoder seek is
            // needed — we just resume decode-ahead from after that frame.
            // This is the click-to-recently-viewed-frame fast path: zero
            // decode work, zero re-seek cost.
            if (worker->cache->has(worker->entity, seekFrame)) {
                nextFrame = seekFrame + 1;
                worker->currentFrame.store(seekFrame);
            } else {
                Result result = worker->decoder->seek(seekFrame);
                if (result == Result::Success) {
                    nextFrame = seekFrame;
                    worker->currentFrame.store(seekFrame);
                } else {
                    std::cerr << "Decode seek failed for frame " << seekFrame << std::endl;
                }
            }

            // Belt-and-braces: if the main thread hasn't bumped target past
            // nextFrame yet, do it ourselves so the decode loop has work.
            FrameNumber currentTarget = worker->targetFrame.load();
            if (nextFrame > currentTarget) {
                worker->targetFrame.store(nextFrame + DECODE_AHEAD_FRAMES);
            }
            worker->seekPending.store(false);
        }

        FrameNumber target = worker->targetFrame.load();

        if (nextFrame > target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Adaptive realtime pacing. `target` = playhead + DECODE_AHEAD_FRAMES;
        // recover the playhead. If we've fallen behind it, jump nextFrame
        // forward so the next decoded frame matches "now," not a stale
        // sequential position. ProResDecoder::decodeFrame uses cheap
        // packet-skipping for these small forward jumps, so this stride
        // cost is bounded.
        //
        // User preference: smooth realtime > getting all source frames. At
        // 4K ProRes 4444 60fps, decoder caps at ~30fps; this pattern
        // produces evenly-spaced 30fps output instead of bursty 76 ms
        // freeze + 5-frame jump every ~300 ms.
        //
        // When decoder keeps up (HAP, smaller content), nextFrame is
        // already >= playhead and this is a no-op.
        const FrameNumber playhead =
            (target > DECODE_AHEAD_FRAMES) ? (target - DECODE_AHEAD_FRAMES) : 0;
        FrameNumber skipped = 0;
        if (nextFrame < playhead) {
            skipped = playhead - nextFrame;
            nextFrame = playhead;
        }

        // Don't decode past media duration. Loop / ping-pong wrap-around is
        // driven by DecodeSystem::update issuing seeks; the worker just
        // parks until that arrives.
        if (nextFrame >= worker->decoder->getDuration()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Skip if cache already has the frame — saves one decode + memcpy.
        // Common during ping-pong cycle when the working set is fully hot.
        if (worker->cache->has(worker->entity, nextFrame)) {
            worker->currentFrame.store(nextFrame);
            nextFrame++;
            continue;
        }

        // Acquire a pixel buffer for this iteration. The std::move into
        // the cache at the bottom of this loop empties frame.data; rather
        // than reallocating ~37.7 MB per 4K-ProRes frame from the OS heap
        // (the path that produced visible playback stutter), we pull from
        // a buffer pool that recycles buffers freed by cache eviction.
        // Cold start: pool is empty, acquire mallocs fresh; once the
        // cache fills and starts evicting, the deleters in FrameCache
        // route those buffers back to the pool, and steady-state acquire
        // is a free-list pop.
        //
        // For RGBA8 this is the obvious w*h*4; HAP's BC* paths set the
        // right size internally during decode. Conservative size here is
        // fine -- BC payloads are smaller than RGBA8 of the same w*h.
        const size_t expectedBytes = static_cast<size_t>(
            worker->decoder->getWidth()) * worker->decoder->getHeight() * 4;
        if (frame.data.size() != expectedBytes) {
            if (worker->pool) {
                frame.data = worker->pool->acquire(expectedBytes);
            } else {
                // Fallback path when no pool wired (e.g. unit tests).
                frame.data.resize(expectedBytes);
            }
        }

        // [DECODE PACE] only fires when adaptive pacing kicked in (nextFrame
        // jumped forward to match the playhead, dropping `skipped` source
        // frames). Confirms the new realtime path is exercised under load
        // and shows the steady-state stride. Quiet for HAP / fast content.
        auto decodeStart = std::chrono::high_resolution_clock::now();
        Result result = worker->decoder->decodeFrame(nextFrame, frame);
        if (skipped > 0) {
            auto decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - decodeStart).count();
            std::cout << "[DECODE PACE] entity=" << static_cast<uint32_t>(worker->entity)
                      << " frame=" << nextFrame
                      << " skipped=" << skipped
                      << " ms=" << decodeMs << std::endl;
        }

        if (result == Result::Success && frame.valid.load(std::memory_order_acquire)) {
            frame.frameNumber = nextFrame;
            worker->cache->put(worker->entity, nextFrame, std::move(frame));
            // After move, frame.data is empty — top of loop re-allocates.
            worker->currentFrame.store(nextFrame);
            nextFrame++;
        } else if (result == Result::EndOfStream) {
            // FFmpeg-encoded HAP fixtures over-report nb_frames by 1 (encoder
            // buffering quirk). Park nextFrame at the duration so the guard
            // above sleeps until a seek arrives. Not an error.
            nextFrame = worker->decoder->getDuration();
        } else {
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
}

} // namespace entity
