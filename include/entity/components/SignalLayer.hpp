// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace entity {

/**
 * SignalLayer component — authoring data for a Signal Output layer.
 *
 * A Signal layer dispatches plugin events (e.g. OSC packets) while the
 * playhead is inside its timeline window. It is deliberately NOT a content
 * layer: no MediaLayer, no Transform. The ECS archetype is:
 *
 *   Layer (Kind::Signal) + SignalLayer + AnimatedProperties
 *
 * Per ADR-0016: system selection is by component composition, never by
 * the Kind enum. Kind::Signal is informational only (UI badge, serialization).
 *
 * SignalOutputSystem (Phase 5/6) iterates view<Layer, SignalLayer, AnimatedProperties>,
 * evaluates the active state each tick, and posts bus::SignalEmit via
 * Engine::postSignalEmit.
 */
struct SignalLayer {
    // -- Nested types ---------------------------------------------------------

    enum class Mode : std::uint8_t {
        Momentary  = 0,  // fire once when the playhead enters the layer window
        Continuous = 1,  // re-evaluate and emit every tick while inside the window
    };

    enum class Transport : std::uint8_t {
        OSC = 0,         // emit via OSC (the only transport in Phase 1)
    };

    // How the output float value is sourced (Continuous mode only).
    enum class ValueSource : std::uint8_t {
        Progress = 0,    // normalized playhead position within the layer [0, 1]
        Keyframe = 1,    // evaluated from an AnimatableProperty::SignalValue track
    };

    // One OSC argument. `isValueBound` marks the single arg slot driven by the
    // continuous value pipeline (ValueSource + range mapping). At most one arg
    // per layer may be value-bound; the rest carry literal i/f/s data.
    struct Arg {
        enum class Type : std::uint8_t { Int = 0, Float = 1, String = 2 };

        Type         type{Type::Int};
        std::int32_t i{0};
        float        f{0.0f};
        std::string  s;
        bool         isValueBound{false};   // true = replace with mapped output value
    };

    // -- Fields ---------------------------------------------------------------

    Mode        mode{Mode::Momentary};
    Transport   transport{Transport::OSC};
    ValueSource valueSource{ValueSource::Progress};  // Continuous mode only

    // OSC address pattern, e.g. "/entity/signal/foo".
    std::string address{"/entity/signal"};

    // Ordered argument list. May be empty (bare bang) or carry literal + bound args.
    std::vector<Arg> args;

    // Range mapping for the value-bound arg (Continuous + isValueBound).
    // Maps srcMin..srcMax -> outMin..outMax, then casts to outType before emit.
    float     srcMin{0.0f};
    float     srcMax{1.0f};
    float     outMin{0.0f};
    float     outMax{1.0f};
    Arg::Type outType{Arg::Type::Float};
};

} // namespace entity
