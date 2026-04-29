/**
 * ProjectManager implementation. See header for the rationale.
 */

#include "entity/project/ProjectManager.hpp"
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
                                const std::string& projectName) {
    if (projectName.empty()) {
        std::cerr << "[ProjectManager] createNew: projectName is empty" << std::endl;
        return false;
    }
    // Reject path separators here — std::filesystem would otherwise quietly
    // create nested directories from "act1/scene2" or similar, which is not
    // what the launcher's "Project name" field means.
    if (projectName.find('/')  != std::string::npos ||
        projectName.find('\\') != std::string::npos) {
        std::cerr << "[ProjectManager] createNew: projectName must not contain "
                  << "path separators ('" << projectName << "')" << std::endl;
        return false;
    }

    const std::filesystem::path projectRoot = parentDir / projectName;

    std::error_code existsEc;
    if (std::filesystem::exists(projectRoot, existsEc)) {
        std::cerr << "[ProjectManager] createNew: target already exists: "
                  << projectRoot.string() << std::endl;
        return false;
    }

    auto mkdir = [](const std::filesystem::path& dir) -> bool {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "[ProjectManager] createNew: failed to create "
                      << dir.string() << ": " << ec.message() << std::endl;
            return false;
        }
        return true;
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
        std::cerr << "[ProjectManager] createNew: failed to write empty project file: "
                  << ProjectSerializer::getLastError() << std::endl;
        // Best-effort cleanup of the half-built tree so the next attempt at
        // the same name doesn't trip the "already exists" guard.
        std::error_code rmEc;
        std::filesystem::remove_all(projectRoot, rmEc);
        return false;
    }

    m_projectPath = projectFile;
    std::cout << "[ProjectManager] Created new project at "
              << projectFile.string() << std::endl;
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

ProjectManager::MediaLibraryEntry& ProjectManager::addMediaFile(const std::string& originalPath) {
    if (auto* existing = findEntry(originalPath)) return *existing;
    m_loadedMediaFiles.push_back(MediaLibraryEntry{originalPath, {}, {}, {}});
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

void ProjectManager::removeMediaFile(const std::string& originalPath) {
    m_loadedMediaFiles.erase(
        std::remove_if(m_loadedMediaFiles.begin(), m_loadedMediaFiles.end(),
            [&](const MediaLibraryEntry& e) { return e.originalPath == originalPath; }),
        m_loadedMediaFiles.end());
}

std::string ProjectManager::decoderPathFor(const std::string& originalPath) const {
    if (auto* e = findEntry(originalPath); e && !e->transcodedPath.empty()) {
        // Verify the transcoded file still exists on disk — a stale project
        // might reference a cache that was deleted since save.
        if (std::filesystem::exists(utf8ToPath(e->transcodedPath))) {
            return e->transcodedPath;
        }
    }
    return originalPath;
}

} // namespace entity
