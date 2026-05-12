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
    (void)registry;  // Show thread: registry untouched (ADR-0014).
    (void)deltaTime;

    // GC: drop pending-allocation cache entries for screens / generatives
    // that have left the active set. releaseComposeTarget isn't on
    // IRenderer, so leaked slots are bounded by MAX_COMPOSE_TARGETS; both
    // archetypes destroy rarely.
    for (auto it = m_pendingAllocations.begin(); it != m_pendingAllocations.end(); ) {
        const std::uint64_t id = static_cast<std::uint64_t>(it->first);
        const bool found = std::any_of(rf.screens.begin(), rf.screens.end(),
            [id](const bus::ScreenSnapshot& s) { return s.entity == id; });
        it = found ? std::next(it) : m_pendingAllocations.erase(it);
    }
    for (auto it = m_pendingGenerativeAllocations.begin();
         it != m_pendingGenerativeAllocations.end(); )
    {
        const std::uint64_t id = static_cast<std::uint64_t>(it->first);
        const bool found = std::any_of(rf.generativeLayers.begin(), rf.generativeLayers.end(),
            [id](const bus::GenerativeLayerSnapshot& g) { return g.entity == id; });
        it = found ? std::next(it) : m_pendingGenerativeAllocations.erase(it);
    }

    // ------------------------------------------------------------------
    // PASS 1 — Render each active generative layer's procedural content
    // into its own compose target (allocated lazily). After this pass
    // every generative layer has a usable TextureRef::compose(slot) the
    // PASS 2 unified composite loop can blit onto a screen.
    //
    // ECS contract: kind dispatch by composition (the presence of
    // MunchersGameState on the snapshot's wire-side `Kind` enum is the
    // discriminator). Adding a new generative kind = new case here.
    // ------------------------------------------------------------------
    {
        ZoneScopedN("Compositor::pass1_generative");
        for (const auto& gl : rf.generativeLayers) {
            if (gl.opacity <= 0.0f) continue;

            const std::uint32_t slot = ensureGenerativeRenderTarget(gl);
            if (slot == UINT32_MAX) continue;

            m_renderer->beginComposeTarget(slot);
            // Caller draws into the layer's RT in layer-local NDC; the
            // UV-space transform travels separately on the matching
            // ContentLayerSnapshot and is applied in PASS 2.
            switch (gl.kind) {
                case bus::GenerativeLayerSnapshot::Kind::Muncher:
                    drawMuncherPlayfield(gl, 1.0f);
                    break;
            }
            m_renderer->endComposeTarget();
        }
    }

    // ------------------------------------------------------------------
    // PASS 2 — For each visible screen, composite every content layer
    // targeting it. One uniform draw path for every kind: bind the
    // layer's texture (Video slot for clips, Compose slot for
    // generatives), apply its UV-space transform, apply opacity ×
    // section-fade, submit via drawTexturedQuad. No kind dispatch in the
    // per-entry loop — that's the whole point of the unification.
    // ------------------------------------------------------------------
    {
        ZoneScopedN("Compositor::pass2_screens");
        for (const auto& screenSnap : rf.screens) {
            if (!screenSnap.visible) continue;

            const std::uint32_t screenSlot = ensureScreenRenderTarget(screenSnap);
            if (screenSlot == UINT32_MAX) continue;

            const std::uint64_t screenId = screenSnap.entity;
            m_renderer->beginComposeTarget(screenSlot);

            // rf.contentLayers is pre-sorted by zOrder ascending in
            // PlaybackTimeAuthority::buildRenderFrame.
            for (const auto& cl : rf.contentLayers) {
                if (cl.targetScreen != UINT64_MAX && cl.targetScreen != screenId) continue;

                const float drawOpacity = cl.opacity * cl.sectionFadeMultiplier;
                if (drawOpacity <= 0.0f) continue;

                glm::mat4 transformMatrix;
                std::memcpy(glm::value_ptr(transformMatrix),
                            cl.transformMatrix.data(),
                            sizeof(float) * 16);

                // Bind the right texture pool by source kind. Bad / unready
                // slots produce TextureRef::invalid() → drawTexturedQuad
                // drops the call silently. Fall back to a tinted colored
                // quad only for the Video kind (clip-decode underrun feedback);
                // an unallocated Compose slot just means PASS 1 hasn't
                // produced output yet for that layer, so we skip rather
                // than flash a debug color over the screen.
                TextureRef tex = TextureRef::invalid();
                TextureColorSpace colorSpace = TextureColorSpace::Linear;
                std::string ocioColorSpace;
                if (cl.sourceSlot >= 0) {
                    switch (cl.sourceKind) {
                        case bus::ContentLayerSnapshot::SourceKind::Video:
                            tex = m_renderer->getVideoTexture(
                                static_cast<uint32_t>(cl.sourceSlot));
                            if (m_playbackPresenter) {
                                const auto& d = m_playbackPresenter->displayState(
                                    static_cast<entt::entity>(cl.entity));
                                colorSpace     = d.colorSpace;
                                ocioColorSpace = d.ocioColorSpace;
                            }
                            break;
                        case bus::ContentLayerSnapshot::SourceKind::Compose:
                            tex = m_renderer->getComposeTargetTexture(
                                static_cast<uint32_t>(cl.sourceSlot));
                            // Generative RT is written in linear-light, no OCIO.
                            break;
                    }
                }

                if (tex.valid()) {
                    m_renderer->drawTexturedQuad(tex, transformMatrix, drawOpacity,
                                                 static_cast<BlendMode>(cl.blendMode),
                                                 colorSpace, ocioColorSpace);
                    continue;
                }

                if (cl.sourceKind == bus::ContentLayerSnapshot::SourceKind::Video) {
                    // Fallback colored quad for video slots not yet uploaded —
                    // gives the user *something* to see while the decoder warms.
                    const uint32_t entityId = static_cast<uint32_t>(cl.entity);
                    glm::vec4 color(
                        ((entityId * 137) % 256) / 255.0f,
                        ((entityId * 211) % 256) / 255.0f,
                        ((entityId * 97)  % 256) / 255.0f,
                        1.0f
                    );
                    m_renderer->drawColoredQuad(transformMatrix, color, drawOpacity);
                }
                // Compose-source with no texture = PASS 1 hasn't rendered
                // this frame yet (RT allocation pending the R2D ack). Skip.
            }

            m_renderer->endComposeTarget();
        }
    }
}

void CompositorSystem::drawMuncherPlayfield(const bus::GenerativeLayerSnapshot& gl,
                                             float drawOpacity) {
    // Draws in LAYER-LOCAL NDC into the generative layer's own compose
    // target (allocated in PASS 1). The layer's UV-space Transform is
    // applied in PASS 2 by drawTexturedQuad when this RT is blitted onto
    // the target screen — this function does not see the layer transform.

    // Helper: build a translate+scale matrix for a quad at normalized
    // image-coords (x, y) ∈ [0, 1] (origin top-left, Y-down) with NDC
    // half-extent `halfSize`. Converts to NDC (-1..1, Y-up). Unit quad is
    // 2 NDC units wide, so the resulting quad covers `2 * halfSize`
    // fraction of the layer's render target.
    auto quadXf = [](float x, float y, float halfSize) {
        const float ndcX = x * 2.0f - 1.0f;
        const float ndcY = 1.0f - y * 2.0f;
        return glm::translate(glm::mat4(1.0f), glm::vec3(ndcX, ndcY, 0.0f)) *
               glm::scale(glm::mat4(1.0f), glm::vec3(halfSize, halfSize, 1.0f));
    };

    // 1) Playfield background fills the entire layer RT.
    const glm::vec4 bgColor(0.05f, 0.05f, 0.09f, 1.0f);
    m_renderer->drawColoredQuad(glm::mat4(1.0f), bgColor, drawOpacity);

    // 2) Maze walls — dark blue quads on every wall cell. Drawn before
    //    pellets / actors so the corridors visually "carve out" the maze.
    constexpr int   kGrid       = 16;
    constexpr float kCellSize   = 1.0f / static_cast<float>(kGrid);
    constexpr float kWallHalf   = kCellSize * 0.5f;
    const glm::vec4 wallColor(0.18f, 0.22f, 0.55f, 1.0f);
    for (int cy = 0; cy < kGrid; ++cy) {
        for (int cx = 0; cx < kGrid; ++cx) {
            const int idx = cy * kGrid + cx;
            if ((gl.muncher_wallBits[idx / 64] >> (idx % 64)) & 1ull) {
                const float px = (static_cast<float>(cx) + 0.5f) * kCellSize;
                const float py = (static_cast<float>(cy) + 0.5f) * kCellSize;
                m_renderer->drawColoredQuad(quadXf(px, py, kWallHalf),
                                             wallColor, drawOpacity);
            }
        }
    }

    // 3) Pellets — tiny pale quads on walkable cells with bits set.
    constexpr float kPelletHalf = 0.006f;
    const glm::vec4 pelletColor(0.95f, 0.95f, 0.80f, 1.0f);
    for (int cy = 0; cy < kGrid; ++cy) {
        for (int cx = 0; cx < kGrid; ++cx) {
            const int idx = cy * kGrid + cx;
            if ((gl.muncher_pelletBits[idx / 64] >> (idx % 64)) & 1ull) {
                const float px = (static_cast<float>(cx) + 0.5f) * kCellSize;
                const float py = (static_cast<float>(cy) + 0.5f) * kCellSize;
                m_renderer->drawColoredQuad(quadXf(px, py, kPelletHalf),
                                             pelletColor, drawOpacity);
            }
        }
    }

    // 4) Ghosts — three colored quads. Sized to fit inside a cell so
    //    the wall collision in GenerativeSystem (kGhostHalfPlay) matches
    //    the visual.
    constexpr float kGhostHalf = 0.020f;
    constexpr glm::vec4 ghostPalette[3] = {
        glm::vec4(0.95f, 0.35f, 0.40f, 1.0f),  // crimson
        glm::vec4(0.35f, 0.85f, 0.95f, 1.0f),  // cyan
        glm::vec4(0.95f, 0.75f, 0.95f, 1.0f),  // pink
    };
    for (int gi = 0; gi < 3; ++gi) {
        const float gx = gl.muncher_ghosts[gi * 2 + 0];
        const float gy = gl.muncher_ghosts[gi * 2 + 1];
        m_renderer->drawColoredQuad(quadXf(gx, gy, kGhostHalf),
                                     ghostPalette[gi], drawOpacity);
    }

    // 5) Muncher. Yellow square with a chomp brightness pulse. Half-size
    //    matches kMuncherHalfPlay in GenerativeSystem so wall collision
    //    aligns with the visual hitbox.
    constexpr float kMuncherHalf = 0.022f;
    const float chomp = 0.85f + 0.15f *
        std::sin(static_cast<float>(gl.muncher_simFrame) * 0.20f);
    const glm::vec4 muncherColor(chomp, chomp * 0.85f, 0.10f, 1.0f);
    m_renderer->drawColoredQuad(quadXf(gl.muncher_x, gl.muncher_y, kMuncherHalf),
                                 muncherColor, drawOpacity);

    // 6) Lives HUD — N tiny yellow squares along the top edge. Reads
    //    1.0=outside-top so they sit above the maze on a 16×16 grid
    //    that uses [0, 1] for the maze interior. Render at y=0.015 so
    //    they hug the top edge inside the compose target.
    constexpr float kLifeHalf = 0.010f;
    const glm::vec4 lifeColor(0.95f, 0.85f, 0.20f, 1.0f);
    const int liveCount = static_cast<int>(gl.muncher_lives);
    for (int i = 0; i < liveCount; ++i) {
        const float lx = 0.03f + 0.025f * static_cast<float>(i);
        const float ly = 0.015f;
        m_renderer->drawColoredQuad(quadXf(lx, ly, kLifeHalf),
                                     lifeColor, drawOpacity);
    }
}


void CompositorSystem::shutdown(entt::registry& registry) {
    std::cout << "CompositorSystem shutdown" << std::endl;
}

std::uint32_t CompositorSystem::ensureGenerativeRenderTarget(
    const bus::GenerativeLayerSnapshot& gl)
{
    if (!m_renderer) return UINT32_MAX;

    const auto entity = static_cast<entt::entity>(gl.entity);
    const uint32_t width  = 1920;  // GenerativeLayer default — kind-specific
    const uint32_t height = 1080;  // override could plumb through later
    // (renderWidth/renderHeight live on the editor-side component; not yet
    // mirrored on the snapshot since the playfield draw is fixed-grid).

    // 1. Editor has acknowledged a slot — reuse, resize in place if dims drifted.
    if (gl.renderTargetSlot >= 0) {
        m_pendingGenerativeAllocations.erase(entity);
        const std::uint32_t slot = static_cast<std::uint32_t>(gl.renderTargetSlot);
        const uint32_t curW = m_renderer->getComposeTargetWidth(slot);
        const uint32_t curH = m_renderer->getComposeTargetHeight(slot);
        if (curW != width || curH != height) {
            m_renderer->resizeComposeTarget(slot, width, height);
        }
        return slot;
    }

    // 2. Pending allocation — reuse cached slot to avoid double-allocating.
    if (auto it = m_pendingGenerativeAllocations.find(entity);
        it != m_pendingGenerativeAllocations.end())
    {
        auto& cached = it->second;
        if (cached.width != width || cached.height != height) {
            if (m_renderer->resizeComposeTarget(cached.slot, width, height)) {
                cached.width  = width;
                cached.height = height;
            }
        }
        return cached.slot;
    }

    // 3. Genuinely new — allocate, cache, post R2D ack so editor writes back.
    const std::uint32_t slot = m_renderer->createComposeTarget(width, height);
    if (slot == UINT32_MAX) {
        std::cerr << "[Compositor] Failed to create render target for generative layer "
                  << gl.entity << std::endl;
        return UINT32_MAX;
    }

    std::cout << "[Compositor] Created render target slot " << slot
              << " for generative layer entity " << gl.entity
              << " (" << width << "x" << height << ")" << std::endl;

    m_pendingGenerativeAllocations[entity] = { slot, width, height };

    if (m_transport) {
        bus::GenerativeLayerRenderTargetAllocated reply{};
        reply.entity = gl.entity;
        reply.slot   = slot;
        reply.width  = width;
        reply.height = height;
        m_transport->send(bus::Direction::R2D,
                          bus::serialize(bus::Message{reply}));
    }

    return slot;
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
