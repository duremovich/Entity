#include "entity/systems/CompositorSystem.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/MappingSurface.hpp"
#include "entity/components/Screen.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
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

void CompositorSystem::update(const bus::RenderFrame& rf,
                               entt::registry& registry,
                               float deltaTime) {
    if (!m_renderer || !m_renderer->isInitialized()) {
        return;
    }

    // Sort the active-clip list by z-order once per frame (stable so ties
    // keep their arrival order from PlaybackTimeAuthority).
    std::vector<const bus::ClipRenderState*> sorted;
    sorted.reserve(rf.activeClips.size());
    for (const auto& crs : rf.activeClips) sorted.push_back(&crs);
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const bus::ClipRenderState* a, const bus::ClipRenderState* b) {
            return a->zOrder < b->zOrder;
        });

    // Iterate ALL visible screens (each gets its own compose target).
    auto screenView = registry.view<Screen>();
    for (auto [screenEntity, screen] : screenView.each()) {
        if (!screen.visible) continue;

        if (!ensureScreenRenderTarget(registry, screenEntity)) continue;

        const std::uint64_t screenId = static_cast<std::uint64_t>(screenEntity);

        m_renderer->beginComposeTarget(screen.renderTargetSlot);

        for (const auto* crs : sorted) {
            // Filter by target screen (UINT64_MAX = all screens).
            if (crs->targetScreen != UINT64_MAX && crs->targetScreen != screenId) {
                continue;
            }

            const entt::entity entity = static_cast<entt::entity>(crs->entity);

            // Section-fade × bus opacity — both sourced from the RenderFrame.
            // No registry read needed; fadeMul is already baked into
            // sectionFadeMultiplier by PlaybackTimeAuthority.
            const float drawOpacity = crs->opacity * crs->sectionFadeMultiplier;

            // [SBG] diag — REMOVE after section-break-glitch fix lands.
            std::cout << "[SBG][compose] entity=" << crs->entity
                      << " opacity=" << crs->opacity
                      << " sectionFadeMul=" << crs->sectionFadeMultiplier
                      << " drawOpacity=" << drawOpacity
                      << std::endl;

            // transformMatrix is column-major float[16] — map directly to glm.
            glm::mat4 transformMatrix;
            std::memcpy(glm::value_ptr(transformMatrix),
                        crs->transformMatrix.data(),
                        sizeof(float) * 16);

            // VideoTexture is Renderer-side state stamped by PlaybackPresenter;
            // read colorSpace / ocioColorSpace from the registry component.
            auto* videoTex = registry.try_get<VideoTexture>(entity);
            if (videoTex && videoTex->isValid()) {
                TextureRef tex = m_renderer->getVideoTexture(
                    static_cast<uint32_t>(crs->slot));
                if (tex.valid()) {
                    m_renderer->drawTexturedQuad(tex, transformMatrix, drawOpacity,
                                                 crs->blendMode, videoTex->colorSpace,
                                                 videoTex->ocioColorSpace);
                    continue;
                }
            }

            // Fallback colored quad for slots not yet uploaded.
            const uint32_t entityId = static_cast<uint32_t>(crs->entity);
            glm::vec4 color(
                ((entityId * 137) % 256) / 255.0f,
                ((entityId * 211) % 256) / 255.0f,
                ((entityId * 97)  % 256) / 255.0f,
                1.0f
            );
            m_renderer->drawColoredQuad(transformMatrix, color, drawOpacity);
        }

        m_renderer->endComposeTarget();
    }
}

void CompositorSystem::shutdown(entt::registry& registry) {
    std::cout << "CompositorSystem shutdown" << std::endl;
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
