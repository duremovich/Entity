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
       ├─ Timeline::update(dt)            ← show-thread fallback (ee99a99)
       └─ DecodeSystem::update()          ← show-thread fallback (a9bcd8b)

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
| `Timeline::update` | step 1 | yes since `ee99a99` | Writes only atomic `m_currentTime` — show-safe. |
| `SectionScheduler::tick` | step 2 | no | Writes `ClipPlaybackPhase` + `Timeline` section state + `ObjectAnimationLayer::frozen` (Phase 3.8). See CODE_ISSUES NEW-08. |
| `AnimationSystem::update` | step 3 | yes via snapshot-bake (2026-05-11) | Editor still writes `Transform` + `MediaLayer` (Clip branch) and `ObjectAnimationOutput` (OA branch) for UI surfaces. Clip tracks are baked into `ClipCatalogEntry`; OA tracks into `ObjectAnimationLayerSnapshot`. Show thread re-evaluates both per render frame in `buildRenderFrame`. Animation stays alive during editor stalls. NEW-07 closed. OA freeze for Locked layers at section breaks handled via `ObjectAnimationLayer::frozen` (ADR-0016). End-of-layer behavior follows `ObjectAnimationLayer::endBehavior` (ADR-0020): `Hold` keeps the last evaluated values applied past the layer's active window (default); `Reset` clears the override. After-end-Hold layers ride the snapshot to keep the show thread in sync during stalls; after-end-Reset layers are filtered out editor-side. |
| `TextSystem::update` | step 3.5 | no -- not needed | Rasterizes dirty Text layers to video-pool textures. Static-per-frame: text content only changes on explicit authoring commands, never on playback. The last-baked texture remains valid during editor stalls so output stays correct. Writes `TextLayerState::textureSlot`/`bakedWidth`/`bakedHeight`; clears `dirty`. |
| `drainContentScannerDeltas` | step 4 | no -- not needed | Filesystem-watcher updates can wait until stall ends. |
| `DecodeSystem::update` | step 5 | yes since `a9bcd8b` | Writes only atomic `worker->targetFrame` — show-safe. |

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
