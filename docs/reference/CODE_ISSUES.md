# Known Code Issues

Re-verified against the codebase on 2026-04-19; HIGH-02 line refs updated 2026-04-28 after the PlaybackController -> PlaybackTimeAuthority + PlaybackPresenter split (Phase D entry, subtask 6). HIGH-02 follow-up (#10) closed and MED-13 fixed 2026-05-08 by Phase A of issue #42; NEW-06 partially addressed (detection only). Full original details in `docs/archive/CODE_REVIEW_2025-11-27.md`.

**Current state**: 0 Critical, 1 High, ~19 Medium, ~13 Low — down from 7 / 18 / 27 / 15.

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
| CRIT-04 | (Phase A, 2026-04-19) | Replaced single mapped constant buffer with per-frame ring of 64 slot × FRAME_COUNT. `beginFrame()` resets the cursor; each `drawMappingSurface()` gets a unique 256-byte slot. Fence sync in `moveToNextFrame()` guarantees no CPU/GPU overlap. Also fixes the prior multi-surface-per-frame overwrite (all draws saw the last memcpy). D3D12Renderer.cpp:2023-2135. |
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
| HIGH-02 | 6944415 (Phase A1 of #42, 2026-05-08) | Structurally closed. CompositorSystem::update now consumes `bus::RenderFrame::activeClips` directly (transform / opacity / blend / target screen / section fade / z-order / mediaFrame / slot all on the snapshot). Registry stays as a parameter for Screen enumeration + MappingSurface calibration only. The "future RenderFrame payload could expose per-active-clip playState that drifts" concern from the prior entry no longer applies — there is one source of truth per tick. Issue #10 (the follow-up audit card) closed by this commit. |

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
- ~~**MED-13** (D3D12Renderer.cpp:561-577) — `moveToNextFrame()` has INFINITE GPU wait. Real hang risk on device loss; pair with Phase A crash-recovery work.~~ **Fixed 2026-05-08 (commit 1c5ccb4, Phase A2 of #42)** — replaced `WaitForSingleObject(INFINITE)` with a 2-second timeout that delegates to `handleDeviceLost` on timeout/failure. Two remaining INFINITE waits in `D3D12Renderer::waitForGpu()` (lines 731 / 750) are tracked under the NEW-06 robust-recovery follow-up.
- **MED-23** (MappingWindow.cpp:112-127) — UI selection state mixed with domain. Phase B UI/domain split should address.
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
| NEW-06 | D3D12Renderer device-removed handling | High | **Partially fixed 2026-05-08 (commit 1c5ccb4, Phase A2 of #42)** — detection landed: `handleDeviceLost` now calls `GetDeviceRemovedReason()`, stores the HRESULT into `m_deviceLostReason` before flipping the latch (matters once handleDeviceLost runs on the show thread post-#42-Phase-B), `IRenderer` exposes `int32_t getDeviceLostReason()`, Engine posts `bus::DeviceLost` on R2D once and exits cleanly. **Robust device recovery still pending**: release/recreate device, re-provision GPU resources, resync UI thread. Two remaining INFINITE waits at `D3D12Renderer.cpp:731,750` (`waitForGpu`) and the `m_deviceLostPosted` plain-bool → atomic-bool migration belong here. Track under a new follow-up issue keyed off NEW-06 with #42 as trigger. |

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
