#pragma once

namespace entity {

// Attached to any layer entity whose media has an audio stream. Pure
// POD — FFmpeg resources live in the AudioDecodeWorker, not here. Named
// generically (not ClipAudio) so a future pure-audio-layer archetype
// reuses it by composition. See docs/reference/ENTITY_ARCHETYPES.md.
struct AudioSource {
    // Media-intrinsic — set when the worker opens; NOT persisted.
    bool hasAudioStream{false};
    int  sourceSampleRate{0};
    int  sourceChannels{0};

    // User-controlled — persisted (project schema v23).
    float gain{1.0f};   // linear
    bool  mute{false};
    bool  solo{false};
};

} // namespace entity
