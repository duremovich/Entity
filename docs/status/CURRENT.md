# Current Status

**Phase**: Phase C — Single-machine MVP. Physical output + surface-driven warp + project persistence are all in. Timeline UX overhaul, undo/redo for property + ripple commands, named sections, HAP decode pipeline + transcoder, and a playback-perf overhaul have all landed since the last update. Currently working through the **so-even-with-hap-cosmic-glacier** roadmap — the playback/render-engine deep-dive that targets Disguise/d3 architecture.
**Last Updated**: 2026-04-25

---

## Goal Recap

Production-quality commercial media server for live performance — a Disguise/Watchout/Pixera replacement. The foundational work (Phase A stabilization + Phase B architecture decomposition) is largely complete; next is Phase C (single-machine MVP feature work).

See `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md` for the master roadmap.
See `~/.claude/plans/so-even-with-hap-cosmic-glacier.md` for the **playback/render engine deep-dive** (Phase C.9 → D handoff). All five open questions are now answered (2026-04-25 decisions section). HAP-first codec, 512 MB frame cache w/ Settings dialog, Director/Renderer split before Phase D, full ACES end-to-end, cluster-ready plumbing from day one.

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

### Phase B — God-file Decomposition (8/8 done)

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

### Phase C — Single-machine MVP (in progress)

| # | Task | Commit |
|---|------|--------|
| 1 + 2 | Physical output driving (per-output swap chains) + surface-driven warp w/ q-trick perspective-correct corner-pin | `d8d9b0b`, `7854fca` |
| 3 | Screen + Model persistence in `ProjectSerializer` (PROJECT_VERSION 1→2, preserve-by-name load strategy) | `9bfb97c` |
| 3.5 | Undo/redo for property-slider commands (gain, opacity, transform sliders) | `087d0d7`, `32d6c6c` |
| 4.1–4.8 | Timeline UX overhaul — keyframe rounding fix, discrete zoom ladder, time-range selection, ripple insert/delete (undoable), Edit menu + key bindings, named sections w/ persistence, snap + zoom + gridlines + key-repeat polish, playhead auto-follow | `cf5c311`, `0a8a334`, `4878413`, `8388a9d`, `e65d32a`, `73099de`, `861a2a6`, `a4c29c7` |
| 5 | Playback perf round 1 — wall pacing under load + decode overhead reduction | `0ba96b3` |
| 6 | **HAP decode pipeline scaffolding** — HapFormat parser handles all 7 variants, HAPDecoder uses libavformat for demux but bypasses libavcodec for the BC-block payload, TextureUploader carries `TextureFormat` for BC{1,3,4,7,6H} uploads, `Decoder` factory probes `.mov` codec_tag for HAP variant routing. snappy added as a vcpkg dep. | `f5cabf8` |
| 6.1 | HapFormat parser unit tests (16 tests, hand-crafted byte fixtures, no FFmpeg dep) | `3ef53ca` |
| 7 | **HAP transcoder** — `HapTranscoder::transcodeToHap` (libavcodec encode), `integration_hap_roundtrip` end-to-end test, `test_media/hap_gradient/test.mov` procedural fixture, ffmpeg vcpkg `snappy` feature enabled | `93c4772` |
| 8 | **Playback perf round 2** — eliminate per-frame copies + remove main-thread sync decode. Steady-state ~5 → 50-500+ FPS depending on clip count; 5-simultaneous-4K-alpha stress holds 140-160 FPS | `59a047b` |
| C.1 #6.1 | TranscodeWorker + TranscodeManager (engine-level transcode-on-import) | `511b367` |
| C.1 #6.2 | `MediaLibraryEntry` (original + transcoded) persistence | `fe54fdb` |
| C.1 #6.3 | Route non-HAP imports through TranscodeManager | `cddcdb1` |
| C.1 #6.4 | MediaBin UI — status column + toolbar + context menu | `7ebde39` |
| C.1 #6.5 | HapTranscoder rounds non-4-aligned dims up via sws_scale | `798b013` |
| C.1 #6.6 | Keep Failed transcode workers visible in MediaBin | `ab0b87f` |
| C.1 #6.7 | MediaBin shows the transcode failure reason | `ab07b97` |
| C.1 #6.8 | DecodeSystem uses the main-thread decoder's path + type (HAP-vs-original disambiguation) | `db1ac2c` |
| C.1 #6.9 | Non-HAP import policy = Ask by default + modal prompt | `385559a` |
| C.1 #6.10 | MediaBin "Remove from library" + right-click hint | `90b46f9` |

---

## Verified Working (as of Phase C #8)

**Build**: `cmake --build build --config Release` is clean, no errors.
**Tests**: 104/104 pass per Phase C #8 commit message (~49 unit, 16 HapFormat unit, ~13 integration suites; HAP roundtrip + Phase B #8 gates included). Run via `ctest -C Release`.
**Binary**: `build/bin/Release/EntityMediaEditor.exe` launches and runs in windowed or `--headless` mode.

Integration tests wired to CTest, labelled `integration`:
- `integration_smoke` — cleared compose target hash
- `integration_multi_screen` — two user screens, hash both
- `integration_png_sequence_seek` — PNG seq decode + seek correctness
- `integration_seek_past_clip_end` — boundary behavior past clip duration
- `integration_screen_persistence_save` / `_load` — round-trip via CTest fixture; load asserts both default and custom screens survive serializer
- `integration_mixed_fps` — 16-frame seq forced to 24fps on a 30fps timeline; asserts tl-frame-10 maps to source frame 8 via `floor(localFrame * srcFps/tlFps)`
- `integration_ping_pong` — 16-frame seq stretched to 64 timeline frames in PingPong mode; asserts tl 8 is forward-phase source 8 and tl 24 is reverse-phase source 7 (mirror-index 15 - (24 % 16))
- `integration_blend` — two solid mid-tone clips stacked on track 0 + track 1; sets Normal/Add/Multiply/Screen on the top clip, asserts four distinct compose-target hashes. Mid-tone channels required: pure 0/1 are fixed points for these modes.
- `integration_blend_difference` — same fixture stack, sets Difference on the top clip, asserts the hash. Different code path from `integration_blend` (shader-based blend pipeline w/ snapshot SRV, not fixed-function blend state) — separate gate so a shader-blend regression is unambiguous.

---

## Known Issues & Gaps

### Actively broken / missing

- **HAP Q YCoCg shader path** — decoder produces BC3 textures with `HapColorSpace::YCoCg_scaled` but `composite_ps.hlsl` samples them as RGBA. HapY content displays as garbled green/orange. Called out as TODO in commit `93c4772`. **Next item up the queue per the so-even-with-hap-cosmic-glacier plan.**
- **HapM second plane (Hap Q Alpha)** — decoder logs warning and drops the alpha plane.
- **HAP HDR (HapH)** — decoder produces BC6H, but compose targets are UNORM8 → HDR values clip. Per Decision 4 in the plan, gated on the Phase C.12 ACES pipeline (FP16 compose targets).
- **Soft-edge / edge blending** — `MappingSurface` components have the data, shader code (`computeSoftEdge`) exists; visual verification on a warped surface still pending.
- **Audio** — zero pipeline.
- **H264/H265** — decodes via `ProResDecoder` (misnamed; it's a generic FFmpeg decoder). Phase C.1 #6 routes these through TranscodeManager → HAP on import by default. Renaming is opportunistic cleanup, not blocking.
- **Network / sync / control** — no OSC, DMX, Art-Net, MIDI, NDI, timecode, genlock.
- **Director/Renderer split** — single Engine class still owns everything. Plan calls this a Phase D entry condition (do it before Phase D feature work attaches to the wrong layer).
- **Frame cache** — per-clip ring buffer is the wrong shape for click-to-seek-into-already-decoded territory. Plan's Phase C.10 replaces with a sparse LRU `FrameCache` keyed by `(clipEntity, frameNumber)`, 512 MB default budget.
- **Settings/Preferences window** — none today; needs to land alongside the cache work to expose the budget.
- **Color pipeline** — sRGB-encoded compose targets, no per-codec input transforms, no per-output display transforms. Plan's Phase C.12 commits to full ACES end-to-end with FP16 compose targets.

### Recently resolved (sessions 2026-04-20 and 2026-04-21, all uncommitted)

- **All 13 blend modes now render.** Added a shader-blend pipeline (`shaders/composite_blend_ps.hlsl` + new `m_blendRootSignature` + `m_texturedPipelineStateBlend` PSO with `BlendEnable=FALSE`) for the 9 modes D3D12 fixed-function blend can't express (Overlay / SoftLight / HardLight / ColorDodge / ColorBurn / Darken / Lighten / Difference / Exclusion). Each `ComposeTarget` got a `snapshotResource` + SRV (new heap region `SNAPSHOT_BASE` in `DescriptorHeapLayout`); `drawTexturedQuad` snapshots the compose target via `CopyResource` before each shader-blend draw and the PS samples both fg (t0) + bg snapshot (t1, sampled at SV_POSITION/dims). Existing fixed-function PSOs (Normal/Add/Multiply/Screen) are unchanged — zero perf regression for those. UI dropdown expanded to 13 entries; `SetClipBlendMode` JSON command round-trips all 13 names. Regression-gated by new `integration_blend_difference` test (golden hash differs from all 4 fixed-function modes' goldens, proving the shader path actually executed).

- **Phase B #8 — blend-mode integration test (closes Phase B).** New `integration_blend` stacks two solid mid-tone clips (`test_media/blend_{bg,fg}/frame_000.png`, generated by `test_media/scripts/generate_blend_fixtures.py`) on track 1 + track 0 and captures one hash per blend mode (Normal/Add/Multiply/Screen). Mid-tone colors are required — the obvious first attempt with saturated red (from `gradient_seq`) produced four identical hashes because pure 0/1 channel values are fixed points for all four modes. With (0.50, 0.25, 0.75) × (0.40, 0.60, 0.20) every mode produces distinct output, so any regression in the compositor's blend-state wiring or shader dispatch fails the test.

- Phase B #20 CI fix — commits `28726b9`, `6182d53`, `d80e4a9`. CI green on a 2026-03 vcpkg baseline with imgui pinned to 1.89.7 + prebuilt gyan.dev FFmpeg in CI.
- Native Save / Save As / Open via `IFileDialog` — commits `8db06d1`, `78e5b5e`. Ctrl+S / Ctrl+Shift+S / Ctrl+O all wired.
- **Phase C #1 — Physical output driving.** `OutputManager` actually drives displays. New per-output swap-chain API on `IRenderer` (`createOutputWindow` / `resize` / `beginOutputFrame` / `clearOutputFrame` / `endOutputFrame`), each backed by a borderless GLFW window + `IDXGISwapChain3` on the assigned display. All output swap chains share the main command list; presented together in `endFrame()`. Engine wires `OutputManager` between compositor and ImGui in `render()`. MappingWindow exposes display dropdown, Source-Screen dropdown, routes Enable/Assign through `OutputManager`. Resolves `NEW-04`.
- **Phase C #2 — Surface-driven output rendering.** `OutputManager::renderToOutput` now iterates `MappingSurface` entities whose `outputIndex` matches the output, drawing each with corner-warp + (`InputRegion × surface.sourceUVs`) combined source UVs + per-surface softEdge/brightness/gamma multiplied with the output's. Outputs with zero surfaces fall back to a fullscreen InputRegion quad so setup is never staring at black.
- **Surface UX.** `+ Physical` auto-creates a fullscreen MappingSurface bound to the new output. Surface properties panel adds an Output dropdown and **sizing presets**: Stretch, Fit, Fit W, Fit H, 1:1 (pixel-perfect). Presets compute corners from source-vs-output dimensions (Screen → compose-target res, OutputDisplay → display res).
- **Perspective-correct corner-pin (q-trick).** Replaced affine UV interpolation with Heckbert's projective q-coordinate technique — eliminates the diagonal "paper fold" crease. CPU-side q computation in `drawMappingSurface` writes per-corner `q` to `sourceUVs[i].w`; VS interpolates `(u*q, v*q, q)`; PS divides `xy/z`. Rectangle case degenerates to identity (q=2 everywhere) — zero cost when no warp.
- **OutputDisplay + Clip.targetScreen persistence.** `OutputDisplay` now saves/loads in `ProjectSerializer`. Clip's `targetScreen` persists by Screen *name* (entt::entity isn't stable across sessions). `Engine::loadProject` pre-disables active outputs (so swap-chain slot IDs aren't leaked when entities are cleared), calls `OutputManager::syncCounterFromRegistry()` post-load to bump the index counter past loaded values, then re-enables any output saved as enabled (re-creates its window).
- **Reload bug fixes.**
  - `clip.loaded = true` in `ProjectManager::load`'s media callback (was left `false` by `ProjectSerializer`, so `DecodeSystem` skipped reloaded clips, leaving video-texture slots empty → cyan bleed-through on stage).
  - `utf8ToPath()` helper in `ProjectManager.cpp` for `std::filesystem::exists` checks. The default `path(string)` constructor on Windows interprets bytes as the active narrow codepage, mangling fullwidth/CJK/emoji filenames (e.g. youtube-dl uses `：` U+FF1A for `:`). Symptom: empty media bin + magenta colored-quad fallback after loading a project with non-ASCII filenames.
- **ESC scoped to outputs.** First ESC disables active physical outputs (live-show safety — accidental ESC mid-show no longer kills the editor). ESC quits only when no outputs are running. New physical outputs default to `enabled=false` with no display assigned (no instant screen-blank on `+ Physical`).
- **Phase C #3 — Screen + Model persistence.** `ProjectSerializer` now writes `models` (name + filepath) and `screens` (transform, dimensions, visibility + `modelName` link) arrays. `PROJECT_VERSION` bumped 1 → 2; v1 files still load (fall back to the default `Main Screen` created by `Engine::createDefaultScreen` at init). Load strategy is **preserve-by-name**: existing Models/Screens matching a saved name are updated in place (so the default Screen keeps its `renderTargetSlot` — compose-target slots currently can't be released). Entities with names not in the saved set get destroyed; saved entries with no existing match get created. User-imported OBJ models reload from their original filepath; the built-in default plane (empty `filepath`) is rebuilt via `createDefaultScreenMesh()`. Smoke-tested round-trip via `scripts/test_screen_persistence_{save,load}.json`.

### Remaining non-bug issues (from `docs/reference/CODE_ISSUES.md`)

- `HIGH-02` — playback state re-read pattern across a tick. Less dangerous now that state is atomic (MED-20 fix); the pattern survived the Phase B #15c PlaybackController extraction unchanged. Worth a follow-up cleanup.
- `HIGH-13` — **closed by Phase C #6** (`f5cabf8`); HAPDecoder is now real, just needs the YCoCg shader path for full HapY support.
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
- `test_media/scripts/generate_blend_fixtures.py` + `test_media/blend_{bg,fg}/frame_000.png` — mid-tone blend-test fixtures
- `scripts/integration/*.json` — integration test scripts
- `tests/goldens/**/*.hash` — FNV-1a pixel hashes
- `.github/workflows/ci.yml` — Windows CI with vcpkg caching + retry

**Architecture Decision Record:** `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md` — section "ADR-2026-04-19" documents the D3D12-now/Metal-later choice.

---

## Recommended Next Steps

Following the **so-even-with-hap-cosmic-glacier** roadmap (decisions locked 2026-04-25). ROI-ordered:

### Phase C.9 — Close out the HAP variant family

1. **HAP Q YCoCg shader path** *(in progress)* — branch `composite_ps.hlsl` (or new `composite_hapq_ps.hlsl` PSO) on the texture's `HapColorSpace`, do the scaled-YCoCg→RGB conversion. Add `integration_hap_q_roundtrip` golden test against a HapY fixture.
2. **HapM second plane** — wire the alpha-plane texture so Hap Q Alpha actually shows alpha.
3. **Per-variant integration tests** — `_alpha_`, `_q_`, `_q_alpha_`, `_alpha_only_`, `_r_`, `_hdr_` round-trip goldens. Locks the family in against regressions.
4. ~~**EOF off-by-one**~~ *(closed)* — `Result::EndOfStream` sentinel; HAPDecoder returns it on `AVERROR_EOF` and `DecodeSystem` parks `nextFrame` at the duration without logging. Underlying cause: FFmpeg-encoded HAP fixtures report `nb_frames=N` but the demuxer delivers `N-1` packets (encoder buffering quirk).
5. **Multi-clip stress with HAP files** — the perf gate: `clipVideos=` consistently <10 ms during steady-state with HAP content.

### Phase C.10 — FrameCache replaces the per-clip ring buffer

Sparse LRU cache keyed by `(clipEntity, frameNumber)`, 512 MB global budget, exposed in a new **Settings/Preferences** ImGui window. Eliminates re-decode on click-to-seek into already-viewed frames. ~1-2 weeks.

### Phase C.11 — Async copy queue

Dedicated `D3D12_COMMAND_LIST_TYPE_COPY` queue so uploads run in parallel with composite. Targets the residual `clipVideos=51-81 ms` blips under multi-clip stress. ~1 week.

### Phase C.12 — Full ACES color pipeline

OpenColorIO (or hand-rolled ACES 1.3 reference transforms). Per-codec input transforms, ACEScg linear working space, per-output display transforms (sRGB / Rec.709 / DCI-P3 / Rec.2020 / PQ HDR). Compose targets become FP16 — unblocks HAP HDR. ~2-3 weeks.

### Phase D entry — Director/Renderer split

Decompose `Engine` into `DirectorService` + `RendererService` with a serializable message bus. **Network-serializable from day one** (per Decision 5) — transport starts in-memory, becomes UDP in Phase E with no message-format changes. Asset references move to content-hash IDs. ~1.5 weeks; lands *before* Phase D feature work (timecode/OSC/audio/NDI) so each one attaches to the right layer.

### Out-of-scope-here but in the master plan

- Audio pipeline (Phase D)
- Undo/redo for non-property commands (Phase C, CommandDispatcher already exists)
- Soft-edge feather visual check (Phase C continuation)
- Mesh warp / cylindrical mapping (Phase C continuation)

---

## Pointers for the next session

- **Playback/render-engine deep-dive plan**: `~/.claude/plans/so-even-with-hap-cosmic-glacier.md` *(this is the active driver)*
- Master roadmap: `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md`
- Issue list: `docs/reference/CODE_ISSUES.md` (last verified 2026-04-19; needs another pass)
- Development history: `docs/status/HISTORY.md`
- Component rules + exceptions: `include/entity/components/CLAUDE.md`
- Integration test pattern: `scripts/integration/README.md`
- ADR for D3D12-vs-abstraction choice: in the master plan under "Architecture Decisions"
