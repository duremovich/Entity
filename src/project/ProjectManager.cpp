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
#include "entity/media/FrameRingBuffer.hpp"
#include <algorithm>
#include <iostream>

namespace entity {

void ProjectManager::initialize(Timeline* timeline, entt::registry* registry, IRenderer* renderer) {
    m_timeline = timeline;
    m_registry = registry;
    m_renderer = renderer;
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

    if (!ProjectSerializer::save(*m_timeline, savePath)) {
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
        if (!std::filesystem::exists(mediaPath)) {
            std::cerr << "[ProjectManager] Media file not found: " << mediaPath << std::endl;
            return;
        }

        MediaType mediaType = detectMediaType(mediaPath);
        if (mediaType == MediaType::Unknown) {
            std::cerr << "[ProjectManager] Unsupported media type: " << mediaPath << std::endl;
            return;
        }

        auto decoder = createDecoder(mediaType);
        if (!decoder) {
            std::cerr << "[ProjectManager] Failed to create decoder for: " << mediaPath << std::endl;
            return;
        }

        Result result = decoder->open(mediaPath);
        if (result != Result::Success) {
            std::cerr << "[ProjectManager] Failed to open media: " << mediaPath << std::endl;
            return;
        }

        // Attach VideoTexture if not already present
        if (!m_registry->all_of<VideoTexture>(clipEntity)) {
            auto& videoTex = m_registry->emplace<VideoTexture>(clipEntity);
            videoTex.descriptorSlot = m_renderer->allocateVideoTextureSlot();
            videoTex.width = decoder->getWidth();
            videoTex.height = decoder->getHeight();
        }

        // Attach FrameBuffer if not already present
        if (!m_registry->all_of<FrameBuffer>(clipEntity)) {
            auto& frameBuffer = m_registry->emplace<FrameBuffer>(clipEntity);
            frameBuffer.ringBuffer = std::make_shared<FrameRingBuffer>();
            frameBuffer.isBuffering.store(true);
        }

        // Emplace decode state (moves decoder in, allocates frame)
        auto& state = m_registry->emplace_or_replace<ClipDecodeState>(clipEntity);
        state.decoder = std::move(decoder);
        state.frame = std::make_unique<DecodedFrame>();

        auto& clip = m_registry->get<Clip>(clipEntity);
        state.frame->allocate(clip.width, clip.height);
        state.lastDecodedFrame = UINT32_MAX;

        addMediaFile(mediaPath);

        std::cout << "[ProjectManager] Loaded media: " << mediaPath << std::endl;
    };

    if (!ProjectSerializer::load(*m_timeline, filepath, loadCallback)) {
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
    if (ProjectSerializer::save(*m_timeline, autosavePath)) {
        std::cout << "[Autosave] " << autosavePath.string() << std::endl;
    } else {
        std::cerr << "[Autosave] Failed: " << ProjectSerializer::getLastError() << std::endl;
    }
}

void ProjectManager::addMediaFile(const std::string& filepath) {
    if (std::find(m_loadedMediaFiles.begin(), m_loadedMediaFiles.end(), filepath)
        == m_loadedMediaFiles.end()) {
        m_loadedMediaFiles.push_back(filepath);
    }
}

} // namespace entity
