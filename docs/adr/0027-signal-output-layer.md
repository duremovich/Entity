# ADR-0027: Signal Output Layer — timeline-driven OSC/plugin event dispatch

- **Status:** Accepted
- **Date:** 2026-06-02
- **Implemented by:** Signal Output Layer plan (`~/.claude/plans/plan-signal-output-layer.md`),
  Phases 1-9. Commits: on `master`.
- **Extends:** ADR-0016 (Timeline Layer abstraction — new Layer::Kind::Signal),
  ADR-0014 (Editor/show thread split — SignalOutputSystem placement).
- **Relates to:** ADR-0005 (open-core plugin scaffold — Apache/GPL boundary),
  ADR-0018 (content-layer unification — explicitly NOT a content layer),
  ADR-0012 (timeline sections — break-park suppression),
  ADR-0013 (control-plane plugins route via CommandDispatcher).

## Context

Live-show operators need to send OSC (and eventually DMX, MIDI) events at
precise playhead positions without writing custom scripts or patching the
signal path manually. The existing control-plane plugin model (ADR-0013)
handles operator-driven commands well but has no way to express
"fire at frame N during playback." A timeline-resident layer type is the
natural expression: place a cue (momentary) or a value-streaming range
(continuous) on the timeline, and have the show thread deliver the packets
to any registered plugin.

## Decision

### Layer archetype — NOT a content layer

A Signal Output Layer occupies a timeline track and has the ECS archetype:

```
Layer (Kind::Signal=3) + SignalLayer + AnimatedProperties
```

**Deliberately absent:** `MediaLayer`, `Transform`. A Signal layer produces
no texture and drives no compositor pass — it is a *control side-effect*,
not a visual source. This sharpens the ADR-0016/0018 boundary: any entity in
a `registry.view<ContentLayerSnapshot>()` or `view<GenerativeLayer>()` sweep
is a texture producer; Signal layers are invisible to those sweeps.

Rendering on the timeline: momentary (mode=Momentary) shows as a diamond
marker at startFrame; continuous shows as a colored span block.

### `bus::SignalEmit` contract

A new `bus::SignalEmit` message type (Phase 1) carries the complete outbound
event over the D2R bus:

```cpp
struct SignalArg { enum class Type{Int,Float,String}; Type type; int32_t i; float f; std::string s; };
struct SignalEmit { enum class Transport{OSC}; Transport transport; std::string address; std::vector<SignalArg> args; };
```

Enums serialize as strings per bus rule 2. `SignalArg` is defined early in
`Message.hpp` so `SignalLayerSnapshot` (which embeds `std::vector<SignalArg>`)
can be defined before `SceneSnapshot` without forward-declaration problems
(vectors require a complete type).

### GPL/Apache plugin-api boundary — `SignalEmitPod`

`bus::SignalEmit` lives in `entity-bus` (GPL). Plugin headers (`plugin-api/`)
are Apache-2.0. The plugin-api carries a primitives-only POD mirror:

```cpp
// plugin-api/include/entity/plugin/PluginContext.hpp
struct SignalArgPod { enum class Type{Int,Float,String}; Type type; int32_t i; float f; char s[128]; };
struct SignalEmitPod { char address[256]; SignalArgPod args[8]; std::size_t argCount; };
```

`Engine::postSignalEmit(const bus::SignalEmit&)` converts to pod internally so
the show-thread path never touches plugin-api types directly.

### Phase-2 inbound-drain seam — Mechanism B chosen

The show-thread evaluator needs to deliver `SignalEmit` events to registered
plugins (e.g. the `osc-sender` worker). Two candidate mechanisms were
evaluated:

**Mechanism A (rejected):** plugin calls `ctx->bus()->drain(Direction::R2D, ...)`.
`InMemoryMessageTransport::drain` steals the whole queue under a mutex.
`Engine::drainRendererToDirector` already owns the R2D drain on the editor
thread, consuming `CaptureCompleted`, `SectionBreakDetected`, and other
mandatory replies. A second consumer on R2D would starve the editor of these
messages — a data-loss race, not a latency concern. There are only two
directions and both are owned.

**Mechanism B (chosen):** append a new virtual method at the bottom of the
`IPluginContext` vtable — `drainSignalEmits(SignalEmitPod* out, std::size_t max)`
— backed by a mutex-guarded `std::queue<SignalEmitPod>` on `EnginePluginContext`.
The show thread calls `Engine::postSignalEmit()` which enqueues into this queue;
the `osc-sender` worker calls `drainSignalEmits()` at the top of each 33ms tick.

Implications:
- `PLUGIN_API_VERSION` bumped 0 → 1 to document the new extension point.
  All plugins rebuild together (static-link), so no runtime mismatch.
- Latency bound: up to one 33ms worker tick between emit and UDP send. Accepted
  for v1; a `wakeCv.notify_one()` path is a documented follow-on.
- The queue is capacity-capped at 256 entries. Saturation logs once per episode
  (not per drop) and outside the mutex to avoid blocking the drain path.

### Show-side evaluation — ADR-0014 compliant

`SignalOutputSystem::evaluate(signalLayers, prevFrame, curFrame, playing, atBreak, emitsOut)`
runs on the show thread, after the section-break detector, unconditionally (fires
through editor stalls). State (armed map, continuous rate-limit map) is show-thread-local;
nothing is written to the registry.

**Momentary crossing detector:**
- `crossed = playing && !atBreak && prevFrame < startFrame && curFrame >= startFrame`
- `armed = true` initially; reset when `curFrame < startFrame` (rewind).
- Fire once when `crossed && armed`; disarm until next re-arm.
- `atBreak` suppresses without consuming the arm — the next real forward crossing
  after GO fires exactly once.

**Continuous emitter:**
- Active while `curFrame ∈ [startFrame, startFrame+duration)` and `playing && !atBreak`.
- Rate-limited: one emit per distinct `curFrame` (rate-limit map per entity).
- Value sourced from `Progress` (normalized [0,1]) or `Keyframe` (linear-only in v1).
- Range-mapped: `srcMin..srcMax → outMin..outMax`, cast to `outType`.

**atBreak reset path:** `SeekCommand`, `SeekToStartCommand`, `SeekToEndCommand`,
`SeekToFrameCommand`, `loadProject`, `closeProject` all call
`Engine::requestSignalReset()` which sets `std::atomic<bool> m_signalResetPending`.
The show thread checks this flag at the top of its evaluate call, clears it, and
calls `m_signalOutputSystem.reset()` — safe because the maps are only mutated from
the show thread.

### Phase-4 bake — `SignalLayerSnapshot` on `SceneSnapshot`

`PlaybackTimeAuthority::buildSceneSnapshot` bakes active signal layers into
`SceneSnapshot::signalLayers` (editor thread, after the generative bake).

**Active window:**
- Continuous: `currentFrame ∈ [startFrame, startFrame+duration)`.
- Momentary: `currentFrame ∈ [startFrame-2, startFrame+duration+2]` — a ±2-frame
  straddling window. The headless `--script` loop runs at thousands of fps; a
  single tick can advance many frames, so exact equality (`== startFrame`) would
  silently drop cues whenever the tick skips over startFrame. The ±2 window ensures
  the layer appears in the snapshot for any reasonable single-tick advance.
  Phase-5's `crossed = prevFrame < startFrame && curFrame >= startFrame` detector
  fires correctly from within the window.

### Transport genericity

`SignalLayer::transport` field reserves the extension point; only `OSC = 0` is
implemented. `SignalLayer::Arg::Type` mirrors `SignalArg::Type` (Int/Float/String).
DMX channel, MIDI note, and ArtNet universe are reserved follow-ons.

## Consequences

### Positive

- **Any OSC target reachable via osc-sender is now scriptable from the timeline.**
  Momentary cues and continuous value streams both work with the standard plugin
  configuration (no code changes required on the plugin side).
- **ADR-0014 compliant.** Zero registry writes from the show thread; the evaluator
  is pure snapshot consumer.
- **ADR-0018 boundary preserved.** Signal layers never enter the compositor, never
  allocate compose targets, and are invisible to all content-layer views.
- **Break-park suppression correct.** `atBreak` suppresses momentary fire without
  consuming the arm; the first real forward crossing after SectionGo fires exactly
  once. Covered by `SignalOutputSystem` unit tests.
- **Project-file persistence.** Schema version 26 adds Signal layer round-trip
  (all fields: mode, transport, valueSource, address, args array, range-mapping).

### Negative / known limitations

1. **One-tick-late reset.** A seek landing inside a momentary window WHILE PLAYING
   can produce at most one spurious emit before the atomic `m_signalResetPending`
   flag is processed by the show thread (~16ms window). Self-recovering, not
   crash-class. The alternative — zeroing show-thread state from the editor thread
   — would violate ADR-0014. Seek-past-the-window is safe (layer not baked).

2. **Momentary drag-drop duration.** Layers created by dragging from the LayersWindow
   receive a default multi-second duration. Hit-testing and collision treat them as
   spans; they render as a diamond and fire once correctly. `duration=1` is the
   clean authored form. A UX follow-on (auto-set duration=1 + collision-exempt for
   Momentary mode) is logged but deferred.

3. **String arg truncation at plugin boundary.** `SignalArgPod::kMaxStringLen = 127`.
   String OSC args longer than 127 bytes are silently truncated at
   `Engine::postSignalEmit(const bus::SignalEmit&)`. The authoring UI does not
   surface this limit. A warning log + UI tooltip are follow-ons.

4. **Linear-only keyframe interpolation.** `SignalOutputSystem::evaluateKeyframeTrack`
   uses linear interpolation only (v1). The per-keyframe `interpolation` enum field
   is baked into `BakedTrack` but not honored by the signal evaluator. Honoring
   the full interpolation enum is a documented follow-on.

5. **Continuous latency to plugin.** Up to one 33ms `osc-sender` worker tick between
   `SignalOutputSystem` evaluation and UDP transmission. The producer does not
   `notify_one()` the worker condvar; a low-latency wake path is a follow-on.

6. **OSC only in v1.** `SignalLayer::Transport` reserves MIDI, DMX, ArtNet, but
   only `Transport::OSC` is wired. Other transports require new plugin integrations.

## Alternatives considered

### Deliver via an existing bus Direction (Mechanism A)

Rejected: `InMemoryMessageTransport::drain` steals the whole queue. The editor's
`drainRendererToDirector` already owns R2D; a second consumer would lose messages.
See "Phase-2 inbound-drain seam" above.

### A dedicated plugin bus Direction for signal traffic

Would require adding a third direction to `IMessageTransport` and updating every
transport implementation including the future UDP transport. The vtable append
(Mechanism B) achieves the same delivery with no new transport protocol.

### Timeline event trigger (fire-and-forget, not a layer)

A non-layer event object (e.g. a `CueTag`-style marker) would be simpler for
momentary cues but cannot express continuous value streaming. A layer with mode
discrimination covers both use cases in one authoring primitive.

### Emit from editor thread (inside CommandDispatcher or SectionScheduler)

Would tie cue timing to editor-thread health, violating the show-output >
editor-UI priority from the project guidelines. The show thread is the correct
home for anything that drives projector-adjacent output.

## References

- Plan: `~/.claude/plans/plan-signal-output-layer.md`
- [ADR-0005: Open-core dual-license + plugin scaffold](0005-open-core-dual-license.md)
- [ADR-0012: Timeline sections and cues](0012-timeline-sections-and-cues.md)
- [ADR-0013: Control-plane plugins route via CommandDispatcher](0013-control-plane-plugins-route-via-command-dispatcher.md)
- [ADR-0014: Editor/show thread split](0014-editor-show-thread-split.md)
- [ADR-0016: Timeline Layer abstraction](0016-timeline-layer-abstraction.md)
- [ADR-0018: Content-layer unification](0018-content-layer-unification.md)
- Implementation:
  - `include/entity/components/SignalLayer.hpp` (component)
  - `include/entity/bus/Message.hpp` (SignalArg, SignalEmit, SignalLayerSnapshot)
  - `src/bus/Serialization.cpp` (wire codecs)
  - `include/entity/systems/SignalOutputSystem.hpp` + `src/systems/SignalOutputSystem.cpp`
  - `src/core/Engine.cpp` (showThreadMain wiring, postSignalEmit overloads)
  - `src/director/PlaybackTimeAuthority.cpp` (buildSceneSnapshot bake)
  - `src/project/ProjectSerializer.cpp` (persistence, schema v26)
  - `plugin-api/include/entity/plugin/PluginContext.hpp` (SignalEmitPod, drainSignalEmits)
  - `src/core/EnginePluginContext.cpp` (queue impl)
  - `plugins/osc-sender/OscSenderPlugin.cpp` (drain + encode)
  - `src/ui/PropertyWindow.cpp` (authoring panel)
  - `src/timeline/TimelineWidgetRender.cpp` (diamond + span rendering)
- Tests:
  - `tests/unit/SignalEmitSerializationTest.cpp`
  - `tests/unit/SignalLayerTest.cpp`
  - `tests/unit/SignalSnapshotBakeTest.cpp`
  - `tests/unit/SignalOutputSystemTest.cpp`
  - `tests/unit/SignalCommandsTest.cpp`
  - `tests/unit/SignalPersistenceTest.cpp`
  - `scripts/integration/signal_momentary_osc.json` (deferred to physical console)
  - `scripts/integration/signal_continuous_osc.json` (deferred)
  - `scripts/integration/signal_break_park.json` (deferred)
