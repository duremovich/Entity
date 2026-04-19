# Current Status

**Phase**: Phase A — Stabilization (see `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md`)
**Last Updated**: 2026-04-19

---

## Goal Recap

Production-quality commercial media server for live performance — a Disguise/Watchout/Pixera replacement. Current work is not feature development; it's stabilizing the foundation so feature work stops regressing.

---

## Phase A: Stabilize the Foundation

Before any new feature work. Every regression now costs 10x when debugging at 6pm the day of a show.

### Progress

| # | Task | State |
|---|------|-------|
| 1 | Rewrite `docs/reference/CODE_ISSUES.md` from ground truth | Done 2026-04-19 |
| 2 | Update stale sub-CLAUDE.md files (components, render, systems) | Done 2026-04-19 |
| 3 | Update `docs/status/CURRENT.md` to reflect Phase A | Done 2026-04-19 |
| 4 | Gate HAPDecoder in the decoder factory | Done 2026-04-19 |
| 5 | Integration test harness scaffolding (`--headless`, `CaptureHash`, smoke + multi_screen tests) | Done 2026-04-19 |
| 6 | Verify / fix remaining real critical bugs | Done 2026-04-19 (CRIT-04 fixed; HIGH-01/-16 re-evaluated as non-bugs) |
| 7 | Crash-recovery baseline (D3D12 device-removed, decode exception safety, auto-save) | Done 2026-04-19 |
| 8 | Add integration tests (seek, mixed-fps, ping-pong, blend modes, round-trip) | Partial — 2 tests passing; media-dependent tests blocked on #10 |
| 9 | CTest + GitHub Actions CI wiring | Done 2026-04-19 |
| 10 | Procedural PNG sequence generator for decode-heavy tests | Pending |

**Phase A is effectively complete for foundation work. 51/51 tests pass under CTest.** Remaining items (#8 expansion, #10 media generator) are extensions, not blockers.

### Rationale for Phase A before Phase B/C/D

Recent git history (last 20 commits): 16 bug fixes, 2 architecture, 2 docs. The regression treadmill will not stop without automated tests, and for a commercial live-performance product the reliability bar is absolute. Phase A is the cost of entry for everything after it.

---

## Verified State Summary

### What works (keep, don't touch)
- ECS core (entt registry, ~13 components, ~7 systems)
- ProRes 4444 decode (FFmpeg, end-to-end)
- PNG sequence decode (stb_image)
- Timeline: multi-track, transport, seek, split/duplicate with keyframes, ping-pong, mixed frame rate
- Keyframe animation (6 properties × 5 interpolation types, binary-search lookup)
- Compositor: z-order, 13 blend modes, per-screen routing via `targetScreen`
- Corner-pin projection mapping (4-corner quad warp, surface editor UI)
- Project save/load (JSON round-trip with version field)
- 3D stage preview (grid, orbit camera, screen quads)
- ImGui docking UI (8 windows)
- FrameRingBuffer (recently hardened, has unit tests)

### What's stubbed / broken
- HAPDecoder: factory constructs it, every method returns `NotImplemented` (Phase A task 4)
- Physical output driving: `OutputManager` enumerates but doesn't drive displays (Phase C)
- Soft-edge/edge-blending: data model exists, shader not wired (Phase C)
- Audio: no pipeline (Phase D)
- Network/sync/control: no OSC/DMX/Art-Net/MIDI/NDI/timecode/genlock (Phase D+)

### Known critical issues (see `docs/reference/CODE_ISSUES.md`)
- **CRIT-04** (live): mapping-surface constant buffer writes without GPU fence sync. Race risk under high load.
- **NEW-06**: no D3D12 device-removed recovery path.
- **HIGH-01, HIGH-02, HIGH-13, HIGH-16**: detailed in issues doc.

---

## Active Files (Phase A scope)

- `docs/reference/CODE_ISSUES.md` — ground-truth issue list
- `src/media/Decoder.cpp` — HAP factory gating
- `src/media/HAPDecoder.cpp` — decide keep-stub vs remove
- `src/render/D3D12Renderer.cpp:2053-2100` — CRIT-04 ring buffer fix
- `src/render/D3D12Renderer.cpp:561-577` — MED-13 infinite GPU wait
- `scripts/` — extend with headless test runner
- `tests/` — expand beyond FrameRingBuffer

---

## Phase B–E+ Preview

Roadmap lives in `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md`. Not in scope until Phase A exits.

- **Phase B**: Decompose D3D12Renderer (2,598 → 4 classes), Engine (1,966 → 3), TimelineWidget (2,121 → UI/input/command split). ECS hygiene pass.
- **Phase C**: Physical output driving, soft-edge feather shader, edge blending, undo/redo, save-file migration, color management foundation, transport hardening (cue stacks).
- **Phase D**: LTC/MTC timecode, OSC, audio, HAP real impl, NDI output, preview/program workflow.
- **Phase E+**: Multi-machine sync, genlock, shader effect pipeline, color correction + LUTs, mesh warp, DMX/Art-Net, 3D content rendering, hardware output cards, show-file redundancy.

---

## Architecture Notes

### Frame Rate Handling (landed 2025-11-28)
- **Timeline frame rate**: project-fixed (e.g. 30fps)
- **Clip frame rate**: source video native rate (e.g. 24fps)
- **Duration**: stored in timeline frames (`clip.duration = totalMediaFrames × timelineFPS/sourceFPS`)
- **Frame mapping**: `Engine::mapToMediaFrame()` converts timeline → source
- **Formula**: `sourceFrame = timelineLocalFrame × (sourceFPS / timelineFPS)`

### Multi-Screen Rendering (landed 2025-11-28)
- Each Screen entity gets unique `renderTargetSlot` (lazy allocated)
- CompositorSystem iterates all visible screens, composites each independently
- Clips filtered by `targetScreen` during composition (null = all screens)
- StageWindow 3D view displays each screen with its unique texture
- Descriptor heap: slot 0 = ImGui, slot 1 = legacy, slots 2–9 = compose targets (MAX=8), slots 10+ = video textures

See `docs/status/HISTORY.md` for full phase history.
