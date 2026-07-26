#include "entity/project/ContentScanner.hpp"
#include "entity/profile/Tracy.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace entity {

namespace fs = std::filesystem;

namespace {

// Whitelist of media extensions (lowercase, with leading dot). Everything
// else in content/ is ignored — we don't want stray .json sidecars or
// .DS_Store cluttering the MediaBin.
const std::unordered_set<std::string>& defaultMediaExts() {
    static const std::unordered_set<std::string> kExts = {
        ".mov", ".mp4", ".m4v", ".avi", ".mkv", ".webm",
        ".png",
        ".hap",  // rarely seen as bare extension; HAP is usually in .mov
    };
    return kExts;
}

// Whitelist of 3D model extensions. Today: OBJ only. glTF/GLB/FBX/STL
// expansion is a planned follow-up — the set is centralized so adding
// extensions is one-line.
const std::unordered_set<std::string>& defaultObjectExts() {
    static const std::unordered_set<std::string> kExts = {
        ".obj",
    };
    return kExts;
}

// User-authored effect pack files (issue #54 Phase 6 hot reload).
const std::unordered_set<std::string>& defaultEffectExts() {
    static const std::unordered_set<std::string> kExts = {
        ".hlsl", ".hlsli", ".json",
    };
    return kExts;
}

std::string lowerExt(const fs::path& p) {
    std::string s = p.extension().string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Convert a path under a watched root to forward-slashed relative form
// against the *project root* so it matches the convention used by
// ProjectManager's Managed paths (consistent across save/load).
std::string toRelativeForward(const fs::path& abs, const fs::path& projectRoot) {
    std::error_code ec;
    fs::path rel = fs::relative(abs, projectRoot, ec);
    if (ec) return {};
    std::string s = rel.generic_string();  // forward slashes
    return s;
}

std::int64_t toUnix(fs::file_time_type ftt) {
    using namespace std::chrono;
    auto sys = time_point_cast<system_clock::duration>(
        ftt - fs::file_time_type::clock::now() + system_clock::now());
    return duration_cast<seconds>(sys.time_since_epoch()).count();
}

bool extInSet(const std::string& lowerWithDot,
              const std::unordered_set<std::string>& set) {
    return set.find(lowerWithDot) != set.end();
}

}  // namespace

ContentScanner::ContentScanner() = default;

ContentScanner::~ContentScanner() {
    stop();
}

void ContentScanner::start(const fs::path& projectRoot) {
    stop();
    if (projectRoot.empty()) return;

    m_projectRoot = projectRoot;
    m_roots.clear();
    m_roots.push_back(WatchRoot{
        DeltaSource::Media,
        projectRoot / "content",
        &defaultMediaExts(),
    });
    m_roots.push_back(WatchRoot{
        DeltaSource::Object,
        projectRoot / "objects",
        &defaultObjectExts(),
    });
    m_roots.push_back(WatchRoot{
        DeltaSource::EffectSource,
        projectRoot / "effects",
        &defaultEffectExts(),
    });
    m_files.clear();
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        m_queue.clear();
    }
    m_stop.store(false, std::memory_order_release);
    m_thread = std::thread([this]() { runLoop(); });
}

void ContentScanner::stop() {
    if (!m_thread.joinable()) return;
    m_stop.store(true, std::memory_order_release);
    m_cv.notify_one();
    m_thread.join();
}

void ContentScanner::setPollIntervalForTesting(std::chrono::milliseconds interval) {
    m_pollInterval = interval;
}

void ContentScanner::tickForTesting() {
    doScan();
}

std::vector<ContentScanner::ScanDelta> ContentScanner::drain() {
    std::vector<ScanDelta> out;
    std::lock_guard<std::mutex> lk(m_queueMutex);
    m_queue.swap(out);
    return out;
}

void ContentScanner::enqueue(ScanDelta d) {
    std::lock_guard<std::mutex> lk(m_queueMutex);
    m_queue.push_back(std::move(d));
}

void ContentScanner::runLoop() {
    tracy::SetThreadName("ContentScanner");
    while (!m_stop.load(std::memory_order_acquire)) {
        // Sleep first so callers that just want a single scan can use
        // tickForTesting + start/stop without racing.
        {
            std::unique_lock<std::mutex> lk(m_cvMutex);
            m_cv.wait_for(lk, m_pollInterval, [this]() {
                return m_stop.load(std::memory_order_acquire);
            });
        }
        if (m_stop.load(std::memory_order_acquire)) break;
        doScan();
    }
}

bool ContentScanner::shouldSkipDir(const fs::path& dirname) const {
    return shouldSkipDirStatic(dirname);
}

bool ContentScanner::shouldSkipFile(const fs::path& filename) const {
    return shouldSkipFileStatic(filename);
}

bool ContentScanner::shouldSkipDirStatic(const fs::path& dirname) {
    const std::string s = dirname.filename().string();
    if (s.empty()) return false;
    return s.front() == '.';  // .archive/, .cache/, .anything
}

bool ContentScanner::shouldSkipFileStatic(const fs::path& filename) {
    const std::string s = filename.filename().string();
    if (s.empty()) return true;
    if (s.front() == '.') return true;
    if (s.front() == '~') return true;
    if (s.size() >= 4 && s.substr(s.size() - 4) == ".tmp")    return true;
    if (s.size() >= 8 && s.substr(s.size() - 8) == ".ffs_tmp") return true;
    return false;
}

bool ContentScanner::isMediaExtensionStatic(const fs::path& ext) {
    std::string s = ext.string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extInSet(s, defaultMediaExts());
}

bool ContentScanner::isObjectExtensionStatic(const fs::path& ext) {
    std::string s = ext.string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extInSet(s, defaultObjectExts());
}

bool ContentScanner::isOpenableForRead(const fs::path& absolute) const {
#ifdef _WIN32
    // CreateFileW with dwShareMode=0 — fails with ERROR_SHARING_VIOLATION
    // if any other process holds the file open for write (e.g. mid-copy
    // by a sync tool that didn't open with FILE_SHARE_READ).
    HANDLE h = CreateFileW(absolute.wstring().c_str(),
                           GENERIC_READ,
                           0,                     // no sharing
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
#else
    // Portable fallback: open + close. Won't catch ShareMode races but
    // at least catches permission/missing-file.
    std::ifstream f(absolute, std::ios::binary);
    return f.good();
#endif
}

void ContentScanner::scanRoot(const WatchRoot& root,
                              std::unordered_map<std::string, FileState>& seen) {
    std::error_code ec;
    if (!fs::exists(root.absoluteDir, ec) || !fs::is_directory(root.absoluteDir, ec)) {
        return;  // root missing — caller's Pass 3 will emit Removed for stale entries
    }

    fs::recursive_directory_iterator it(
        root.absoluteDir,
        fs::directory_options::skip_permission_denied,
        ec);
    if (ec) {
        std::cerr << "[ContentScanner] iterate failed (" << root.absoluteDir
                  << "): " << ec.message() << std::endl;
        return;
    }
    for (; it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) {
            std::cerr << "[ContentScanner] entry error: " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        const fs::directory_entry& entry = *it;
        if (entry.is_directory(ec)) {
            if (shouldSkipDir(entry.path())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (shouldSkipFile(entry.path())) continue;
        if (!extInSet(lowerExt(entry.path()), *root.extensions)) continue;

        const std::string rel = toRelativeForward(entry.path(), m_projectRoot);
        if (rel.empty()) continue;

        FileState st;
        st.source = root.source;
        std::error_code statEc;
        st.sizeBytes  = static_cast<std::int64_t>(fs::file_size(entry.path(), statEc));
        if (statEc) continue;
        auto ftt = fs::last_write_time(entry.path(), statEc);
        if (statEc) continue;
        st.mtimeUnix = toUnix(ftt);
        seen.emplace(rel, st);
    }
}

void ContentScanner::doScan() {
    if (m_projectRoot.empty() || m_roots.empty()) return;

    // Pass 1 — walk every watch root, build a set of currently-present
    // files. The map is keyed by project-relative path; roots are
    // sibling directories so keys can't collide across roots.
    std::unordered_map<std::string, FileState> seen;
    for (const auto& root : m_roots) {
        scanRoot(root, seen);
    }

    // Pass 2 — diff against m_files. Stability + openable gates unchanged
    // from the single-root version.
    for (auto& [rel, currentStat] : seen) {
        auto knownIt = m_files.find(rel);
        if (knownIt == m_files.end()) {
            // First sighting. stableTicks=1 (this tick), can't emit yet.
            currentStat.stableTicks = 1;
            currentStat.reported    = false;
            m_files[rel] = currentStat;
            continue;
        }
        FileState& known = knownIt->second;
        if (known.sizeBytes == currentStat.sizeBytes &&
            known.mtimeUnix == currentStat.mtimeUnix) {
            // Stable for another tick.
            known.stableTicks = std::min(known.stableTicks + 1, 1000);
            if (!known.reported && known.stableTicks >= 2) {
                // Third signal: try opening exclusively.
                fs::path absolute = m_projectRoot / rel;
                if (isOpenableForRead(absolute)) {
                    enqueue({DeltaKind::Added, known.source, rel});
                    known.reported = true;
                }
                // If not openable, leave stableTicks where it is and
                // re-try next tick. Don't reset; size+mtime didn't
                // change.
            }
        } else {
            // Size or mtime changed — file is being rewritten. Reset.
            known.sizeBytes  = currentStat.sizeBytes;
            known.mtimeUnix  = currentStat.mtimeUnix;
            known.stableTicks = 1;
            // Media/Object: if we'd previously reported Added, don't
            // re-emit; the existing entry will pick up the new content
            // next decode open. (TranscodeManager's in-place replace per
            // ADR-0009 also lands in this branch — silent by design.)
            // EffectSource: re-arm so the edit re-emits Added once the
            // write stabilizes — that Added IS the hot-reload trigger.
            if (known.source == DeltaSource::EffectSource) {
                known.reported = false;
            }
        }
    }

    // Pass 3 — anything that's gone.
    for (auto it2 = m_files.begin(); it2 != m_files.end(); ) {
        if (seen.find(it2->first) == seen.end()) {
            if (it2->second.reported) {
                enqueue({DeltaKind::Removed, it2->second.source, it2->first});
            }
            it2 = m_files.erase(it2);
        } else {
            ++it2;
        }
    }
}

}  // namespace entity
