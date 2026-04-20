# Current Status

**Phase**: Phase B — God-file decomposition (done except more integration tests)
**Last Updated**: 2026-04-20

---

## Goal Recap

Production-quality commercial media server for live performance — a Disguise/Watchout/Pixera replacement. The foundational work (Phase A stabilization + Phase B architecture decomposition) is largely complete; next is Phase C (single-machine MVP feature work).

See `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md` for the full roadmap and architecture decisions.

---

## Where We Are

### Phase A — Stabilization ✅ Complete

All seven foundation items landed:

| # | Task | Commit |
|---|------|--------|
| 1 | Rewrite stale `CODE_ISSUES.md` from ground truth | `f35c9ae` |
| 2 | Update stale sub-CLAUDE.md files | `f35c9ae` |
| 3 | Update `CURRENT.md` | `f35c9ae` |
| 4 | Gate HAPDecoder in factory (returns explicit error, no silent-stub) | `f35c9ae` |
| 5 | Integration test harness (`--headless`, `CaptureHash`, 4 tests) | `f35c9ae`, `3b032dc` |
| 6 | CRIT-04 fixed (per-frame ring of mapping-surface CB slots) | `f35c9ae` |
| 7 | Crash-recovery baseline (device-lost detection, decode exception safety, 30s autosave) | `f35c9ae` |
| 9 | CTest + GitHub Actions CI wiring | `f35c9ae` |
| 10 | Procedural PNG sequence generator + 2 decode-heavy tests | `3b032dc` |

**Critical count 7 → 0.** `NEW-06` (device-removed) fixed in Phase A task 7. `HIGH-01`, `HIGH-16` re-evaluated as non-bugs with invariants documented in code.

### Phase B — God-file Decomposition (7/7 done)

Architectural re-shape so neither D3D12 nor Engine internals leak through the project. Per `ADR-2026-04-19`: D3D12 stays native, `IRenderer` interface preserves optionality for a future Metal backend.

| # | Task | Commit |
|---|------|--------|
| 17 | Define `IRenderer` pure-virtual interface + opaque `TextureRef` | `070b556` |
| 18 | Eliminate D3D12 leakage outside `src/render/` (VideoTexture, CompositorSystem, OutputManager, StageWindow) | `ecadc83` |
| 19 | Offline HLSL compilation via DXC (kills runtime `d3dcompiler.dll` dep, unblocks MSL via SPIRV-Cross) | `7d8a7c8` |
| 12 | Extract `DescriptorHeapLayout` (slot-index math, heap constants) | `b224bde` |
| 13 | Extract `TextureUploader` (video-texture pool + upload pipeline) | `2cdf086` |
| 14 | Extract `D3D12Device` (device + queue ownership) | `9b6a142` |
| 15a | Promote `ClipDecodeState` to ECS component (DOTS-correct) | `9f03af1` |
| 15b | Extract `ProjectManager` (project path, media library, autosave) | `891b6ae` |
| 16 | Split `TimelineWidget.cpp` (2,121 → 347 core) into render/input/core | `e5b78a3` |
| 15c | Extract `PlaybackController` from Engine (frame timing + clip-frame math + per-frame seek-aware updates) | `f0a47c2` |
| 11 | ECS hygiene pass — Transform/AnimatedProperties/Clip documented as principled exceptions, no churn | (docs) |
| 20 | CI fix — gyan.dev prebuilt FFmpeg in CI, vcpkg baseline bumped 2023-08 → 2026-03, imgui pinned 1.89.7 via overrides | `28726b9`, `6182d53`, `d80e4a9` |

**Still pending in Phase B:**

| # | Task | Notes |
|---|------|-------|
| 8 | More integration tests (mixed-fps, ping-pong, blend, round-trip) | Blocked on: needs new script commands (`SetClipPlaybackMode`, `SetClipFramerate`); blend test requires non-trivial scene. |

---

## Verified Working (as of Phase B #15c PlaybackController extraction)

**Build**: `cmake --build build --config Release` is clean, no errors.
**Tests**: 53/53 pass (49 unit, 4 integration) under `ctest -C Release`.
**Binary**: `build/bin/Release/EntityMediaEditor.exe` launches and runs in windowed or `--headless` mode.

Integration tests wired to CTest, labelled `integration`:
- `integration_smoke` — cleared compose target hash
- `integration_multi_screen` — two user screens, hash both
- `integration_png_sequence_seek` — PNG seq decode + seek correctness
- `integration_seek_past_clip_end` — boundary behavior past clip duration

---

## Known Issues & Gaps

### Actively broken / missing

- **No Save As / Open file dialogs.** Ctrl+S writes to `project.entity` in current working directory if no project is open — hardcoded fallback. Real product needs Win32 `IFileDialog`.
- **Physical output driving** — `OutputManager` enumerates displays but doesn't drive them. Phase C.
- **Soft-edge / edge blending** — `MappingSurface` components have the data, no shader path.
- **Audio** — zero pipeline.
- **HAP codec** — gated in factory, unimplemented.
- **H264/H265** — not supported.
- **Network / sync / control** — no OSC, DMX, Art-Net, MIDI, NDI, timecode, genlock.

### Remaining non-bug issues (from `docs/reference/CODE_ISSUES.md`)

- `HIGH-02` — playback state re-read pattern across a tick. Less dangerous now that state is atomic (MED-20 fix); Phase B refactor will remove the pattern when `PlaybackController` lands.
- `HIGH-13` — HAPDecoder not implemented (gated in factory).
- ~20 medium-priority items; see the doc.

---

## Architecture Inventory

**New classes / files since we started:**

- `include/entity/render/IRenderer.hpp` — backend-agnostic interface with `TextureRef` opaque handle
- `include/entity/render/D3D12Device.hpp` + `src/render/D3D12Device.cpp` — device + queue ownership
- `include/entity/render/DescriptorHeapLayout.hpp` — header-only slot-index math
- `include/entity/render/TextureUploader.hpp` + `src/render/TextureUploader.cpp` — video texture pool
- `include/entity/components/ClipDecodeState.hpp` — per-clip decoder + frame as ECS component
- `include/entity/project/ProjectManager.hpp` + `src/project/ProjectManager.cpp` — project path, media library, autosave
- `test_media/scripts/generate_gradient_seq.py` — procedural PNG fixture generator
- `test_media/gradient_seq/*.png` — 16-frame HSV gradient committed as test fixture
- `scripts/integration/*.json` — integration test scripts
- `tests/goldens/**/*.hash` — FNV-1a pixel hashes
- `.github/workflows/ci.yml` — Windows CI with vcpkg caching + retry

**Architecture Decision Record:** `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md` — section "ADR-2026-04-19" documents the D3D12-now/Metal-later choice.

---

## Recommended Next Steps

1. **Ship Save/Open file dialogs.** Small, high-impact for real use. Win32 `IFileDialog` via `WindowManager`.
2. **Fix CI.** Most pragmatic path is using `FedericoCarboni/setup-ffmpeg` GH Action for pre-built FFmpeg, keep vcpkg for local dev. Baseline bisect is also an option.
3. **Phase C kickoff** — physical output driving, soft-edge feather, edge blending, undo/redo, color management. Phase B decomposition is done.

After those, Phase C begins (physical output, soft-edge feather, edge blending, undo/redo, color management).

---

## Pointers for the next session

- Full assessment + plan: `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md`
- Issue list (up-to-date): `docs/reference/CODE_ISSUES.md`
- Development history: `docs/status/HISTORY.md`
- Component rules + exceptions: `include/entity/components/CLAUDE.md`
- Integration test pattern: `scripts/integration/README.md`
- ADR for D3D12-vs-abstraction choice: in the plan file under "Architecture Decisions"
