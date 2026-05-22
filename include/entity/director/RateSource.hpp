#pragma once

namespace entity {

// A monotonic real-time frequency reference — answers only "how much
// real time has elapsed." See docs/adr/0025-clock-rate-vs-position-authority.md.
// Carries no playback position. Implementations: SystemRateSource (wall
// clock), AudioRateSource (audio device sample clock).
class RateSource {
public:
    virtual ~RateSource() = default;

    // Monotonically increasing accumulated real-time seconds. The origin
    // is arbitrary and per-implementation; callers only ever diff it.
    virtual double now() const = 0;

    // False when this source has stalled and must not be used as the
    // master (e.g. audio device underrun). SystemRateSource is always true.
    virtual bool healthy() const = 0;

    virtual const char* name() const = 0;
};

} // namespace entity
