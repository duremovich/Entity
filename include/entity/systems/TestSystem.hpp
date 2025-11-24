#pragma once

#include "entity/systems/System.hpp"
#include <iostream>

namespace entity {

/**
 * TestSystem - Validates system infrastructure works.
 * This is temporary and will be removed once real systems are implemented.
 *
 * Purpose:
 * - Verify system registration mechanism works
 * - Confirm initialize/update/shutdown lifecycle functions
 * - Validate deltaTime propagation
 * - Provide visual confirmation through console output
 */
class TestSystem : public System {
public:
    void initialize(entt::registry& registry) override {
        std::cout << "[TestSystem] Initialized" << std::endl;
    }

    void update(entt::registry& registry, float deltaTime) override {
        m_updateCount++;

        // Log every 60 frames (~1 sec at 60fps)
        if (m_updateCount % 60 == 0) {
            std::cout << "[TestSystem] Update " << m_updateCount
                      << " (deltaTime: " << deltaTime << "s)" << std::endl;
        }
    }

    void shutdown(entt::registry& registry) override {
        std::cout << "[TestSystem] Shutdown after " << m_updateCount
                  << " updates" << std::endl;
    }

    const char* getName() const override {
        return "TestSystem";
    }

private:
    uint64_t m_updateCount{0};
};

} // namespace entity
