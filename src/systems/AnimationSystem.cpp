/**
 * AnimationSystem Implementation
 *
 * Evaluates keyframes at the current timeline frame and updates
 * Transform and MediaLayer components accordingly.
 *
 * Keyframe frames are relative to clip start, so a keyframe at frame 10
 * occurs 10 frames after the clip begins on the timeline.
 */

#include "entity/systems/AnimationSystem.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Clip.hpp"

#include <iostream>

namespace entity {

void AnimationSystem::initialize(entt::registry& registry) {
    std::cout << "AnimationSystem initialized" << std::endl;
}

void AnimationSystem::update(entt::registry& registry, float deltaTime) {
    if (!m_timeline) {
        return;
    }

    // Get current timeline frame
    FrameNumber currentFrame = m_timeline->getCurrentFrame();

    // Query all entities with AnimatedProperties
    auto view = registry.view<AnimatedProperties>();

    static int debugFrame = 0;
    bool logAnimation = m_debugLogging && (debugFrame++ % 60 == 0);

    for (auto entity : view) {
        auto& animProps = view.get<AnimatedProperties>(entity);

        // Skip if no keyframes
        if (!animProps.hasAnyKeyframes()) {
            continue;
        }

        // Get clip for frame offset calculation (optional - without clip, use timeline frame directly)
        auto* clip = registry.try_get<Clip>(entity);
        FrameNumber localFrame = currentFrame;

        if (clip) {
            // Calculate local frame within the clip
            // Only animate if clip is active at current frame
            if (currentFrame < clip->startFrame ||
                currentFrame >= clip->startFrame + clip->duration) {
                continue; // Clip not active, skip animation
            }
            localFrame = currentFrame - clip->startFrame;
        }

        // Get components to update
        auto* transform = registry.try_get<Transform>(entity);
        auto* mediaLayer = registry.try_get<MediaLayer>(entity);

        if (logAnimation) {
            std::cout << "[Animation] Entity " << static_cast<uint32_t>(entity)
                      << " localFrame=" << localFrame
                      << " tracks=" << animProps.tracks.size() << std::endl;
        }

        // Evaluate each track and apply to appropriate component
        for (const auto& track : animProps.tracks) {
            if (!track.enabled || !track.hasKeyframes()) {
                continue;
            }

            float value = track.evaluate(localFrame);

            switch (track.property) {
                case AnimatableProperty::PositionX:
                    if (transform) {
                        transform->position.x = value;
                        transform->dirty = true;
                        if (logAnimation) {
                            std::cout << "  PositionX = " << value << std::endl;
                        }
                    }
                    break;

                case AnimatableProperty::PositionY:
                    if (transform) {
                        transform->position.y = value;
                        transform->dirty = true;
                        if (logAnimation) {
                            std::cout << "  PositionY = " << value << std::endl;
                        }
                    }
                    break;

                case AnimatableProperty::Rotation:
                    if (transform) {
                        transform->rotation.z = value;
                        transform->dirty = true;
                        if (logAnimation) {
                            std::cout << "  Rotation = " << value << std::endl;
                        }
                    }
                    break;

                case AnimatableProperty::ScaleX:
                    if (transform) {
                        transform->scale.x = value;
                        transform->dirty = true;
                        if (logAnimation) {
                            std::cout << "  ScaleX = " << value << std::endl;
                        }
                    }
                    break;

                case AnimatableProperty::ScaleY:
                    if (transform) {
                        transform->scale.y = value;
                        transform->dirty = true;
                        if (logAnimation) {
                            std::cout << "  ScaleY = " << value << std::endl;
                        }
                    }
                    break;

                case AnimatableProperty::Opacity:
                    if (mediaLayer) {
                        mediaLayer->opacity = value;
                        if (logAnimation) {
                            std::cout << "  Opacity = " << value << std::endl;
                        }
                    }
                    break;
            }
        }
    }
}

void AnimationSystem::shutdown(entt::registry& registry) {
    std::cout << "AnimationSystem shutdown" << std::endl;
}

} // namespace entity
