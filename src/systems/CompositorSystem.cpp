#include "entity/systems/CompositorSystem.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/renderer/PlaybackPresenter.hpp"
#include "entity/components/MappingSurface.hpp"
#include "entity/components/Screen.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include <iostream>

namespace entity {

CompositorSystem::CompositorSystem(IRenderer* renderer, bus::IMessageTransport* transport)
    : m_transport(transport)
    , m_renderer(renderer)
{
}

void CompositorSystem::initialize(entt::registry& registry) {
    std::cout << "CompositorSystem initialized" << std::endl;
}

void CompositorSystem::update(const bus::RenderFrame& rf,
                               entt::registry& registry,
                               float deltaTime) {
    ZoneScopedN("CompositorSystem::update");
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

    // GC: remove pending-allocation cache entries for screens that no longer
    // exist in the snapshot. releaseComposeTarget is not available on IRenderer,
    // so leaked slots are bounded by MAX_COMPOSE_TARGETS (8); screen destroy is rare.
    for (auto it = m_pendingAllocations.begin(); it != m_pendingAllocations.end(); ) {
        const std::uint64_t id = static_cast<std::uint64_t>(it->first);
        const bool found = std::any_of(rf.screens.begin(), rf.screens.end(),
            [id](const bus::ScreenSnapshot& s) { return s.entity == id; });
        it = found ? std::next(it) : m_pendingAllocations.erase(it);
    }

    // Iterate all visible screens from the RenderFrame snapshot. Screen
    // enumeration and slot lookup use the snapshot exclusively; registry is
    // never written here. If a slot hasn't been allocated yet, allocate it
    // now and post an R2D ScreenRenderTargetAllocated reply so the editor
    // thread writes the slot back into Screen for the next SceneSnapshot.
    for (const auto& screenSnap : rf.screens) {
        if (!screenSnap.visible) continue;

        const std::uint32_t slot = ensureScreenRenderTarget(screenSnap);
        if (slot == UINT32_MAX) continue;

        const std::uint64_t screenId = screenSnap.entity;

        m_renderer->beginComposeTarget(slot);

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

            // Gate on the snapshot slot (baked on editor thread into ClipCatalogEntry
            // then forwarded into ClipRenderState). Avoids reading descriptorSlot from
            // the registry component (data race: editor writes it in drainRendererToDirector).
            if (crs->slot >= 0) {
                TextureRef tex = m_renderer->getVideoTexture(
                    static_cast<uint32_t>(crs->slot));
                if (tex.valid()) {
                    // Per ADR-0014 the show thread must not read the registry —
                    // editor's `registry.destroy()` on clip delete races any
                    // try_get<VideoTexture>. PlaybackPresenter caches the
                    // colour-space tags show-thread-locally and exposes them
                    // via `displayState()`; on a miss the default
                    // (Linear / empty OCIO) reproduces the prior fallback.
                    TextureColorSpace colorSpace = TextureColorSpace::Linear;
                    std::string ocioColorSpace;
                    if (m_playbackPresenter) {
                        const auto& display = m_playbackPresenter->displayState(entity);
                        colorSpace     = display.colorSpace;
                        ocioColorSpace = display.ocioColorSpace;
                    }
                    m_renderer->drawTexturedQuad(tex, transformMatrix, drawOpacity,
                                                 crs->blendMode, colorSpace,
                                                 ocioColorSpace);
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

        // Generative layers (Muncher v1). The editor-side bake already
        // filtered to active timeline frames; here we only filter by
        // targetScreen and skip layers with no kind-specific data.
        //
        // V1 procedural draw: dim playfield background + a yellow square
        // for the Muncher at its (x, y) position. Real maze + ghosts +
        // pellets land once sprite-atlas rendering exists; for now every
        // game entity is a transformed colored quad through the existing
        // drawColoredQuad path (no new HLSL / PSO work).
        for (const auto& gl : rf.generativeLayers) {
            if (gl.targetScreen != UINT64_MAX && gl.targetScreen != screenId) continue;

            const float drawOpacity = gl.opacity;
            if (drawOpacity <= 0.0f) continue;

            drawMuncherPlayfield(gl, drawOpacity);
        }

        m_renderer->endComposeTarget();
    }
}

void CompositorSystem::drawMuncherPlayfield(const bus::GenerativeLayerSnapshot& gl,
                                             float drawOpacity) {
    // 1) Dim playfield background covering the whole compose target. Keeps
    //    the layer visible even when the Muncher is in a corner; later
    //    this becomes the maze tile draw.
    const glm::vec4 bgColor(0.06f, 0.06f, 0.10f, 1.0f);
    m_renderer->drawColoredQuad(glm::mat4(1.0f), bgColor, drawOpacity);

    // 2) Muncher: yellow square at (muncher_x, muncher_y). Convert from
    //    [0, 1] image-coords (origin top-left, Y-down) to NDC (-1..1,
    //    Y-up). The unit quad is 2 NDC units wide, so a `kHalfSize` scale
    //    yields a `2 * kHalfSize` × NDC-units quad — i.e. that fraction
    //    of the full screen size.
    constexpr float kHalfSize = 0.045f;  // ≈ 9% of screen per side
    const float ndcX = gl.muncher_x * 2.0f - 1.0f;
    const float ndcY = 1.0f - gl.muncher_y * 2.0f;
    const glm::mat4 muncherXf =
        glm::translate(glm::mat4(1.0f), glm::vec3(ndcX, ndcY, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(kHalfSize, kHalfSize, 1.0f));

    // Chomp pulse so the player reads as alive even when stationary —
    // simFrame % 30 cycles brightness between ~0.75 and ~1.0.
    const float chomp = 0.85f + 0.15f *
        std::sin(static_cast<float>(gl.muncher_simFrame) * 0.20f);
    const glm::vec4 muncherColor(chomp, chomp * 0.85f, 0.10f, 1.0f);
    m_renderer->drawColoredQuad(muncherXf, muncherColor, drawOpacity);
}


void CompositorSystem::shutdown(entt::registry& registry) {
    std::cout << "CompositorSystem shutdown" << std::endl;
}

std::uint32_t CompositorSystem::ensureScreenRenderTarget(const bus::ScreenSnapshot& screenSnap) {
    if (!m_renderer) return UINT32_MAX;

    const auto entity = static_cast<entt::entity>(screenSnap.entity);

    // 1. Snapshot has caught up — drop the pending-allocation cache entry and
    //    use the confirmed slot. Check for dimension drift in place.
    if (screenSnap.renderTargetValid && screenSnap.renderTargetSlot != UINT32_MAX) {
        m_pendingAllocations.erase(entity);
        const std::uint32_t slot = screenSnap.renderTargetSlot;
        const uint32_t currentWidth  = m_renderer->getComposeTargetWidth(slot);
        const uint32_t currentHeight = m_renderer->getComposeTargetHeight(slot);
        if (currentWidth != screenSnap.width || currentHeight != screenSnap.height) {
            m_renderer->resizeComposeTarget(slot, screenSnap.width, screenSnap.height);
            // If resize fails, render at old dims this tick. Editor retries on next snapshot.
        }
        return slot;
    }

    // 2. Snapshot hasn't acknowledged our allocation yet — reuse the cached slot
    //    so we don't double-allocate. Resize in place if dimensions drifted.
    if (auto it = m_pendingAllocations.find(entity); it != m_pendingAllocations.end()) {
        auto& cached = it->second;
        if (cached.width != screenSnap.width || cached.height != screenSnap.height) {
            if (m_renderer->resizeComposeTarget(cached.slot, screenSnap.width, screenSnap.height)) {
                cached.width  = screenSnap.width;
                cached.height = screenSnap.height;
            }
            // Resize failed: keep old slot at old dims until editor catches up.
        }
        return cached.slot;
    }

    // 3. Genuinely new entity — allocate, cache, and post the R2D reply.
    const std::uint32_t slot = m_renderer->createComposeTarget(screenSnap.width, screenSnap.height);
    if (slot == UINT32_MAX) {
        std::cerr << "[Compositor] Failed to create render target for screen entity "
                  << screenSnap.entity << std::endl;
        return UINT32_MAX;
    }

    std::cout << "[Compositor] Created render target slot " << slot
              << " for screen entity " << screenSnap.entity
              << " (" << screenSnap.width << "x" << screenSnap.height << ")" << std::endl;

    m_pendingAllocations[entity] = { slot, screenSnap.width, screenSnap.height };

    // Post R2D reply so the editor thread writes the slot back into Screen
    // for the next SceneSnapshot. No registry write on the show thread.
    if (m_transport) {
        bus::ScreenRenderTargetAllocated reply{};
        reply.entity = screenSnap.entity;
        reply.slot   = slot;
        reply.width  = screenSnap.width;
        reply.height = screenSnap.height;
        m_transport->send(bus::Direction::R2D,
                          bus::serialize(bus::Message{reply}));
    }

    return slot;
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
            1.0f
        );
    }
}

} // namespace entity
