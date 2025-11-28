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

    // Get current timeline frame for visibility checks
    FrameNumber currentFrame = 0;
    if (m_timeline) {
        currentFrame = m_timeline->getCurrentFrame();
    }

    // Get the view for Transform+MediaLayer entities (used for all screens)
    auto view = registry.view<Transform, MediaLayer>();

    // Iterate ALL visible screens (each gets its own compose target)
    auto screenView = registry.view<Screen>();
    for (auto [screenEntity, screen] : screenView.each()) {
        if (!screen.visible) continue;

        // Ensure this screen has a valid render target
        if (!ensureScreenRenderTarget(registry, screenEntity)) {
            continue;
        }

        // Collect clips for THIS screen
        std::vector<entt::entity> sortedEntities;
        sortedEntities.reserve(view.size_hint());

        for (auto entity : view) {
            auto& layer = view.get<MediaLayer>(entity);
            if (!layer.visible) continue;

            // Check if this entity has a Clip component - if so, verify it's active
            auto* clip = registry.try_get<Clip>(entity);
            if (clip) {
                if (!isClipActiveAtFrame(*clip, currentFrame)) {
                    continue;  // Clip is not active at current frame, skip
                }

                // Filter by target screen:
                // - Include if clip targets ALL screens (entt::null)
                // - Include if clip targets THIS specific screen
                if (clip->targetScreen != entt::null && clip->targetScreen != screenEntity) {
                    continue;  // Clip targets a different screen
                }
            }

            sortedEntities.push_back(entity);
        }

        // Sort by z-order (lower values render first, higher values on top)
        std::sort(sortedEntities.begin(), sortedEntities.end(),
            [&view](entt::entity a, entt::entity b) {
                const auto& layerA = view.get<MediaLayer>(a);
                const auto& layerB = view.get<MediaLayer>(b);
                return layerA.zOrder < layerB.zOrder;
            });

        // Begin rendering to THIS screen's compose target
        m_renderer->beginComposeTarget(screen.renderTargetSlot);

        // Render each visible layer to the compose target
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
                m_renderer->drawColoredQuad(transformMatrix, color, layer.opacity);
            }
        }

        // End rendering to this screen's compose target
        m_renderer->endComposeTarget();
    }
}

void CompositorSystem::shutdown(entt::registry& registry) {
    std::cout << "CompositorSystem shutdown" << std::endl;
}

bool CompositorSystem::isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const {
    return frame >= clip.startFrame && frame < (clip.startFrame + clip.duration);
}

bool CompositorSystem::ensureScreenRenderTarget(entt::registry& registry, entt::entity screenEntity) {
    auto* screen = registry.try_get<Screen>(screenEntity);
    if (!screen) return false;

    // Check if render target needs creation or recreation
    bool needsCreate = !screen->renderTargetValid ||
                       screen->renderTargetSlot == UINT32_MAX;

    // Check if dimensions changed (stored in ComposeTarget vs Screen component)
    if (screen->renderTargetValid && screen->renderTargetSlot != UINT32_MAX) {
        uint32_t currentWidth = m_renderer->getComposeTargetWidth(screen->renderTargetSlot);
        uint32_t currentHeight = m_renderer->getComposeTargetHeight(screen->renderTargetSlot);
        if (currentWidth != screen->width || currentHeight != screen->height) {
            // Dimensions changed, need new target
            // Note: Old target slot becomes a "gap" - acceptable for typical screen counts
            needsCreate = true;
            screen->renderTargetValid = false;
        }
    }

    if (needsCreate) {
        uint32_t slot = m_renderer->createComposeTarget(screen->width, screen->height);
        if (slot == UINT32_MAX) {
            std::cerr << "[Compositor] Failed to create render target for screen: "
                      << screen->name << std::endl;
            return false;
        }
        screen->renderTargetSlot = slot;
        screen->renderTargetValid = true;
        std::cout << "[Compositor] Created render target slot " << slot
                  << " for screen: " << screen->name
                  << " (" << screen->width << "x" << screen->height << ")" << std::endl;
    }

    return true;
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
