#include "entity/media/TranscodeManager.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <system_error>

namespace entity {

namespace {

// Fallback cache dir — system temp + subdir so the user's %TEMP% stays
// tidy. Created lazily on first enqueue via setCacheDir(default).
std::filesystem::path defaultCacheDir() {
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    if (ec) base = std::filesystem::current_path();
    return base / "entity_hap_cache";
}

} // namespace

// --- Free helpers ----------------------------------------------------------

bool isHapMediaType(MediaType t) {
    switch (t) {
        case MediaType::VideoHAP:
        case MediaType::VideoHAPAlpha:
        case MediaType::VideoHAPQ:
        case MediaType::VideoHAPR:
            return true;
        default:
            return false;
    }
}

std::string shortHashOfPath(const std::string& absPath) {
    // FNV-1a 64-bit, first 8 hex chars. Matches the hash style used for
    // integration-test golden names elsewhere in the repo.
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : absPath) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ull;
    }
    std::ostringstream oss;
    oss << std::hex << (h & 0xFFFFFFFFull);  // 8 hex chars
    std::string s = oss.str();
    // Left-pad to exactly 8 chars.
    while (s.size() < 8) s.insert(s.begin(), '0');
    return s;
}

// --- TranscodeManager ------------------------------------------------------

void TranscodeManager::setCacheDir(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheDir = dir;
}

std::filesystem::path TranscodeManager::deriveDstPath(const std::string& absSrcPath,
                                                       const std::string& variant) const {
    const std::filesystem::path src(absSrcPath);
    std::string stem = src.stem().string();
    // Image-sequence sources contain '%' — that's not a legal Windows filename
    // char, replace with something harmless so the cache file opens.
    for (char& c : stem) if (c == '%') c = '_';
    std::ostringstream name;
    name << stem << "_" << shortHashOfPath(absSrcPath) << "." << variant << ".mov";

    const std::filesystem::path dir = m_cacheDir.empty() ? defaultCacheDir() : m_cacheDir;
    return dir / name.str();
}

std::string TranscodeManager::enqueue(const std::string& absSrcPath,
                                       const std::string& variant,
                                       double srcFps) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (auto it = m_workers.find(absSrcPath); it != m_workers.end()) {
        return it->second->dstPath();
    }

    // Ensure cache dir exists.
    const std::filesystem::path dir = m_cacheDir.empty() ? defaultCacheDir() : m_cacheDir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cerr << "TranscodeManager: failed to create cache dir " << dir
                  << ": " << ec.message() << std::endl;
    }

    const std::filesystem::path dst = deriveDstPath(absSrcPath, variant);
    auto worker = std::make_unique<TranscodeWorker>(
        absSrcPath, dst.string(), variant, srcFps);
    std::string dstStr = worker->dstPath();
    m_workers.emplace(absSrcPath, std::move(worker));
    return dstStr;
}

bool TranscodeManager::has(const std::string& absSrcPath) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_workers.find(absSrcPath) != m_workers.end();
}

std::optional<TranscodeStatus> TranscodeManager::statusOf(const std::string& absSrcPath) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_workers.find(absSrcPath);
    if (it == m_workers.end()) return std::nullopt;

    TranscodeStatus s;
    const TranscodeWorker& w = *it->second;
    s.state       = w.state();
    s.progress01  = w.progress01();
    s.framesDone  = w.framesDone();
    s.framesTotal = w.framesTotal();
    s.result      = w.result();
    s.variant     = w.variant();
    if (s.state == TranscodeState::Done) {
        s.outputPath = w.dstPath();
    }
    return s;
}

void TranscodeManager::cancel(const std::string& absSrcPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_workers.find(absSrcPath); it != m_workers.end()) {
        it->second->requestCancel();
    }
}

void TranscodeManager::clearFinished() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_workers.begin(); it != m_workers.end();) {
        const TranscodeState st = it->second->state();
        if (st == TranscodeState::Done || st == TranscodeState::Failed) {
            // Destructor joins the thread — terminal states mean the thread
            // has already exited, so join() is effectively free.
            it = m_workers.erase(it);
        } else {
            ++it;
        }
    }
}

void TranscodeManager::joinAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [_, w] : m_workers) w->requestCancel();
    m_workers.clear();  // Destructors join
}

} // namespace entity
