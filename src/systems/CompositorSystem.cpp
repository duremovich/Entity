/**
 * CompositorSystem Implementation
 *
 * Renders visible layers by querying entities with Transform and MediaLayer,
 * sorting by z-order, and issuing draw calls.
 *
 * Timeline-aware: Only renders clips that are active at the current frame.
 * For entities with VideoTexture components, renders video frames.
 * Falls back to colored quads for entities without video textures.
 */

#include "entity/systems/CompositorSystem.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/MappingSurface.hpp"
#include "entity/components/Screen.hpp"

#include <algorithm>
#include <vector>
#include <iostream>
#include <DirectXMath.h>

namespace entity {

CompositorSystem::CompositorSystem(D3D12Renderer* renderer)
    : m_renderer(renderer)
{
}

void CompositorSystem::initialize(entt::registry& registry) {
    std::cout << "CompositorSystem initialized" << std::endl;
}

void CompositorSystem::update(entt::registry& registry, float deltaTime) {
    if (!m_renderer || !m_renderer->isInitialized()) {
        return;
    }

    // Ensure compose target exists (create at 1920x1080 if not)
    if (!m_renderer->isComposeTargetReady()) {
        m_renderer->createComposeTarget(1920, 1080);
    }

    // Get current timeline frame for visibility checks
    FrameNumber currentFrame = 0;
    if (m_timeline) {
        currentFrame = m_timeline->getCurrentFrame();
    }

    static int collectDebugFrame = 0;
    bool logCollection = m_debugLogging && (collectDebugFrame++ % 60 == 0);

    // Find the first visible screen to composite for
    // TODO: Support multiple screens with multiple render targets
    entt::entity targetScreenEntity = entt::null;
    auto screenView = registry.view<Screen>();
    for (auto [screenEntity, screen] : screenView.each()) {
        if (screen.visible) {
            targetScreenEntity = screenEntity;
            if (logCollection) {
                std::cout << "[Compositor] Compositing for screen: " << screen.name << std::endl;
            }
            break;
        }
    }

    // Collect clips for the target screen
    auto view = registry.view<Transform, MediaLayer>();
    std::vector<entt::entity> sortedEntities;
    sortedEntities.reserve(view.size_hint());

    for (auto entity : view) {
        auto& layer = view.get<MediaLayer>(entity);
        if (!layer.visible) continue;

        // Check if this entity has a Clip component - if so, verify it's active
        auto* clip = registry.try_get<Clip>(entity);
        if (clip) {
            if (!isClipActiveAtFrame(*clip, currentFrame)) {
                if (logCollection) std::cout << "  [Compositor] Entity " << static_cast<uint32_t>(entity)
                    << ": SKIPPED (clip not active at frame " << currentFrame
                    << ", start=" << clip->startFrame << " dur=" << clip->duration << ")" << std::endl;
                continue;  // Clip is not active at current frame, skip
            }

            // Filter by target screen:
            // - Include if clip has no target (entt::null means "all screens")
            // - Include if clip targets the active screen
            if (clip->targetScreen != entt::null && clip->targetScreen != targetScreenEntity) {
                if (logCollection) std::cout << "  [Compositor] Entity " << static_cast<uint32_t>(entity)
                    << ": SKIPPED (targets different screen)" << std::endl;
                continue;  // Clip targets a different screen
            }
        }

        sortedEntities.push_back(entity);

        // Log when a clip entity is queued for rendering
        if (logCollection && clip) {
            auto* videoTex = registry.try_get<VideoTexture>(entity);
            std::cout << "  [Compositor] Entity " << static_cast<uint32_t>(entity)
                << ": QUEUED (clip active, videoTex=" << (videoTex ? "yes" : "no");
            if (videoTex) {
                std::cout << " valid=" << videoTex->isValid()
                    << " srv=" << videoTex->srvHandle.ptr
                    << " " << videoTex->width << "x" << videoTex->height;
            }
            std::cout << ")" << std::endl;
        }
    }

    // Sort by z-order (lower values render first, higher values on top)
    std::sort(sortedEntities.begin(), sortedEntities.end(),
        [&view](entt::entity a, entt::entity b) {
            const auto& layerA = view.get<MediaLayer>(a);
            const auto& layerB = view.get<MediaLayer>(b);
            return layerA.zOrder < layerB.zOrder;
        });

    if (m_debugLogging) {
        static int frameCount = 0;
        if (frameCount++ % 60 == 0) {
            std::cout << "[CompositorSystem] Rendering " << sortedEntities.size()
                      << " layers at frame " << currentFrame << std::endl;
        }
    }

    // DEBUG: Log entity count (only when debug logging enabled)
    static int debugFrame = 0;
    bool logThisFrame = m_debugLogging && (debugFrame++ % 60 == 0);  // Every second at 60fps

    // Begin rendering to compose target (offscreen texture)
    m_renderer->beginComposeTarget();

    // Render each visible layer to the compose target
    int drawIndex = 0;
    for (auto entity : sortedEntities) {
        auto& transform = view.get<Transform>(entity);
        const auto& layer = view.get<MediaLayer>(entity);

        // Get transform matrix and convert from glm to DirectX
        const glm::mat4& glmMatrix = transform.getMatrix();

        // Convert glm::mat4 to DirectX::XMMATRIX (both are column-major)
        DirectX::XMMATRIX transformMatrix = DirectX::XMMatrixSet(
            glmMatrix[0][0], glmMatrix[0][1], glmMatrix[0][2], glmMatrix[0][3],
            glmMatrix[1][0], glmMatrix[1][1], glmMatrix[1][2], glmMatrix[1][3],
            glmMatrix[2][0], glmMatrix[2][1], glmMatrix[2][2], glmMatrix[2][3],
            glmMatrix[3][0], glmMatrix[3][1], glmMatrix[3][2], glmMatrix[3][3]
        );

        // Check if entity has a valid video texture
        auto* videoTex = registry.try_get<VideoTexture>(entity);
        auto* clip = registry.try_get<Clip>(entity);

        if (videoTex && videoTex->isValid() && videoTex->srvHandle.ptr != 0) {
            // Draw textured quad for video layers with blend mode
            m_renderer->drawTexturedQuad(videoTex->srvHandle, transformMatrix, layer.opacity, layer.blendMode);
        } else {
            // Fallback to colored quad for non-video layers
            uint32_t entityId = static_cast<uint32_t>(entity);
            DirectX::XMFLOAT4 color(
                ((entityId * 137) % 256) / 255.0f,
                ((entityId * 211) % 256) / 255.0f,
                ((entityId * 97) % 256) / 255.0f,
                1.0f
            );
            if (logThisFrame) {
                std::cout << "  [" << drawIndex << "] Entity " << entityId
                          << ": COLORED";
                if (clip) {
                    std::cout << " (HAS CLIP: " << clip->filepath << ")";
                }
                if (videoTex) {
                    std::cout << " (VideoTex: slot=" << videoTex->descriptorSlot
                              << " valid=" << videoTex->isValid()
                              << " srv=" << videoTex->srvHandle.ptr
                              << " " << videoTex->width << "x" << videoTex->height << ")";
                }
                std::cout << std::endl;
            }
            m_renderer->drawColoredQuad(transformMatrix, color, layer.opacity);
        }
        drawIndex++;
    }

    // End rendering to compose target (transitions back to main render target)
    m_renderer->endComposeTarget();
}

void CompositorSystem::shutdown(entt::registry& registry) {
    std::cout << "CompositorSystem shutdown" << std::endl;
}

bool CompositorSystem::isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const {
    return frame >= clip.startFrame && frame < (clip.startFrame + clip.duration);
}

void CompositorSystem::renderMappingSurfaces(entt::registry& registry, D3D12_GPU_DESCRIPTOR_HANDLE textureSrv) {
    if (!m_renderer || !m_renderer->isInitialized() || textureSrv.ptr == 0) {
        return;
    }

    // Query all mapping surfaces
    auto view = registry.view<MappingSurface>();

    // Collect visible surfaces sorted by index
    std::vector<std::pair<entt::entity, uint32_t>> sortedSurfaces;

    for (auto [entity, surface] : view.each()) {
        if (surface.visible) {
            sortedSurfaces.push_back({entity, surface.surfaceIndex});
        }
    }

    // Sort by surface index (lower indices render first)
    std::sort(sortedSurfaces.begin(), sortedSurfaces.end(),
        [](const auto& a, const auto& b) {
            return a.second < b.second;
        });

    if (m_debugLogging && !sortedSurfaces.empty()) {
        static int frameCount = 0;
        if (frameCount++ % 60 == 0) {
            std::cout << "[CompositorSystem] Rendering through " << sortedSurfaces.size()
                      << " mapping surfaces" << std::endl;
        }
    }

    // Render texture through each visible mapping surface
    for (const auto& [entity, index] : sortedSurfaces) {
        auto& surface = registry.get<MappingSurface>(entity);

        // Convert corner positions from glm to DirectX
        DirectX::XMFLOAT2 corners[4];
        DirectX::XMFLOAT2 sourceUVs[4];

        for (int i = 0; i < 4; ++i) {
            corners[i] = DirectX::XMFLOAT2(surface.corners[i].x, surface.corners[i].y);
            sourceUVs[i] = DirectX::XMFLOAT2(surface.sourceUVs[i].x, surface.sourceUVs[i].y);
        }

        // Pack soft edge values (left, right, top, bottom)
        DirectX::XMFLOAT4 softEdges(
            surface.softEdge.left,
            surface.softEdge.right,
            surface.softEdge.top,
            surface.softEdge.bottom
        );

        // Render through this surface
        m_renderer->drawMappingSurface(
            textureSrv,
            corners,
            sourceUVs,
            softEdges,
            surface.brightness,
            surface.gamma,
            1.0f  // Per-surface opacity could be added later
        );
    }
}

} // namespace entity
