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
       └─ Detects section breaks; writes ClipPlaybackPhase + Timeline state.
       └─ Wall-clock-anchored (steady_clock), NOT dt-accumulator.

3.  AnimationSystem::update(dt)
       └─ Clip branch: evaluates keyframe tracks; writes Transform + MediaLayer fields.
       └─ OA branch: evaluates ObjectAnimationLayer keyframe tracks (PositionX/Y/Z,
          RotationX/Y/Z, ScaleX/Y/Z); writes ObjectAnimationOutput. Skips re-evaluation
          for Locked layers with frozen=true (set by SectionScheduler at a section break).
          See ADR-0016.

4.  drainContentScannerDeltas()
       └─ Folds filesystem-watcher deltas into MediaBin.

5.  DecodeSystem::update()
       └─ Maps timeline frame → media frame per clip; sets atomic
          worker->targetFrame. Lazily creates per-clip DecodeWorker
          threads. Reads Clip + FrameBuffer; writes none.

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
1.  if (editor heartbeat > 50ms stale):
       ├─ Timeline::update(dt)            ← show-thread fallback (cf103bd)
       └─ DecodeSystem::update()          ← show-thread fallback (8492438)

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

6.  CompositorSystem::update(renderFrame)
       └─ Walks rf.activeClips (snapshot, not registry). Sorts by zOrder.
       └─ Per visible screen: ensureScreenRenderTarget(); for each
          clip with matching targetScreen, draw textured quad.
       └─ Posts R2D ScreenRenderTargetAllocated when allocating a new
          compose target slot.

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
- **Show-thread fallback (1) before everything else**: catches up stalled
  editor state so compositor reads aren't using a frozen snapshot.

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

Constraint per ADR-0014: **systems called from the show thread must not
write the registry.** That's why only some systems have fallbacks.

| System | Editor-tick site | Show-thread fallback? | Notes |
|---|---|---|---|
| `Timeline::update` | step 1 | ✅ since `cf103bd` | Writes only atomic `m_currentTime` — show-safe. |
| `SectionScheduler::tick` | step 2 | ❌ | Writes `ClipPlaybackPhase` + `Timeline` section state + `ObjectAnimationLayer::frozen` (Phase 3.8). See CODE_ISSUES NEW-08. |
| `AnimationSystem::update` | step 3 | ✅ via snapshot-bake (2026-05-11) | Editor still writes `Transform` + `MediaLayer` (Clip branch) and `ObjectAnimationOutput` (OA branch) for UI surfaces. Clip tracks are baked into `ClipCatalogEntry`; OA tracks into `ObjectAnimationLayerSnapshot`. Show thread re-evaluates both per render frame in `buildRenderFrame`. Animation stays alive during editor stalls. NEW-07 closed. OA freeze for Locked layers at section breaks handled via `ObjectAnimationLayer::frozen` (ADR-0016). |
| `drainContentScannerDeltas` | step 4 | ❌ — not needed | Filesystem-watcher updates can wait until stall ends. |
| `DecodeSystem::update` | step 5 | ✅ since `8492438` | Writes only atomic `worker->targetFrame` — show-safe. |

NEW-08 is the remaining open gap. NEW-07 was closed 2026-05-11 by the
snapshot-bake approach in `docs/design/animation-snapshot-bake.md`:
keyframe tracks travel through `bus::ClipCatalogEntry` and the show
thread re-evaluates them per render frame, so animation stays alive
during editor stalls without any registry writes from the show thread.

The plan for NEW-08 follows the same shape (see
`docs/design/section-scheduler-snapshot-bake.md`), but with the added
state-machine wiring the SectionScheduler needs — detector-on-show +
applier-on-editor + wall-clock anchor for continuation phase.

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
- `include/entity/systems/CLAUDE.md` — operator-facing rule sheet
