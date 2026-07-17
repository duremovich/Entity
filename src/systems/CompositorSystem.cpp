#include "entity/systems/CompositorSystem.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/renderer/PlaybackPresenter.hpp"
#include "entity/components/OutputSurface.hpp"
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

void CompositorSystem::update(bus::RenderFrame& rf,
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
    for (auto it = m_pendingEffectAllocations.begin();
         it != m_pendingEffectAllocations.end(); )
    {
        const std::uint64_t id = static_cast<std::uint64_t>(it->first);
        const bool found = std::any_of(rf.contentLayers.begin(), rf.contentLayers.end(),
            [id](const bus::ContentLayerSnapshot& cl) {
                return cl.entity == id && !cl.effects.empty();
            });
        it = found ? std::next(it) : m_pendingEffectAllocations.erase(it);
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
                case bus::GenerativeLayerSnapshot::Kind::Text:
                    drawTextLayer(gl, 1.0f);
                    break;
                case bus::GenerativeLayerSnapshot::Kind::Solid:
                    // Flat fill: one fullscreen quad in the layer's own RT.
                    // Layer opacity/blend apply in PASS 2 like any layer.
                    m_renderer->drawColoredQuad(
                        glm::mat4(1.0f),
                        glm::vec4(gl.solidColor[0], gl.solidColor[1],
                                  gl.solidColor[2], gl.solidColor[3]),
                        1.0f);
                    break;
            }
            m_renderer->endComposeTarget();
        }
    }

    // ------------------------------------------------------------------
    // PASS 1.5 — Per-layer effect chains. For each ContentLayerSnapshot
    // that carries non-empty `effects`, walk the chain via ping-pong
    // compose targets and produce a post-effects texture in
    // `ContentLayerSnapshot::postEffectsSlot`. PASS 2 prefers
    // postEffectsSlot over sourceSlot when set. Kind-blind: works
    // identically for Video and Compose source kinds. Issue #54.
    // ------------------------------------------------------------------
    {
        ZoneScopedN("Compositor::pass1_5_effects");

        for (auto& cl : rf.contentLayers) {
            if (cl.effects.empty()) continue;
            // Input-not-ready gate — but only when the first enabled effect
            // actually samples its input. A chain that opens with a
            // generator (inputCount == 0) seeds itself and can run with no
            // source texture at all.
            if (cl.sourceSlot < 0) {
                bool firstEnabledNeedsInput = false;
                for (const auto& fx : cl.effects) {
                    if (!fx.enabled) continue;
                    firstEnabledNeedsInput = (fx.inputCount > 0);
                    break;
                }
                if (firstEnabledNeedsInput) continue;
            }

            // Size effect RTs from the layer's source content dims (clip
            // media size / generative render size) so effects run in
            // content space before the UV transform — keeps blur radii and
            // pixelate cells texel-true. 0 = unknown → 1920x1080 fallback;
            // 4096 ceiling bounds compose-pool pressure.
            const std::uint32_t rtW =
                std::clamp(cl.sourceWidth  ? cl.sourceWidth  : 1920u, 16u, 4096u);
            const std::uint32_t rtH =
                std::clamp(cl.sourceHeight ? cl.sourceHeight : 1080u, 16u, 4096u);

            std::uint32_t slotA = UINT32_MAX, slotB = UINT32_MAX;
            if (!ensureEffectPingPongTargets(cl.entity,
                                              cl.effectChainSlotA,
                                              cl.effectChainSlotB,
                                              rtW, rtH,
                                              slotA, slotB)) {
                continue;  // R2D ack still in flight — try again next frame
            }

            // Resolve the chain's input as a TextureRef. The first effect
            // reads from the layer's raw source (Video or Compose); each
            // subsequent effect reads the previous output (always Compose).
            // No source (generator-led chain) → invalid ref; drawEffectPass
            // skips the SRV bind and the generator never samples it.
            TextureRef currentInput = TextureRef::invalid();
            if (cl.sourceSlot >= 0) {
                currentInput =
                    (cl.sourceKind == bus::ContentLayerSnapshot::SourceKind::Video)
                        ? TextureRef::video(static_cast<std::uint32_t>(cl.sourceSlot),
                                            cl.sourceGeneration)  // #90 draw guard
                        : TextureRef::compose(static_cast<std::uint32_t>(cl.sourceSlot));
            }

            std::uint32_t currentOutput = slotA;
            std::uint32_t nextOutput    = slotB;

            bool ran = false;
            for (const auto& fx : cl.effects) {
                if (!fx.enabled) continue;
                m_renderer->beginComposeTarget(currentOutput);
                m_renderer->drawEffectPass(currentInput, fx.kindIdHash,
                                            fx.paramBlob.data(),
                                            fx.paramBlob.size(),
                                            rtW, rtH);
                m_renderer->endComposeTarget();
                currentInput = TextureRef::compose(currentOutput);
                std::swap(currentOutput, nextOutput);
                ran = true;
            }

            // Publish the final output slot. The last write went into
            // `currentInput`'s slot (post-swap), so PASS 2 reads from
            // there. If no effect actually ran (all disabled), keep
            // postEffectsSlot at -1 so PASS 2 falls back to sourceSlot.
            if (ran) {
                cl.postEffectsSlot = static_cast<std::int32_t>(currentInput.slot);
            }
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
            //
            // Route lookup per ADR-0021 M3. `routes` is the canonical
            // destination list (Plane A). For Tiled mode a layer can match
            // a single screen via multiple routes — each its own UV crop —
            // so we collect all matches and emit one drawTexturedQuad per
            // match below. `targetScreen` (the deprecated single-field
            // alias) is honored as a one-route fallback for any payload
            // that predates ADR-0021's route list.
            for (const auto& cl : rf.contentLayers) {
                static constexpr bus::ContentLayerRoute kAliasRoute{
                    UINT64_MAX, {0.0f, 0.0f, 1.0f, 1.0f}};
                std::array<const bus::ContentLayerRoute*, 8> matchedRoutes{};
                size_t matchedCount = 0;
                if (!cl.routes.empty()) {
                    for (const auto& r : cl.routes) {
                        if (r.screen == UINT64_MAX || r.screen == screenId) {
                            if (matchedCount < matchedRoutes.size()) {
                                matchedRoutes[matchedCount++] = &r;
                            }
                        }
                    }
                } else if (cl.targetScreen == UINT64_MAX || cl.targetScreen == screenId) {
                    matchedRoutes[matchedCount++] = &kAliasRoute;
                }
                if (matchedCount == 0) continue;

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
                // If PASS 1.5 produced a post-effects RT, PASS 2 reads
                // from it (always Compose, linear). Otherwise fall back to
                // the layer's source slot resolved by sourceKind.
                if (cl.postEffectsSlot >= 0) {
                    tex = m_renderer->getComposeTargetTexture(
                        static_cast<uint32_t>(cl.postEffectsSlot));
                } else if (cl.sourceSlot >= 0) {
                    switch (cl.sourceKind) {
                        case bus::ContentLayerSnapshot::SourceKind::Video:
                            tex = m_renderer->getVideoTexture(
                                static_cast<uint32_t>(cl.sourceSlot));
                            // #90 F1: getVideoTexture returns a skip-generation
                            // ref; stamp the snapshot's captured generation so
                            // resolveTextureHandle rejects the draw if the slot
                            // was freed + reassigned since the bake (the common
                            // no-effects clip path — without this, only the
                            // effect-chain input at PASS 1.5 was guarded).
                            tex.generation = cl.sourceGeneration;
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
                    // ADR-0021 M3: emit one draw per matched route. The
                    // texture, transform, opacity, blend, and OCIO are
                    // layer-wide; only `uvRect` differs per route. For
                    // Direct mode there's exactly one matched route with
                    // identity uvRect — pixel-identical to pre-M3 behavior.
                    // For Tiled, each route's uvRect crops the source to a
                    // different sub-region of the same screen.
                    for (size_t mi = 0; mi < matchedCount; ++mi) {
                        const auto* r = matchedRoutes[mi];
                        const glm::vec4 uvRect{r->uvRect[0], r->uvRect[1],
                                                r->uvRect[2], r->uvRect[3]};
                        m_renderer->drawTexturedQuad(tex, transformMatrix, drawOpacity,
                                                     static_cast<BlendMode>(cl.blendMode),
                                                     colorSpace, ocioColorSpace, uvRect);
                    }
                    continue;
                }

                if (cl.sourceKind == bus::ContentLayerSnapshot::SourceKind::Video) {
                    // Fallback colored quad for video slots not yet uploaded —
                    // gives the user *something* to see while the decoder warms.
                    // Fallback ignores uvRect — it's a debug-color placeholder.
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
    // Size from the layer's authored render-target dims (mirrored onto the
    // snapshot from GenerativeLayer::renderWidth/Height). Clamp: 16 floor
    // guards degenerate authored values, 4096 ceiling bounds pool pressure.
    const uint32_t width  = std::clamp(gl.renderWidth,  16u, 4096u);
    const uint32_t height = std::clamp(gl.renderHeight, 16u, 4096u);

    // 1. Editor has acknowledged a slot — reuse, resize in place if dims
    //    drifted. Called unconditionally every tick (#75): the same-size
    //    early-out is two compares, and the call is what cancels an
    //    abandoned deferral gate and rebuilds a target whose previous
    //    resize failed mid-swap. A false return means deferred/failed —
    //    render at the old dims this tick and retry next tick.
    if (gl.renderTargetSlot >= 0) {
        m_pendingGenerativeAllocations.erase(entity);
        const std::uint32_t slot = static_cast<std::uint32_t>(gl.renderTargetSlot);
        m_renderer->resizeComposeTarget(slot, width, height);
        return slot;
    }

    // 2. Pending allocation — reuse cached slot to avoid double-allocating.
    //    Unconditional call for the same #75 reasons as path 1.
    if (auto it = m_pendingGenerativeAllocations.find(entity);
        it != m_pendingGenerativeAllocations.end())
    {
        auto& cached = it->second;
        if (m_renderer->resizeComposeTarget(cached.slot, width, height)) {
            cached.width  = width;
            cached.height = height;
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

bool CompositorSystem::ensureEffectPingPongTargets(std::uint64_t layerEntity,
                                                    std::int32_t snapshotSlotA,
                                                    std::int32_t snapshotSlotB,
                                                    std::uint32_t width,
                                                    std::uint32_t height,
                                                    std::uint32_t& outSlotA,
                                                    std::uint32_t& outSlotB) {
    if (!m_renderer) return false;
    const auto ent = static_cast<entt::entity>(layerEntity);

    // 1) Editor has both slots — reuse / resize in place. Unconditional
    //    calls every tick (#75): the same-size early-out is cheap and the
    //    call self-heals abandoned deferral gates / failed rebuilds. Unlike
    //    the screen path, a deferred or failed resize here returns false —
    //    the effect-pass UV/viewport math assumes the requested dims, so
    //    running it against stale-size RTs would mis-scale live output.
    //    The caller skips the effect chain for that tick and retries.
    if (snapshotSlotA >= 0 && snapshotSlotB >= 0) {
        m_pendingEffectAllocations.erase(ent);
        outSlotA = static_cast<std::uint32_t>(snapshotSlotA);
        outSlotB = static_cast<std::uint32_t>(snapshotSlotB);
        const bool okA = m_renderer->resizeComposeTarget(outSlotA, width, height);
        const bool okB = m_renderer->resizeComposeTarget(outSlotB, width, height);
        return okA && okB;
    }

    auto& cached = m_pendingEffectAllocations[ent];
    cached.width  = width;
    cached.height = height;

    auto allocSide = [&](std::uint32_t& slot, std::uint8_t side) -> bool {
        if (slot != UINT32_MAX) return true;
        const std::uint32_t newSlot = m_renderer->createComposeTarget(width, height);
        if (newSlot == UINT32_MAX) {
            std::cerr << "[Compositor] Effect ping-pong RT alloc failed for layer "
                      << layerEntity << " side " << int(side) << std::endl;
            return false;
        }
        slot = newSlot;
        if (m_transport) {
            bus::EffectChainRenderTargetAllocated reply{};
            reply.entity = layerEntity;
            reply.slot   = newSlot;
            reply.side   = side;
            reply.width  = width;
            reply.height = height;
            m_transport->send(bus::Direction::R2D,
                              bus::serialize(bus::Message{reply}));
        }
        return true;
    };

    // Mirror any side the editor has already acked into the cache so we
    // don't reallocate it; the other side keeps going until the editor
    // catches up.
    if (snapshotSlotA >= 0) cached.slotA = static_cast<std::uint32_t>(snapshotSlotA);
    if (snapshotSlotB >= 0) cached.slotB = static_cast<std::uint32_t>(snapshotSlotB);

    if (!allocSide(cached.slotA, 0)) return false;
    if (!allocSide(cached.slotB, 1)) return false;

    outSlotA = cached.slotA;
    outSlotB = cached.slotB;
    return true;
}

std::uint32_t CompositorSystem::ensureScreenRenderTarget(const bus::ScreenSnapshot& screenSnap) {
    if (!m_renderer) return UINT32_MAX;

    const auto entity = static_cast<entt::entity>(screenSnap.entity);

    // 1. Snapshot has caught up — drop the pending-allocation cache entry and
    //    use the confirmed slot. resizeComposeTarget is called unconditionally
    //    every tick (#75): the same-size early-out is two compares, and the
    //    call is what cancels an abandoned deferral gate and rebuilds a
    //    target whose previous resize failed mid-swap. On false (deferred or
    //    failed) we render at the old dims this tick and retry next tick.
    if (screenSnap.renderTargetValid && screenSnap.renderTargetSlot != UINT32_MAX) {
        m_pendingAllocations.erase(entity);
        const std::uint32_t slot = screenSnap.renderTargetSlot;
        m_renderer->resizeComposeTarget(slot, screenSnap.width, screenSnap.height);
        return slot;
    }

    // 2. Snapshot hasn't acknowledged our allocation yet — reuse the cached slot
    //    so we don't double-allocate. Unconditional call for the same #75
    //    reasons as path 1; the cache updates only on success so a deferral
    //    keeps being retried.
    if (auto it = m_pendingAllocations.find(entity); it != m_pendingAllocations.end()) {
        auto& cached = it->second;
        if (m_renderer->resizeComposeTarget(cached.slot, screenSnap.width, screenSnap.height)) {
            cached.width  = screenSnap.width;
            cached.height = screenSnap.height;
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

// renderOutputSurfaces was removed in issue #74: zero callers, and it read
// OutputSurface registry views from show-side code (ADR-0014 violation).

void CompositorSystem::drawTextLayer(const bus::GenerativeLayerSnapshot& gl,
                                     float drawOpacity) {
    // Text has already been rasterized into a video-pool slot by TextSystem
    // on the editor thread. We just blit it into the active compose target.
    if (gl.textTextureSlot < 0) return;
    // #90 F2: carry the captured generation so resolveTextureHandle rejects the
    // draw if this text slot was freed + reassigned to a clip since the bake.
    const auto ref = TextureRef::video(static_cast<std::uint32_t>(gl.textTextureSlot),
                                       gl.textTextureGeneration);
    m_renderer->drawTexturedQuad(ref, glm::mat4(1.0f), drawOpacity,
                                  BlendMode::Normal, TextureColorSpace::Linear);
}

} // namespace entity
