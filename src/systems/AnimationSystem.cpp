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
#include "entity/components/Effect.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/effects/EffectKind.hpp"

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

        // Resolve the clip-local frame origin. Clip-backed entities source it
        // from Clip; Generative content layers (Layer + Transform + MediaLayer,
        // no Clip) source it from Layer. OA layers also carry a Layer but are
        // handled by the dedicated OA branch below — here they fall through
        // with no Transform/MediaLayer to write, so the property writes no-op.
        auto* clip  = registry.try_get<Clip>(entity);
        auto* layer = registry.try_get<Layer>(entity);
        FrameNumber localFrame = currentFrame;

        if (clip) {
            // Only animate while the clip is active at the current frame.
            if (currentFrame < clip->startFrame ||
                currentFrame >= clip->startFrame + clip->duration) {
                continue; // Clip not active, skip animation
            }
            localFrame = currentFrame - clip->startFrame;
        } else if (layer) {
            // Generative content layer — clip-relative frame from Layer.
            if (currentFrame < layer->startFrame ||
                currentFrame >= layer->startFrame + layer->duration) {
                continue; // Layer not active, skip animation
            }
            localFrame = currentFrame - layer->startFrame;
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
                case AnimatableProperty::RotationZ:
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

        // Inactive-window handling honors the per-layer EndBehavior (ADR-0020).
        // beforeStart: always reset — no last-evaluated value exists to hold.
        // afterEnd + Hold:  leave ObjectAnimationOutput populated so the target
        //                   stays parked where the animation left it. The flags
        //                   keep flowing into buildSceneSnapshot's fold-in, which
        //                   keeps overriding the Screen's base position.
        // afterEnd + Reset: today's pre-ADR-0020 behavior — clear so the screen
        //                   snaps back to its Stage-configured base.
        const bool beforeStart = currentFrame < layer.startFrame;
        const bool afterEnd    = currentFrame >= layer.startFrame + layer.duration;
        if (beforeStart || afterEnd) {
            const bool resetNow = beforeStart ||
                (afterEnd && oal.endBehavior == ObjectAnimationLayer::EndBehavior::Reset);
            if (resetNow) {
                if (auto* out = registry.try_get<ObjectAnimationOutput>(entity)) {
                    *out = ObjectAnimationOutput{};
                }
            }
            // Hold mode leaves ObjectAnimationOutput untouched.
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
                    // Legacy: AnimatableProperty::Rotation is documented as "Z
                    // rotation for 2D clips" in AnimatedProperties.hpp, but on
                    // an OA layer it has always mapped to Y (the natural turning
                    // axis for a screen). New OA work uses RotationZ explicitly
                    // (case below); the legacy Rotation→Y mapping stays here so
                    // pre-RotationZ projects keep behaving the same.
                    out.rotationOverride[1] = value;
                    out.hasRotation = true;
                    break;
                case AnimatableProperty::RotationX:
                    out.rotationOverride[0] = value;
                    out.hasRotation = true;
                    break;
                case AnimatableProperty::RotationZ:
                    out.rotationOverride[2] = value;
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

    // -- Effect-param eval (Phase 3, 2026-05-25) ----------------------------
    //
    // EffectAnimatedParameters carries hash-keyed Keyframe vectors per
    // animated param. Mirror the Transform/Opacity write-back: evaluate
    // each track at the layer-local frame, store the result in
    // EffectParameters.values[slot] so PropertyWindow sliders display the
    // evaluated value and the next snapshot bake sees the same number.
    //
    // The show-side bake in PlaybackTimeAuthority::buildSceneSnapshot also
    // evaluates these tracks for its own paramBlob writes — redundant but
    // cheap, and necessary because PTA's bake also ships the keyframes
    // through BakedEffectTrack for show-thread re-derivation during editor
    // stalls.
    if (m_effectKindRegistry) {
        auto effectView = registry.view<Effect, EffectAnimatedParameters, EffectParameters>();
        for (auto fxEnt : effectView) {
            const auto& fx    = effectView.get<Effect>(fxEnt);
            const auto& anim  = effectView.get<EffectAnimatedParameters>(fxEnt);
            auto&       params = effectView.get<EffectParameters>(fxEnt);

            if (anim.tracks.empty() || !fx.enabled) continue;
            if (fx.ownerLayer == entt::null || !registry.valid(fx.ownerLayer)) {
                continue;
            }

            // Same layer-local frame resolution as the Transform branch
            // above. Clip-backed entities source from Clip; Layer-backed
            // (Generative / OA) source from Layer. Don't evaluate when
            // the layer is outside its active window — the static
            // EffectParameters value should hold (effect is off-screen
            // anyway, but keeping behavior consistent matches "tracks
            // only contribute while the layer is active").
            FrameNumber layerStart = 0;
            FrameNumber layerEnd   = 0;
            if (const auto* clip = registry.try_get<Clip>(fx.ownerLayer)) {
                layerStart = clip->startFrame;
                layerEnd   = clip->startFrame + clip->duration;
            } else if (const auto* lay = registry.try_get<Layer>(fx.ownerLayer)) {
                layerStart = lay->startFrame;
                layerEnd   = lay->startFrame + lay->duration;
            } else {
                continue;
            }
            if (currentFrame < layerStart || currentFrame >= layerEnd) continue;
            const FrameNumber localFrame = currentFrame - layerStart;

            const effects::EffectKind* kind = m_effectKindRegistry->find(fx.kindId);
            if (!kind) continue;

            for (const auto& track : anim.tracks) {
                if (!track.enabled || track.keyframes.empty()) continue;

                // Find the schema slot matching this track's param-name hash.
                std::size_t slotN = SIZE_MAX;
                for (std::size_t i = 0; i < kind->params.size(); ++i) {
                    if (effects::fnv1a32(kind->params[i].name) == track.paramKeyHash) {
                        slotN = i;
                        break;
                    }
                }
                if (slotN == SIZE_MAX) continue;
                const auto& schema = kind->params[slotN];
                if (schema.type != ParamValue::Type::Float) continue; // v1

                // Ensure the values vector is sized to the schema before
                // we write — newly-created effects sometimes have an
                // empty `values` until first edit lands.
                if (params.values.size() < kind->params.size()) {
                    params.values.resize(kind->params.size(), ParamValue::makeFloat(0.0f));
                    for (std::size_t i = 0; i < kind->params.size(); ++i) {
                        if (params.values[i].type == ParamValue::Type::Float &&
                            params.values[i].f4[0] == 0.0f && params.values[i].i == 0)
                        {
                            params.values[i] = kind->params[i].defaultValue;
                        }
                    }
                }

                const float def = schema.defaultValue.f4[0];
                const float v   = evaluateKeyframes(track.keyframes, localFrame, def);
                params.values[slotN] = ParamValue::makeFloat(v);
            }
        }
    }
}

void AnimationSystem::shutdown(entt::registry& registry) {
    std::cout << "AnimationSystem shutdown" << std::endl;
}

} // namespace entity
