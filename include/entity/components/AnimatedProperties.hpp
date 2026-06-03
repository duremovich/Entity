#pragma once

#include "../core/Types.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

namespace entity {

/**
 * Interpolation type for keyframes
 * Visual shapes in timeline:
 * - Linear: Diamond
 * - Step (Hold): Square
 * - EaseIn: Left hourglass, right diamond
 * - EaseOut: Left diamond, right hourglass
 * - EaseInOut: Full hourglass
 */
enum class InterpolationType {
    Linear,     // Linear interpolation between keyframes (diamond shape)
    Step,       // Instant change at keyframe - hold previous value (square shape)
    EaseIn,     // Ease in only - starts slow, ends linear (half hourglass left)
    EaseOut,    // Ease out only - starts linear, ends slow (half hourglass right)
    EaseInOut   // Smooth ease in/out - cubic bezier (full hourglass shape)
};

/**
 * Animatable property identifiers
 */
enum class AnimatableProperty {
    PositionX,
    PositionY,
    Rotation,      // Z rotation for 2D clips. Legacy: on OA layers this enum
                   // historically maps to Y-axis rotation. New OA work uses
                   // RotationZ explicitly. Don't reorder — wire-stable index.
    ScaleX,
    ScaleY,
    Opacity,
    // 3D axes for ObjectAnimationLayer. Appended in order — wire-stable indices.
    PositionZ,
    RotationX,
    RotationY,
    ScaleZ,
    RotationZ,     // Z-axis (roll) for OA layers. No-op on Clip's 2D branch.
    // Signal Output Layer. Appended last — wire-stable index (do not reorder).
    SignalValue,   // Scalar value keyframed on a SignalLayer arg track.
};

/**
 * Convert property enum to display string
 */
inline const char* getPropertyName(AnimatableProperty prop) {
    switch (prop) {
        case AnimatableProperty::PositionX: return "Position X";
        case AnimatableProperty::PositionY: return "Position Y";
        case AnimatableProperty::Rotation:  return "Rotation";
        case AnimatableProperty::ScaleX:    return "Scale X";
        case AnimatableProperty::ScaleY:    return "Scale Y";
        case AnimatableProperty::Opacity:   return "Opacity";
        case AnimatableProperty::PositionZ: return "Position Z";
        case AnimatableProperty::RotationX: return "Rotation X";
        case AnimatableProperty::RotationY: return "Rotation Y";
        case AnimatableProperty::ScaleZ:    return "Scale Z";
        case AnimatableProperty::RotationZ:    return "Rotation Z";
        case AnimatableProperty::SignalValue:  return "Signal Value";
        default: return "Unknown";
    }
}

/**
 * A single keyframe storing a value at a specific frame
 */
struct Keyframe {
    FrameNumber frame{0};           // Frame number (relative to clip start)
    float value{0.0f};              // Property value at this frame
    InterpolationType interpolation{InterpolationType::Linear};

    // Bezier control points for EaseInOut (normalized 0-1)
    float easeIn{0.42f};            // Control point for ease-in
    float easeOut{0.58f};           // Control point for ease-out

    bool operator<(const Keyframe& other) const {
        return frame < other.frame;
    }
};

/**
 * A track of keyframes for a single property
 */
struct KeyframeTrack {
    AnimatableProperty property{AnimatableProperty::PositionX};
    std::vector<Keyframe> keyframes;
    bool enabled{true};             // Track can be muted

    /**
     * Add a keyframe, maintaining sorted order
     * Uses binary search for O(log n) insertion instead of O(n log n) sort
     */
    void addKeyframe(FrameNumber frame, float value,
                     InterpolationType interp = InterpolationType::Linear) {
        // Find insertion point using binary search
        auto it = std::lower_bound(keyframes.begin(), keyframes.end(), frame,
            [](const Keyframe& k, FrameNumber f) { return k.frame < f; });

        // Check if keyframe already exists at this frame
        if (it != keyframes.end() && it->frame == frame) {
            // Update existing keyframe
            it->value = value;
            it->interpolation = interp;
            return;
        }

        // Insert at correct position (maintains sorted order)
        keyframes.insert(it, Keyframe{frame, value, interp});
    }

    /**
     * Remove keyframe at frame
     * Uses binary search for O(log n) lookup
     */
    bool removeKeyframe(FrameNumber frame) {
        auto it = std::lower_bound(keyframes.begin(), keyframes.end(), frame,
            [](const Keyframe& k, FrameNumber f) { return k.frame < f; });

        if (it != keyframes.end() && it->frame == frame) {
            keyframes.erase(it);
            return true;
        }
        return false;
    }

    /**
     * Get keyframe at exact frame (or nullptr) - const version
     * Uses binary search for O(log n) lookup
     */
    const Keyframe* getKeyframeAt(FrameNumber frame) const {
        auto it = std::lower_bound(keyframes.begin(), keyframes.end(), frame,
            [](const Keyframe& k, FrameNumber f) { return k.frame < f; });

        if (it != keyframes.end() && it->frame == frame) {
            return &(*it);
        }
        return nullptr;
    }

    /**
     * Get keyframe at exact frame (or nullptr) - mutable version
     * Uses binary search for O(log n) lookup
     */
    Keyframe* getKeyframeAt(FrameNumber frame) {
        auto it = std::lower_bound(keyframes.begin(), keyframes.end(), frame,
            [](const Keyframe& k, FrameNumber f) { return k.frame < f; });

        if (it != keyframes.end() && it->frame == frame) {
            return &(*it);
        }
        return nullptr;
    }

    /**
     * Evaluate track at given frame with interpolation
     * Uses binary search for O(log n) keyframe lookup
     */
    float evaluate(FrameNumber frame) const {
        if (keyframes.empty()) {
            return getDefaultValue(property);
        }

        // Before first keyframe
        if (frame <= keyframes.front().frame) {
            return keyframes.front().value;
        }

        // After last keyframe
        if (frame >= keyframes.back().frame) {
            return keyframes.back().value;
        }

        // Find first keyframe >= frame using binary search
        auto it = std::lower_bound(keyframes.begin(), keyframes.end(), frame,
            [](const Keyframe& k, FrameNumber f) { return k.frame < f; });

        // If exact match, return value
        if (it != keyframes.end() && it->frame == frame) {
            return it->value;
        }

        // it points to first keyframe after frame, so interpolate between (it-1) and it
        if (it != keyframes.begin() && it != keyframes.end()) {
            const Keyframe& kf1 = *(it - 1);
            const Keyframe& kf2 = *it;

            // Calculate interpolation factor (0-1)
            float t = static_cast<float>(frame - kf1.frame) /
                      static_cast<float>(kf2.frame - kf1.frame);

            return interpolate(kf1, kf2, t);
        }

        return keyframes.back().value;
    }

    /**
     * Check if track has any keyframes
     */
    bool hasKeyframes() const {
        return !keyframes.empty();
    }

private:
    /**
     * Get default value for a property
     */
    static float getDefaultValue(AnimatableProperty prop) {
        switch (prop) {
            case AnimatableProperty::PositionX:
            case AnimatableProperty::PositionY:
            case AnimatableProperty::PositionZ:
            case AnimatableProperty::Rotation:
            case AnimatableProperty::RotationX:
            case AnimatableProperty::RotationY:
            case AnimatableProperty::RotationZ:
                return 0.0f;
            case AnimatableProperty::ScaleX:
            case AnimatableProperty::ScaleY:
            case AnimatableProperty::ScaleZ:
            case AnimatableProperty::Opacity:
                return 1.0f;
            default:
                return 0.0f;
        }
    }

public:
    /**
     * Interpolate between two keyframes
     * Note: Uses kf2's interpolation type (incoming behavior) - setting ease
     * on a keyframe affects how motion arrives at that keyframe.
     *
     * Public so the free-function evaluator at the bottom of this header —
     * used for hash-keyed effect-param tracks where the AnimatableProperty
     * enum doesn't apply — can share this math. AnimationSystem +
     * PlaybackTimeAuthority both go through that free function.
     */
    static float interpolate(const Keyframe& kf1, const Keyframe& kf2, float t) {
        // Step is special - check kf1 for hold behavior (hold until next keyframe)
        if (kf1.interpolation == InterpolationType::Step) {
            return kf1.value;
        }

        // Use kf2's interpolation type (incoming behavior)
        switch (kf2.interpolation) {
            case InterpolationType::Step:
                // If destination is step, use linear to arrive then hold
                return kf1.value + (kf2.value - kf1.value) * t;

            case InterpolationType::EaseIn: {
                // Ease in - starts slow, ends at full speed
                float easedT = t * t;
                return kf1.value + (kf2.value - kf1.value) * easedT;
            }

            case InterpolationType::EaseOut: {
                // Ease out - starts at full speed, ends slow
                float easedT = 1.0f - (1.0f - t) * (1.0f - t);
                return kf1.value + (kf2.value - kf1.value) * easedT;
            }

            case InterpolationType::EaseInOut: {
                // Cubic bezier easing (smooth ease in/out)
                t = cubicBezier(t, kf2.easeIn, kf2.easeOut);
                return kf1.value + (kf2.value - kf1.value) * t;
            }

            case InterpolationType::Linear:
            default:
                // Linear interpolation
                return kf1.value + (kf2.value - kf1.value) * t;
        }
    }

    /**
     * Cubic bezier easing function
     * Control points: (0,0), (easeIn, 0), (easeOut, 1), (1,1)
     */
    static float cubicBezier(float t, float easeIn, float easeOut) {
        // Simplified cubic bezier for ease curves
        float t2 = t * t;
        float t3 = t2 * t;
        float mt = 1.0f - t;
        float mt2 = mt * mt;
        float mt3 = mt2 * mt;

        // Bezier formula: B(t) = (1-t)^3*P0 + 3*(1-t)^2*t*P1 + 3*(1-t)*t^2*P2 + t^3*P3
        // P0 = 0, P1 = easeIn, P2 = easeOut, P3 = 1
        float y = 3.0f * mt2 * t * 0.0f +      // P1.y = 0
                  3.0f * mt * t2 * 1.0f +       // P2.y = 1
                  t3 * 1.0f;                    // P3.y = 1

        return y;
    }
};

/**
 * Component that holds all animation tracks for an entity
 */
struct AnimatedProperties {
    std::vector<KeyframeTrack> tracks;

    /**
     * Get or create track for a property
     */
    KeyframeTrack& getOrCreateTrack(AnimatableProperty property) {
        for (auto& track : tracks) {
            if (track.property == property) {
                return track;
            }
        }

        // Create new track
        tracks.push_back({});
        tracks.back().property = property;
        return tracks.back();
    }

    /**
     * Get track for property (or nullptr if none)
     */
    KeyframeTrack* getTrack(AnimatableProperty property) {
        for (auto& track : tracks) {
            if (track.property == property) {
                return &track;
            }
        }
        return nullptr;
    }

    const KeyframeTrack* getTrack(AnimatableProperty property) const {
        for (const auto& track : tracks) {
            if (track.property == property) {
                return &track;
            }
        }
        return nullptr;
    }

    /**
     * Add a keyframe to a property track
     */
    void addKeyframe(AnimatableProperty property, FrameNumber frame, float value,
                     InterpolationType interp = InterpolationType::Linear) {
        getOrCreateTrack(property).addKeyframe(frame, value, interp);
    }

    /**
     * Evaluate a property at given frame
     * Returns default value if no keyframes exist
     */
    float evaluate(AnimatableProperty property, FrameNumber frame) const {
        const KeyframeTrack* track = getTrack(property);
        if (track && track->enabled && track->hasKeyframes()) {
            return track->evaluate(frame);
        }

        // Return default values
        switch (property) {
            case AnimatableProperty::ScaleX:
            case AnimatableProperty::ScaleY:
            case AnimatableProperty::ScaleZ:
            case AnimatableProperty::Opacity:
                return 1.0f;
            default:
                return 0.0f;
        }
    }

    /**
     * Check if any property has keyframes
     */
    bool hasAnyKeyframes() const {
        for (const auto& track : tracks) {
            if (track.hasKeyframes()) return true;
        }
        return false;
    }

    /**
     * Get total number of keyframes across all tracks
     */
    size_t getTotalKeyframeCount() const {
        size_t count = 0;
        for (const auto& track : tracks) {
            count += track.keyframes.size();
        }
        return count;
    }

    /**
     * Clear all keyframes from all tracks
     */
    void clearAll() {
        tracks.clear();
    }
};

/**
 * Evaluate an unkeyed Keyframe vector at a given frame. Same math as
 * KeyframeTrack::evaluate but without an AnimatableProperty default
 * lookup — caller passes the fallback value to use when the vector is
 * empty.
 *
 * Used by hash-keyed tracks (EffectAnimatedParameters::NamedTrack)
 * which can't be ranged through the AnimatableProperty enum. Shared
 * by AnimationSystem (editor-thread per-tick eval) and
 * PlaybackTimeAuthority::buildSceneSnapshot (editor bake of show-side
 * effect params). Keeping it inline-in-header avoids a one-function .cpp.
 */
inline float evaluateKeyframes(const std::vector<Keyframe>& kfs,
                               FrameNumber frame,
                               float defaultIfEmpty) {
    if (kfs.empty()) return defaultIfEmpty;
    if (frame <= kfs.front().frame) return kfs.front().value;
    if (frame >= kfs.back().frame)  return kfs.back().value;
    auto it = std::lower_bound(kfs.begin(), kfs.end(), frame,
        [](const Keyframe& k, FrameNumber f) { return k.frame < f; });
    if (it != kfs.end() && it->frame == frame) return it->value;
    if (it == kfs.begin() || it == kfs.end()) return kfs.back().value;
    const Keyframe& prev = *(it - 1);
    const Keyframe& next = *it;
    const float t = static_cast<float>(frame - prev.frame)
                  / static_cast<float>(next.frame - prev.frame);
    return KeyframeTrack::interpolate(prev, next, t);
}

} // namespace entity
