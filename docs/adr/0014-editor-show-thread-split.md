# ADR-0014: Editor/Show thread split — snapshot-driven show thread with zero registry writes

- **Status:** Accepted
- **Date:** 2026-05-08
- **Context source:** Working plan
  `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md`
  (Phase D, issue #42, stages 1-5).
- **Implemented by:** Issue #42, five stages:
  - Stage 1 — `beginShowFrame`/`endShowFrame` + `beginEditorFrame`/`endEditorFrame`;
    per-role D3D12 command allocators and fences. Commits `4d2d1f8`–`e62b5a8`.
  - Stage 2 — `bus::SceneSnapshot`, `bus::RenderFrame`, `buildSceneSnapshot`/
    `buildRenderFrame`. Commit `ca6c193`.
  - Stage 3 — show thread spawned in `Engine::run`; `D2R` channel carries
    `RenderFrame`; latest-wins delivery on `D2RChannel`. Commit `ca6c193`.
  - Stage 4 — `Affinity` enum + `processQueue(affinity)`, `ScreenRenderTargetAllocated`
    R2D reply, `CreateOutputWindowRequest`/`OutputWindowReady` bus types, `crs->slot`
    data-race fix. Commits `aab0189`–`512ff7b`.
  - Stage 5 — remove deprecated `beginFrame`/`endFrame`; ADR; doc update.
    (This ADR.)
- **Amends:** ADR-0003 (Director/Renderer split). ADR-0003 describes the
  logical service split; this ADR records how the two services run on
  separate threads within a single process.
- **Amended:** 2026-07-03, issue #74 — the "Show-Thread Fallback Pattern"
  section was rewritten. The original fallback re-entered
  `DecodeSystem::update`/`AudioSystem::update` with the live registry on
  the show thread, relying on the false claim that the editor isn't
  writing during a stall (project load is a stall *and* a bulk registry
  mutation). The fallback is now registry-free (`tickFromSnapshot`).
- **Amended:** 2026-07-04, issue #76 — the last known show-thread registry
  *write* removed: `OutputManager::ensureOutputWindow` used to write
  `OutputDisplay::outputWindowSlot/width/height` from `renderOutputs`.
  Output-window slots now live in a show-thread-owned map inside
  OutputManager; window creation is driven by the baked `OutputSnapshot`
  (geometry included — no `m_availableDisplays` cross-thread read), the
  registry field is a display-only mirror updated via the R2D
  `OutputWindowSlotUpdated` reply (same pattern as
  `ScreenRenderTargetAllocated`), and teardown is reconciliation-based:
  an entity vanishing from the frame's outputs (delete, project
  load/close) destroys its window on the show thread. The sanctioned
  show-side registry access list (MappingSurface reads +
  same-tick VideoTexture colorSpace writes) is once again exhaustive.
- **Amended:** 2026-07-04, issue #75 — compose-target *structure* races
  closed (§2 only ever guarded RT *contents*). The show thread grows
  `m_composeTargets` at runtime (screen add, first effect on a layer)
  and `resizeComposeTarget` swaps sub-resources + rewrites SRV
  descriptors in place, while editor ImGui frames read the same slots.
  Three mechanisms now carry that safety: (1) the vector is `reserve()`d
  to `MAX_COMPOSE_TARGETS` at init so `emplace_back` never reallocates;
  (2) editor-facing accessors bound-check an atomically-published count,
  stored only after a target is fully initialized; (3) each target has
  an `EditorReadGate` (`include/entity/render/EditorReadGate.hpp`) — a
  seq_cst generation gate keyed to editor-frame begin/submit counters.
  A resize closes the gate (editor readers skip the slot for a frame or
  two), defers until every editor frame begun before the close has
  submitted its command list — an editor command list can hold a
  compose SRV handle for up to a frame between record and
  ExecuteCommandLists, which `waitForGpu()` alone cannot see — then
  drains the GPU, swaps, and reopens. Deferral returns `false` to
  CompositorSystem, which already retries every tick; output renders at
  the old dimensions meanwhile, and an editor stall simply keeps the
  resize deferred without blocking the show thread.

## Context

ADR-0003 split the code into Director (timeline, commands) and Renderer
(GPU, compositor, decode) with a serializable bus between them. After Phase D
entry landed (213/213 ctest), the two services still ran **on one thread**.
Show-thread and editor-thread code was organized but not separated: the
render loop's `compositor->update` required holding `m_registryMutex`, the
only thing preventing data races on the registry.

Three concrete problems this created:

1. **Editor UI freezes the show.** Any modal loop — file picker,
   long command, GC pause — stalls the next `Present`. For live projection
   this is a show-stopper; a 200 ms editor hang produces a visible drop.

2. **The mutex is a lie.** `m_registryMutex` was grabbed in two places but
   the show side still called `registry.try_get<VideoTexture>()` to read
   `descriptorSlot` (a plain `uint32_t` the editor thread writes). It was
   a data race regardless of the mutex scope.

3. **No path to cluster.** ADR-0003's cluster promise requires Director and
   Renderer to live in separate processes (Phase E). Two-service-one-thread
   is the wrong muscle memory for that split.

### Threading option considered

**Option i — Shared registry, fine-grained locking.** Both threads take
per-operation read/write locks. Common in game engines. In practice:
lock granularity is hard to get right, missed locks are silent data races,
and `entt::registry` is not designed for concurrent access.

**Option ii — Snapshot-driven show thread.** Editor thread is the
**sole writer** of the registry. Once per tick it snapshots all state
needed by the show thread into a plain-struct `SceneSnapshot` (no
`entt::` types) delivered over the `D2R` channel. The show thread never
writes the registry; it reads only the snapshot. Registry access on the
show side is zero writes and zero reads except:
- `MappingSurface` components (read for projection calibration, written
  only by editor-thread commands).
- `VideoTexture::colorSpace` / `ocioColorSpace` (written by
  `PlaybackPresenter::present` on the show thread in the same tick,
  before compositor reads them — no cross-thread race).

## Decision

**Option ii.** The show thread owns `compositor->update`, `outputManager->render`,
`endShowFrame`, and output `Present`. The editor thread owns the registry,
`buildSceneSnapshot`, `drainRendererToDirector`, ImGui, and editor `Present`.

Key design choices that made option ii viable:

### 1. `bus::SceneSnapshot::clipCatalog`

Every per-clip field the show thread needs — `descriptorSlot`, `opacity`,
`blendMode`, `targetScreen`, `zOrder`, `transformMatrix`, `sectionBehavior`,
`mediaStartFrame`, `slot` — is baked into `ClipCatalogEntry` by
`buildSceneSnapshot` on the editor thread. `buildRenderFrame` (show thread)
consumes `clipCatalog` with zero registry reads. This is the load-bearing
invariant of the design: **no cross-thread registry reads for clip data**.

### 2. Triple-buffered compose targets (`ComposeTarget::TRIPLE = 3`)

The show thread writes compose-target sub-resources round-robin
(`writeIndex % 3`), then stores `writeIndex` into `lastStableIndex`
(release-store). The editor thread reads `lastStableIndex` (acquire-load)
and displays `srvHandles[lastStableIndex % 3]` in ImGui. Three slots:
- Show thread can be recording sub-resource N+1 while...
- Editor thread is displaying sub-resource N, and...
- Sub-resource N-1 is idle (GPU may still be reading it, but no CPU thread
  writes it after the store).

This eliminates the "show overwrites compose target while editor samples it
for ImGui" race without a mutex or copy.

Note this guards RT *contents* only. Vector growth and the
`resizeComposeTarget` resource swap are structural mutations with their own
protection (reserve + published count + per-target `EditorReadGate`) — see
the 2026-07-04 / #75 amendment above.

### 3. `ScreenRenderTargetAllocated` R2D reply

The last show-thread registry write was `Screen::renderTargetSlot` in
`CompositorSystem::ensureScreenRenderTarget`. Replacing it with a
**request/reply pattern** removes the write: when the compositor allocates a
new slot, it posts `ScreenRenderTargetAllocated` on the `R2D` channel; the
editor drains it in `drainRendererToDirector` and writes `Screen::renderTargetSlot`
there (editor thread only). The slot is available in `SceneSnapshot` on the
next tick.

### 4. `Affinity` enum on `Command`

`CommandDispatcher::processQueue(engine, Affinity affinity)` skips commands
whose affinity doesn't match the calling thread and re-queues them in order
for the other thread. Show thread drains `Affinity::Show` commands (Play,
Pause, Seek, SectionGo, …) after each `buildRenderFrame`. Editor thread
drains `Affinity::Editor` commands (everything else, including
`WaitFrames`/`SleepMs`/`AssertShowFrameCount` which must sequence on the
editor thread for script correctness) and `Affinity::Either` commands
(ExitCommand).

### 5. HWND / swap-chain handshake deferred

Output windows need a GLFW HWND created on the main thread but a D3D12
swap chain initialized on any thread. `CreateOutputWindowRequest` and
`OutputWindowReady` bus message types stub the handshake: Director posts
the request (editor thread); Renderer creates the GLFW window on the
editor thread via the D2R drain, then posts `OutputWindowReady` back. The
GLFW/D3D12 wiring is deferred to a follow-up; the message types and
serialization are present.

## Consequences

**Enables:**
- Editor UI modality (file pickers, long commands) cannot stall the show
  thread. The projector pipeline runs at full rate regardless of UI state.
- Zero registry writes on the show thread. `m_registryMutex` (`std::shared_mutex`
  in `Engine`) is removed entirely — it was the Stage 3 temporary guard while
  show-thread registry reads were being eliminated. With the show thread having
  no registry access, the lock was editor-only and a no-op.
- The threading muscle memory is correct for Phase E: Director → Renderer
  is already a serialized snapshot; making it cross-process is a transport
  swap.
- Integration tests can assert show-thread frame counts independently of
  editor-thread script timing (`AssertShowFrameCountAtLeast` + command
  affinity).

**Forbids:**
- Show thread reading writable registry fields for clip data. Any new
  per-clip field needed on the show side must go through `ClipCatalogEntry`
  (editor bakes it into the snapshot). Adding it directly to the show path
  is a data race.
- Two threads calling `glfwPollEvents` or creating GLFW windows. Output
  window creation must route through the editor thread via the
  `CreateOutputWindowRequest`/`OutputWindowReady` handshake.

**Forces:**
- One tick of snapshot latency for any editor-side state change to reach
  the show thread. This is acceptable: the show thread runs at 60 Hz and
  the snapshot is a cheap memcpy of plain structs.
- `CompositorSystem` holds a `bus::IMessageTransport*` for R2D replies.
  It must be wired with `setTransport` before `update` is called.
- Per-role D3D12 command allocators and fences (`m_editorAllocators`,
  `m_showAllocators`, `m_editorFence`, `m_showFence`). Show fence gates
  compose-target + output swap-chain reuse; editor fence gates back-buffer
  reuse. Both submit to the same direct command queue (thread-safe per
  D3D12 spec for `ExecuteCommandLists`/`Signal`).

### Show-Thread Fallback Pattern for Editor Stalls

*Added after Stage 5: commits `ee99a99` (Timeline) and `a9bcd8b`
(DecodeSystem). Rewritten 2026-07-03 by issue #74, which made the
fallback registry-free.*

The split as described above prevents UI modality from stalling the show
thread's `Present`. But it doesn't, by itself, prevent UI modality from
stalling the **content** the show thread renders. `Engine::update`
on the editor thread ticks Timeline, SectionScheduler, AnimationSystem,
ContentScanner deltas, and DecodeSystem each frame. When the editor
thread blocks (native modal dialog, OS resize/move loop, slow project
load), `Engine::update` stops running, and every editor-tick system
stops with it. The show thread keeps Present-ing fresh frames at 60 Hz
— but with a frozen Timeline, frozen decode targets, frozen animations.
User-visible result: projector output appears frozen on the last good
frame for the duration of the stall.

**Heartbeat + show-thread fallback.** The editor thread stamps
`Engine::m_lastEditorTickNs` (atomic) at the top of each `Engine::run`
loop iteration. The show thread checks staleness once per show tick
(after its D2R snapshot drain, after the launcher-idle early-out):

```cpp
if (m_timeAuthority &&
    m_bulkRegistryMutation.load(std::memory_order_acquire) == 0) {
    const int64_t lastEditor = m_lastEditorTickNs.load(std::memory_order_relaxed);
    const int64_t nowNs = /* QPC ns */;
    constexpr int64_t kEditorStaleNs = 50'000'000; // 50 ms
    if (lastEditor != 0 && (nowNs - lastEditor) > kEditorStaleNs) {
        const std::int64_t rateNowNs =
            static_cast<std::int64_t>(m_timeAuthority->rateNow() * 1e9);
        if (m_decodeSystem) m_decodeSystem->tickFromSnapshot(m_cachedSceneSnapshot, rateNowNs);
        if (m_audioSystem)  m_audioSystem->tickFromSnapshot(m_cachedSceneSnapshot, rateNowNs);
    }
}
```

(`Timeline::update` no longer needs the gate at all — it runs
unconditionally on the show thread and only writes atomics.)

**The invariant (issue #74): the show thread never calls
`update(registry)`. A stall fallback must consume `SceneSnapshot` and
per-worker atomics only.** The original design let the show thread
re-enter `DecodeSystem::update`/`AudioSystem::update` with the live
registry, justified by the claim "during a stall the editor isn't
writing." That claim was false: a synchronous `ProjectManager::load` on
the editor thread freezes the heartbeat for seconds while actively
destroying/clearing/emplacing registry state — heartbeat staleness is
evidence the editor is *busy*, not that the registry is *quiescent*.
Concurrent EnTT view iteration + structural mutation is UB (the
critical race of issue #74). The fix is structural, not a lock: the
fallback ticks (`tickFromSnapshot` on both systems) read only the
show-local `m_cachedSceneSnapshot` clip catalog (via
`entity/director/CatalogClipMath`, the same math `buildRenderFrame`
uses) and steer only *existing* workers' atomics — `targetFrame`,
`seekTarget`/`seekPending`, `pingPongReverse`, `mixSource.active`,
`lastExpectedSample`. Worker lifecycle (create/retire/reap) stays
editor-only, asserted at the top of both `update()`s.

**Supporting mechanics (issue #74):**
- **Worker-map mutex.** `DecodeSystem::m_workers` /
  `AudioSystem::m_workers` are guarded by a `TracyLockable` mutex — the
  show thread (tickFromSnapshot, and `PlaybackPresenter`'s per-frame
  `getWorker`, previously an unguarded race) copies the `shared_ptr`
  out under a brief lock. Leaf-lock rules (documented next to the
  mutexes): never held across `thread::join` / worker cv / FrameCache /
  AudioMixer / decoder calls; readers never keep a raw worker pointer
  past the lock scope; map mutation stays editor-thread-only.
- **Wake-overlap safety.** The heartbeat is stamped at the *top* of the
  editor iteration, so on stall-exit the editor's `update()` and the
  show thread's `tickFromSnapshot` can overlap. This is safe by
  construction: all shared mutable state is worker atomics + the
  mutex-guarded maps; both threads compute steering inputs from the
  same Timeline atomics and (catalog-mirrored) clip values, so
  concurrent steers differ by at most one tick — last-writer-wins on
  value stores, and duplicate seeks are idempotent (`seekPending`
  gates pile-up). The audio discontinuity threshold scales with the
  measured gap since the last steering tick
  (`AudioDecodeWorker::lastExpectedSampleNs`), so a steering-authority
  handoff never mis-reads normal playback advance as a scrub (the old
  fixed 3-tick threshold equalled the 50 ms stall threshold at 60 fps
  timelines — a guaranteed audible ring-clear on every stall entry).
- **Bulk-mutation gate.** `Engine::RegistryMutationScope` (RAII) marks
  `loadProject`/`closeProject`; the fallback stands down while it is
  set. Not required for memory safety — the fallback is registry-free —
  but the cached catalog still describes the *old* project during a
  load, so steering would burn decode I/O against the load, and (after
  4096 destroys of one entity index) a recycled EnTT id could alias
  onto the new project's workers. New bulk-mutation sites must take the
  scope.
- **Known coverage gap (accepted).** The catalog is baked from
  `view<Clip, VideoTexture>` with an allocated slot, so an
  audio-bearing clip whose slot isn't provisioned yet (~one frame after
  placement) is invisible to the audio fallback for that window. Slot
  provisioning completes within a frame; the editor path covers it
  outside stalls.

The 50 ms threshold is intentionally > one normal editor frame (16 ms).
A false-positive fire is harmless post-#74 (snapshot + atomics only,
idempotent), but the gate avoids double-steering and worker-map lock
traffic at 60 Hz during healthy operation. When the editor wakes back
up, its heartbeat refreshes and the fallback skips on the next
iteration.

**Currently covered systems:**
- `Timeline::update` — runs unconditionally on the show thread;
  `m_currentTime` is atomic.
- `DecodeSystem::tickFromSnapshot` — issue #74. Registry-free steering
  of existing decode workers from the clip catalog. (Video workers
  don't self-advance — they decode to `targetFrame` and park — so
  without this the display freezes ~270 ms into any stall.)
- `AudioSystem::tickFromSnapshot` — issue #74. Drives
  `mixSource.active` (a Loop clip ending mid-stall must go silent) and
  discontinuity re-seeks. Audio workers otherwise self-advance via the
  ring + device callback. Gain/mute/solo mirroring and the
  `hasAudioStream` registry write stay editor-only.
- `AnimationSystem` — baked into the snapshot, re-evaluated show-side
  per render frame (NEW-07).
- `SectionScheduler` — detection show-side, registry-mutating apply
  editor-side via R2D reply (NEW-08).

**Constraint for future fallback systems.** Per the invariant above:
a stall fallback must be a dedicated snapshot-consuming entry point
(`tickFromSnapshot`-style), never a re-entry of the registry-taking
`update()`. Atomic or show-thread-local state is fine;
`registry.get<T>(e).field = ...` — or even a registry *view iteration*
— is not.

**Known gaps** (tracked in `docs/reference/CODE_ISSUES.md`):
- ~~**NEW-07** AnimationSystem~~ — closed 2026-05-11 (snapshot bake).
- ~~**NEW-08** SectionScheduler~~ — closed (show-side detection +
  wall-clock continuation anchor).
- ~~**NEW-09** No regression test for the fallback~~ — closed by issue
  #74: `integration_decode_stall_fallback` (stall mid-playback; decode
  must advance through it, audio must not seek-storm) and
  `integration_load_during_playback` (the crash-class repro).

**Future Systems rule.** Any new editor-tick system that drives output
must, at design time, choose one of:

1. **Add a registry-free `tickFromSnapshot`** next to the existing
   DecodeSystem/AudioSystem calls, consuming `SceneSnapshot` + atomics
   only. Right when the per-tick work is fundamentally about advancing
   internal state (decode targets, seek positions).
2. **Bake results into `SceneSnapshot`** so the show thread reads
   pre-evaluated values from the snapshot instead of re-running
   evaluation. Right answer for systems that map registry data to
   render state (AnimationSystem is the prototype case).
3. **Accept the freeze** for that subsystem, document it in
   CODE_ISSUES.md, and confirm the freeze isn't user-visible.
   Acceptable for low-frequency systems (e.g. ContentScanner).

The diagnostic methodology is in
`docs/reference/TROUBLESHOOTING.md` under "Threading Issues" — when a
"frozen output" symptom surfaces, the first move is the marker test
(per-frame counter rendered into the same swap chain) to disambiguate
display-layer freezes from content-pipeline freezes.

## Alternatives considered

- **Option i (shared registry, fine-grained locking).** Rejected: `entt` is
  not designed for concurrent access; missed locks are silent data races;
  profiling D3D12 lock contention under real show load is expensive
  debugging work that option ii makes unnecessary.
- **One-tick double-buffer instead of triple-buffer for compose targets.**
  Would work for the D3D12 resource hazard, but double-buffer means the
  editor always displays the previous tick's frame. Triple-buffer lets the
  editor display the *current* tick's frame when the show thread beat the
  ImGui sample — the common case at 60 Hz.
- **Mutex around compose-target ImGui display.** Simpler, but any stall
  on the show thread (including `waitForGpu`) delays the editor, which
  defeats the point of splitting the threads.

## References

- ADR-0003 — Director/Renderer logical service split.
- `include/entity/bus/Message.hpp` — `SceneSnapshot`, `RenderFrame`,
  `ScreenRenderTargetAllocated`, `CreateOutputWindowRequest`, `OutputWindowReady`.
- `src/core/Engine.cpp` — `buildSceneSnapshot`, `buildRenderFrame`,
  `showThreadMain`, `drainRendererToDirector`.
- `src/systems/CompositorSystem.cpp` — `ensureScreenRenderTarget` R2D reply.
- `include/entity/command/Commands.hpp` — `Affinity` enum, per-command tags.
- `src/command/CommandDispatcher.cpp` — `processQueue(engine, affinity)`.
- `include/entity/render/D3D12Renderer.hpp` — `ComposeTarget::TRIPLE`,
  per-role allocators + fences.
- `docs/reference/CODE_ISSUES.md` — HIGH-02 (registry cross-thread), NEW-06
  (device-removed, show thread), MED-13 (infinite GPU wait).
