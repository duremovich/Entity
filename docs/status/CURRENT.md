# Current Status

**Phase**: Phase C — Single-machine MVP. Physical output + surface-driven warp + project persistence are all in. Timeline UX overhaul, undo/redo for property + ripple commands, named sections, HAP decode pipeline + transcoder, playback-perf overhaul, Settings UI, **engine-global FrameCache** (replacing per-clip FrameRingBuffer), and **async D3D12 COPY queue** for texture uploads have all landed. **Phase C.12 OCIO-native color pipeline is COMPLETE** (pivoted 2026-04-27 from hand-rolled ACES to match Disguise's direction; subtasks #1-#11 all shipped). OcioManager + runtime DXC + FP16 compose + GPU processor cache + rendering integration + per-decoder tagging + golden rebake + Settings/Preferences "Color" section + per-output OCIO display+view UI + project persistence (PROJECT_VERSION 5→6) + MediaBin per-clip OCIO input color-space override + HAP HDR (BC6H_UF16) FP16 survival smoke test + **OCIO ODT correctness CPU-side unit tests + ACES end-to-end smoke integration test**. **CI green at 143/143** (135 + 7 new `OcioOdtTests` + new `integration_aces_smoke`). After C.12, the Director/Renderer split is the only remaining Phase D entry condition.
**Last Updated**: 2026-04-28

---

## Goal Recap

Production-quality commercial media server for live performance — a Disguise/Watchout/Pixera replacement. The foundational work (Phase A stabilization + Phase B architecture decomposition) is largely complete; next is Phase C (single-machine MVP feature work).

See `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md` for the master roadmap.
See `~/.claude/plans/so-even-with-hap-cosmic-glacier.md` for the **playback/render engine deep-dive** (Phase C.9 → D handoff). All five open questions are now answered (2026-04-25 decisions section). HAP-first codec, 512 MB frame cache w/ Settings dialog, Director/Renderer split before Phase D, full ACES end-to-end, cluster-ready plumbing from day one.

**Active driver:** `~/.claude/plans/quick-thought-before-we-pure-thompson.md` — the OCIO-native rewrite of Phase C.12 (supersedes `~/.claude/plans/jaunty-launching-tulip.md`, which committed to hand-rolled ACES). Decision: OCIO is the right abstraction for a Disguise/Watchout-class media server; user .ocio configs + camera log encodings + 3D LUTs all fall out for free, where hand-rolled ACES would force a rewrite the moment a user shows up with LogC footage. The bundled ACES Studio Config 1.3 ships inside the OCIO library (since 2.4) — no on-disk resource needed.

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
| C #9.1 | HAP Q YCoCg shader path + `integration_hap_q_roundtrip` | `525b6ee` |
| C #9.2 | `Result::EndOfStream` sentinel — silence HAPDecoder EOF spam | `973f91d` |
| C #9.3 | `integration_hap_basic_roundtrip` (Hap1 / BC1 RGB) | `4998e65` |
| C #9.4 | ProResDecoder also returns `EndOfStream` on EOF | `30e3df9` |
| C #10.0 | Settings/Preferences window + machine-global JSON config (`%APPDATA%/Entity/settings.json`) | `2d35dd0` |
| C #10.1 | **FrameCache** replaces per-clip FrameRingBuffer end-to-end. Sparse LRU keyed by `(clipEntity, FrameNumber)`, `shared_ptr<const DecodedFrame>` + RAII `FrameLease` for safe eviction-while-leased, single-mutex thread-safety, live-tunable budget via `setMaxBytes` (Preferences "Apply" wires through). Decode-thread fast path: `cache.has` short-circuits FFmpeg seek when re-seeking into cached territory — that's the click-to-recently-viewed-frame zero-decode-work case. PlaybackController reads via lease + `nearestTo` fallback during Playing only. `FrameRingBuffer` + `BufferSystem` deleted; `FrameBuffer` is now a 0-byte marker; `DecodedFrame` in its own header. 13 cache unit tests + 2 integration tests (`cache_hit_after_seek`, `cache_budget_stress`) gate both halves of the Phase C.10 done-when. | `19da3e0` |
| C #11 | **Async D3D12 COPY queue.** `D3D12Device` owns a second `D3D12_COMMAND_LIST_TYPE_COPY` queue. `D3D12Renderer` owns a per-`FRAME_COUNT` ring of copy command allocators + a single `m_copyCommandList` + a monotonic `m_uploadFence`. `uploadVideoFrameToSlot` records into the copy list; `endFrame` executes it on the copy queue, signals the upload fence, has the direct queue `Wait` on it (only if uploads were recorded this frame), then executes the direct list — copy work overlaps with last frame's render and this frame's composite waits only as long as the upload actually takes. `TextureUploader` strips all explicit barriers — textures rest in `D3D12_RESOURCE_STATE_COMMON`, implicit promotion handles `COMMON → COPY_DEST` and `COMMON → PIXEL_SHADER_RESOURCE` (the COPY queue can't transition to PIXEL_SHADER_RESOURCE anyway, so this is the only correct model). `waitForGpu` drains both queues. 102/102 green. PIX overlap verification is user-local manual work; the cross-queue handoff is exercised by all 17 integration tests on real GPU. | `ea62168` |
| C.12 #1 | **OCIO dependency + `OcioManager` skeleton.** `opencolorio` (vcpkg 2.5.1) added; `find_package(OpenColorIO CONFIG REQUIRED)` linked into `EntityMediaCore`. `OcioManager` loads the bundled ACES Studio Config 1.3 via `Config::CreateFromBuiltinConfig("studio-config-v2.1.0_aces-v1.3_ocio-v2.3")` — no on-disk resource needed since OCIO 2.4+ ships builtins inside the library. `CreateFromFile` overrides when `Settings.ocioConfigPath` is set; falls back through the rest of `BuiltinConfigRegistry` (recommended-only) and ultimately to `CreateRaw` so the editor always launches with a non-null config. Read-only accessors (config / displays / views / color spaces / defaults) feed the Settings + MappingWindow UIs in C.12 #7-8. No rendering path consumes OcioManager yet — that lands in C.12 #4. 5 unit tests gate the loader. | `f2e5f09` |
| C.12 #2 | **Runtime DXC compile path.** `RuntimeShaderCompiler` wraps `IDxcCompiler3::Compile` against in-memory HLSL source. Used by C.12 #5 to splice OCIO-emitted function fragments onto `composite_ps`/`mapping_surface_ps` — those PSOs become config-dependent and can't stay on the offline DXC path. Cache keyed by FNV-1a of `(source, entry, profile, includeDirs, extraArgs)`; failed compiles are cached too so a bad OCIO config doesn't re-attempt every frame. `dxcompiler.lib` linked from the Windows SDK; `dxcompiler.dll` + `dxil.dll` deployed next to `EntityMediaEditor.exe` via POST_BUILD copy keyed off the same `DXC_EXECUTABLE` the offline shader build uses (dxil signs the DXIL). 6 unit tests gate compile/cache/error-reporting behavior. | `08b87f4` |
| C.12 #3 | **FP16 compose targets + capture-buffer pass (CI red opens).** Compose targets flip `R8G8B8A8_UNORM → R16G16B16A16_FLOAT` to give the OCIO display transform unclamped headroom. Affected: `createComposeTarget` + 3 PSOs (solid-color, composite-textured base, shader-blend); the mapping-surface PSO + all swap chains stay UNORM8 (sRGB SDR remains the default output). Snapshot resources go lazy — workloads without shader-blend modes save ~265 MB. New capture-buffer + tone-mapping pass: an UNORM8 m_captureResource gets fed by `aces_capture_vs.hlsl` + `aces_capture_ps.hlsl` (sRGB-only stub for now; subtask 5 swaps for an OCIO-emitted display transform). Goldens hash this UNORM8 capture, not raw FP16, so they stay portable. `captureComposeTargetToPNG` + `readComposeTargetPixels` route through the new pass. **CI: 96/96 unit green, 8/17 integration tests fail with hash mismatches (no crashes)** — closes at subtask 6. | `9b8c361` |
| C.12 #4 | **OcioManager GPU processor cache + GpuShaderDesc bake.** `buildInputProcessor(srcCs, working="ACEScg")` + `buildDisplayProcessor(working, display, view)` both call `Config::getProcessor` → `getDefaultGPUProcessor` → `extractGpuShaderInfo(GPU_LANGUAGE_HLSL_DX11)` and fold the result into a project-side `OcioGpuProcessor` value type that owns: HLSL function source, OCIO's sanitized function name (OCIO collapses consecutive underscores — read `getFunctionName()` back from the desc, don't trust the name we passed in), 1D + 2D + 3D LUT metadata + LUT data copied off OCIO's pointers, uniform descriptors with the OCIO Processor kept alive via `ConstProcessorRcPtr` (the getter callbacks reference processor-internal state). `unordered_map<key, shared_ptr<const OcioGpuProcessor>>` cache guarded by a mutex; `clearProcessorCache()` for the subtask-7 Settings reload path. Source-name mangling produces stable HLSL identifiers (every non-`[A-Za-z0-9_]` becomes `_`). DescriptorHeapLayout reserves `OCIO_LUT_BASE` region (32 slots) past `SNAPSHOT_BASE`. **No rendering path consumes `OcioGpuProcessor` yet** — subtask 5 wires it. 8 unit tests gate emission contracts. | `0f4ba2a` |
| C.12 #5 | **OCIO into composite_ps + mapping_surface_ps + capture.** Splice strategy turned out far simpler than feared: the bundled ACES Studio config emits self-contained HLSL with **zero textures and zero uniforms** — just static const arrays + helper fns + the main transform fn. `RuntimeShaderCompiler` concatenates OCIO's text onto the host shader and compiles with `-D ENTITY_OCIO_INPUT_FN=<name>` / `-D ENTITY_OCIO_DISPLAY_FN=<name>`. Host shaders gate the call sites with `#ifdef` so the offline-compiled path stays identical to pre-C.12. Per-input-CS PSO cache (5 PSOs each: 4 fixed-function blends + 1 shader-blend), per-(display,view) cache for mapping_surface, plus a pinned `m_ocioCapturePipelineState` for the canonical sRGB display+view that the integration-test capture pass uses. `setOcioManager(mgr)` eagerly bakes default input + display + capture PSOs at startup. CMake POST_BUILD copies `composite_ps.hlsl`/`composite_blend_ps.hlsl`/`mapping_surface_ps.hlsl`/`aces_capture_ps.hlsl` + `common.hlsli`/`mapping_surface.hlsli` next to the exe so RuntimeShaderCompiler can read them. CI red continued (6/17 integration). | `6161557` |
| C.12 #6 | **Per-decoder OCIO color-space tagging + golden rebake (CI red CLOSES).** `DecodedFrame` + `VideoTexture` grow `std::string ocioColorSpace`; PlaybackController stamps it on upload, CompositorSystem hands it to `drawTexturedQuad`, the renderer uses it to pick the per-clip OCIO PSO. Decoder tags: HAP RGB / HapY (post-YCoCg) → `"Linear Rec.709 (sRGB)"`, HapA → `"Raw"` (identity), ProRes via `srcFrame->colorspace` (BT.2020 → `"Linear Rec.2020"`, else default Rec.709), PNG → `"sRGB - Display"`. All 22 goldens rebaked + spot-verified visually. Latent C.12 #3 capture-pass bug surfaced + fixed in transit: `CopyTextureRegion(srcBox=nullptr)` on the reused-but-bigger `m_captureResource` returned `E_INVALIDARG` on the second screen — pass an explicit srcBox bounded to the current compose target. Was masked during the red window because the first hash mismatch always exited the test before the second screen. 2 new unit tests in `DecoderColorSpaceTests`. **123/123 green** (was 104/121). | `e8e7453` |
| C.12 #7 | **Settings/Preferences "Color" section.** `Settings` grows `ocioConfigPath` + `defaultVideoInputCs` + `defaultPngInputCs` + `defaultDisplay` + `defaultView` (all defensive-loaded — older settings.json files load without complaint). New process-wide snapshot `activeSettings()` / `publishActiveSettings()` (mutex-guarded copy, called by Engine on load + Preferences-OK) lets worker-thread decoders read user defaults without plumbing Settings through every constructor. ProResDecoder consults `defaultVideoInputCs` on `AVCOL_SPC_UNSPECIFIED`; PNGSequenceDecoder always consults `defaultPngInputCs` (PNG has no per-file color-space metadata). Explicit codec metadata still wins (BT.709/BT.2020 ProRes, HAP variant tags). Preferences modal grows a Color collapsing header: OCIO config path text input + Browse… (HWND-aware native dialog routed through WindowManager), 4 OCIO-config-driven dropdowns (color space × 2, display, view) when OcioManager is bound, free-form text fields as fallback when it isn't. Tooltip flags "config switches require app restart in C.12." 5 new SettingsTests (defaults, roundtrip, missing-key fallback, wrong-type fallback, snapshot publish/read isolation). **128/128 green.** | `da429d8` |
| C.12 #8 | **Per-output OCIO display+view UI + project persistence (PROJECT_VERSION 5→6).** `OutputDisplay` grows `ocioDisplay` + `ocioView` strings (empty = use the active OCIO config's defaults at draw time — the renderer's existing `setOcioManager` + per-(display,view) PSO cache from C.12 #5 already handle empty-string lookup as "config default", so no renderer wiring change was needed for this subtask). MappingWindow's per-output Calibration block grows a Display dropdown (populated from `OcioManager::listDisplays()`) + a View dropdown (filtered by selected display via `listViews(displayForViews)`). Selecting a different display clears the view (views are display-scoped — stale pairs aren't safe). When OcioManager isn't bound (config failed to load), both fall back to free-form text inputs so values can still be set for next launch. Brightness slider stays as a pre-ODT scale; the gamma slider is relabelled `Gamma trim` to flag its post-ODT calibration role (kept for legacy projector tuning workflows; not a colorimetric knob). `ProjectSerializer::PROJECT_VERSION` bumped 5→6; outputs save block writes `ocioDisplay` + `ocioView`; load block defaults missing keys to empty strings, so v5 files load without complaint. New `tests/unit/ProjectSerializerTests.cpp` — round-trip preserves explicit `Rec.1886`/`Rec.709`, empty-empty round-trip stays empty, hand-rolled v5 JSON without OCIO keys loads with empty defaults. **131/131 green.** |
| C.12 #9 | **MediaBin per-clip OCIO input color-space override.** `ProjectManager::MediaLibraryEntry` grows an optional `inputColorSpaceOverride` string (empty = "Auto (decoder)" — let the codec/decoder tag drive the OCIO input transform; non-empty forces a specific OCIO color space for every clip backed by this media entry). MediaBin grows a 7th column "Color space" with a dropdown populated from `OcioManager::listColorSpaces()` plus an "Auto (decoder)" sentinel; when OcioManager isn't bound (config load failed) it falls back to a free-form text input so the value can still be captured for next launch. PlaybackController gained a `setProjectManager(...)` hook + a private `applyInputColorSpaceOverride(clip, videoTex)` that runs immediately after the decoder's OCIO tag is stamped onto `videoTex.ocioColorSpace`, replacing it with `entry.inputColorSpaceOverride` when set. Engine wires both objects together at construction. `ProjectSerializer` emits `inputColorSpaceOverride` only when non-empty (older v6 files without the key load cleanly with an empty default — same forward-compat shape as the OCIO display/view fields from #8, no version bump). 3 new ProjectSerializerTests gate the round-trip: explicit value preserved, empty stays empty, missing-key load defaults to empty. Latent: nearest-fallback upload path now also stamps `videoTex.ocioColorSpace = f.ocioColorSpace` (matching the cache-hit path) so the override applies consistently when playback falls back to the closest cached frame. **134/134 green.** |
| C.12 #11 | **OCIO ODT correctness unit tests + ACES smoke integration test.** Closes Phase C.12. New `tests/unit/OcioOdtTests.cpp` (7 tests, 1 disabled-by-default DumpDisplaysAndViews discovery harness) pins the math behind the rendering pipeline by running OCIO's *CPU* processor against known scene-linear inputs: ACEScg(0.18) through the default sRGB SDR view lands in the [0.20, 0.60] display-code midtones (catches ODT identity-pass-through regressions); ACEScg(1.0) rolls off to [0.50, 0.99] (catches a missing RRT shoulder); ACEScg(4.0) clamps to ≥0.85 ≤1.0 (mirrors the C.12 #10 HAP HDR smoke test on the GPU side); ACEScg(0.0) stays in |x|<0.05; pure red/green/blue ACEScg keep their channel dominance through the ODT (catches matrix-mismatch / channel-swap regressions); a PQ-flavored display+view (Studio Config v2.1.0 ships `Rec.2100-PQ - Display` with `ACES 1.1 - HDR Video (... nits & Rec.2020 lim)` views — display name carries PQ, view names just say HDR, so the heuristic looks for PQ in display + HDR in view) verifies the EOTF is actually applied: mid-gray and white produce in-range PQ codewords differing by ≥0.05. New integration test `integration_aces_smoke` (`scripts/integration/aces_smoke.json` + record companion + `tests/goldens/aces_smoke/frame_0.hash` baking to FNV-1a `884c5ee8a4a18c65` for the 64×64 capture) loads `test_media/aces_test_pattern/frame_000.png` (procedural fixture from new `test_media/scripts/generate_aces_test_pattern.py`: 64×64 with 16-row bands of black-to-white gradient / sRGB mid-gray 188 / pure primaries / pure secondaries) and hashes the compose target after the full PNG → sRGB tag → ACEScg working space → ACES sRGB ODT capture pipeline. The fixture lives in its own subdirectory so PNGSequenceDecoder's parent-dir scan only sees this single frame. Visual sanity: gradient compresses through the tone curve (no banding visible at 64-wide), primaries desaturate as expected (sRGB-narrow primaries through the wider ACEScg gamut land slightly inside their original hue), mid-gray stays achromatic. **143/143 green** (135 + 7 unit + 1 integration). |
| C.12 #10 | **HAP HDR (BC6H_UF16) FP16 survival smoke test.** Synthetic 64×64 HapH fixture under `test_media/hap_hdr/test.mov` — `tests/tools/generate_hap_fixture.cpp` grew a `haph` variant that hand-encodes a constant-color BC6H mode-11 block (10:10:10:10:10:10 endpoints all set to e=561, which after the BC6H_UF16 unquantize + finishUnquantize pipeline decodes to half-float 0x43FE ≈ 3.996, ≈ 4.0 within BC6H's 10-bit endpoint precision), tiles it across a 16-block image, wraps it in a SEC_HAPHU_RAW (0xA2) HAP section, and writes packets directly through libavformat (no encoder — FFmpeg's HAP encoder rejects HDR inputs). Container codec_tag is `'Hap1'`, not `'HapH'` — FFmpeg's `ff_codec_movvideo_tags` table only registers Hap1 / Hap5 / HapY / Hap7 / HapA / HapM, so the MOV muxer rejects 'HapH' with "Tag HapH incompatible with output codec id 187". The mismatch is harmless: HAPDecoder dispatches purely off codec_id (= AV_CODEC_ID_HAP), and `parseHapPacket` reads the per-frame texture format from the section type byte (0xA2 → BC6H_UF16) — codec_tag's only role is HAP-vs-other-codec routing. The misleading "Hap (BC1 RGB)" log line at decoder open is a metadata wart only. New `scripts/integration/hap_hdr_smoke.json` + record companion + `tests/goldens/hap_hdr_smoke/frame_0.hash`. Verified the captured PNG: every pixel is RGBA(244,244,244,255) — 0.957 normalized after the OCIO ACES sRGB ODT — clearly rolled-off (not 255 clipped, not linearly passed-through), gating the FP16 compose path. dxgi=95 (DXGI_FORMAT_BC6H_UF16) confirmed in upload log. **135/135 green.** |

---

## Verified Working (as of Phase C.12 #11 — CI green, Phase C.12 closed)

**Build**: `cmake --build build --config Release` is clean, no errors.
**Tests**: 143/143 pass via `ctest -C Release` — 124 unit (13 FrameCache, 16 HapFormat, 13 Settings, 4 TranscodeManager, 5 OcioManager, 6 RuntimeShaderCompiler, 8 OcioGpuProcessor, 2 DecoderColorSpace, 6 ProjectSerializer, 7 new OcioOdt + the keyframe-roundtrip / ripple-time / decoder-interface suites) + 19 integration (all 22 goldens rebaked end-to-end through the OCIO ACES SDR pipeline + `hap_hdr_smoke` + `aces_smoke`).
**Binary**: `build/bin/Release/EntityMediaEditor.exe` launches and runs in windowed or `--headless` mode. `dxcompiler.dll` + `dxil.dll` ship next to it for runtime shader compilation.

Integration tests wired to CTest, labelled `integration`:
- `integration_smoke` — cleared compose target hash
- `integration_multi_screen` — two user screens, hash both
- `integration_png_sequence_seek` — PNG seq decode + seek correctness
- `integration_seek_past_clip_end` — boundary behavior past clip duration
- `integration_screen_persistence_save` / `_load` — round-trip via CTest fixture; load asserts both default and custom screens survive serializer
- `integration_sections_persistence_save` / `_load` — named sections round-trip
- `integration_mixed_fps` — 16-frame seq forced to 24fps on a 30fps timeline; asserts tl-frame-10 maps to source frame 8 via `floor(localFrame * srcFps/tlFps)`
- `integration_ping_pong` — 16-frame seq stretched to 64 timeline frames in PingPong mode; asserts tl 8 is forward-phase source 8 and tl 24 is reverse-phase source 7 (mirror-index 15 - (24 % 16))
- `integration_blend` — two solid mid-tone clips stacked on track 0 + track 1; sets Normal/Add/Multiply/Screen on the top clip, asserts four distinct compose-target hashes. Mid-tone channels required: pure 0/1 are fixed points for these modes.
- `integration_blend_difference` — same fixture stack, sets Difference on the top clip, asserts the hash. Different code path from `integration_blend` (shader-based blend pipeline w/ snapshot SRV, not fixed-function blend state) — separate gate so a shader-blend regression is unambiguous.
- `integration_hap_roundtrip` (Hap5/Alpha), `integration_hap_basic_roundtrip` (Hap1), `integration_hap_q_roundtrip` (HapY YCoCg) — three HAP variants gated end-to-end.
- `integration_hap_hdr_smoke` — synthetic HapH (BC6H_UF16) constant-color block (≈ half-float 4.0 across the frame) gates the FP16 compose path. Captured pixels are RGBA(244,244,244) post-ACES — clearly rolled off (not clipped at 255, not linearly passed-through). Phase C.12 gate that BC6H_UF16 uploads + the OCIO display transform survive HDR-amplitude inputs.
- `integration_aces_smoke` — fixed sRGB test pattern (gradient + mid-gray + primaries + secondaries) round-trips PNG → sRGB tag → ACEScg working space → ACES sRGB ODT and hashes the capture. Phase C.12 #11 regression sentinel for OCIO config / decoder tagging / OcioGpuProcessor emission / capture-pass shader drift.
- `integration_cache_hit_after_seek` — warms cache by seeking 8 → 0, re-seeks to 8, asserts frame still cached and pixel hash matches `png_sequence_seek/frame_8.hash`. Phase C.10 gate (cache hits land instead of re-decodes).
- `integration_cache_budget_stress` — shrinks cache to 80 KB against 256 KB working set, runs 24 random seeks across 16 frames, asserts `bytesUsed <= maxBytes && entryCount > 0` after each wave. Cache pins at exactly 81920/81920 with 5 entries — eviction is provably doing real work.

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
- ~~**Frame cache**~~ ✅ shipped in Phase C #10.1 (`19da3e0`). Sparse LRU `FrameCache` keyed by `(clipEntity, FrameNumber)`, 512 MB default budget, `FrameLease` RAII for safe eviction-while-leased. Replaces per-clip ring buffer end-to-end.
- ~~**Settings/Preferences window**~~ ✅ shipped in Phase C #10.0 (`2d35dd0`); cache-budget setting wires through live to `FrameCache::setMaxBytes` since #10.1.
- ~~**Color pipeline**~~ — Phase C.12 #1-#6 shipped 2026-04-27. FP16 ACEScg working space, OCIO input transforms tagged per decoder (HAP RGB / HapY / ProRes / PNG), OCIO display+view ODT in mapping_surface + a pinned sRGB-display ODT in the capture pass. Bundled ACES Studio Config 1.3 via `Config::CreateFromBuiltinConfig` — no on-disk resource. Per-clip PSO selection by OCIO color-space name; legacy gamma-only path stays as fallback when OcioManager isn't bound. Remaining C.12 work (#7-#11) is configurability + extra tests on top of the green pipeline.

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
- `include/entity/core/Settings.hpp` + `src/core/Settings.cpp` — machine-global JSON config (Phase C #10.0)
- `include/entity/media/DecodedFrame.hpp` — extracted from FrameRingBuffer.hpp (Phase C #10.1)
- `include/entity/media/FrameCache.hpp` + `src/media/FrameCache.cpp` — engine-global LRU frame cache + `FrameLease` RAII handle (Phase C #10.1)
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

### ~~Phase C.10 — FrameCache replaces the per-clip ring buffer~~ ✅ done (`19da3e0`)

### ~~Phase C.11 — Async copy queue~~ ✅ done (`ea62168`)

### Phase C.12 — OCIO-native color pipeline (in progress)

Active driver: `~/.claude/plans/quick-thought-before-we-pure-thompson.md`. OpenColorIO 2.5.1 (vcpkg), bundled ACES Studio Config 1.3 via `Config::CreateFromBuiltinConfig` (no on-disk resource needed). Per-codec input transforms, ACEScg linear working space, per-output OCIO display+view transforms (sRGB / Rec.709 / DCI-P3 / Rec.2020 / PQ HDR). FP16 compose targets unblock HAP HDR. User .ocio configs + camera log encodings + 3D LUTs + look files all fall out for free. ~4-5 weeks.

**Status:**
- ✅ #1 — OCIO dep + `OcioManager` skeleton (`f2e5f09`)
- ✅ #2 — Runtime DXC compile path (`08b87f4`)
- ✅ #3 — FP16 compose targets + capture-buffer pass (`9b8c361`) *(opened the CI-red window)*
- ✅ #4 — `OcioManager` GPU processor cache + `GpuShaderDesc` HLSL emission (`0f4ba2a`)
- ✅ #5 — OCIO into composite_ps + mapping_surface_ps + capture (`6161557`)
- ✅ #6 — Per-decoder OCIO tags + 22-golden rebake (`e8e7453`) *(closed CI red — 123/123)*
- ✅ #7 — Settings + Preferences "Color" section + process-wide `activeSettings()` snapshot for worker-thread decoder defaults (128/128) (`da429d8`)
- ✅ #8 — Per-output OCIO display+view UI + project persistence (PROJECT_VERSION 5→6); 3 new ProjectSerializerTests (131/131)
- ✅ #9 — MediaBin per-clip OCIO input color-space override; 3 new ProjectSerializerTests (134/134)
- ✅ #10 — HAP HDR (BC6H_UF16) FP16 survival smoke test (`integration_hap_hdr_smoke`); synthetic HapH fixture via extended `generate_hap_fixture haph` (135/135)
- ✅ #11 — `integration_aces_smoke` + 7 OCIO ODT correctness unit tests (143/143). **Phase C.12 closed.**

The previous hand-rolled-ACES plan (`~/.claude/plans/jaunty-launching-tulip.md`) is **superseded** — left in place for reference.

### Phase D entry — Director/Renderer split

Decompose `Engine` into `DirectorService` + `RendererService` with a serializable message bus. **Network-serializable from day one** (per Decision 5) — transport starts in-memory, becomes UDP in Phase E with no message-format changes. Asset references move to content-hash IDs. ~1.5 weeks; lands *before* Phase D feature work (timecode/OSC/audio/NDI) so each one attaches to the right layer.

### Out-of-scope-here but in the master plan

- Audio pipeline (Phase D)
- Undo/redo for non-property commands (Phase C, CommandDispatcher already exists)
- Soft-edge feather visual check (Phase C continuation)
- Mesh warp / cylindrical mapping (Phase C continuation)

---

## Pointers for the next session

- **Active C.12 plan (OCIO-native color pipeline)**: `~/.claude/plans/quick-thought-before-we-pure-thompson.md` — **this is the current driver**
- Superseded C.12 plan (hand-rolled ACES, kept for reference): `~/.claude/plans/jaunty-launching-tulip.md`
- Playback/render-engine deep-dive plan: `~/.claude/plans/so-even-with-hap-cosmic-glacier.md`
- Master roadmap: `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md`
- Issue list: `docs/reference/CODE_ISSUES.md` (last verified 2026-04-19; needs another pass)
- Development history: `docs/status/HISTORY.md`
- Component rules + exceptions: `include/entity/components/CLAUDE.md`
- Integration test pattern: `scripts/integration/README.md`
- ADR for D3D12-vs-abstraction choice: in the master plan under "Architecture Decisions"
