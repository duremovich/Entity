#include "entity/systems/TextSystem.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/render/TextRasterizer.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/TextLayerState.hpp"
#include "entity/components/RemotePatch.hpp"
#include "entity/remote/RemoteControlStore.hpp"

#include <iostream>

namespace entity {

struct TextSystem::Impl {
    TextRasterizer rasterizer;
};

TextSystem::TextSystem(IRenderer* renderer)
    : m_renderer(renderer)
    , m_impl(new Impl{})
{}

void TextSystem::initialize(entt::registry& registry) {
    // Free the video-pool slot when a TextLayerState entity is destroyed
    // (delete, undo of paste, etc.). The observer fires on the editor thread
    // before the component is erased, so textureSlot is still valid here.
    // shutdown() intentionally does NOT free slots — the renderer is torn
    // down before Director, so m_renderer is dangling at shutdown.
    registry.on_destroy<TextLayerState>().connect<&TextSystem::onTextLayerDestroyed>(this);
    std::cout << "TextSystem initialized\n";
}

void TextSystem::shutdown(entt::registry& registry) {
    // Disconnect the on_destroy observer before teardown so it can't fire
    // against a dangling m_renderer. Slot cleanup at shutdown is intentionally
    // skipped — the renderer is torn down before Director, so m_renderer is
    // dangling at this point. GPU resources are released with the renderer.
    registry.on_destroy<TextLayerState>().disconnect<&TextSystem::onTextLayerDestroyed>(this);
    delete m_impl;
    m_impl = nullptr;
}

void TextSystem::update(entt::registry& registry, float /*deltaTime*/) {
    ZoneScopedN("TextSystem");

    if (!m_renderer || !m_impl) return;

    // ADR-0028: pull engaged remote text into TextLayerState before the dirty
    // scan so remote-driven updates flow through the normal rasterize path.
    // Editor-thread only (consumeText holds textMutex; ADR-0014 compliant).
    if (m_remoteStore) {
        auto remoteView = registry.view<GenerativeLayer, TextLayerState, RemotePatch>();
        for (auto [entity, gen, state, rp] : remoteView.each()) {
            if (rp.storeSlot < 0) continue;
            if (!m_remoteStore->engaged(rp.storeSlot)) continue;
            if (auto txt = m_remoteStore->consumeText(rp.storeSlot, rp.lastTextGen)) {
                state.text  = *txt;
                state.dirty = true;
            }
        }
    }

    auto view = registry.view<Layer, GenerativeLayer, TextLayerState>();

    // Early-out when nothing is dirty.
    bool anyDirty = false;
    for (auto [entity, layer, gen, state] : view.each()) {
        if (state.dirty) { anyDirty = true; break; }
    }
    if (!anyDirty) return;

    for (auto [entity, layer, gen, state] : view.each()) {
        if (!state.dirty) continue;

        // Build spec. Use the layer's render dimensions as the bake target.
        TextRasterSpec spec;
        spec.text       = state.text;
        spec.fontFamily = utf8ToWide(state.fontFamily);
        spec.fontSize   = state.fontSize;
        spec.color      = state.color;
        spec.alignment  = static_cast<uint8_t>(state.alignment);
        spec.bold       = state.bold;
        spec.italic     = state.italic;
        spec.width      = (gen.renderWidth  > 0) ? gen.renderWidth  : 1920u;
        spec.height     = (gen.renderHeight > 0) ? gen.renderHeight : 1080u;

        TextRasterResult result = m_impl->rasterizer.rasterize(spec);
        if (result.bgra.empty()) {
            std::cerr << "[TextSystem] Rasterize failed for entity "
                      << static_cast<uint64_t>(entity) << "\n";
            continue;
        }

        // Swizzle BGRA → RGBA in-place (uploadVideoFrameToSlot expects RGBA8).
        for (std::size_t i = 0; i + 3 < result.bgra.size(); i += 4) {
            std::swap(result.bgra[i], result.bgra[i + 2]);
        }

        // #89: the editor thread must never recreate a slot's texture in place
        // — the show thread may be mid-record sampling it, and our own ImGui
        // frame may already hold its SRV. On a bake-size change, take a fresh
        // slot and schedule-free the old one (the show thread reclaims it once
        // nothing can reference it).
        if (state.textureSlot >= 0 && state.bakedWidth != 0 &&
            (state.bakedWidth != result.width || state.bakedHeight != result.height)) {
            m_renderer->scheduleVideoTextureSlotFree(
                static_cast<uint32_t>(state.textureSlot));
            state.textureSlot = -1;
        }

        // Allocate slot on first use.
        if (state.textureSlot < 0) {
            const uint32_t slot = m_renderer->allocateVideoTextureSlot();
            if (slot == UINT32_MAX) {
                std::cerr << "[TextSystem] Video texture pool exhausted\n";
                continue;
            }
            state.textureSlot = static_cast<int>(slot);
        }

        // Immediate editor-thread upload — TextSystem runs on the editor
        // thread, so the regular uploadVideoFrameToSlot (records into the
        // show-thread copy list) would have its copy discarded. See ADR-0014.
        const bool ok = m_renderer->uploadVideoFrameToSlotImmediate(
            static_cast<uint32_t>(state.textureSlot),
            result.bgra.data(),
            result.width,
            result.height);

        if (!ok) {
            std::cerr << "[TextSystem] uploadVideoFrameToSlot failed for slot "
                      << state.textureSlot << "\n";
            // The slot may now hold a half-created texture at these dims while
            // bakedWidth still says 0 (upload creates the texture BEFORE the
            // copy step that can fail) — retrying at a different size would
            // then hit the renderer's in-place-recreate refusal forever.
            // Abandon the slot; the next tick (dirty stays set) starts clean.
            m_renderer->scheduleVideoTextureSlotFree(
                static_cast<uint32_t>(state.textureSlot));
            state.textureSlot = -1;
            continue;
        }

        state.bakedWidth  = result.width;
        state.bakedHeight = result.height;
        state.dirty       = false;
    }
}

void TextSystem::onTextLayerDestroyed(entt::registry& registry, entt::entity entity) {
    // Called by the on_destroy<TextLayerState> signal just before the component
    // is erased. The component is still alive here — textureSlot is readable.
    if (!m_renderer) return;
    const auto* tls = registry.try_get<TextLayerState>(entity);
    if (tls && tls->textureSlot >= 0) {
        // #89: deferred — the show thread frees the slot behind its gate.
        m_renderer->scheduleVideoTextureSlotFree(static_cast<uint32_t>(tls->textureSlot));
    }
}

} // namespace entity
