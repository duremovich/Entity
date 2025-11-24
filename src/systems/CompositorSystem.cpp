/**
 * CompositorSystem Implementation
 *
 * Renders visible layers by querying entities with Transform and MediaLayer,
 * sorting by z-order, and issuing draw calls.
 */

#include "entity/systems/CompositorSystem.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"

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
    // Nothing to initialize for now
}

void CompositorSystem::update(entt::registry& registry, float deltaTime) {
    static int frameCount = 0;
    if (frameCount++ % 60 == 0) {
        std::cout << "[CompositorSystem] Update called (frame " << frameCount << ")" << std::endl;
    }

    if (!m_renderer || !m_renderer->isInitialized()) {
        std::cerr << "[CompositorSystem] Renderer not initialized!" << std::endl;
        return;
    }

    // Query all entities with Transform and MediaLayer components
    auto view = registry.view<Transform, MediaLayer>();

    if (frameCount % 60 == 1) {
        std::cout << "[CompositorSystem] Found " << view.size_hint() << " entities with Transform+MediaLayer" << std::endl;
    }

    // Collect and sort entities by z-order
    std::vector<entt::entity> sortedEntities;
    sortedEntities.reserve(view.size_hint());

    for (auto entity : view) {
        auto& layer = view.get<MediaLayer>(entity);
        if (layer.visible) {
            sortedEntities.push_back(entity);
        }
    }

    // Sort by z-order (lower values render first, higher values on top)
    std::sort(sortedEntities.begin(), sortedEntities.end(),
        [&view](entt::entity a, entt::entity b) {
            const auto& layerA = view.get<MediaLayer>(a);
            const auto& layerB = view.get<MediaLayer>(b);
            return layerA.zOrder < layerB.zOrder;
        });

    // Render each visible layer
    for (auto entity : sortedEntities) {
        const auto& transform = view.get<Transform>(entity);
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

        // For Phase 1, use a default color based on entity ID (for variety)
        // In Phase 2, this will sample from VideoTexture
        uint32_t entityId = static_cast<uint32_t>(entity);
        DirectX::XMFLOAT4 color(
            ((entityId * 137) % 256) / 255.0f,  // R: pseudo-random but stable per entity
            ((entityId * 211) % 256) / 255.0f,  // G
            ((entityId * 97) % 256) / 255.0f,   // B
            1.0f                                // A
        );

        // Draw the quad
        if (frameCount % 60 == 1) {
            std::cout << "[CompositorSystem] Drawing entity " << static_cast<uint32_t>(entity)
                      << " with color (" << color.x << ", " << color.y << ", " << color.z
                      << ") opacity=" << layer.opacity << std::endl;
        }
        m_renderer->drawColoredQuad(transformMatrix, color, layer.opacity);
    }

    if (frameCount % 60 == 1) {
        std::cout << "[CompositorSystem] Drew " << sortedEntities.size() << " quads" << std::endl;
    }
}

void CompositorSystem::shutdown(entt::registry& registry) {
    // Nothing to clean up for now
}

} // namespace entity
