#pragma once

#include "System.hpp"
#include <entt/entt.hpp>

namespace entity {

class IRenderer;
namespace remote { class RemoteControlStore; }

// TextSystem — editor-thread only (ADR-0014).
//
// Each editor tick, iterates all entities with Layer + GenerativeLayer +
// TextLayerState. For each with dirty==true, rasterizes the text via
// TextRasterizer, uploads to a video-pool descriptor slot via IRenderer,
// and clears dirty. Show thread reads the baked textureSlot from the
// GenerativeLayerSnapshot (baked by PlaybackTimeAuthority::buildSceneSnapshot).
//
// No show-thread fallback needed: text is static-per-frame. If the editor
// stalls, the previously-baked texture keeps rendering unchanged.
class TextSystem : public System {
public:
    explicit TextSystem(IRenderer* renderer);

    void setRenderer(IRenderer* renderer) { m_renderer = renderer; }

    // ADR-0028: engaged remote text arrives via this store. Bound once at
    // Engine init; nullptr = no remote text (pre-project or store absent).
    // Editor thread only (consumeText holds textMutex; ADR-0014 compliant).
    void setRemoteControlStore(remote::RemoteControlStore* store) {
        m_remoteStore = store;
    }

    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "TextSystem"; }

private:
    void onTextLayerDestroyed(entt::registry& registry, entt::entity entity);

    IRenderer*                       m_renderer{nullptr};
    remote::RemoteControlStore*      m_remoteStore{nullptr};
    // TextRasterizer owns DWrite/D2D/WIC COM factories; heap-allocated so
    // TextSystem header doesn't pull Windows headers into Engine.hpp.
    struct Impl;
    Impl* m_impl{nullptr};
};

} // namespace entity
