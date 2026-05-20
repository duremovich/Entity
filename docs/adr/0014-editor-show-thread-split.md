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
(DecodeSystem).*

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
before it builds its RenderFrame:

```cpp
const int64_t lastEditor = m_lastEditorTickNs.load(std::memory_order_relaxed);
const int64_t nowNs = /* QPC ns */;
constexpr int64_t kEditorStaleNs = 50'000'000; // 50 ms
if (lastEditor != 0 && (nowNs - lastEditor) > kEditorStaleNs) {
    m_timeline->update(m_timeAuthority->getDeltaTime());
    if (m_decodeSystem) {
        m_decodeSystem->update(m_registry,
            static_cast<float>(m_timeAuthority->getDeltaTime()));
    }
}
```

The 50 ms threshold is intentionally > one normal editor frame (16 ms)
so the editor and show threads never both run a system in the same wall
second during steady-state playback. When the editor wakes back up, its
heartbeat refreshes and the show-thread fallback skips on the next
iteration.

**Currently covered systems:**
- `Timeline::update` — `ee99a99`. Safe because `m_currentTime` is
  atomic; advancing it from either thread is well-defined.
- `DecodeSystem::update` — `a9bcd8b`. Safe because the only state
  it mutates per call is `worker->targetFrame.store(...)` (atomic).
  The `view<Clip, FrameBuffer>` iteration is read-only on the registry,
  and during a stall the editor isn't writing.

**Constraint for future fallback systems.** Per the "sole writer" rule
above, a system can only be called from the show thread during the
fallback window if it does **not** write registry components. Atomic
or thread-local state is fine; `registry.get<T>(e).field = ...` is not.

**Known gaps** (tracked in `docs/reference/CODE_ISSUES.md`):
- **NEW-07** AnimationSystem freezes during editor stalls. It writes
  `Transform` and `MediaLayer` components, so a naive show-thread
  fallback violates the constraint.
- **NEW-08** SectionScheduler freezes the same way. It mutates
  `Timeline` section state and `ClipPlaybackPhase` components.
- **NEW-09** No regression test for the fallback. `ee99a99` and
  `a9bcd8b` could silently break in a future refactor.

**Future Systems rule.** Any new editor-tick system that drives output
must, at design time, choose one of:

1. **Avoid registry writes** in `update` and add a show-thread fallback
   call next to the existing Timeline / DecodeSystem block. Cleanest
   if the system's per-tick work is fundamentally about advancing
   internal state.
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
