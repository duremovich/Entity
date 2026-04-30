/**
 * CompositorSystem Implementation
 *
 * Renders visible layers by querying entities with Transform and MediaLayer,
 * sorting by z-order, and issuing draw calls through IRenderer.
 *
 * Timeline-aware: Only renders clips that are active at the current frame.
 * For entities with VideoTexture components, renders video frames.
 * Falls back to colored quads for entities without video textures.
 */

#include "entity/systems/CompositorSystem.hpp"
#include "entity/render/IRenderer.hpp"
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

namespace entity {

CompositorSystem::CompositorSystem(IRenderer* renderer)
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

            // IRenderer takes glm::mat4 directly — no DirectX conversion needed here
            const glm::mat4& transformMatrix = transform.getMatrix();

            // Check if entity has a valid video texture
            auto* videoTex = registry.try_get<VideoTexture>(entity);

            if (videoTex && videoTex->isValid()) {
                // Draw textured quad for video layers with blend mode + colour-space hint
                // (HAP Q content needs in-shader YCoCg→RGB; everything else is Linear).
                TextureRef tex = m_renderer->getVideoTexture(videoTex->descriptorSlot);
                if (tex.valid()) {
                    m_renderer->drawTexturedQuad(tex, transformMatrix, layer.opacity,
                                                 layer.blendMode, videoTex->colorSpace,
                                                 videoTex->ocioColorSpace);
                    continue;
                }
                // Fall through to colored-quad fallback if the slot isn't ready yet.
            }

            // Fallback to colored quad for non-video layers
            uint32_t entityId = static_cast<uint32_t>(entity);
            glm::vec4 color(
                ((entityId * 137) % 256) / 255.0f,
                ((entityId * 211) % 256) / 255.0f,
                ((entityId * 97)  % 256) / 255.0f,
                1.0f
            );
            m_renderer->drawColoredQuad(transformMatrix, color, layer.opacity);
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

    // If we already own a slot but the dimensions drifted, resize in
    // place — DON'T allocate a new slot. Allocating fresh slots on
    // every resize leaks descriptor heap entries and after
    // MAX_COMPOSE_TARGETS resizes corrupts the heap (#31).
    if (screen->renderTargetValid && screen->renderTargetSlot != UINT32_MAX) {
        uint32_t currentWidth  = m_renderer->getComposeTargetWidth(screen->renderTargetSlot);
        uint32_t currentHeight = m_renderer->getComposeTargetHeight(screen->renderTargetSlot);
        if (currentWidth != screen->width || currentHeight != screen->height) {
            if (m_renderer->resizeComposeTarget(screen->renderTargetSlot,
                                                 screen->width, screen->height)) {
                return true;  // slot ID and descriptor heap entries unchanged
            }
            // Resize failed (e.g. OOM). Fall back to a fresh allocation
            // attempt — better than rendering to a stale-sized target.
            screen->renderTargetValid = false;
            needsCreate = true;
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

void CompositorSystem::renderMappingSurfaces(entt::registry& registry, TextureRef texture) {
    if (!m_renderer || !m_renderer->isInitialized() || !texture.valid()) {
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

    // Render texture through each visible mapping surface (glm types all the way)
    for (const auto& [entity, index] : sortedSurfaces) {
        auto& surface = registry.get<MappingSurface>(entity);

        const glm::vec4 softEdges(
            surface.softEdge.left,
            surface.softEdge.right,
            surface.softEdge.top,
            surface.softEdge.bottom
        );

        m_renderer->drawMappingSurface(
            texture,
            surface.corners.data(),
            surface.sourceUVs.data(),
            softEdges,
            surface.brightness,
            surface.gamma,
            1.0f  // Per-surface opacity could be added later
        );
    }
}

} // namespace entity
