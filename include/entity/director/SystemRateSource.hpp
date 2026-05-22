#pragma once
#include "entity/director/RateSource.hpp"
#include <chrono>

namespace entity {

// The always-available rate source: the OS monotonic clock. This is the
// behaviour Entity had before ADR-0025 — extracted behind the interface.
class SystemRateSource : public RateSource {
public:
    double now() const override;
    bool   healthy() const override { return true; }
    const char* name() const override { return "SystemRateSource"; }

private:
    using Clock = std::chrono::steady_clock; // == high_resolution_clock on MSVC
};

} // namespace entity
