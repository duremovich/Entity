#include "entity/media/MediaProbeWorker.hpp"

#include "entity/media/Decoder.hpp"
#include "entity/media/TranscodeManager.hpp"  // isHapMediaType

#include <chrono>
#include <filesystem>
#include <iostream>
#include <utility>

namespace entity {

namespace {

// Capture the on-disk file's size + mtime as the (size, mtime-unix-seconds)
// pair persisted alongside a probe result. Mirrors ContentScanner's
// pattern (`toUnix` + `last_write_time` + `file_size`); no shared header
// yet — duplicating the helper is cheaper than introducing one for two
// call sites that may diverge in error semantics later.
struct FileStat {
    std::int64_t sizeBytes;
    std::int64_t mtimeUnix;
};

std::optional<FileStat> statFile(const std::string& absolutePath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::uintmax_t sz = fs::file_size(absolutePath, ec);
    if (ec) return std::nullopt;
    const auto ftt = fs::last_write_time(absolutePath, ec);
    if (ec) return std::nullopt;
    using namespace std::chrono;
    auto sys = time_point_cast<system_clock::duration>(
        ftt - fs::file_time_type::clock::now() + system_clock::now());
    const auto mtime = duration_cast<seconds>(sys.time_since_epoch()).count();
    return FileStat{static_cast<std::int64_t>(sz), static_cast<std::int64_t>(mtime)};
}

}  // namespace

CodecTier classifyCodec(std::string_view ffmpegName) {
    if (ffmpegName.empty()) return CodecTier::Unknown;
    // HAP family — designed for realtime media servers.
    if (ffmpegName == "hap")    return CodecTier::Good;
    // Intra-frame / image sequences — heavy but predictable.
    if (ffmpegName == "prores") return CodecTier::OK;
    if (ffmpegName == "dnxhd")  return CodecTier::OK;
    if (ffmpegName == "cfhd")   return CodecTier::OK;  // CineForm
    if (ffmpegName == "png")    return CodecTier::OK;
    if (ffmpegName == "dpx")    return CodecTier::OK;
    if (ffmpegName == "tiff")   return CodecTier::OK;
    // Interframe / GOP codecs — seek-hostile, slow random-access.
    if (ffmpegName == "h264")   return CodecTier::Bad;
    if (ffmpegName == "hevc")   return CodecTier::Bad;
    if (ffmpegName == "vp9")    return CodecTier::Bad;
    if (ffmpegName == "av1")    return CodecTier::Bad;
    if (ffmpegName == "mpeg4")  return CodecTier::Bad;
    return CodecTier::OK;  // unknowns default to "playable but unverified"
}

std::string prettyCodec(std::string_view ffmpegName) {
    if (ffmpegName == "hap")    return "HAP";
    if (ffmpegName == "prores") return "ProRes";
    if (ffmpegName == "dnxhd")  return "DNxHD";
    if (ffmpegName == "cfhd")   return "CineForm";
    if (ffmpegName == "h264")   return "H.264";
    if (ffmpegName == "hevc")   return "H.265";
    if (ffmpegName == "vp9")    return "VP9";
    if (ffmpegName == "av1")    return "AV1";
    if (ffmpegName == "mpeg4")  return "MPEG-4";
    if (ffmpegName == "png")    return "PNG sequence";
    if (ffmpegName == "dpx")    return "DPX sequence";
    if (ffmpegName == "tiff")   return "TIFF sequence";
    if (ffmpegName.empty())     return "(unknown)";
    return std::string(ffmpegName);
}

MediaProbeWorker::MediaProbeWorker(
    std::function<std::string(const std::string&)> resolveAbsolutePath)
    : m_resolveAbsolutePath(std::move(resolveAbsolutePath)) {
    m_thread = std::thread([this]() { runLoop(); });
}

MediaProbeWorker::~MediaProbeWorker() {
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        m_stop.store(true, std::memory_order_release);
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void MediaProbeWorker::enqueue(const std::string& storedPath) {
    if (storedPath.empty()) return;

    // Resolve on the caller's thread — keeps the worker out of
    // ProjectManager. Empty resolved path == we can't probe (Linked
    // file with no override yet, etc); skip silently.
    std::string abs = m_resolveAbsolutePath ? m_resolveAbsolutePath(storedPath)
                                            : storedPath;
    if (abs.empty()) return;

    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        if (m_known.count(storedPath)) return;  // already queued / done
        m_known.insert(storedPath);
        m_queue.push_back({storedPath, std::move(abs)});
    }
    m_pending.fetch_add(1, std::memory_order_relaxed);
    m_totalSinceReset.fetch_add(1, std::memory_order_relaxed);
    m_cv.notify_one();
}

bool MediaProbeWorker::enqueueOrSeed(const std::string& storedPath,
                                     std::int64_t lastSizeBytes,
                                     std::int64_t lastMtimeUnix,
                                     const ProbeInfo& cachedProbe) {
    if (storedPath.empty()) return false;

    std::string abs = m_resolveAbsolutePath ? m_resolveAbsolutePath(storedPath)
                                            : storedPath;
    if (abs.empty()) return false;

    // Cache-hit gate: persisted probe is valid AND on-disk size+mtime
    // match what we recorded at probe time. Both sides of the gate
    // matter — a `valid=false` cache means the prior probe failed
    // (e.g. corrupted file) and we should retry, not seed a useless
    // sentinel.
    if (cachedProbe.valid && lastSizeBytes > 0) {
        const auto stat = statFile(abs);
        if (stat && stat->sizeBytes == lastSizeBytes
                 && stat->mtimeUnix == lastMtimeUnix) {
            {
                std::lock_guard<std::mutex> lk(m_queueMutex);
                if (m_known.count(storedPath)) return true;  // already seeded / probed
                m_known.insert(storedPath);
            }
            {
                std::unique_lock<std::shared_mutex> wlk(m_resultsMutex);
                m_results.emplace(storedPath, cachedProbe);
            }
            // Don't touch counters — no work was queued, MediaBin's
            // "X / N indexed" UI shouldn't count seed-hits as items.
            // Don't touch m_freshlyCompleted — Engine already has this
            // data on the entry, no writeback needed.
            return true;
        }
    }

    // Re-probe path. Mirrors enqueue() but re-uses the abs we resolved.
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        if (m_known.count(storedPath)) return false;
        m_known.insert(storedPath);
        m_queue.push_back({storedPath, std::move(abs)});
    }
    m_pending.fetch_add(1, std::memory_order_relaxed);
    m_totalSinceReset.fetch_add(1, std::memory_order_relaxed);
    m_cv.notify_one();
    return false;
}

std::optional<ProbeInfo> MediaProbeWorker::tryGet(
    const std::string& storedPath) const {
    std::shared_lock<std::shared_mutex> lk(m_resultsMutex);
    auto it = m_results.find(storedPath);
    if (it == m_results.end()) return std::nullopt;
    return it->second;
}

std::vector<CompletedProbe> MediaProbeWorker::drainCompletedFreshProbes() {
    std::vector<CompletedProbe> out;
    std::lock_guard<std::mutex> lk(m_freshMutex);
    out.swap(m_freshlyCompleted);
    return out;
}

std::size_t MediaProbeWorker::pendingCount() const noexcept {
    return m_pending.load(std::memory_order_relaxed);
}

std::size_t MediaProbeWorker::doneCount() const noexcept {
    return m_doneSinceReset.load(std::memory_order_relaxed);
}

std::size_t MediaProbeWorker::totalEnqueuedSinceReset() const noexcept {
    return m_totalSinceReset.load(std::memory_order_relaxed);
}

void MediaProbeWorker::resetCounters() {
    // Reset ONLY counters; m_results / m_known stay (cached probes are
    // still correct). doneSinceReset and totalSinceReset are paired so
    // the UI can render "X / Y" with X<=Y at all times.
    m_doneSinceReset.store(0, std::memory_order_relaxed);
    m_totalSinceReset.store(0, std::memory_order_relaxed);
    // m_pending is the live count of outstanding work and stays
    // accurate across resets; if any items are still queued they keep
    // counting toward both m_pending and the next batch's totals as
    // they get re-counted via enqueue's fetch_add. (In practice
    // resetCounters is called when m_pending == 0 — at the start of a
    // fresh load.)
}

void MediaProbeWorker::runLoop() {
    while (true) {
        QueueEntry entry;
        {
            std::unique_lock<std::mutex> lk(m_queueMutex);
            m_cv.wait(lk, [this]() {
                return m_stop.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (m_stop.load(std::memory_order_acquire) && m_queue.empty()) return;
            entry = std::move(m_queue.front());
            m_queue.pop_front();
        }

        ProbeInfo info = doProbe(entry.absolutePath);

        // Stat AFTER the probe so the recorded size+mtime correspond
        // to the file as the probe saw it — close-in-time, but stat
        // failure (file deleted between open and now) doesn't drop
        // the result; we just persist sizeBytes/mtimeUnix = 0, which
        // forces a re-probe next session.
        const auto stat = statFile(entry.absolutePath);
        const std::int64_t sz = stat ? stat->sizeBytes : 0;
        const std::int64_t mt = stat ? stat->mtimeUnix : 0;

        {
            std::unique_lock<std::shared_mutex> wlk(m_resultsMutex);
            m_results.emplace(entry.storedPath, info);  // copy (fresh queue below)
        }
        {
            std::lock_guard<std::mutex> lk(m_freshMutex);
            m_freshlyCompleted.push_back(
                {entry.storedPath, std::move(info), sz, mt});
        }
        m_pending.fetch_sub(1, std::memory_order_relaxed);
        m_doneSinceReset.fetch_add(1, std::memory_order_relaxed);
    }
}

ProbeInfo MediaProbeWorker::doProbe(const std::string& absolutePath) const {
    // Mirrors the synchronous probe that previously lived in
    // MediaBinWindow.cpp. detectMediaType + decoder.open +
    // probeSourceCodecName each touch FFmpeg's avformat layer; total
    // cost 30-60 ms on 4K ProRes, fine on a worker thread.
    ProbeInfo info;
    const MediaType mt = detectMediaType(absolutePath);
    info.isHap = isHapMediaType(mt);

    if (mt != MediaType::Unknown) {
        if (auto dec = createDecoder(mt); dec) {
            if (dec->open(absolutePath) == Result::Success) {
                info.width       = dec->getWidth();
                info.height      = dec->getHeight();
                info.framerate   = dec->getFrameRate();
                info.totalFrames = dec->getDuration();
                info.hasAlpha    = dec->hasAlpha();
                info.valid       = true;
            }
        }
    }
    info.sourceCodecName  = probeSourceCodecName(absolutePath);
    info.tier             = classifyCodec(info.sourceCodecName);
    info.displayCodecName = prettyCodec(info.sourceCodecName);
    return info;
}

}  // namespace entity
