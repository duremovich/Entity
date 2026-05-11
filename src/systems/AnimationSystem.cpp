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
#include "entity/profile/Tracy.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/ObjectAnimationOutput.hpp"

#include <iostream>

namespace entity {

void AnimationSystem::initialize(entt::registry& registry) {
    std::cout << "AnimationSystem initialized" << std::endl;
}

void AnimationSystem::update(entt::registry& registry, float deltaTime) {
    ZoneScopedN("AnimationSystem::update");
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

                // 3D axes — only relevant for OA layers; no-op on Clip entities
                // that lack ObjectAnimationLayer. Handled in the OA branch below.
                case AnimatableProperty::PositionZ:
                case AnimatableProperty::RotationX:
                case AnimatableProperty::RotationY:
                case AnimatableProperty::ScaleZ:
                    break;
            }
        }
    }

    // --- Object Animation Layer branch ---
    // For entities that have both AnimatedProperties and ObjectAnimationLayer,
    // evaluate all tracks using Layer::startFrame as the clip-local origin and
    // write results into ObjectAnimationOutput on the same entity.
    // buildSceneSnapshot (Phase 3.4) reads ObjectAnimationOutput and folds it
    // into the target's ScreenSnapshot entry.
    // ADR-0014: this branch runs on the editor thread only and writes only the
    // ObjectAnimationOutput component — never the target Screen/Prop registry data.
    auto oaView = registry.view<AnimatedProperties, ObjectAnimationLayer, Layer>();
    for (auto entity : oaView) {
        const auto& oal   = oaView.get<ObjectAnimationLayer>(entity);
        const auto& layer = oaView.get<Layer>(entity);

        // If inactive, reset any existing output so stale has* flags don't bleed
        // into buildSceneSnapshot's fold-in (phase 3.4). Belt-and-suspenders with
        // the active-frame guard in buildSceneSnapshot itself.
        if (currentFrame < layer.startFrame ||
            currentFrame >= layer.startFrame + layer.duration) {
            if (auto* out = registry.try_get<ObjectAnimationOutput>(entity)) {
                *out = ObjectAnimationOutput{};
            }
            continue;
        }

        // Locked OA layers hold their last-evaluated output at a section break.
        // SectionScheduler::seedContinuationAt sets frozen=true; clearAllContinuation
        // (GO / Stop) clears it. Skip re-evaluation while frozen so the held
        // ObjectAnimationOutput feeds buildSceneSnapshot unchanged.
        if (oal.sectionBehavior == SectionBehavior::Locked && oal.frozen) {
            continue;
        }

        FrameNumber localFrame = currentFrame - layer.startFrame;

        // Reset output every tick so stale values don't linger when keyframes are removed
        auto& out = registry.get_or_emplace<ObjectAnimationOutput>(entity);
        out = ObjectAnimationOutput{};

        auto& animProps = oaView.get<AnimatedProperties>(entity);
        if (!animProps.hasAnyKeyframes()) continue;

        for (const auto& track : animProps.tracks) {
            if (!track.enabled || !track.hasKeyframes()) continue;
            float value = track.evaluate(localFrame);

            switch (track.property) {
                case AnimatableProperty::PositionX:
                    out.positionOverride[0] = value;
                    out.hasPosition = true;
                    break;
                case AnimatableProperty::PositionY:
                    out.positionOverride[1] = value;
                    out.hasPosition = true;
                    break;
                case AnimatableProperty::PositionZ:
                    out.positionOverride[2] = value;
                    out.hasPosition = true;
                    break;
                case AnimatableProperty::Rotation:
                case AnimatableProperty::RotationY:
                    // NOTE (3.5): AnimatableProperty::Rotation is documented as
                    // "Z rotation for 2D clips" in AnimatedProperties.hpp:34, but
                    // here it maps to rotationOverride[1] (Y-axis) — the natural
                    // "turning" axis for a Screen in 3D space. A user who adds a
                    // "Rotation" keyframe to an OA layer will see Y-rotation, not Z.
                    // Phase 3.5 should either rename the OA-side display label to
                    // "Rotation Y" or introduce a dedicated RotationZ enum value and
                    // map it consistently across both Clip and OA branches.
                    out.rotationOverride[1] = value;
                    out.hasRotation = true;
                    break;
                case AnimatableProperty::RotationX:
                    out.rotationOverride[0] = value;
                    out.hasRotation = true;
                    break;
                case AnimatableProperty::ScaleX:
                    out.sizeOverride[0] = value;
                    out.hasSize = true;
                    break;
                case AnimatableProperty::ScaleY:
                    out.sizeOverride[1] = value;
                    out.hasSize = true;
                    break;
                case AnimatableProperty::ScaleZ:
                    out.sizeOverride[2] = value;
                    out.hasSize = true;
                    break;
                case AnimatableProperty::Opacity:
                    break;  // opacity on OA layers is not yet surfaced
            }
        }
    }
}

void AnimationSystem::shutdown(entt::registry& registry) {
    std::cout << "AnimationSystem shutdown" << std::endl;
}

} // namespace entity
