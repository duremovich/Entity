/**
 * ProjectManager implementation. See header for the rationale.
 */

#include "entity/project/ProjectManager.hpp"
#include "entity/project/MediaVersioning.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodedFrame.hpp"
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

// Inverse of WindowManager::pathToUtf8. The std::string -> filesystem::path
// constructor on Windows interprets the bytes as the active narrow codepage
// (typically Windows-1252), so a UTF-8 path with characters outside that
// page (fullwidth colon, CJK, emoji, curly quotes...) gets garbled before it
// reaches std::filesystem::exists. Convert UTF-8 -> wide -> path so the
// native API sees the right bytes.
std::filesystem::path utf8ToPath(const std::string& utf8) {
    if (utf8.empty()) return {};
#ifdef _WIN32
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                          static_cast<int>(utf8.size()),
                                          nullptr, 0);
    if (wlen <= 0) return std::filesystem::path(utf8);  // best-effort
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()),
                        w.data(), wlen);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(utf8);
#endif
}

}  // namespace

namespace entity {

void ProjectManager::initialize(Timeline* timeline, entt::registry* registry, IRenderer* renderer) {
    m_timeline = timeline;
    m_registry = registry;
    m_renderer = renderer;
}

bool ProjectManager::createNew(const std::filesystem::path& parentDir,
                                const std::string& projectName,
                                std::string* errorOut) {
    auto fail = [errorOut](std::string msg) -> bool {
        std::cerr << "[ProjectManager] createNew: " << msg << std::endl;
        if (errorOut) *errorOut = std::move(msg);
        return false;
    };

    if (projectName.empty()) {
        return fail("Project name is empty.");
    }
    // Reject path separators here — std::filesystem would otherwise quietly
    // create nested directories from "act1/scene2" or similar, which is not
    // what the launcher's "Project name" field means.
    if (projectName.find('/')  != std::string::npos ||
        projectName.find('\\') != std::string::npos) {
        return fail("Project name must not contain path separators ('" +
                    projectName + "').");
    }

    const std::filesystem::path projectRoot = parentDir / projectName;

    std::error_code existsEc;
    if (std::filesystem::exists(projectRoot, existsEc)) {
        return fail("A folder named '" + projectName +
                    "' already exists at this location. "
                    "Pick a different name or delete the existing one.");
    }

    auto mkdir = [&fail](const std::filesystem::path& dir) -> bool {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!ec) return true;
        // permission_denied wraps Win32 ERROR_ACCESS_DENIED; surface a
        // hint so the user knows it's not a name/path problem.
        if (ec == std::errc::permission_denied) {
            return fail("This location requires administrator rights to "
                        "write to (cannot create '" + dir.string() +
                        "'). Pick a different folder, or relaunch Entity "
                        "as administrator.");
        }
        return fail("Failed to create '" + dir.string() + "': " + ec.message());
    };

    // Build the subdirectory tree. create_directories cascades, so
    // content/unsorted creates content/ on the way; .cache/thumbnails
    // creates .cache/ on the way.
    if (!mkdir(projectRoot / kContentDir / kUnsortedDir)) return false;
    if (!mkdir(projectRoot / kPresetsDir))                return false;
    if (!mkdir(projectRoot / kObjectsDir))                return false;
    if (!mkdir(projectRoot / kExportsDir))                return false;
    if (!mkdir(projectRoot / kSnapshotsDir))              return false;
    if (!mkdir(projectRoot / kCacheDir / kThumbnailsDir)) return false;

    // Write a minimal v7 .entity at the root by routing through
    // ProjectSerializer with a temporary Timeline+registry. This keeps the
    // schema version in lockstep with PROJECT_VERSION automatically rather
    // than hand-rolling JSON that would silently rot when v8 lands.
    const std::filesystem::path projectFile =
        projectRoot / (projectName + ProjectSerializer::FILE_EXTENSION);

    // New project starts with no media. Clear any leftover library state
    // from a prior project so the empty save reflects "new project."
    m_loadedMediaFiles.clear();
    m_projectPath.clear();

    entt::registry tmpRegistry;
    Timeline tmpTimeline(tmpRegistry);
    if (!ProjectSerializer::save(tmpTimeline, projectFile, this)) {
        std::string serializerErr = ProjectSerializer::getLastError();
        // Best-effort cleanup of the half-built tree so the next attempt at
        // the same name doesn't trip the "already exists" guard.
        std::error_code rmEc;
        std::filesystem::remove_all(projectRoot, rmEc);
        return fail("Failed to write project file '" + projectFile.string() +
                    "': " +
                    (serializerErr.empty() ? std::string("unknown error")
                                           : serializerErr));
    }

    m_projectPath = projectFile;
    std::cout << "[ProjectManager] Created new project at "
              << projectFile.string() << std::endl;
    return true;
}

bool ProjectManager::saveAsBundle(const std::filesystem::path& parentDir,
                                   const std::string& projectName) {
    namespace fs = std::filesystem;

    if (projectName.empty()) {
        std::cerr << "[ProjectManager] saveAsBundle: projectName is empty" << std::endl;
        return false;
    }
    if (projectName.find('/')  != std::string::npos ||
        projectName.find('\\') != std::string::npos) {
        std::cerr << "[ProjectManager] saveAsBundle: projectName must not contain "
                  << "path separators ('" << projectName << "')" << std::endl;
        return false;
    }
    if (m_projectPath.empty()) {
        // No source root to copy from. The caller can use createNew() +
        // save() instead if they want a fresh structured project.
        std::cerr << "[ProjectManager] saveAsBundle: no current project to bundle from "
                     "(save the project once at its current location first)" << std::endl;
        return false;
    }

    const fs::path srcRoot     = m_projectPath.parent_path();
    const fs::path newRoot     = parentDir / projectName;
    const fs::path newFile     = newRoot / (projectName + ProjectSerializer::FILE_EXTENSION);

    std::error_code ec;
    if (fs::exists(newRoot, ec)) {
        std::cerr << "[ProjectManager] saveAsBundle: target already exists: "
                  << newRoot.string() << std::endl;
        return false;
    }

    // Build the destination subdirectory tree. create_directories cascades
    // so missing intermediate parents on the parentDir side are created.
    fs::create_directories(newRoot, ec);
    if (ec) {
        std::cerr << "[ProjectManager] saveAsBundle: failed to create root "
                  << newRoot.string() << ": " << ec.message() << std::endl;
        return false;
    }

    // Sub-directories that participate in the bundle. .cache/ is excluded;
    // it's machine-local and regenerable. Each entry is copied recursively
    // when it exists in the source; absent ones are silently skipped.
    const std::array<const char*, 5> bundleDirs = {
        kContentDir, kPresetsDir, kObjectsDir, kExportsDir, kSnapshotsDir,
    };

    auto copyOptions = fs::copy_options::recursive
                     | fs::copy_options::skip_symlinks
                     | fs::copy_options::overwrite_existing;

    for (const char* dirName : bundleDirs) {
        const fs::path srcDir = srcRoot / dirName;
        if (!fs::exists(srcDir, ec)) continue;

        const fs::path dstDir = newRoot / dirName;
        fs::create_directories(dstDir, ec);
        fs::copy(srcDir, dstDir, copyOptions, ec);
        if (ec) {
            std::cerr << "[ProjectManager] saveAsBundle: failed to copy "
                      << srcDir.string() << " -> " << dstDir.string()
                      << ": " << ec.message() << std::endl;
            // Best-effort cleanup of the half-written destination so a
            // retry isn't blocked by the "target exists" guard.
            std::error_code rmEc;
            fs::remove_all(newRoot, rmEc);
            return false;
        }
    }

    // Write the .entity at the new root. We reuse the existing save() path
    // by temporarily pointing m_projectPath at the new file; on failure
    // restore the old path + tear down the partial bundle.
    const fs::path oldProjectPath = m_projectPath;
    m_projectPath = newFile;
    if (!save(newFile)) {
        m_projectPath = oldProjectPath;
        std::error_code rmEc;
        fs::remove_all(newRoot, rmEc);
        std::cerr << "[ProjectManager] saveAsBundle: project file write failed; "
                     "rolled back partial bundle" << std::endl;
        return false;
    }

    std::cout << "[ProjectManager] Saved bundle to " << newFile.string() << std::endl;
    return true;
}

bool ProjectManager::save(const std::filesystem::path& filepath) {
    if (!m_timeline) {
        std::cerr << "[ProjectManager] Cannot save: timeline not set" << std::endl;
        return false;
    }

    std::filesystem::path savePath = filepath;
    if (savePath.empty()) {
        savePath = m_projectPath.empty() ? std::filesystem::path("project.entity")
                                         : m_projectPath;
    }

    std::cout << "[ProjectManager] Saving project to " << savePath.string() << "..." << std::endl;

    // Create the parent directory if needed — without this, scripted Save
    // commands writing to fresh test_output/ subdirs hit ofstream::open
    // failure in ProjectSerializer.
    if (savePath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(savePath.parent_path(), ec);
        if (ec) {
            std::cerr << "[ProjectManager] Failed to create parent directory '"
                      << savePath.parent_path().string() << "': " << ec.message() << std::endl;
            return false;
        }
    }

    if (!ProjectSerializer::save(*m_timeline, savePath, this)) {
        std::cerr << "[ProjectManager] Save failed: " << ProjectSerializer::getLastError() << std::endl;
        return false;
    }

    m_projectPath = savePath;
    std::cout << "[ProjectManager] Project saved successfully" << std::endl;
    return true;
}

void ProjectManager::closeProject() {
    m_loadedMediaFiles.clear();
    m_projectPath.clear();
    m_nonHapImportPolicy = NonHapImportPolicy::Ask;
    m_autosaveAccumulator = 0.0;
}

bool ProjectManager::load(const std::filesystem::path& filepath) {
    if (!m_timeline || !m_registry || !m_renderer) {
        std::cerr << "[ProjectManager] Cannot load: dependencies not initialized" << std::endl;
        return false;
    }

    std::cout << "[ProjectManager] Loading project from " << filepath.string() << "..." << std::endl;

    // Clear existing clip decode state (destructors release decoders + frames).
    m_registry->clear<ClipDecodeState>();
    m_loadedMediaFiles.clear();

    // Per-clip callback: detect media type, open a decoder, attach the
    // ECS components a fully-loaded clip needs (VideoTexture, FrameBuffer,
    // ClipDecodeState).
    ProjectSerializer::MediaLoadCallback loadCallback =
        [this](entt::entity clipEntity, const std::string& mediaPath) {
        // clip.filepath is the canonical original path; if the library
        // has a transcoded HAP file for it, open that instead. Falls
        // through to the original when no transcode exists (yet).
        const std::string openPath = decoderPathFor(mediaPath);

        if (!std::filesystem::exists(utf8ToPath(openPath))) {
            std::cerr << "[ProjectManager] Media file not found: " << openPath << std::endl;
            return;
        }

        MediaType mediaType = detectMediaType(openPath);
        if (mediaType == MediaType::Unknown) {
            std::cerr << "[ProjectManager] Unsupported media type: " << openPath << std::endl;
            return;
        }

        auto decoder = createDecoder(mediaType);
        if (!decoder) {
            std::cerr << "[ProjectManager] Failed to create decoder for: " << openPath << std::endl;
            return;
        }

        Result result = decoder->open(openPath);
        if (result != Result::Success) {
            std::cerr << "[ProjectManager] Failed to open media: " << openPath << std::endl;
            return;
        }

        // Attach VideoTexture if not already present
        if (!m_registry->all_of<VideoTexture>(clipEntity)) {
            auto& videoTex = m_registry->emplace<VideoTexture>(clipEntity);
            videoTex.descriptorSlot = m_renderer->allocateVideoTextureSlot();
            videoTex.width = decoder->getWidth();
            videoTex.height = decoder->getHeight();
        }

        // Marker tag for DecodeSystem (decoded frames live in the engine cache).
        if (!m_registry->all_of<FrameBuffer>(clipEntity)) {
            m_registry->emplace<FrameBuffer>(clipEntity);
        }

        // Emplace decode state (moves decoder in, allocates frame)
        auto& state = m_registry->emplace_or_replace<ClipDecodeState>(clipEntity);
        state.decoder = std::move(decoder);
        state.frame = std::make_unique<DecodedFrame>();

        auto& clip = m_registry->get<Clip>(clipEntity);
        state.frame->allocate(clip.width, clip.height);
        state.lastDecodedFrame = UINT32_MAX;

        // Mark the clip as loaded so DecodeSystem actually picks it up.
        // ProjectSerializer sets loaded=false during deserialization; without
        // flipping it back here the decode worker skips the clip and the
        // compose target stays empty (user-visible as cyan bleed-through on
        // the stage preview after a project reload).
        clip.loaded = true;

        // Original path is the identity — library already has it from the
        // serializer loading mediaLibrary above, but this is idempotent.
        addMediaFile(mediaPath);

        std::cout << "[ProjectManager] Loaded media: " << openPath << std::endl;
    };

    if (!ProjectSerializer::load(*m_timeline, filepath, loadCallback, this)) {
        std::cerr << "[ProjectManager] Load failed: " << ProjectSerializer::getLastError() << std::endl;
        return false;
    }

    m_projectPath = filepath;
    std::cout << "[ProjectManager] Project loaded successfully" << std::endl;
    return true;
}

void ProjectManager::tickAutosave(double deltaTime) {
    if (!m_timeline) return;
    m_autosaveAccumulator += deltaTime;
    if (m_autosaveAccumulator < m_autosaveInterval) return;
    m_autosaveAccumulator = 0.0;

    std::filesystem::path autosavePath =
        m_projectPath.empty() ? std::filesystem::path("autosave.entity")
                              : m_projectPath;
    autosavePath += ".autosave";

    // Write directly via the serializer — autosave is a side channel, don't
    // touch m_projectPath. Operator still expects "Save" to write the real file.
    if (ProjectSerializer::save(*m_timeline, autosavePath, this)) {
        std::cout << "[Autosave] " << autosavePath.string() << std::endl;
    } else {
        std::cerr << "[Autosave] Failed: " << ProjectSerializer::getLastError() << std::endl;
    }
}

ProjectManager::MediaLibraryEntry&
ProjectManager::addMediaFile(const std::string& originalPath, PathKind kind) {
    if (auto* existing = findEntry(originalPath)) return *existing;
    MediaLibraryEntry entry;
    entry.originalPath = originalPath;
    entry.pathKind = kind;
    m_loadedMediaFiles.push_back(std::move(entry));
    return m_loadedMediaFiles.back();
}

void ProjectManager::setTranscodedPath(const std::string& originalPath,
                                       const std::string& transcodedPath,
                                       const std::string& variant) {
    auto& entry = addMediaFile(originalPath);  // create-or-get
    entry.transcodedPath = transcodedPath;
    entry.variant = variant;
}

const ProjectManager::MediaLibraryEntry*
ProjectManager::findEntry(const std::string& originalPath) const {
    auto it = std::find_if(m_loadedMediaFiles.begin(), m_loadedMediaFiles.end(),
        [&](const MediaLibraryEntry& e) { return e.originalPath == originalPath; });
    return (it != m_loadedMediaFiles.end()) ? &(*it) : nullptr;
}

ProjectManager::MediaLibraryEntry*
ProjectManager::findEntry(const std::string& originalPath) {
    auto it = std::find_if(m_loadedMediaFiles.begin(), m_loadedMediaFiles.end(),
        [&](const MediaLibraryEntry& e) { return e.originalPath == originalPath; });
    return (it != m_loadedMediaFiles.end()) ? &(*it) : nullptr;
}

const ProjectManager::MediaLibraryEntry*
ProjectManager::findLatestInGroup(const std::string& storedPath) const {
    if (storedPath.empty()) return nullptr;

    const std::string targetGroup = groupKeyOf(storedPath);
    // Determine the pathKind we're scoping to: prefer the kind of an
    // exact-match entry if one exists; else fall back to any entry in
    // the group (which all share kind by construction — Linked and
    // Managed paths would have different group keys).
    PathKind scopeKind = PathKind::Linked;
    bool kindKnown = false;
    if (auto* exact = findEntry(storedPath); exact) {
        scopeKind = exact->pathKind;
        kindKnown = true;
    }

    const MediaLibraryEntry* best = nullptr;
    std::string bestTag;
    for (const auto& e : m_loadedMediaFiles) {
        if (groupKeyOf(e.originalPath) != targetGroup) continue;
        if (kindKnown && e.pathKind != scopeKind) continue;
        // Skip entries whose file isn't on disk — pin to a present file
        // when one exists; only fall back to missing if the entire group
        // is missing.
        if (e.missingOnDisk) continue;

        // Re-derive tag (cheap; group is small).
        const std::string stem = std::filesystem::path(e.originalPath).stem().string();
        const std::string tag  = parseVersion(stem).tag;
        if (best == nullptr || compareVersionTags(bestTag, tag) < 0) {
            best    = &e;
            bestTag = tag;
        }
    }

    if (best) return best;

    // If everything is missing, fall back to highest-tag regardless of
    // missingOnDisk so the decoder still gets *some* path to surface as
    // "missing" downstream rather than nullptr.
    for (const auto& e : m_loadedMediaFiles) {
        if (groupKeyOf(e.originalPath) != targetGroup) continue;
        if (kindKnown && e.pathKind != scopeKind) continue;
        const std::string stem = std::filesystem::path(e.originalPath).stem().string();
        const std::string tag  = parseVersion(stem).tag;
        if (best == nullptr || compareVersionTags(bestTag, tag) < 0) {
            best    = &e;
            bestTag = tag;
        }
    }
    return best;
}

void ProjectManager::removeMediaFile(const std::string& originalPath) {
    m_loadedMediaFiles.erase(
        std::remove_if(m_loadedMediaFiles.begin(), m_loadedMediaFiles.end(),
            [&](const MediaLibraryEntry& e) { return e.originalPath == originalPath; }),
        m_loadedMediaFiles.end());
}

std::string ProjectManager::resolveMediaPath(const std::string& storedPath) const {
    if (storedPath.empty()) return storedPath;

    const auto* entry = findEntry(storedPath);
    if (!entry) {
        // Not in the library — return as-is. Callers that hit this path
        // are typically pre-registration (importMedia copying a fresh
        // file in) and already have an absolute path.
        return storedPath;
    }
    if (entry->pathKind == PathKind::Linked) {
        return storedPath;
    }
    // Managed: resolve against project root. If no project is loaded yet
    // (m_projectPath empty), there's no root to resolve against — fall
    // back to the relative string. Caller must treat that as "can't
    // open until a project is loaded."
    if (m_projectPath.empty()) return storedPath;
    return (m_projectPath.parent_path() /
            std::filesystem::path(storedPath)).string();
}

bool ProjectManager::restoreOriginal(const std::string& canonicalPath) {
    namespace fs = std::filesystem;

    if (m_projectPath.empty()) {
        std::cerr << "[ProjectManager] restoreOriginal: no project loaded." << std::endl;
        return false;
    }
    auto* entry = findEntry(canonicalPath);
    if (!entry) {
        std::cerr << "[ProjectManager] restoreOriginal: entry not found: "
                  << canonicalPath << std::endl;
        return false;
    }
    if (entry->pathKind != PathKind::Managed) {
        std::cerr << "[ProjectManager] restoreOriginal: entry is not Managed: "
                  << canonicalPath << std::endl;
        return false;
    }
    if (entry->archivedOriginal.empty()) {
        std::cerr << "[ProjectManager] restoreOriginal: no archive recorded for "
                  << canonicalPath << std::endl;
        return false;
    }

    const fs::path projectRoot = m_projectPath.parent_path();
    const fs::path archiveAbs  = projectRoot / fs::path(entry->archivedOriginal);
    const fs::path canonicalAbs = projectRoot / fs::path(canonicalPath);

    std::error_code ec;
    if (!fs::exists(archiveAbs, ec)) {
        std::cerr << "[ProjectManager] restoreOriginal: archive file missing on disk: "
                  << archiveAbs.string() << std::endl;
        return false;
    }

    // Drop the HAP at the canonical path. The archive (the original) is
    // about to take its place; HAP is regenerable via re-transcode.
    if (fs::exists(canonicalAbs, ec)) {
        fs::remove(canonicalAbs, ec);
        if (ec) {
            std::cerr << "[ProjectManager] restoreOriginal: failed to remove "
                      << canonicalAbs.string() << ": " << ec.message() << std::endl;
            return false;
        }
    }

    // Move the archive into the canonical location. Same-volume rename is
    // atomic; cross-volume falls back to copy + remove. (Per-folder
    // .archive/ is always a sibling under the same root so cross-volume
    // shouldn't happen in practice — the fallback is paranoia.)
    std::error_code renameEc;
    fs::rename(archiveAbs, canonicalAbs, renameEc);
    if (renameEc) {
        fs::copy_file(archiveAbs, canonicalAbs,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[ProjectManager] restoreOriginal: failed to relocate archive "
                      << archiveAbs.string() << " -> " << canonicalAbs.string()
                      << ": " << ec.message() << std::endl;
            return false;
        }
        fs::remove(archiveAbs, ec);  // best-effort
    }

    // Reset the entry. transcodedPath / variant are Linked-side fields but
    // clearing them is harmless if we ever come through this path twice.
    entry->archivedOriginal.clear();
    entry->originalCodec.clear();
    entry->transcodedPath.clear();
    entry->variant.clear();

    std::cout << "[ProjectManager] Restored original at " << canonicalPath
              << " from archive" << std::endl;
    return true;
}

int ProjectManager::rebuildStructure() {
    namespace fs = std::filesystem;
    if (m_projectPath.empty()) {
        std::cerr << "[ProjectManager] rebuildStructure: no project loaded." << std::endl;
        return 0;
    }
    const fs::path root = m_projectPath.parent_path();

    // Same set createNew() builds. Order-independent — each call just
    // creates its own missing parents via create_directories.
    const std::vector<fs::path> targets = {
        root / kContentDir / kUnsortedDir,
        root / kPresetsDir,
        root / kObjectsDir,
        root / kExportsDir,
        root / kSnapshotsDir,
        root / kCacheDir / kThumbnailsDir,
    };

    int created = 0;
    for (const auto& p : targets) {
        std::error_code ec;
        if (fs::exists(p, ec)) continue;
        fs::create_directories(p, ec);
        if (ec) {
            std::cerr << "[ProjectManager] rebuildStructure: failed to create "
                      << p.string() << ": " << ec.message() << std::endl;
            continue;
        }
        ++created;
    }

    std::cout << "[ProjectManager] rebuildStructure: created "
              << created << " directories under " << root.string() << std::endl;
    return created;
}

ProjectManager::FindMediaResult
ProjectManager::findMissingManagedMedia(const std::filesystem::path& searchDir) {
    namespace fs = std::filesystem;
    FindMediaResult result;

    if (m_projectPath.empty()) {
        std::cerr << "[ProjectManager] findMissingMedia: no project loaded." << std::endl;
        return result;
    }
    std::error_code ec;
    if (!fs::exists(searchDir, ec) || !fs::is_directory(searchDir, ec)) {
        std::cerr << "[ProjectManager] findMissingMedia: search dir not found: "
                  << searchDir.string() << std::endl;
        return result;
    }

    const fs::path projectRoot = m_projectPath.parent_path();

    // 1. Identify Managed entries whose canonical paths are missing.
    struct Missing {
        std::string canonical;   // project-relative path
        std::string filename;    // basename for lookup
    };
    std::vector<Missing> missing;
    for (const auto& e : m_loadedMediaFiles) {
        if (e.pathKind != PathKind::Managed) continue;
        const fs::path canonicalAbs = projectRoot / fs::path(e.originalPath);
        if (fs::exists(canonicalAbs, ec)) continue;
        Missing m;
        m.canonical = e.originalPath;
        m.filename  = fs::path(e.originalPath).filename().string();
        missing.push_back(std::move(m));
    }
    result.missingBefore = static_cast<int>(missing.size());
    if (missing.empty()) {
        std::cout << "[ProjectManager] findMissingMedia: nothing missing."
                  << std::endl;
        return result;
    }

    // 2. Build a filename → first-found-path index over `searchDir`.
    //    We accept any extension that appears in the missing set so the
    //    walk doesn't bother indexing unrelated files. Filenames with the
    //    same basename are deduped by first-encounter (stable across
    //    invocations isn't guaranteed, but matches typical "I have a
    //    backup folder" recovery flows).
    std::unordered_set<std::string> wantedFilenames;
    for (const auto& m : missing) wantedFilenames.insert(m.filename);

    std::unordered_map<std::string, fs::path> found;
    fs::recursive_directory_iterator it(searchDir,
                                         fs::directory_options::skip_permission_denied,
                                         ec);
    if (ec) {
        std::cerr << "[ProjectManager] findMissingMedia: failed to iterate "
                  << searchDir.string() << ": " << ec.message() << std::endl;
        return result;
    }
    for (; it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string fname = it->path().filename().string();
        if (wantedFilenames.find(fname) == wantedFilenames.end()) continue;
        // First-encounter wins.
        found.emplace(fname, it->path());
    }

    // 3. Copy each match into its canonical location.
    for (const auto& m : missing) {
        auto hit = found.find(m.filename);
        if (hit == found.end()) {
            ++result.stillMissing;
            continue;
        }
        ++result.matched;

        const fs::path target = projectRoot / fs::path(m.canonical);
        if (target.has_parent_path()) {
            fs::create_directories(target.parent_path(), ec);
            if (ec) {
                std::cerr << "[ProjectManager] findMissingMedia: failed to create "
                          << target.parent_path().string() << ": " << ec.message()
                          << std::endl;
                ++result.stillMissing;
                continue;
            }
        }
        fs::copy_file(hit->second, target,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[ProjectManager] findMissingMedia: copy failed "
                      << hit->second.string() << " -> " << target.string()
                      << ": " << ec.message() << std::endl;
            ++result.stillMissing;
            continue;
        }
        std::cout << "[ProjectManager] findMissingMedia: restored "
                  << m.canonical << " from "
                  << hit->second.string() << std::endl;
        ++result.copied;
    }

    std::cout << "[ProjectManager] findMissingMedia: "
              << "missing=" << result.missingBefore
              << " matched=" << result.matched
              << " copied="  << result.copied
              << " stillMissing=" << result.stillMissing
              << std::endl;
    return result;
}

ProjectManager::CollectResult
ProjectManager::collectLinkedIntoProject(const std::string& subfolder) {
    namespace fs = std::filesystem;

    CollectResult result;
    if (m_projectPath.empty()) {
        std::cerr << "[ProjectManager] collect: no project loaded." << std::endl;
        return result;
    }

    const fs::path projectRoot = m_projectPath.parent_path();
    const std::string sub = subfolder.empty()
                                ? std::string(kUnsortedDir)
                                : subfolder;
    const fs::path targetDirAbs = projectRoot / kContentDir / sub;
    const fs::path archiveDirAbs = targetDirAbs / kArchiveDir;

    std::error_code ec;
    fs::create_directories(targetDirAbs, ec);
    if (ec) {
        std::cerr << "[ProjectManager] collect: failed to create target dir "
                  << targetDirAbs.string() << ": " << ec.message() << std::endl;
        return result;
    }

    // Snapshot the canonical paths to process — the loop below mutates
    // entries (remove + re-add) which would invalidate iterators.
    struct Pending {
        std::string oldOriginalPath;
        std::string transcodedPath;
        std::string variant;
        std::string inputColorSpaceOverride;
        MediaType   detectedMediaType{MediaType::Unknown};
    };
    std::vector<Pending> pending;
    for (const auto& e : m_loadedMediaFiles) {
        if (e.pathKind != PathKind::Linked) {
            ++result.alreadyManaged;
            continue;
        }
        Pending p;
        p.oldOriginalPath         = e.originalPath;
        p.transcodedPath          = e.transcodedPath;
        p.variant                 = e.variant;
        p.inputColorSpaceOverride = e.inputColorSpaceOverride;
        p.detectedMediaType       = detectMediaType(e.originalPath);
        pending.push_back(std::move(p));
    }

    for (const auto& p : pending) {
        const fs::path src(p.oldOriginalPath);
        if (!fs::exists(src, ec)) {
            std::cerr << "[ProjectManager] collect: source missing, "
                         "leaving Linked: " << p.oldOriginalPath << std::endl;
            ++result.missing;
            continue;
        }

        // Choose target filename, collision-suffixed.
        fs::path target = targetDirAbs / src.filename();
        if (fs::exists(target, ec)) {
            const std::string stem = src.stem().string();
            const std::string ext  = src.extension().string();
            for (int i = 1; i < 1000; ++i) {
                fs::path candidate = targetDirAbs /
                    (stem + " (" + std::to_string(i) + ")" + ext);
                if (!fs::exists(candidate, ec)) {
                    target = candidate;
                    break;
                }
            }
        }

        const bool hasTranscode = !p.transcodedPath.empty()
                                  && fs::exists(fs::path(p.transcodedPath), ec);

        std::string newArchivedOriginalRel;
        std::string newOriginalCodec;

        if (hasTranscode) {
            fs::create_directories(archiveDirAbs, ec);
            if (ec) {
                std::cerr << "[ProjectManager] collect: failed to create archive dir "
                          << archiveDirAbs.string() << ": " << ec.message() << std::endl;
                continue;
            }
            const fs::path archiveTarget = archiveDirAbs / target.filename();
            fs::copy_file(src, archiveTarget,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "[ProjectManager] collect: failed to archive "
                          << src.string() << " -> " << archiveTarget.string()
                          << ": " << ec.message() << std::endl;
                continue;
            }
            // Move cache-dir transcode → canonical path. rename() is atomic
            // when same volume; cross-volume falls back to copy + remove.
            const fs::path transSrc(p.transcodedPath);
            std::error_code renameEc;
            fs::rename(transSrc, target, renameEc);
            if (renameEc) {
                fs::copy_file(transSrc, target,
                              fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::cerr << "[ProjectManager] collect: failed to relocate transcode "
                              << transSrc.string() << " -> " << target.string()
                              << ": " << ec.message() << std::endl;
                    continue;
                }
                fs::remove(transSrc, ec);  // best-effort cleanup
            }
            fs::path archiveRel = fs::relative(archiveTarget, projectRoot, ec);
            newArchivedOriginalRel =
                ec ? archiveTarget.generic_string() : archiveRel.generic_string();
            newOriginalCodec = MediaTypeToString(p.detectedMediaType);
        } else {
            fs::copy_file(src, target,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "[ProjectManager] collect: failed to copy "
                          << src.string() << " -> " << target.string()
                          << ": " << ec.message() << std::endl;
                continue;
            }
        }

        const fs::path canonicalRelPath = fs::relative(target, projectRoot, ec);
        const std::string canonical =
            ec ? target.generic_string() : canonicalRelPath.generic_string();

        // Rebuild the entry as Managed. removeMediaFile + addMediaFile
        // is the cleanest way given the library is keyed by originalPath.
        removeMediaFile(p.oldOriginalPath);
        auto& fresh = addMediaFile(canonical, PathKind::Managed);
        fresh.transcodedPath          = "";
        fresh.variant                 = "";
        fresh.archivedOriginal        = newArchivedOriginalRel;
        fresh.originalCodec           = newOriginalCodec;
        fresh.inputColorSpaceOverride = p.inputColorSpaceOverride;

        result.rewrites.emplace_back(p.oldOriginalPath, canonical);
        ++result.collected;
    }

    if (result.collected > 0) {
        std::cout << "[ProjectManager] Collect: " << result.collected
                  << " entries moved into content/" << sub << "/";
        if (result.missing > 0) {
            std::cout << " (" << result.missing << " missing, left as Linked)";
        }
        std::cout << std::endl;
    }
    return result;
}

std::string ProjectManager::decoderPathFor(const std::string& originalPath) const {
    if (originalPath.empty()) return originalPath;

    // Issue #27: clip.filepath is a logical reference. If it carries no
    // `_v<tag>` suffix the resolver auto-rolls to the latest version in
    // the group. If it does carry a tag, we treat the path as pinned and
    // require an exact-match library entry.
    const std::string stem = std::filesystem::path(originalPath).stem().string();
    const ParsedVersion pv = parseVersion(stem);

    auto resolveEntry = [this](const MediaLibraryEntry& e) -> std::string {
        if (!e.transcodedPath.empty()) {
            std::string transcodeAbs = e.transcodedPath;
            if (e.pathKind == PathKind::Managed && !m_projectPath.empty()) {
                transcodeAbs = (m_projectPath.parent_path() /
                                std::filesystem::path(e.transcodedPath)).string();
            }
            if (std::filesystem::exists(utf8ToPath(transcodeAbs))) {
                return transcodeAbs;
            }
        }
        return resolveMediaPath(e.originalPath);
    };

    if (pv.tag.empty()) {
        // Auto-roll mode. Pick the latest member of the group; fall back
        // to the path as-given if the group is empty (file may exist on
        // disk but not yet in the library — pre-scanner code paths hit
        // this).
        if (auto* latest = findLatestInGroup(originalPath); latest) {
            return resolveEntry(*latest);
        }
        return resolveMediaPath(originalPath);
    }

    // Pinned mode — exact match required.
    if (auto* e = findEntry(originalPath); e) {
        return resolveEntry(*e);
    }
    // Pinned-but-missing: return the resolved path so downstream sees
    // "this file should exist here but doesn't" rather than silently
    // rolling to a different version.
    return resolveMediaPath(originalPath);
}

} // namespace entity
