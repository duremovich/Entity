# Known Code Issues

Re-verified against the codebase on 2026-04-19; HIGH-02 line refs updated 2026-04-28 after the PlaybackController -> PlaybackTimeAuthority + PlaybackPresenter split (Phase D entry, subtask 6). HIGH-02 follow-up (#10) closed and MED-13 fixed 2026-05-08 by Phase A of issue #42; NEW-06 partially addressed (detection only). HIGH-02 fully closed and NEW-06 robust-recovery status updated 2026-05-08 by Phase B of issue #42 (editor/show thread split). NEW-07/08/09 added 2026-05-08 evening after the output-freeze-during-editor-drag debug session — same root cause as `a9bcd8b` (DecodeSystem fallback) but for AnimationSystem, SectionScheduler, and missing test coverage. Full original details in `docs/archive/CODE_REVIEW_2025-11-27.md`.

**Current state**: 0 Critical, 1 High, ~20 Medium, ~14 Low — down from 7 / 18 / 27 / 15.
NEW-07 closed 2026-05-11 (AnimationSystem snapshot-bake).
Phase 3.8 (2026-05-11): OA freeze hook landed — `SectionScheduler::seedContinuationAt`
freezes Locked OA layers at section breaks; `clearAllContinuation` unfreezes on GO/Stop.
NEW-08 closed 2026-05-20 (SectionScheduler show-thread detection): break-crossing
detection moved to the show thread; the editor applies it via
`SectionScheduler::handleBreakAt` from an R2D `SectionBreakDetected` drain, and
continuation phase is re-derived show-side from the wall-clock anchor.

Most issues have been fixed. This document was previously stale; the version before this rewrite listed all original issues as open even though commits had closed most of them. Always verify against current code before acting on an ID.

---

## Verification Method

Every ID below was cross-checked against:
1. `git log --grep="Fix <ID>"` — explicit fix commits
2. Direct file inspection at the flagged location
3. Recent refactors that may have incidentally addressed the issue

---

## Critical Issues

### Still Open

_(none)_

### Fixed (7)

| ID | Fixed In | Verified |
|----|----------|----------|
| CRIT-01 | 525b700 | FrameRingBuffer race fix with count validation + wraparound safety checks |
| CRIT-02 | d854aec | DecodeWorker use-after-free fixed via `std::shared_ptr<DecodeWorker>` |
| CRIT-03 | e102aca | Static `firstUpload` replaced with per-instance member |
| CRIT-04 | (Phase A, 2026-04-19) | Replaced single mapped constant buffer with per-frame ring of 64 slot × FRAME_COUNT. `beginFrame()` resets the cursor; each `drawOutputSurface()` call (formerly `drawMappingSurface()`, renamed in ADR-0021 M4) gets a unique 256-byte slot. Fence sync in `moveToNextFrame()` guarantees no CPU/GPU overlap. Also fixes the prior multi-surface-per-frame overwrite (all draws saw the last memcpy). D3D12Renderer.cpp:2023-2135. |
| CRIT-05 | 92456b1 | `m_currentFrame` now reuses buffer when dimensions match |
| CRIT-06 | 9c4df69 | `Clip` destructor + move semantics + deleted copy operators (Clip.hpp:70-152) |
| CRIT-07 | c1311d9 | `ProResDecoder::convertToRGBA()` buffer overflow protection added |

---

## High Priority Issues

### Still Open (1)

| ID | File:Line | Details |
|----|-----------|---------|
| HIGH-13 | HAPDecoder.cpp | Every method returns `Result::NotImplemented`. Phase A mitigation landed: factory no longer constructs the stub (Decoder.cpp now returns nullptr for HAP types with a clear log). Actual codec implementation deferred to Phase D. |

### Recently fixed

| ID | Fixed In | Details |
|----|----------|---------|
| HIGH-02 | 31b928b (Phase A1), Phase B of #42 (2026-05-08) | **Fully closed.** Phase A1 moved all per-clip data onto `bus::RenderFrame::activeClips` (transform / opacity / blend / target screen / section fade / z-order / mediaFrame / slot). Phase B landed the show thread (ADR-0014): editor thread is now the sole registry writer; `m_registryMutex` wrapper removed; `CompositorSystem::update` has zero registry writes. The `videoTex->descriptorSlot` data race (show thread reading a plain `uint32_t` written by the editor) was fixed by gating on `crs->slot >= 0` (baked from editor thread via `ClipCatalogEntry::descriptorSlot`) instead of reading `VideoTexture::descriptorSlot` on the show thread. Issue #10 closed. |

### Re-evaluated (Not Actually Bugs)

| ID | Re-evaluated | Reason |
|----|--------------|--------|
| HIGH-01 | 2026-04-19 | `shared_ptr<FrameRingBuffer>` is assigned once per clip (Timeline.cpp:295,443; DecodeSystem.cpp:378; Engine.cpp:1182,1419,1808) during main-thread clip setup, never reassigned. No concurrent writes to the shared_ptr itself occur. Atomic state fields handle cross-thread reads of the pointed-to buffer. Invariant should be documented but no code fix needed. |
| HIGH-16 | 2026-04-19 | Raw pointers `DecodeSystem*` / `AnimationSystem*` into `std::vector<std::unique_ptr<System>>` are actually stable. Vector reallocation moves unique_ptr wrappers but the managed System objects stay at their original heap addresses. No dangling pointer risk as long as systems aren't removed mid-run (they aren't). Fragile as a pattern — Phase B Engine decomposition will replace with explicit typed accessors — but not a live bug. |

### Fixed (13)

### Fixed (13)

| ID | Fixed In |
|----|----------|
| HIGH-03 | 9d83dfc — Seek error handling now preserves buffer on failure |
| HIGH-04 | 934407c — Compose target released in `shutdown()` |
| HIGH-05 | 934407c — Viewport/scissor set in `beginFrame()` |
| HIGH-06 | 934407c — Descriptor heap bounds check with logging |
| HIGH-07 | 934407c — Legacy `m_videoUploadBuffer` released in shutdown |
| HIGH-08 | 5d886a0 — AnimatedProperties copied on split/duplicate |
| HIGH-10 | 555db42 — MediaBinWindow O(n²) → O(n) |
| HIGH-11 | dba164a — Keyframe insertion O(n) binary-search-based |
| HIGH-12 | f5357e2 — PNG file size validation |
| HIGH-14 | (verified in code) — ProResDecoder now validates and clamps frame number (ProResDecoder.cpp:223-226, 261-268) |
| HIGH-15 | (superseded by CRIT-01 fix) — `isFull()` still racy to `push()` at FrameRingBuffer.cpp:16 but impact is a dropped frame, not corruption; acceptable per push contract |
| HIGH-17 | 519218f — PropertyWindow static widget state leak fixed |
| HIGH-18 | 5b8789f — `m_engine{nullptr}` initialized in MediaBinWindow |

---

## Medium Priority Issues

### Confirmed Fixed (7)

| ID | Fixed In |
|----|----------|
| MED-01 | e54c650 — Transform `mutable` removed |
| MED-04 | 354744b — TimelineTrack clips kept sorted |
| MED-08 | ad80ce1 — Frame duration uses integer arithmetic |
| MED-09 | 6c22ba9 — Premultiply alpha rounding corrected |
| MED-19 | dba164a — Keyframe lookup uses binary search |
| MED-20 | 78732ae — Timeline `m_playbackState` atomic |
| MED-22 | 5ef66e4 — MappingWindow dynamic surface naming |

### Still Open / Unverified (~20)

Not individually re-verified in this pass. The original list (MED-02, MED-03, MED-05, MED-06, MED-07, MED-10–18, MED-21, MED-23–27) is largely still applicable. Highlights of what matters most for Phase B:

- **MED-05** (Engine.cpp) — Three separate maps for per-clip data. Fragmentation; consolidate into a single per-clip struct. Touches Phase B Engine decomposition.
- ~~**MED-13** (D3D12Renderer.cpp:561-577) — `moveToNextFrame()` has INFINITE GPU wait. Real hang risk on device loss; pair with Phase A crash-recovery work.~~ **Fixed 2026-05-08 (commit d92fd13, Phase A2 of #42)** — replaced `WaitForSingleObject(INFINITE)` with a 2-second timeout that delegates to `handleDeviceLost` on timeout/failure. Two remaining INFINITE waits in `D3D12Renderer::waitForGpu()` (lines 731 / 750) are tracked under the NEW-06 robust-recovery follow-up.
- **MED-23** (OutputsWindow.cpp:112-127, renamed from MappingWindow in ADR-0021 M4) — UI selection state mixed with domain. Phase B UI/domain split should address.
- **MED-03** (VideoTexture.hpp:30-32) — Dangling `currentFrame` pointer risk. Audit ownership before Phase C output work.

---

## Low Priority Issues

### Confirmed Fixed (3)

| ID | Fixed In |
|----|----------|
| LOW-07 | 7e2e011 — FrameNumber sentinel uses proper max |
| LOW-09 | 8e3c748 — Expansion state cleared for invalid entities |
| LOW-14 | 5835577 (incidentally) — `MAX_COMPOSE_TARGETS` constant added, heap layout documented |

### Still Open

Remaining LOW-* items (LOW-01–06, LOW-08, LOW-10–13, LOW-15) largely still apply. Low impact; address opportunistically during Phase B refactors.

---

## New Issues Found During Verification

Not in the original review, surfaced while writing this update:

| ID | File:Line | Severity | Details |
|----|-----------|----------|---------|
| NEW-01 | HAPDecoder.cpp:20,46,67,90,109,127 | High | 6 `TODO` blocks spanning every method. The entire decoder is a scaffolded stub. Factory path (Decoder.cpp) constructs it anyway. |
| NEW-02 | Engine.cpp:91-92 | Low | Commented-out `Transport` class — dead code. Either implement or delete. |
| NEW-03 | Engine.cpp | Low | 4 `TODO` comments for unfinished integration points. |
| NEW-04 | OutputManager.cpp | ~~Medium~~ **Fixed (Phase C #1, 2026-04-20)** | OutputManager now drives physical displays. Per-output swap chains on `IRenderer`; `renderOutputs()` fans the selected Screen's compose target to each enabled Physical output with InputRegion UV cropping. Mapping-surface warping + soft edges deferred to Phase C #2. |
| NEW-05 | TestSystem.hpp | Medium | Pure skeleton. Phase A plan calls for replacing with a real integration test harness. |
| NEW-06 | D3D12Renderer device-removed handling | High | **Partially fixed — detection complete, recovery pending.** Phase A2 (#42, commit d92fd13): `handleDeviceLost` calls `GetDeviceRemovedReason()`, stores HRESULT in `m_deviceLostReason`, Engine posts `bus::DeviceLost` on R2D once and exits. Phase B (#42, 2026-05-08, ADR-0014): `handleDeviceLost` now always runs on the show thread (show thread owns all D3D12 Present calls); `m_deviceLostPosted` plain-bool is safe on the show thread (single writer). **Still pending**: robust device recovery — release/recreate device, re-provision GPU resources, resync show thread and editor thread. Two INFINITE waits in `D3D12Renderer::waitForGpu()` remain (called only from shutdown / resize paths, not the per-frame hot path). File as a follow-up issue. |
| NEW-07 | AnimationSystem.cpp / PlaybackTimeAuthority.cpp | ~~High~~ **Fixed (2026-05-11)** | **AnimationSystem editor-stall freeze.** Animated clip properties (opacity, transform, rotation, scale) used to freeze on the projector output during editor stalls because `AnimationSystem::update` only ticked from `Engine::update`. **Resolution**: per [`docs/design/animation-snapshot-bake.md`](../design/animation-snapshot-bake.md), `bus::ClipCatalogEntry` now carries `position`/`rotation`/`scale` plus a `tracks` vector (snapshot of `AnimatedProperties.tracks`). `PlaybackTimeAuthority::buildSceneSnapshot` bakes them on the editor thread; the show-side `buildRenderFrame` re-evaluates tracks at the current Timeline frame (already show-safe) via `applyBakedAnimation`, overriding `ClipRenderState::transformMatrix` / `opacity` for animated clips. Editor-side `AnimationSystem::update` still runs as belt-and-suspenders for the PropertyWindow / TimelineWidget UI surfaces that read `Transform` / `MediaLayer` directly. Evaluator math lives in `src/director/PlaybackTimeAuthority.cpp` anon namespace and must stay in sync with `KeyframeTrack::evaluate` in `include/entity/components/AnimatedProperties.hpp`. |
| NEW-08 | SectionScheduler.cpp / Engine.cpp | ~~Medium~~ **Fixed (2026-05-20)** | **SectionScheduler editor-stall freeze.** `SectionScheduler::tick` used to be the sole break-crossing detector and only fired from `Engine::update`, so during editor stalls breaks fired late and continuation phase froze. **Resolution** (per [`docs/design/section-scheduler-snapshot-bake.md`](../design/section-scheduler-snapshot-bake.md), with two divergences noted there): break-crossing *detection* moved to the show thread (`Engine::showThreadMain`, the `SectionDetect` zone) — it runs every show frame regardless of editor health, snaps + pauses the playhead, raises `Timeline::sectionAtBreak()`, and posts an R2D `bus::SectionBreakDetected`. The editor *applies* the crossing via the new `SectionScheduler::handleBreakAt` from `Engine::drainRendererToDirector` (seeds `ClipPlaybackPhase`, raises the scheduler latch). Crossing detection was removed from `tick()` entirely (single detector, no dual-detector race). Continuation phase is re-derived show-side from the wall-clock anchor (`ClipPlaybackPhase::continuationStartTimeNs/SeedFrames`, baked into `bus::ClipCatalogEntry`) in `mapToMediaFrameFromCatalog`, so Loop/PingPong clips keep cycling at a parked break during a stall. `go()` recomputes the phase from the anchor before snapshotting tail-hold frames. |
| NEW-09 | tests/integration/ | Low | **No regression test for editor-stall fallback.** The Timeline (`ee99a99`) and DecodeSystem (`a9bcd8b`) show-thread fallbacks have no automated coverage. A future refactor of `Engine::showThreadMain` or `m_lastEditorTickNs` could silently break the feature. Need an integration test that simulates editor heartbeat staleness and asserts both systems advance from the show thread. Tracked on roadmap. **Tracy note (2026-05-10):** A baseline .tracy capture taken after issue #43 Phase 8 can serve as a reference: if the show-thread fallback breaks, the `FrameMarkNamed("Show")` intervals will stop advancing during a simulated stall. |

---

## Recommended Next Actions (Phase A)

1. Gate `HAPDecoder` in the factory (return `NotImplemented` instead of constructing stub) — addresses HIGH-13 / NEW-01.
2. Add D3D12 device-removed recovery path — addresses NEW-06 and MED-13.
3. Audit remaining MED-03 / MED-05 before Phase B decomposition touches those files.
4. Delete `TestSystem.hpp` skeleton; replace with integration test harness — addresses NEW-05.
5. Either delete dormant mapped constant buffer or keep it documented — CRIT-04 stays latent either way, but the code is cleaner if deleted.

---

## Maintenance Notes

- When adding a new issue, use the next available number in its severity tier (don't renumber existing).
- When fixing an issue, include the ID in the commit message (`Fix HIGH-XX: ...`) so future audits can grep the log.
- Re-verify this doc whenever the last re-verification date is >3 months old. Stale bug lists are worse than no bug lists — they mislead both humans and AI assistants.
