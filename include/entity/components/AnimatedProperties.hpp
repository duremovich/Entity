#pragma once

#include "../core/Types.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

namespace entity {

/**
 * Interpolation type for keyframes
 */
enum class InterpolationType {
    Linear,     // Linear interpolation between keyframes
    Step,       // Instant change at keyframe (hold previous value)
    EaseInOut   // Smooth ease in/out (cubic bezier)
};

/**
 * Animatable property identifiers
 */
enum class AnimatableProperty {
    PositionX,
    PositionY,
    Rotation,      // Z rotation for 2D
    ScaleX,
    ScaleY,
    Opacity
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
     */
    void addKeyframe(FrameNumber frame, float value,
                     InterpolationType interp = InterpolationType::Linear) {
        // Check if keyframe exists at this frame
        for (auto& kf : keyframes) {
            if (kf.frame == frame) {
                kf.value = value;
                kf.interpolation = interp;
                return;
            }
        }

        // Add new keyframe
        keyframes.push_back({frame, value, interp});
        std::sort(keyframes.begin(), keyframes.end());
    }

    /**
     * Remove keyframe at frame
     */
    bool removeKeyframe(FrameNumber frame) {
        auto it = std::find_if(keyframes.begin(), keyframes.end(),
            [frame](const Keyframe& kf) { return kf.frame == frame; });
        if (it != keyframes.end()) {
            keyframes.erase(it);
            return true;
        }
        return false;
    }

    /**
     * Get keyframe at exact frame (or nullptr) - const version
     */
    const Keyframe* getKeyframeAt(FrameNumber frame) const {
        for (const auto& kf : keyframes) {
            if (kf.frame == frame) return &kf;
        }
        return nullptr;
    }

    /**
     * Get keyframe at exact frame (or nullptr) - mutable version
     */
    Keyframe* getKeyframeAt(FrameNumber frame) {
        for (auto& kf : keyframes) {
            if (kf.frame == frame) return &kf;
        }
        return nullptr;
    }

    /**
     * Evaluate track at given frame with interpolation
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

        // Find surrounding keyframes
        for (size_t i = 0; i < keyframes.size() - 1; ++i) {
            const Keyframe& kf1 = keyframes[i];
            const Keyframe& kf2 = keyframes[i + 1];

            if (frame >= kf1.frame && frame < kf2.frame) {
                // Calculate interpolation factor (0-1)
                float t = static_cast<float>(frame - kf1.frame) /
                          static_cast<float>(kf2.frame - kf1.frame);

                return interpolate(kf1, kf2, t);
            }
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
            case AnimatableProperty::Rotation:
                return 0.0f;
            case AnimatableProperty::ScaleX:
            case AnimatableProperty::ScaleY:
            case AnimatableProperty::Opacity:
                return 1.0f;
            default:
                return 0.0f;
        }
    }

    /**
     * Interpolate between two keyframes
     */
    static float interpolate(const Keyframe& kf1, const Keyframe& kf2, float t) {
        switch (kf1.interpolation) {
            case InterpolationType::Step:
                // Hold first value until next keyframe
                return kf1.value;

            case InterpolationType::EaseInOut: {
                // Cubic bezier easing
                t = cubicBezier(t, kf1.easeIn, kf1.easeOut);
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

} // namespace entity
