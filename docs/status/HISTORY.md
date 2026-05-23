# Development History

Detailed completion notes for Entity Media Server phases.

---

## Phase D: Feature work (in progress)

### Section-Break Behavior — Three Fixes (2026-05-23)

Three issues observed during real cueing once the section-break
detector-on-show / applier-on-editor split (NEW-08, 2026-05-20) and the
seek-sync preroll gate (ADR-0026, 2026-05-22) landed:

1. **Trailing-edge clip popped out at break with fadeSeconds = 0.** A
   clip whose right edge sat exactly on a section break disappeared the
   moment the playhead reached the break — should hold visible during
   the at-break park, then cut (or fade) on GO. Cause: `sectionFadeTail-
   Frames(endFrame)` returned `ceil(0 * fps) = 0`, collapsing
   `isClipActiveAtFrame`'s window to `[start, end)` and dropping the
   clip at `currentFrame == end`. With `fadeSeconds > 0` the held-
   visible behavior fell out for free since `ceil(...) >= 1`. **Fix**:
   `sectionFadeTailFrames` returns `max(1, ramp)` for any clip whose
   end aligns with a break, independent of fadeSeconds. The clip now
   stays visible for the at-break park frame, then drops on
   `currentFrame == end+1` when `fadeSeconds == 0` (instant cut after
   GO, matching the intended "hard cut" semantic) or follows the
   existing tail ramp when positive. Symmetric with the leading-edge
   at-break visibility gate. Commit `5ecd210`.

2. **Generative layers ignored section fades entirely.** Text and
   Muncher layers queued at a break popped on at full opacity instead
   of waiting invisible until GO. Cause: the generative fold-in to
   `bus::ContentLayerSnapshot` hardcoded
   `c.sectionFadeMultiplier = 1.0f` with a stale `// SectionScheduler
   integration is NEW-08` comment; the unified PASS 2 compositor's
   `opacity × sectionFadeMultiplier` multiply produced no envelope.
   **Fix**: lift `computeSectionFadeMultiplier` off `Clip` — the body
   now takes `(FrameNumber layerStart, FrameNumber layerEnd)` as pure
   timeline math, no component dependence. The `Clip` overload is a
   one-line trampoline so the existing call sites are unchanged. Wire
   the generative fold-in to call the overload with
   `(gl.startFrame, gl.startFrame + gl.duration)`. Extend the
   generative snapshot filter in `buildSceneSnapshot` the same way
   `isClipActiveAtFrame` was extended in Fix 1 — include
   `sectionFadeTailFrames(endFrame)` in the upper bound so trailing-
   edge generatives stay in the snapshot during the at-break park.
   Generatives now get the same at-break visibility gate, the same
   fade-in/fade-out ramps, and the same trailing-edge hold clips do.
   Commit `28c69b5`.

3. **Cold-decoder hitch after GO at a queued clip.** Clips queued at a
   break had no decode worker until GO fired — the cold FFmpeg open +
   seek + first-frame decode (~50–300 ms on 4K ProRes) ran inside the
   seek-sync gate, producing a visible delay before motion. Same hitch
   hit any GO path: Spacebar after parking, cue-jump straight into the
   next section, scrub-then-Play. **Fix**: continuous sliding-window
   prefetch in `DecodeSystem::update`. Every editor tick (when not
   `Stopped`) sweep `view<Clip, FrameBuffer>` and bootstrap a worker
   for any not-yet-started clip whose `startFrame` is within
   `kPrefetchAheadSeconds` (5 s) of the playhead. The worker opens
   FFmpeg, seeks to `mediaStartFrame`, and pre-fills its ring buffer
   in the background. When the playhead (or cue-jump) reaches the
   clip, the seek-sync gate releases on the first tick because
   `isClipReadyAt` is already true. Naturally covers all three
   approach-direction cases — no SectionScheduler wiring, no Timeline
   helper, the sliding window subsumes the natural-break, at-break-
   parking, and cue-jump cases. Editor-thread only (writes
   `m_workers`); runs while `Paused` so an operator who pauses,
   scrubs, then Plays still benefits. Tracy zone:
   `DecodeSystem::prefetchUpcoming`. Commit `6297353`.

**Files.** `src/director/PlaybackTimeAuthority.cpp` +
`include/entity/director/PlaybackTimeAuthority.hpp` (Fixes 1 + 2),
`src/systems/DecodeSystem.cpp` +
`include/entity/systems/DecodeSystem.hpp` (Fix 3). New unit cases in
`tests/unit/PlaybackTimeAuthorityTests.cpp` (Fix 1: 3 cases) and
`tests/unit/SectionFadeTests.cpp` (Fix 2: 3 cases). 611/611 ctest
green at `-j 1` after all three.

**Fix 3 follow-up — the actual cause was LRU eviction, not cold
bootstrap (commit `f15ad17`).** Manual verification showed the hitch
still firing after Fix 3 with full 3-second `SeekSyncController`
preroll timeouts on a project of two clips referencing the same
source MP4. Diagnostic logging added to the gate readiness predicates
revealed the failing worker was *already* initialized, not mid-seek,
and `worker->currentFrame` was past the requested `mediaFrame` — the
FrameCache simply did not have the frame. The misdiagnosis came from
not recognising that `DecodeSystem::update`'s existing bootstrap path
(line ~82, predates Fix 3) already creates a worker for every loaded
`Clip + FrameBuffer` entity regardless of activity. Fix 3's sliding-
window prefetch was therefore a no-op in practice — by the time the
prefetch sweep ran each tick, `m_workers.contains(entity)` was already
true. What actually broke playback was the global `FrameCache` LRU:
while Clip A plays for 100 seconds at 4K (huge per-frame data), it
evicts every one of Clip B's pre-decoded frames. Clip B's worker still
thinks it has decoded those frames (its `nextFrame` is past the
eviction point), so it idles on backpressure waiting for `targetFrame`
to advance — it never re-decodes. **Fix 4** adds cache-miss recovery
in `DecodeSystem::update`: when a clip is active and
`worker->currentFrame >= mediaFrame` but `!cache.has(entity, mediaFrame)`,
force a `seekClip(entity, mediaFrame)`. The decode thread's existing
fast path (`DecodeSystem.cpp:555`) skips the actual decoder seek when
the cache happens to be hot, so this is cheap in the steady state.
Audio side: same parallel-shape prefetch sweep added to
`AudioSystem::update` so audio workers are bootstrapped in lockstep
with video for late-load scenarios (also mostly a no-op given the
existing bootstrap path, but kept as a safety net). Diagnostic logs
on the gate predicates rate-limited to 1 line/sec/entity, left in
place — fires only on actual stalls and prints enough state
(`workerCurrent` / `target` / `lastReq`) to debug future regressions
without adding more printlns. **Lesson:** seek-sync preroll timeouts
in a multi-clip project with long-form content should look at LRU
eviction first, not cold-bootstrap. The cache contract was
"once-decoded, stays decoded for a while"; in practice it's whatever
fits in the budget at this instant.

### Seek-Sync Preroll Gate — ADR-0026 (2026-05-22)

Adds a seek-sync preroll gate so playback after a seek starts cleanly. On
Play, the timeline playhead is held parked at the target frame (audio
silent) until every active decoder has genuinely caught up to that frame,
then released — both streams begin from the same frame, instead of with a
glitchy interval while the video decoder decodes forward from its GOP
keyframe and the audio worker re-seeks and prerolls its ring.

Scope note: the plan (`tidy-nibbling-kurzweil` Part 2) was originally
motivated by a multi-second mid-clip-seek desync (audio seconds ahead of
video). That turned out to be a separate `ProResDecoder` inter-frame-seek
bug — it returned the GOP keyframe mislabeled as the target frame — fixed
independently in commit `12590ae`. The preroll gate does not fix that
desync; it relies on decoders reporting their position honestly. Its role
is the clean-start one above.

Implemented in four phases (plan `tidy-nibbling-kurzweil` Part 2, Phases A-D):

**Phase A — Gate plumbing.** `std::atomic<bool> m_seekSyncGate` added to
`Timeline`. `Timeline::play()` sets the gate on every `->Playing`
transition. `update()` only advances `m_currentTime` when the gate is
clear (acquire order). `pause()`, `stop()`, `seekToFrame()`, and
`clear()` all clear the gate so a stale hold can never deadlock the
transport. `isSeekSyncGated()` and `setSeekSyncGate()` exposed as public
accessors for `SeekSyncController`.

**Phase B — Readiness queries + AudioSystem decouple.**
`DecodeSystem::isClipReadyAt(entity, mediaFrame)` — returns true when the
worker is initialized, not mid-seek, and `FrameCache::has` the target
frame. `AudioSystem::isWorkerSeekReady(entity)` — returns true when the
audio worker is initialized, not mid-seek, and the ring buffer holds at
least `kAudioPrerollFrames = 2048` samples (~43 ms @ 48 kHz).
`AudioSystem::getWorkerSeekTargetFrame(entity, clipFps)` — converts the
worker's sample-domain `seekTarget` to a media frame number for
integration-test assertion. AudioSystem's mixer-gating decoupled: a
separate `shouldSteer` predicate drives worker steering; `mixSource.active`
is additionally gated by `!isSeekSyncGated()` so audio is silent during
the preroll hold without suppressing the seek itself.

**Phase C — SeekSyncController.** New plain class (not an ECS System)
`SeekSyncController` lives on `Engine` (the only object that can name
both Director-side and Renderer-side subsystems per ADR-0014/ADR-0003).
Injectable `std::function<bool()>` readiness predicates let unit tests
supply fakes without standing up a full Engine. `tick(Timeline*)` is a
no-op when the gate is clear; when active, polls `videoReady` (∀ active
Clips: `isClipReadyAt`) and `audioReady` (∀ active Clip+AudioSources:
`isWorkerSeekReady`); releases the gate when both return true. Timeout
failsafe: releases with a warning log after `kPrerollTimeoutMs = 3000` ms
so a decoder that cannot reach the target frame (missing media, seek
error) never hangs playback indefinitely. Wired in `Engine::initialize()`
after AudioSystem; ticked from `Engine::update()` at step 5.6, right after
`AudioSystem::update` (step 5.5).

**Phase D — Tests, ADR, docs.** Eleven `SeekSyncControllerTest` unit tests
exercise the gate state machine through injectable predicates:
no-op-when-clear, release-on-first-tick, release-after-N-ticks,
null-predicate-counts-ready, timeout-release, pause/stop/seek-during-
preroll-clear, re-engage-after-release, null-Timeline-no-crash. Two
integration scripts: `seek_sync_mid_clip.json` (seeks to frame 60, plays,
asserts video near frame 60 + audio steered near frame 60) and
`seek_sync_frame_zero.json` (plays from frame 0, asserts playback advanced
— the no-regression gate for a keyframe-cold start). New script command
`AssertAudioWorkerSeekFrame` asserts the audio worker's seek-target frame
with tolerance. ADR-0026 written.

**Files.** `include/entity/core/SeekSyncController.hpp` (new),
`src/core/SeekSyncController.cpp` (new), `include/entity/core/Engine.hpp`
(`m_seekSyncController` member + fwd decl), `src/core/Engine.cpp`
(readiness closures + tick call), `include/entity/timeline/Timeline.hpp`
(`m_seekSyncGate` + accessors), `src/timeline/Timeline.cpp`
(gate engage/clear sites), `include/entity/systems/AudioSystem.hpp`
(`isWorkerSeekReady`, `getWorkerSeekTargetFrame` declarations),
`src/systems/AudioSystem.cpp` (implementations + decouple),
`include/entity/command/Commands.hpp`
(`AssertAudioWorkerSeekFrameCommand`),
`src/command/Commands.cpp` + `src/command/CommandDispatcher.cpp`
(implementation + registration), `tests/unit/SeekSyncControllerTest.cpp`
(new), `scripts/integration/seek_sync_mid_clip.json` (new),
`scripts/integration/seek_sync_frame_zero.json` (new),
`tests/CMakeLists.txt` (unit + integration registrations),
`docs/adr/0026-seek-sync-preroll-gate.md` (new),
`docs/adr/README.md` (index entry),
`docs/reference/SYSTEM_ORDERING.md` (step 5.6 + fallback table row),
`scripts/CLAUDE.md` (`AssertAudioWorkerSeekFrame` command doc).
`CMakeLists.txt` (`SeekSyncController.cpp` source registration).

---

### Continuation Clock Onto RateSource — Phase G (2026-05-21)

Migrated the section-break continuation-phase wall-clock anchor from raw
`std::chrono::steady_clock` onto the active `RateSource` so the main
timeline and continuation phase share one clock domain (the audio crystal
when `AudioRateSource` is active).

**G1 — SectionScheduler injection.** `SectionScheduler` gained a
`setTimeAuthority(PlaybackTimeAuthority*)` public method and a
`m_timeAuthority` private member. The anonymous-namespace helper
`steadyNowNs()` in `SectionScheduler.cpp` was removed; both call sites
(`seedContinuationAt` and `advanceContinuation`) now compute
`int64_t(m_timeAuthority->rateNow() * 1e9)` when the authority is wired,
and fall back to `int64_t{0}` (dt-accumulator path) otherwise. The
`<chrono>` include in `SectionScheduler.cpp` was removed. `Director.cpp`
calls `m_sectionScheduler->setTimeAuthority(m_timeAuthority.get())` after
both objects are constructed.

**G2 — Show-side re-derivation.** `mapToMediaFrameFromCatalog` (file-
local free function in `PlaybackTimeAuthority.cpp`) gained a `nowNs`
parameter. The previous `std::chrono::steady_clock::now()` call inside the
function was replaced with the caller-supplied `nowNs`. `buildRenderFrame`
snapshots `rateNow() * 1e9` once per render frame into `rateNowNs` and
passes it to every `mapToMediaFrameFromCatalog` call, so all clips in one
frame share the same instant. The `<chrono>` include in
`PlaybackTimeAuthority.cpp` was removed (no remaining usages).

**G3 — Behaviour preservation.** All 47 section-related tests pass
(ctest `-j 1 -R "section|Section"`) with no changes to any test or
production logic.

**G4 — New test.** `scripts/integration/audio_loop_break_synced.json`:
Loop+Normal tone clip, section break at 2 s, play past break to park,
`ClearAudioCapture`, wait 2 s while parked, `AssertAudioCaptureRms 0.005`.
Verifies the continuation loop keeps producing audio after the break on the
shared clock. Registered in `tests/CMakeLists.txt` with `TIMEOUT 30`.

**Files.** `include/entity/director/SectionScheduler.hpp` (forward decl +
`setTimeAuthority` public + `m_timeAuthority` private member),
`src/director/SectionScheduler.cpp` (`PlaybackTimeAuthority.hpp` include,
`<chrono>` removed, `steadyNowNs` helper removed, two call sites rerouted),
`src/director/Director.cpp` (wiring call),
`src/director/PlaybackTimeAuthority.cpp` (`<chrono>` removed,
`mapToMediaFrameFromCatalog` gets `nowNs` param, `buildRenderFrame` snapshots
`rateNowNs`),
`scripts/integration/audio_loop_break_synced.json` (new),
`tests/CMakeLists.txt` (new test registration).

### SectionScheduler Show-Thread Detection — NEW-08 closed (2026-05-20)

Closed the last editor-stall gap. `SectionScheduler::tick` was the sole
section-break crossing detector and ran only on the editor thread, so an
editor stall (modal drag, slow project load) let the playhead sail past a
break — the cue fired late with a visible glitch — and froze continuation
phase for clips parked at a break.

**Show-thread detection.** Break-crossing detection moved into
`Engine::showThreadMain` (the `SectionDetect` Tracy zone), which runs every
show frame regardless of editor health. When playing and not already
at-break, it finds the first `Timeline::Section::breakFrame` the playhead
crossed since the last show frame, snaps + pauses the playhead, raises
`Timeline::sectionAtBreak()`, and posts a new R2D `bus::SectionBreakDetected`
message. Sections are read live via `Timeline::copySectionsAndRate()`
(already show-safe); detector state is two show-thread-local vars. A
discontinuity guard (`max(2 frames, 5×dt)`) skips command-seeks.

**Editor-side apply.** `SectionScheduler::handleBreakAt(Timecode)` —
extracted from the old `tick()` crossing branch — is called from
`Engine::drainRendererToDirector` on the `SectionBreakDetected` drain. It
runs the registry-mutating catch-up on the editor thread (the sole registry
writer per ADR-0014): raises the scheduler at-break latch and seeds
`ClipPlaybackPhase`. Staleness guard: drops the message if a seek or Play
landed in the gap (`sectionAtBreak()` cleared, or state no longer Paused).
Crossing detection was **removed from `tick()` entirely** — one detector,
one applier, no dual-detector race.

**Continuation phase survives a stall.** `bus::ClipCatalogEntry` gained
`phase_continuationStartTimeNs` / `phase_continuationSeedFrames` (baked from
the existing `ClipPlaybackPhase` wall-clock anchor). The show-side
`mapToMediaFrameFromCatalog` re-derives the live source phase from the
anchor + `steady_clock::now()` instead of the snapshot-frozen
`phase_sourcePhaseFrames`, so Loop/PingPong clips keep cycling at a parked
break while the editor is stalled. `SectionScheduler::go()` calls
`advanceContinuation(0.0)` first so a GO after a stall snapshots the phase
the user actually saw, not a stale seed value.

Implemented per `docs/design/section-scheduler-snapshot-bake.md` with two
divergences (show-only detection; live `copySectionsAndRate()` instead of a
baked section list) — see that doc's header.

**Files.** `include/entity/bus/Message.hpp`, `src/bus/Serialization.cpp`
(2 `ClipCatalogEntry` fields + `SectionBreakDetected` message),
`include/entity/director/SectionScheduler.hpp` + `src/director/SectionScheduler.cpp`
(`handleBreakAt`, `tick()` detection removed, `go()` phase recompute),
`src/director/PlaybackTimeAuthority.cpp` (bake + show-side derivation),
`src/core/Engine.cpp` (show detector + R2D drain arm),
`include/entity/timeline/Timeline.hpp` (threading comment).
New test: `scripts/integration/section_break_show_detect.json`.

### OSC Outbound Sender + Mapping Table (2026-05-19)

Ten-phase implementation (Phases 1-10). Adds an outbound OSC sender plugin that
broadcasts Entity transport and section state to external show-control software
at 30 Hz; refactors the existing inbound receiver to table-driven dispatch with
per-project route overrides; and adds a per-project OSC mapping editor in the
Show Control window.

**IPluginContext transport accessor (Phase 1).** New
`IPluginContext::getTransportSnapshot()` returns a `TransportSnapshot` struct
(state enum, current frame, seconds, active/next section indices and frames,
project name) snapshotted from the editor thread. Implemented in
`EnginePluginContext::getTransportSnapshot` by reading atomic fields on
`Timeline` and `ProjectManager`. Additive at the vtable bottom; no
`PLUGIN_API_VERSION` bump. `TransportState` enum: `Stopped`, `Playing`, `Paused`.

**Outbound sender plugin (Phases 2-4).** New `plugins/osc-sender/` plugin
(Apache-2.0). Opens a `SOCK_DGRAM` UDP send socket; spawns a 30 Hz worker that
polls `getTransportSnapshot()`, diffs against last-sent state, and broadcasts
OSC packets to all enabled destinations. Nine default events: `transport.state`
(string, on-change), `transport.frame` (int32, 30 Hz when playing),
`transport.seconds` (float32, 30 Hz when playing), `section.active.index/frame`
(int32, on-change), `section.next.index/frame` (int32, on-change),
`project.name` (string, on-change), `heartbeat` (int32 counter, 1 Hz).
Multiple events per tick coalesced into one `#bundle` packet.
`oscSenderEnabled` + `oscSenderDestinationsJson` added to `Settings.json` and
the Settings UI (Preferences > OSC Sender section). Destinations JSON shape:
`[{"host":"...","port":N,"enabled":true/false}, ...]`. Settings UI shows a
per-row host/port/enabled table with Add/Remove.

**Per-project mapping table (Phases 5-6).** `oscInboundMappingsJson` and
`oscOutboundMappingsJson` string blobs added to `ProjectManager` (loaded and
saved with the project; default empty). The receiver plugin is refactored from
hardcoded if/else dispatch to a runtime route table built from the per-project
JSON. Table hot-reloads on hash change every 250 ms (between `recvfrom` timeout
cycles) without restarting the socket or the worker. Inbound JSON shape: bare
top-level array of `{address, captureKey?, commands:[{type, params}]}` objects.
Params templates support four substitution tokens: `$arg0i`, `$arg0f`,
`$capturei`, `$capturef`; any unresolvable token logs Warn + noops the command
(Phase 6 arg-missing fix — `/entity/seek` with no numeric arg drops cleanly
instead of seeking to frame 0). Outbound JSON shape:
`{"events":{"<id>":{"enabled":bool,"address":"..."}}}`.

**OSC Mappings UI (Phase 8).** Two new tabs ("OSC In", "OSC Out") added to the
existing `ShowControlWindow` (Window > Show Control). OSC In tab: editable
table of routes (address + command type + params string); Add/Remove row
buttons; params cell renders with a red background when it holds syntactically
invalid JSON (`nlohmann::json::accept` check). OSC Out tab: fixed 9-row table
one row per default event, address override (InputTextWithHint showing the
default as hint when empty) + enabled checkbox; Restore Defaults clears the
JSON blob. Both tabs write directly to the per-project `oscInbound/
OutboundMappingsJson` blobs via `ProjectManager`; changes take effect on the
next receiver hot-reload cycle or sender tick.

**Shared headers + tests (Phase 9).** Wire-format encode helpers lifted into
`plugins/osc-sender/OscWire.hpp` (`namespace osc`, no Winsock); route-table
logic lifted into `plugins/osc-receiver/OscInboundMappings.hpp`
(`namespace osc_inbound`, no plugin-api). `OscReceiverPlugin.cpp` now includes
the shared header and deletes its inline duplicate (~215 lines removed).
`OscMappingTableTest.cpp` therefore exercises the live receiver code, not a
parallel copy. Two GTest files: `OscEncodingTest.cpp` (15 tests — int32/float32
round-trips, string padding, full message and bundle build+parse) and
`OscMappingTableTest.cpp` (30 tests — JSON parse, default-routes fallback,
literal and capture `matchPattern`, `expandTemplate` with all four tokens plus
nullopt paths). `scripts/osc_smoke_listen.py` (stdlib-only, ~111 lines) decodes
and pretty-prints inbound OSC packets including bundles and nested bundles.

**Files (new).** `plugin-api/include/entity/plugin/PluginContext.hpp` (modified
for `getTransportSnapshot`), `plugins/osc-sender/OscSenderPlugin.cpp`,
`plugins/osc-sender/OscWire.hpp`, `plugins/osc-sender/manifest.json`,
`plugins/osc-sender/CMakeLists.txt`,
`plugins/osc-receiver/OscInboundMappings.hpp`,
`plugins/osc-sender/README.md`, `plugins/osc-receiver/README.md`,
`tests/unit/OscEncodingTest.cpp`, `tests/unit/OscMappingTableTest.cpp`,
`scripts/osc_smoke_listen.py`.

**Files (modified).** `include/entity/core/EnginePluginContext.hpp`,
`src/core/EnginePluginContext.cpp`, `include/entity/core/Settings.hpp`,
`src/core/Settings.cpp`, `include/entity/project/ProjectManager.hpp`,
`src/project/ProjectManager.cpp`, `src/project/ProjectSerializer.cpp`,
`include/entity/timeline/Timeline.hpp`, `src/timeline/Timeline.cpp`,
`include/entity/ui/ShowControlWindow.hpp`, `src/ui/ShowControlWindow.cpp`,
`src/ui/SettingsWindow.cpp`, `src/command/Commands.cpp`,
`src/director/PlaybackTimeAuthority.cpp`,
`plugins/osc-receiver/OscReceiverPlugin.cpp`,
`tests/CMakeLists.txt`.

---

### Text Generator Layer (2026-05-18)

Seven-phase implementation (Phases 1-7). Adds a new Generative sub-kind
(`TextLayerState` presence = Text) to the compositor's content-layer pipeline.
Text layers render authored strings to a video-pool texture via DirectWrite +
Direct2D (Windows) and composite through the existing PASS 2 path, indistinguishable
from other content-layer kinds.

**Foundation (Phases 1-2).** New `TextLayerState` component
(`include/entity/components/TextLayerState.hpp`): `text`, `fontFamily`, `fontSize`,
`color (vec4)`, `alignment`, `bold`, `italic`, runtime `dirty`/`textureSlot`/
`bakedWidth`/`bakedHeight`. New `TextRasterizer` (`src/render/TextRasterizer.cpp`):
DirectWrite factory + text format, D2D render target over a video-pool CPU-mapped
surface. `TextSystem` (editor-thread only, step 3.5) walks
`view<GenerativeLayer, TextLayerState>()`, rasterizes dirty entries, updates
`textureSlot`. On-destroy observer (`on_destroy<TextLayerState>`) frees the
video-pool slot on entity deletion. Bus snapshot extended: `TextLayerSnapshot`
in `bus::SceneSnapshot` carries `textureSlot`; compositor PASS 2 routes it via
the `Compose` source-kind path (same descriptor pool as Generative PASS 1 targets).

**Editor integration (Phases 3-4).** `Engine::createTextLayer` + undoable
`CreateTextLayerCommand` (JSON: `CreateTextLayer`). LayersWindow "Text" drag-source.
PropertyWindow Text panel: live-editable text, font family, font size (with DragFloat),
color picker, alignment radio, bold/italic checkboxes. All edits go through
`SetTextContent` / `SetTextFont` etc. undoable commands so Ctrl+Z works per-property.

**Persistence (Phase 5).** Project schema v21. `ProjectSerializer` save branch emits
`kind="generative"`, `sub_kind="text"`, `text_state` object with all authoring fields.
Load branch creates `Layer(Kind::Generative)` + `GenerativeLayer` + `Transform` +
`MediaLayer` + `TextLayerState(dirty=true)` for Text entries; `MunchersGameState` for
Muncher entries. Dead-code guard removed; clean branch structure with an explicit
`// only 'clip' entries reach this branch` comment.

**Delete / copy / paste generalization (Phase 6).** `Timeline::DeletedLayerKind` enum
extended with `Generative=2`. `DeletedClipSnapshot` gains Generative payload
(`genLayer`, `hadMuncher`, `munchersState`, `hadTextLayerState`, `textLayerState`).
`snapshotClipForDelete` / `restoreDeletedClip` handle all three kinds. Clipboard
snapshot / materialize restructured to an if/else (OA | Generative | Clip) with a
shared tail for EffectChain deep-clone + ContentRoutingRef shallow-copy, so no ops
silently drop those components for any kind. `TimelineWidgetInput` cut/copy gate
changed to `isCopyable = hasIdx && !OA`, making it kind-blind for future kinds.

**Integration tests + docs (Phase 7).** New script commands: `Undo`,
`AssertTrackLayerCount`, `AssertTextLayerState`, `SetTextLayerProperties`.
Round-trip test (`text_layer_persistence_save/load.json`): creates Text layer with all
fields set, saves v21 project, loads and asserts every field survived.
Generative layer ops test (`generative_layer_ops.json`): delete + undo for Muncher
and Text, copy + paste for both (including `AssertTextLayerState` on pasted entity
to confirm deep-copy). `ENTITY_ARCHETYPES.md` updated with Text sub-kind table and
delete/copy-paste notes. `SYSTEM_ORDERING.md` updated with TextSystem at step 3.5
and show-thread fallback table row. `scripts/CLAUDE.md` updated with all new commands.

**Files (new).** `include/entity/components/TextLayerState.hpp`,
`include/entity/systems/TextSystem.hpp`, `src/systems/TextSystem.cpp`,
`src/render/TextRasterizer.hpp`, `src/render/TextRasterizer.cpp`,
`scripts/integration/text_layer_create.json`,
`scripts/integration/text_layer_persistence_save.json`,
`scripts/integration/text_layer_persistence_load.json`,
`scripts/integration/generative_delete_smoke.json`,
`scripts/integration/generative_layer_ops.json`.

**Files (modified).** `include/entity/components/CLAUDE.md`,
`include/entity/command/Commands.hpp`, `src/command/Commands.cpp`,
`src/command/CommandDispatcher.cpp`, `src/core/Engine.cpp`,
`src/timeline/Timeline.cpp`, `src/timeline/TimelineWidgetInput.cpp`,
`include/entity/timeline/Timeline.hpp`,
`include/entity/project/ProjectSerializer.hpp`,
`src/project/ProjectSerializer.cpp`,
`include/entity/bus/Message.hpp`, `src/bus/Serialization.cpp`,
`src/ui/PropertyWindow.cpp`, `src/ui/LayersWindow.cpp`,
`tests/CMakeLists.txt`,
`docs/reference/ENTITY_ARCHETYPES.md`,
`docs/reference/SYSTEM_ORDERING.md`,
`scripts/CLAUDE.md`.

---

### Content Routing Library + Feed Maps (2026-05-17)

ADR-0022 (six commits, L1-L5 + docs). Promotes Plane A content routing
from inline `ContentRouting` component (ADR-0021) to a **library of
named, reusable `ContentRoutingAsset` entities** that clips reference
via `ContentRoutingRef`. Adds a third routing kind, **Feed Map**, for
named-region authoring of LED-wall content.

**Data model.** New components in `include/entity/components/`:
`ContentRoutingAsset` (library entity with `kind`, `targets`,
`autoBoundScreen`, source size, autosync bookkeeping),
`ContentRoutingRef` (per-layer pointer to an asset). Helpers in
`ContentRoutingAssetOps.hpp` (`ensureAutoDirectAsset`,
`setLayerTargetScreen`, `setLayerCustomRouting`, `tryGetAsset`).
`RouteMode` extends to `{Direct, Tiled, FeedMap}`; `RouteTarget` gains
optional `name` for Feed Map region labels.

**System.** New `RoutingLibrarySystem` (editor-thread only) reconciles
the library every tick: creates one auto-direct asset per Screen,
name-syncs from `Screen::name` until the user diverges it
(`lastSyncedScreenName` bookkeeping), cascade-deletes orphan auto-direct
entries when a Screen is destroyed, clears any `ContentRoutingRef::asset`
that points at the deleted entry. Wired into `Director` alongside
`AnimationSystem`; ticked from `Engine::update` right after animation.

**Snapshot bake.** `PlaybackTimeAuthority::buildSceneSnapshot` resolves
`ContentRoutingRef → ContentRoutingAsset` on the editor thread before
populating `bus::ContentLayerSnapshot.routes`. The bus wire format
from ADR-0021 is unchanged — the show thread has zero awareness of the
asset library.

**UI.** `PropertyWindow`'s "Target Screen" relabels to "Content Routing"
and sources its dropdown from the library (auto-direct entries
alphabetical by Screen name, then user-created; "Default (All visible)"
sentinel at top). `ContentRoutingWindow` is a two-pane library
browser: "+ Add" with Direct / Tiled / Feed Map options, right-click
Delete with usage-count confirm dialog, per-kind detail editor on the
right with name input, kind dropdown, kind-specific authoring extras
(Tiled count/axis wizard; Feed Map source canvas size + Export
Template), targets table, and an interactive canvas.

**Feed Map.** Source canvas resolution + named per-target regions.
"Export Template..." writes an SVG to `<cwd>/.feed-templates/<name>.svg`
with outlined rects + region/screen labels — the deliverable for
content creators authoring matching source content. Targets table
displays pixel coordinates when kind = FeedMap (DragInt scaled by
`sourceWidth`/`Height`); UV storage and wire format unchanged.

**Canvas preview (L5).** When a clip is selected in the timeline, the
canvas background renders the clip's most-recently-uploaded video
frame. New `IRenderer::getVideoTextureIDForSlot(slot)` exposes the
per-slot SRV via `TextureUploader::gpuHandle`. Drag any region body
on the canvas to move it; drag any of four corner handles to resize.
Overlapping regions are allowed by design (content that repeats one
source slice to multiple screens). Hover tooltip shows pixel
coordinates for Feed Map, UV for Tiled.

**Persistence.** Project schema v19 → v20. New top-level
`contentRoutingAssets` array; per-layer `contentRoutingAssetName`
reference. v19 inline `contentRouting` JSON still read for one-version
backward compat. Migration on load: single-target Direct → existing
auto-direct asset; multi-target → fresh "Custom Routing N" asset;
empty targets → null ref ("all visible"). All four existing
`ContentRouting*` round-trip tests still pass.

**Files.** New: `ContentRoutingAsset.hpp`, `ContentRoutingRef.hpp`,
`ContentRoutingAssetOps.hpp`, `RoutingLibrarySystem.{hpp,cpp}`. Touched:
`PlaybackTimeAuthority.cpp` (bake), `PropertyWindow.cpp` (dropdown +
kind badge), `ContentRoutingWindow.{hpp,cpp}` (full rewrite to library
browser + canvas authoring), `Commands.cpp`
(`applyClipTargetScreen` + `applyContentRoutingSpec` route through
helpers), `ProjectSerializer.cpp` (v20 schema + v19 migration),
`Director.{hpp,cpp}` + `Engine.{hpp,cpp}` (system wiring),
`IRenderer.hpp` + `D3D12Renderer.{hpp,cpp}` (poster-frame slot
accessor), CMakeLists.txt, mock IRenderer in
`DirectorRendererRoundtripTests.cpp`. ADR `docs/adr/0022-...md` and
`ENTITY_ARCHETYPES.md` updated.

**Known deferred / out-of-scope** (not filed as roadmap cards):
`UndoableCommand` subclasses for library mutations (currently direct
registry writes — no undo for Create/Delete/Rename/SetField); drag-on-
empty-canvas to *create* regions ("+ Add Region" + table is the
authoring entry today); custom-font load to support real Unicode
glyphs in ImGui labels; region-overlap warnings (intentionally
allowed); off-canvas warnings.

**Commits:** `daa6450` (L1) → `e1cb249` (L2) → `279f673` (L3) →
`95e93e6` (L4-minimal) → `3cb8071` (docs) → `9da42b1` (L5).
510/510 ctest green.

---

### Editor/Show Thread Split (2026-05-08)

Issue #42 (five stages). Editor thread is now the **sole registry writer**.
Show thread owns GPU compositing and output Present; editor thread owns
registry, ImGui, project I/O, and command dispatch. Modal editor loops
cannot stall the projector pipeline.

**Stage 1** — Per-role D3D12 command allocators + fences
(`m_editorAllocators`, `m_showAllocators`, `m_editorFence`, `m_showFence`).
`beginShowFrame`/`endShowFrame` + `beginEditorFrame`/`endEditorFrame` replace
the deprecated `beginFrame`/`endFrame` single-list path throughout the
codebase. Both submit to the same direct command queue (D3D12-spec
thread-safe for `ExecuteCommandLists`/`Signal`).

**Stage 2** — `bus::SceneSnapshot` + `bus::RenderFrame`. Editor thread bakes
all per-clip state into `SceneSnapshot::clipCatalog` (`ClipCatalogEntry` per
clip: slot, opacity, blendMode, transformMatrix, targetScreen, zOrder,
sectionBehavior, …). Show thread calls `buildRenderFrame` from the snapshot
with zero registry reads for clip data.

**Stage 3** — Show thread spawned in `Engine::run`. `D2RChannel` carries
`RenderFrame` with latest-wins delivery (old snapshot superseded by new one
before the show thread drains). Show thread: drain D2R → `buildRenderFrame` →
`compositor->update` → `outputManager->render` → `endShowFrame`. Editor thread:
`buildSceneSnapshot` → send D2R → drain R2D → ImGui → `endEditorFrame`.

**Stage 4** — `Affinity` enum on `Command`; `processQueue(engine, affinity)`
skips wrong-affinity commands and re-queues them in order. Show thread drains
`Affinity::Show` (Play, Pause, Seek, SectionGo, …). Editor drains
`Affinity::Editor` + `Affinity::Either`. `ScreenRenderTargetAllocated` R2D
reply eliminates the last show-thread registry write: compositor posts it when
allocating a compose-target slot; editor writes `Screen::renderTargetSlot` in
`drainRendererToDirector`. `CreateOutputWindowRequest`/`OutputWindowReady`
bus types stub the GLFW/swap-chain handshake for output windows (wiring
deferred). `videoTex->descriptorSlot` data race fixed by gating on
`crs->slot >= 0` (baked from editor thread) instead of reading
`VideoTexture::descriptorSlot` on the show thread.

**Stage 5** — Remove deprecated `beginFrame`/`endFrame`; write ADR-0014;
update `CODE_ISSUES.md` (HIGH-02 fully closed, NEW-06 updated); update
`include/entity/bus/CLAUDE.md` with new message types.

**Files**: `src/core/Engine.cpp`, `src/render/D3D12Renderer.cpp`,
`include/entity/render/D3D12Renderer.hpp`, `include/entity/render/IRenderer.hpp`,
`src/systems/CompositorSystem.cpp`, `include/entity/systems/CompositorSystem.hpp`,
`src/command/CommandDispatcher.cpp`, `include/entity/command/Commands.hpp`,
`include/entity/bus/Message.hpp`, `src/bus/Serialization.cpp`,
`docs/adr/0014-editor-show-thread-split.md`.

---

### Tracy Profiler Integration (2026-05-10)

Issue #43 (eight phases). Live CPU/GPU profiling via Tracy 0.13.1 (`on-demand`
feature only; `gui-tools` excluded — POSIX `MAP_FAILED` breaks MSVC). See
ADR-0015 for full rationale and architecture.

**Build integration.** `ENTITY_ENABLE_TRACY` CMake option (default ON) gates
the dependency. `PUBLIC` compile definitions on `EntityMediaCore` propagate to
`EntityMediaEditor` and test targets. `entity-plugin-api` is intentionally
excluded. Tracy.exe is obtained from the v0.13.1 GitHub release, not vendored.

**Wrapper header.** `include/entity/profile/Tracy.hpp` — single include for all
Tracy macros. Provides full stubs (`using TracyD3D12Ctx = void*`, stub
`SetThreadName`, all macros) when disabled, so call sites need no per-line
`#ifdef` guards.

**Frame marks.** Two named frame contexts: `FrameMarkNamed("Editor")` in
`Engine::run` and `FrameMarkNamed("Show")` in `Engine::showThreadMain`, matching
the two independent render timelines from ADR-0014.

**D3D12 GPU zones.** Single `TracyD3D12Ctx` on `D3D12Renderer`, show-thread
only. A cross-function zone spans `beginShowFrame` → `endShowFrame` using a
heap-allocated `thread_local tracy::D3D12ZoneScope*` (non-movable type). The
delete site is at the very top of `endShowFrame`, before any early return
(device-lost or copy-list-close failure), so the destructor fires on every
code path.

**Per-frame plots.** Sampled once per show frame: `FrameCache bytes used`,
`FrameCache hit rate %`, `FrameCache entries`, `Decode queue depth` (Σ
`max(0, targetFrame − currentFrame)` across initialized decode workers).
`FrameCache::consumeAccessCounters()` atomically resets hit/miss counters
each interval.

**Thread names.** All worker threads named at startup: `"Decode #N"` (per
entity uint32), `"ContentScanner"`, `"MediaProbe"`, `"Transcode"`, `"OSC"`.
The OSC plugin uses `#if defined(TRACY_ENABLE) / #include <tracy/Tracy.hpp>`
directly (Apache-2.0 boundary — GPL wrapper excluded).

**TracyLockable.** `FrameCache::m_mutex` converted to
`TracyLockable(std::mutex, m_mutex)`; all 10 lock sites updated to
`std::lock_guard<LockableBase(std::mutex)>`.

**Files**: `include/entity/profile/Tracy.hpp`, `CMakeLists.txt`,
`vcpkg.json`, `src/core/Engine.cpp`, `src/render/D3D12Renderer.cpp`,
`include/entity/render/D3D12Renderer.hpp`, `src/systems/DecodeSystem.cpp`,
`src/media/FrameCache.cpp`, `include/entity/media/FrameCache.hpp`,
`src/project/ContentScanner.cpp`, `src/media/MediaProbeWorker.cpp`,
`src/media/TranscodeWorker.cpp`, `plugins/osc-receiver/OscReceiverPlugin.cpp`,
`docs/adr/0015-profiling-with-tracy.md`.

---

### OSC Receiver Plugin + Preferences (2026-05-08)

Inbound OSC over UDP for triggering Entity from external show-control
software (Network Cues, stage-manager consoles, Companion, TouchOSC).
Hand-rolled OSC 1.0 parser; Winsock listener on port 53000 (default,
configurable via Preferences); routes a fixed namespace into Director
commands:

```
/entity/play  /entity/pause  /entity/stop
/entity/section/next
/entity/cue/{number}/go
/entity/seek <int frame>
```

**First control-plane plugin shipped.** Routing departs from ADR-0005's
stated bus-based model — control-plane plugins call
`CommandDispatcher::enqueue(typeName, paramsJson)` via narrow
`IPluginContext` accessors instead. Bus stays Director↔Renderer-only
until Phase E forces multi-process. Documented in ADR-0013.

**Plugin-API additions** (additive at vtable bottom, no
`PLUGIN_API_VERSION` bump):
- `enqueueCommand(typeName, paramsJson)`
- `registerShutdownHook(fn)` — engine joins worker threads before
  tearing down dispatcher/bus
- `getBoolSetting` / `getIntSetting` — narrow stringly-typed Settings
  accessors so the GPL Settings struct stays out of the Apache-2.0
  header

**Settings UI**: new "OSC Receiver" section in Preferences (enable
checkbox + listener port, restart-required). Defaults: enabled, 53000.

**File-association support**: editor accepts a positional
`<project.entity>` argument; Windows hands this to the editor on
double-click after the user sets a file association. Falls back to
the launcher on load failure so a stale association doesn't dead-end.

**Files**: `plugins/osc-receiver/`,
`plugin-api/include/entity/plugin/PluginContext.hpp`,
`include/entity/core/{Settings,Engine,EnginePluginContext}.hpp`,
`src/core/{Settings,Engine,EnginePluginContext}.cpp`,
`src/ui/SettingsWindow.cpp`, `apps/editor/main.cpp`,
`docs/adr/0013-control-plane-plugins-route-via-command-dispatcher.md`.

**Roadmap status**: Issue #2 ("Phase D: OSC control surface") still
open — outbound OSC sender and per-project custom mapping UI remain
as future subtasks.

---

## Phase 5: Projection Mapping (In Progress)

### Mixed Frame Rate Support (2025-11-28)

Fixed critical bug where videos played at timeline frame rate instead of their native frame rate. A 24fps video on a 30fps timeline was playing 25% faster and ending early.

**Root Cause**: 1:1 frame mapping between timeline frames and source frames. The system treated frame numbers as interchangeable regardless of frame rate.

**Fix**: Added frame rate ratio conversion throughout the codebase:

1. **Duration calculation**: `clip.duration` now stored in timeline frames:
   ```
   duration = totalMediaFrames × (timelineFPS / sourceFPS)
   ```

2. **Frame mapping**: `Engine::mapToMediaFrame()` converts timeline frames to source frames:
   ```
   sourceFrame = timelineLocalFrame × (sourceFPS / timelineFPS)
   ```

3. **Timeline display**: All frame↔time conversions in `TimelineWidget.cpp` now use timeline frame rate instead of clip frame rate.

4. **Clip operations**: `Timeline::splitClip()` and `duplicateClip()` properly handle mixed frame rates.

5. **Project loading**: `ProjectSerializer` recalculates duration from `totalMediaFrames` to handle old project files.

**New Features**:
- Added `frameBlending` field to Clip component (renderer support pending)
- Timeline frame rate is no longer changed when loading clips

**Files**: Clip.hpp, Engine.cpp, DecodeSystem.cpp, TimelineWidget.cpp, Timeline.cpp, ProjectSerializer.cpp, MediaBinWindow.cpp

---

### Multi-Screen Targeting Fix (2025-11-28)

Fixed critical descriptor heap slot collision that caused video to disappear when a second screen was created.

**Root Cause**: Compose targets and video textures were allocated overlapping descriptor heap slots:
- Compose targets used slots `2 + slot` (0, 1, 2, ...)
- Video textures used slots `3 + slot` (0, 1, 2, ...)
- When compose target 1 was created (heap slot 3), it overwrote video texture 0's SRV

**Fix**:
- Added `MAX_COMPOSE_TARGETS = 8` constant to D3D12Renderer
- Fixed heap layout: compose targets at slots 2-9, video textures at slots 10+
- Video textures now use slot `2 + MAX_COMPOSE_TARGETS + slot`

**New Script Commands**:
- `AddScreen` - Create new screen via script
- `SetClipTargetScreen` - Set clip's target screen via script

**Files**: D3D12Renderer.hpp/cpp, Commands.hpp/cpp, CommandDispatcher.cpp

---

### Scrubbing/Seek Freeze Fix (2025-11-28)

Fixed playback freeze when dragging playhead or clips backwards then pressing play.

**Root Cause**: Multiple interacting issues:
1. Race condition: decode thread used stale `targetFrame` after seek was signaled
2. Buffer thrashing: rapid seeks during drag operations cleared buffer repeatedly
3. Duplicate seeks: single timeline click triggered multiple seek calls

**Fix**:
1. Update `targetFrame` atomically before signaling seek in `seekClip()`
2. Added seek debouncing in TimelineWidget (only seek when time actually changes)
3. Added scrubbing mode that prevents decoder seeks during drag operations:
   - Timeline sets `m_isScrubbing = true` when drag starts
   - DecodeSystem skips seek detection while scrubbing
   - On drag release, scrubbing ends and one final seek is triggered
   - Applied to ruler drag, clip drag, and clip trim operations

**Files**: DecodeSystem.cpp/hpp, Timeline.hpp, TimelineWidget.cpp/hpp

---

### Window Management Improvements (2025-11-27)

Implemented per-window undocking via right-click context menu and fixed ping-pong playback mode.

**Window Management Features**:
- Layout Lock: Windows locked by default to prevent accidental undocking
- Right-click Undock: Right-click on any window tab to undock/dock individual windows
- Visual Feedback: Undocked windows shrink by 10 pixels for clear indication
- Menu Access: Windows > Dock/Undock submenu for all windows
- Keyboard Shortcut: Ctrl+L to toggle layout lock
- State Sync: Dock state syncs with ImGui reality

**Ping-Pong Playback Fixes**:
- Increased buffer capacity for ping-pong clips (up to 256 frames)
- Fixed buffer stalling at 0/32 by using `getFrame()` instead of `consumeUpTo()`
- Improved seek logic for bidirectional playback
- Added reverse phase detection for proper backward playback

**Files**: WindowManager.hpp/cpp, DecodeSystem.cpp, Engine.cpp

---

### 3D Stage Visualization (2025-11-26)

Implemented 3D stage visualization with floor grid and camera controls.

**Features**:
- Floor Grid: Major/minor grid lines with perspective, colored coordinate axes (red X, blue Z)
- Screen Quad: Composited video displayed on 3D quad in space (16:9 aspect, elevated above floor)
- Camera Controls: Orbit (left drag), Pan (middle/shift+left), Zoom (scroll/right drag)
- View Presets: Reset, Front, Top, Side buttons in toolbar
- 2D/3D Toggle: Switch between flat composited view and 3D stage visualization

**Files Created**: Camera.hpp, Stage3DRenderer.hpp/cpp
**Files Modified**: StageWindow.hpp/cpp

---

### Keyframe UI Enhancements (2025-11-25)

Improved keyframe editing interface in both timeline and properties panel.

**Timeline Enhancements**:
- Expanded clip view shows property tracks (Position X/Y, Scale X/Y, Rotation, Opacity)
- Left-hand track header with property names, stopwatch icons, keyframe navigator
- Keyframe diamonds on property tracks show keyframe positions
- Click on clip arrow to expand/collapse property tracks

**Properties Panel**:
- Each animatable property has keyframe controls
- Stopwatch button (gold when has keyframes, gray when static)
- `<` / `>` Navigate to previous/next keyframe
- Diamond button to add/remove keyframe at current frame
- Clicking navigation buttons seeks timeline to keyframe position

**Files**: TimelineWidget.cpp, PropertyWindow.cpp

---

### Keyframe Animation System (2025-11-24)

Implemented property keyframing for clip animation with linear interpolation.

**Animatable Properties**: PositionX, PositionY (NDC), Rotation (degrees), ScaleX, ScaleY, Opacity

**Interpolation Types**: Linear (default), Step (hold), EaseInOut (cubic bezier placeholder)

**Components**: AnimatedProperties.hpp, AnimationSystem.hpp/cpp

---

## Phase 4: Media Decoding Pipeline (Complete)

### PNGSequenceDecoder (2025-11-24)

Implemented PNG sequence decoder with stb_image library.

**Features**:
- Directory scanning for all PNG files
- Alphabetical sorting for deterministic sequence ordering
- Alpha channel detection from first frame
- Premultiplied alpha conversion
- Full error handling

**Files**: PNGSequenceDecoder.hpp/cpp

### ProResDecoder (2025-11-23)

FFmpeg-based ProRes 4444 decoding with YUV to RGBA conversion.

### Interactive Timeline UI (2025-11-23)

**Features**:
- Timeline Zoom: Alt + Mouse Wheel (10-500 px/sec)
- Ruler Scrubbing: Click and drag on time ruler
- Clip Repositioning: Click and drag clips
- Deferred resize pattern for D3D12 stability

---

## Phase 3: ECS Components (Complete)

- System infrastructure (System.hpp base class)
- Component refactoring (Clip, FrameBuffer as pure data)
- FrameRingBuffer (lock-free circular buffer)
- Decoder base class + implementations

---

## Phase 2: Core Engine (Complete)

- D3D12 renderer initialization
- ImGui integration
- Window management
- Basic rendering pipeline

---

## Phase 1: Project Scaffold (Complete)

- CMake/vcpkg build system
- Directory structure
- Dependency setup (EnTT, FFmpeg, ImGui, GLFW)
