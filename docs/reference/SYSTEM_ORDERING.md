# System Ordering & Dependencies

Per-frame system execution order, dependency graph, and show-thread
fallback coverage for the Entity editor.

The authoritative order lives in code — `Engine::update()` for the editor
thread and `Engine::showThreadMain()` for the show thread. This doc is
the readable mirror.

For *why* the threading is split this way, see
`docs/adr/0014-editor-show-thread-split.md`.

---

## Editor thread (per editor frame)

`Engine::update()`. Runs at editor framerate (vsync-bound, ~60 Hz under
load, thousands of Hz in `--headless --script` mode with no decode work).

```
1.  Timeline::update(dt)
       └─ Advances m_currentTime when Playing. Atomic write.

2.  SectionScheduler::tick()
       └─ Break-crossing DETECTION runs on the show thread now (NEW-08).
          tick() applies the catch-up: advances ClipPlaybackPhase
          continuation while parked, drops a stale at-break latch on
          manual resume, resets post-break anchors on a playback scrub.
       └─ The editor applies a show-detected crossing via handleBreakAt()
          from drainRendererToDirector (step 10).
       └─ Wall-clock-anchored (steady_clock), NOT dt-accumulator.

3.  AnimationSystem::update(dt)
       └─ Clip branch: evaluates keyframe tracks; writes Transform + MediaLayer fields.
       └─ OA branch: evaluates ObjectAnimationLayer keyframe tracks (PositionX/Y/Z,
          RotationX/Y/Z, ScaleX/Y/Z); writes ObjectAnimationOutput. Skips re-evaluation
          for Locked layers with frozen=true (set by SectionScheduler at a section break).
          See ADR-0016.

3.5 TextSystem::update()
       └─ For each active TextLayerState with dirty=true: rasterizes the text string
          to a video-pool texture via TextRasterizer (DirectWrite + D2D on Windows).
          Clears dirty flag after successful rasterize. Allocates a video-pool slot
          on first use; slot freed by the on_destroy<TextLayerState> observer
          (TextSystem::onTextLayerDestroyed) when the entity is deleted. Writes
          TextLayerState::textureSlot, bakedWidth, bakedHeight.
       └─ Static-per-frame: the last-baked texture remains valid during editor stalls,
          so no show-thread fallback is needed (text doesn't animate — it only changes
          on explicit authoring commands).

4.  drainContentScannerDeltas()
       └─ Folds filesystem-watcher deltas into MediaBin.

5.  DecodeSystem::update()
       └─ Maps timeline frame → media frame per clip; sets atomic
          worker->targetFrame. Lazily creates per-clip DecodeWorker
          threads. Reads Clip + FrameBuffer; writes none.

5.5 AudioSystem::update()
       └─ Per active Clip+AudioSource: creates/destroys AudioDecodeWorker
          threads lazily, steers seekTarget (atomic) on playhead jumps,
          mirrors gain/mute/solo from AudioSource into the AudioMixer slot,
          gates active flag from the clip's active window + ClipPlaybackPhase
          continuation state. Reads Clip + AudioSource + ClipPlaybackPhase;
          writes only atomic worker fields — never the registry. Show-thread
          fallback fires on editor stalls (same 50ms heartbeat gate as
          DecodeSystem).

5.6 SeekSyncController::tick()
       └─ No-op when m_seekSyncGate is clear (99.9% of ticks — returns on
          first branch). When the gate is active (engaged by Timeline::play()
          on every ->Playing transition), polls two injectable readiness
          predicates: videoReady (∀ active Clips: DecodeSystem::isClipReadyAt)
          and audioReady (∀ active Clip+AudioSources: AudioSystem::isWorkerSeekReady).
          Releases the gate by calling Timeline::setSeekSyncGate(false) when
          both predicates return true. Falls back to releasing after
          kPrerollTimeoutMs (3000 ms) so a broken decoder can never hang
          playback indefinitely (ADR-0026).
       └─ Must tick AFTER AudioSystem::update (step 5.5) so worker seek
          state and ring-buffer fill are current when the readiness predicate
          fires. Must tick BEFORE m_lastEditorTickNs.store (step 6) so the
          gate release happens on the same editor tick that sees the ready
          state.
       └─ Show-thread fallback: not needed. The gate holds Timeline::update
          from advancing m_currentTime. During an editor stall, the show-
          thread Timeline::update also reads m_seekSyncGate (acquire order)
          and respects the hold — so the gate extends across editor stalls
          without SeekSyncController needing a show-thread presence.

6.  m_lastEditorTickNs.store(now)
       └─ Heartbeat the show thread polls for stall detection.

7.  ImGui new-frame + UI rendering
       └─ Editor windows draw. Per-window state writes (mostly through
          CommandDispatcher).

8.  CommandDispatcher::processQueue(Editor + Either affinity)
       └─ Drains pending commands; writes registry.

9.  buildSceneSnapshot()
       └─ Bakes registry state into bus::SceneSnapshot::clipCatalog +
          screens + outputs. Single-producer publish to D2R bus.

10. drainRendererToDirector()
       └─ Reads R2D replies from show thread (e.g.
          ScreenRenderTargetAllocated → writes Screen::renderTargetSlot).

11. beginEditorFrame / endEditorFrame
       └─ Editor swap chain Present.
```

### Dependencies between editor systems

- **SectionScheduler before AnimationSystem**: section-fade multipliers
  are read by AnimationSystem in some paths.
- **AnimationSystem before DecodeSystem**: not strictly required —
  Transform doesn't drive decode — but conceptually animation should be
  evaluated before any system that might read its outputs.
- **All five tick systems (1-5) before buildSceneSnapshot (9)**: the
  snapshot is the contract. Anything that wants to be visible to the
  show thread next frame must have written its state before the bake.
- **Command dispatch (8) before buildSceneSnapshot (9)**: commands that
  mutate the registry need to land before the snapshot so the show
  thread sees them on this frame, not next.

---

## Show thread (per output frame)

`Engine::showThreadMain()`. Runs independently from the editor at output
framerate (typically vsync on the primary output, 60 Hz).

```
1.  Timeline::update(dt)
       └─ Advances Timeline::m_currentTime when Playing. Runs every show
          frame unconditionally (ee99a99) — playback pace is decoupled
          from editor health.

1.1 SectionScheduler break-crossing detection  (NEW-08, the `SectionDetect` zone)
       └─ When Playing and not already at-break, finds the first
          Section::breakFrame the playhead just crossed, snaps + pauses
          the playhead, raises Timeline::sectionAtBreak(), and posts an
          R2D bus::SectionBreakDetected. Show-thread-local last-seen
          state; sections read live via Timeline::copySectionsAndRate();
          no registry writes. The editor applies the crossing via
          SectionScheduler::handleBreakAt (editor step 10).

1.2 SignalOutputSystem::evaluate  (ADR-0027, the `SignalOutputSystem` zone)
       └─ Evaluates active Signal layers from the cached SceneSnapshot's
          baked SignalLayerSnapshot entries — momentary crossing
          detection + continuous value mapping — and posts bus::SignalEmit
          via Engine::postSignalEmit. Runs unconditionally on the show
          thread (no registry reads/writes), so signal output keeps
          firing through editor stalls. Control-plane plugins drain the
          emits via IPluginContext::drainSignalEmits.

1.3 (after the D2R drain) if editor heartbeat > 50ms stale
    and no bulk registry mutation in progress:
       └─ DecodeSystem::tickFromSnapshot()  ← registry-free fallback (#74)
       └─ AudioSystem::tickFromSnapshot()   ← registry-free fallback (#74)
       Steers existing workers' atomics from m_cachedSceneSnapshot's
       clip catalog (same CatalogClipMath as buildRenderFrame). Never
       touches the registry; worker lifecycle stays editor-only.

2.  CommandDispatcher::processQueue(Show affinity)
       └─ Drains Play / Pause / Seek / SectionGo. Writes atomic
          playback state on Timeline; never writes the registry.

3.  Drain D2R bus
       └─ Pull latest SceneSnapshot (latest-wins; older snapshots
          superseded if a newer one arrived before drain).

4.  beginShowFrame
       └─ Reset show-side command allocator; open command list.
       └─ Open Tracy D3D12 zone (cross-function, closed at endShowFrame).

5.  PlaybackPresenter::present()
       └─ Uploads decoded frames to GPU textures; caches color-space
          tags show-thread-locally (no registry reads in hot path).

6.  CompositorSystem::update(renderFrame)         [three-pass — ADR-0018 + issue #54]
       PASS 1 (producer):
         └─ Per active GenerativeLayerSnapshot:
            ensureGenerativeRenderTarget(); beginComposeTarget(slot);
            drawMuncherPlayfield(gl) in layer-local NDC; endComposeTarget().
            Posts R2D GenerativeLayerRenderTargetAllocated on first
            allocation per layer entity.
       PASS 1.5 (per-layer effect chains, issue #54):
         └─ For each ContentLayerSnapshot with non-empty `effects`:
            ensureEffectPingTarget(); ping-pong two compose targets
            through the chain; write final slot to `postEffectsSlot`.
            Posts R2D EffectChainRenderTargetAllocated (side 0/1) on
            first allocation per layer entity.
         └─ Kind-blind — works identically for Video and Compose
            sourceKinds. PASS 1.5 is a no-op for layers without effects.
       PASS 2 (unified composite):
         └─ Per visible screen: ensureScreenRenderTarget(); for each
            ContentLayerSnapshot in rf.contentLayers (pre-sorted by
            zOrder) matching this screen, drawTexturedQuad with
            sourceKind dispatching the descriptor pool
            (Video → video texture, Compose → generative layer RT).
            When `postEffectsSlot >= 0`, PASS 2 reads from that
            Compose slot instead of `sourceSlot`.
         └─ Posts R2D ScreenRenderTargetAllocated on first
            allocation per screen entity.
       └─ rf.contentLayers is built show-side by
          PlaybackTimeAuthority::buildRenderFrame from activeClips +
          generativeLayers, with per-layer effects folded in from
          scene.layerEffects (issue #54).

7.  OutputManager::renderOutputs()
       └─ Per enabled OutputDisplay: composite the assigned Screen's
          compose target to that output's swap chain, with InputRegion
          UV cropping and any per-output calibration overlay.

8.  endShowFrame
       └─ Close Tracy D3D12 zone. Execute command list.
       └─ Present each enabled output swap chain.
```

### Dependencies between show-thread systems

- **PlaybackPresenter before CompositorSystem**: compositor reads
  textures the presenter just uploaded.
- **CompositorSystem before OutputManager**: outputs read screen compose
  targets that the compositor just drew into.
- **Stall fallback after the D2R drain**: tickFromSnapshot reads
  m_cachedSceneSnapshot, so it runs after the drain that refreshes it
  (and after the launcher-idle early-out — no content to steer there).

---

## Worker threads (independent of editor/show)

These don't appear in the per-frame ordering — they run in parallel and
talk to the editor/show pair via atomic fields or message queues.

| Worker | Owned by | Communicates via |
|---|---|---|
| `Decode #N` (per clip) | `DecodeSystem` | atomic `targetFrame`; FrameRingBuffer |
| `ContentScanner` | `ContentScanner` | delta queue, drained on editor thread step 4 |
| `MediaProbe` | `MediaProbeWorker` | result queue, drained on editor thread |
| `Transcode` | `TranscodeManager` | result queue, drained on editor thread |
| `OSC` (plugin) | `OscReceiverPlugin` | `CommandDispatcher::enqueue` |

---

## Show-thread fallback coverage

When the editor thread stalls (Win32 modal dialog, OS resize/move loop,
slow project load), `Engine::update()` stops running, so every editor-
tick system stops with it. The show thread polls `m_lastEditorTickNs`
and, when stale, takes over critical time-driven systems so the
projector output stays alive.

Constraint per ADR-0014 (amended by issue #74): **the show thread never
calls `update(registry)` — a stall fallback is a dedicated registry-free
entry point consuming `SceneSnapshot` + worker atomics only.** Even
"read-only" view iteration is out: heartbeat staleness doesn't mean the
registry is quiescent (a synchronous project load is a stall AND a bulk
mutation — the pre-#74 crash class).

| System | Editor-tick site | Show-thread fallback? | Notes |
|---|---|---|---|
| `Timeline::update` | step 1 | yes since `ee99a99` | Writes only atomic `m_currentTime` — show-safe. |
| `SectionScheduler` | editor step 2 / show step 1.1 | yes via detector-on-show split (2026-05-20) | Break-crossing *detection* runs on the show thread (`SectionDetect`, show step 1.1) — it snaps + pauses the playhead and posts R2D `SectionBreakDetected`. The editor *applies* it via `handleBreakAt` (registry writes: `ClipPlaybackPhase`, scheduler latch). Continuation phase is re-derived show-side from the wall-clock anchor in `mapToMediaFrameFromCatalog` using the active `RateSource` time (Phase G, 2026-05-21) — same clock domain as the editor-side seed — so it never freezes during a stall and stays in sync with the audio crystal when audio is active. NEW-08 closed. |
| `AnimationSystem::update` | step 3 | yes via snapshot-bake (2026-05-11) | Editor still writes `Transform` + `MediaLayer` (Clip branch) and `ObjectAnimationOutput` (OA branch) for UI surfaces. Clip tracks are baked into `ClipCatalogEntry`; OA tracks into `ObjectAnimationLayerSnapshot`. Show thread re-evaluates both per render frame in `buildRenderFrame`. Animation stays alive during editor stalls. NEW-07 closed. OA freeze for Locked layers at section breaks handled via `ObjectAnimationLayer::frozen` (ADR-0016). End-of-layer behavior follows `ObjectAnimationLayer::endBehavior` (ADR-0020): `Hold` keeps the last evaluated values applied past the layer's active window (default); `Reset` clears the override. After-end-Hold layers ride the snapshot to keep the show thread in sync during stalls; after-end-Reset layers are filtered out editor-side. |
| `TextSystem::update` | step 3.5 | no -- not needed | Rasterizes dirty Text layers to video-pool textures. Static-per-frame: text content only changes on explicit authoring commands, never on playback. The last-baked texture remains valid during editor stalls so output stays correct. Writes `TextLayerState::textureSlot`/`bakedWidth`/`bakedHeight`; clears `dirty`. |
| `drainContentScannerDeltas` | step 4 | no -- not needed | Filesystem-watcher updates can wait until stall ends. |
| `DecodeSystem::update` | step 5 | yes via `tickFromSnapshot` (#74, replaced `a9bcd8b`'s update() re-entry) | update() is editor-only (asserted). The fallback steers existing workers' atomics (`targetFrame`, seek, `pingPongReverse`) from the clip catalog — zero registry access. Worker map guarded by a mutex (also covers PlaybackPresenter's per-frame `getWorker`). |
| `AudioSystem::update` | step 5.5 | yes via `tickFromSnapshot` (#74) | update() is editor-only (asserted). The fallback drives `mixSource.active` + discontinuity re-seeks from the catalog; discontinuity threshold scales with the measured steering-tick gap so fallback handoffs never seek-storm (audible ring-clears). Gain/mute/solo mirroring + the `hasAudioStream` registry write stay editor-only. |
| `SignalOutputSystem::evaluate` | show step 1.2 (show-native) | n/a — already show-native (ADR-0027) | Never runs on the editor tick at all: the editor only bakes `SignalLayerSnapshot` entries into the scene snapshot; evaluation (crossing detection, value mapping, `bus::SignalEmit` posts) happens on the show thread with zero registry access, so signal output fires through editor stalls by construction. |
| `SeekSyncController::tick` | step 5.6 | no — not needed | Polls readiness predicates and releases `Timeline::m_seekSyncGate` when all active decoders reach the parked frame. The gate itself is an `std::atomic<bool>` read by both the show-thread and editor-thread `Timeline::update`; both respect the hold without SeekSyncController needing a show-thread presence. During an editor stall the gate stays held (no tick = no release), which is correct — audio and video decode workers keep running independently, so by the time the editor resumes the predicates may already be satisfied and the gate releases on the first tick. Timeout failsafe (3000 ms) guards against indefinite holds (ADR-0026). |

Every editor-tick system that drives the projector output now has a
stall path. NEW-07 was closed 2026-05-11 by the snapshot-bake approach
in `docs/design/animation-snapshot-bake.md`: keyframe tracks travel
through `bus::ClipCatalogEntry` and the show thread re-evaluates them
per render frame.

NEW-08 was closed 2026-05-20 with the detector-on-show / applier-on-
editor split (see `docs/design/section-scheduler-snapshot-bake.md`).
Because SectionScheduler is a state machine — not a stateless evaluator
like AnimationSystem — the snapshot-bake shape alone was not enough: the
show thread runs the *detector* (and snaps the playhead immediately so
output never sails past the break), while the registry-mutating *apply*
stays on the editor thread, reached by an R2D `SectionBreakDetected`
message. Continuation phase rides a wall-clock anchor (`steady_clock`),
re-derived show-side, so it advances even with no editor ticks.

---

## Adding a new system

Decide affinity up front:

1. **Editor-affinity** (writes registry, runs from `Engine::update`):
   most systems. Add to the appropriate step above based on its
   dependencies.

2. **Show-affinity** (snapshot reads only, runs from `Engine::showThreadMain`):
   reserved for systems that need to fire per render frame. Today this
   is `PlaybackPresenter` and `CompositorSystem`.

If the system is editor-affinity AND time-driven (its work is what the
user sees on the projector each frame), ask: "what happens if
`Engine::update` stops for 5 seconds while the user drags the editor
window?" If the answer is "user-visible content freeze on output,"
the system needs one of:

- A show-thread fallback in `Engine::showThreadMain` (only if its
  per-frame work can be done without writing the registry).
- An editor-thread snapshot-bake that publishes its results into
  `SceneSnapshot::clipCatalog`, so the show thread reads pre-computed
  values.

See ADR-0014's "Show-Thread Fallback Pattern" section for the full
checklist.

---

## See also

- `docs/adr/0014-editor-show-thread-split.md` — threading architecture rationale
- `docs/reference/ECS_PRINCIPLES.md` — the ECS rules
- `docs/reference/ENTITY_ARCHETYPES.md` — what data the systems are reading and writing
- `include/entity/systems/CLAUDE.md` — operator-facing rule sheet (gitignored; maintainer-local only)
