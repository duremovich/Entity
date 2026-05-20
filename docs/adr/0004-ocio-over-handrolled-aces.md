# ADR-0004: OCIO over hand-rolled ACES for the color pipeline

- **Status:** Accepted (supersedes the hand-rolled ACES plan in
  `~/.claude/plans/jaunty-launching-tulip.md`)
- **Date:** 2026-04-26
- **Context source:** `~/.claude/plans/quick-thought-before-we-pure-thompson.md`
- **Implemented by:** Phase C.12 #1-#11 (commits `f2e5f09`, `08b87f4`,
  `9b8c361`, `0f4ba2a`, `6161557`, `e8e7453`, `da429d8`, `a579bb0`,
  `456b2ad`, plus #10 + #11 ending at `29bc546`)

## Context

Going into Phase C.12 we needed a real color pipeline: linear working space,
per-codec input transforms, per-output display transforms, and FP16 compose
targets so HAP HDR (BC6H_UF16) survives composite without clipping.

The previous plan (`jaunty-launching-tulip.md`) committed to a hand-rolled
ACES 1.3 pipeline — ~500 LOC of HLSL matrix math, a simplified Narkowicz RRT,
a fixed `OutputTransform` enum on each output. That plan would have shipped
faster (~2-3 weeks vs ~4-5 for OCIO) but locked the engine into one math path,
couldn't ingest user `.ocio` configs, had no slot for camera log encodings
(LogC, Log3G10, S-Log), and shipped zero infrastructure for 3D LUTs / look
files.

An established media server made the same call in April 2024 — replaced its
ACES-only pipeline with OCIO, ACES Studio Config 1.3 as the default. Resolve,
Nuke, Houdini, and the entire Foundry ecosystem run on OCIO. The vocabulary is
what professional colorists already speak.

## Decision

Use **OpenColorIO 2.x** (vcpkg `opencolorio` 2.5.1) as the color pipeline.
Bundled ACES Studio Config 1.3 via `Config::CreateFromBuiltinConfig(
"studio-config-v2.1.0_aces-v1.3_ocio-v2.3")` — OCIO 2.4+ ships builtins
*inside the library*, so no on-disk resource ships with Entity.

Users override by setting Settings → OCIO config path → custom `.ocio`. Any
Resolve/Nuke-exported config drops in.

Compose targets become `R16G16B16A16_FLOAT`. Swap chains stay UNORM8 — sRGB
SDR is the common output case; HDR display routing is a Phase D concern.
Capture-buffer pass tone-maps FP16 → UNORM8 via OCIO's display transform so
integration-test goldens stay portable across machines.

Per-codec input color-space tags (string-named OCIO color spaces, not enum):
- HAP RGB / HapY post-YCoCg → `"Linear Rec.709 (sRGB)"`
- HapA → `"Raw"` (identity)
- ProRes via `srcFrame->colorspace` → `"Linear Rec.709 (sRGB)"` /
  `"Linear Rec.2020"` / settings default
- PNG → `"sRGB - Display"`

Per-output `OutputDisplay.ocioDisplay` + `ocioView` are project-persistent.
Per-clip `MediaLibraryEntry.inputColorSpaceOverride` overrides the decoder
tag when set. PROJECT_VERSION bumped 5→6.

Runtime DXC compile path (`RuntimeShaderCompiler`) splices OCIO-emitted HLSL
fragments onto `composite_ps`, `mapping_surface_ps`, and `aces_capture_ps` at
startup. The bundled ACES Studio config emits self-contained HLSL with zero
textures and zero uniforms — just static const arrays + helper fns.

## Consequences

**Enables:**
- ACES end-to-end (per-codec input transform, ACEScg working space,
  per-output ODT — sRGB, Rec.709, DCI-P3, Rec.2020, PQ HDR).
- User-supplied `.ocio` configs from Resolve/Nuke without a translation
  layer.
- Camera log encodings (LogC, Log3G10, S-Log, Cineon) drop in for free —
  they're already in the bundled config.
- 3D LUTs and `.cube` look files as a free side-effect (LUTs on OCIO's data
  path).
- HAP HDR (BC6H_UF16) survives composite at FP16 amplitude. Verified by
  `integration_hap_hdr_smoke` (HapH constant ≈ 4.0 → ACES-rolled-off
  RGBA(244,244,244)).

**Forbids:**
- Runtime OCIO config hot-swap in C.12 (requires app restart). Future
  cleanup; not enough payoff to gate on now.
- Per-clip look-file dropdown (Phase D).

**Forces:**
- vcpkg pulls in opencolorio + expat + yaml-cpp + openexr + imath. First CI
  build slow; cached after.
- Compose targets at 4K × 8 × FRAME_COUNT × FP16 ≈ 2 GB VRAM (vs ~1 GB at
  UNORM8). Mitigated by lazy-allocating snapshot resources (only used by
  shader-blend modes; saves ~265 MB on workloads without).
- Shader-blend modes blend in linear ACEScg, not display-referred sRGB.
  After Effects works in display space; results may differ slightly. UI
  tooltip flags this; no per-clip toggle in C.12.
- Capture-pass shader is config-dependent — can't be pre-baked offline. The
  runtime DXC compile path is the dependency that makes everything else
  possible.

## Alternatives considered

- **Hand-rolled ACES 1.3** (the superseded plan). Ships in 2-3 weeks. Locks
  out user configs, camera logs, 3D LUTs, look files. Forces a from-scratch
  rewrite the moment a user shows up with LogC footage — which, for the
  stated audience, is when, not if.
- **No color pipeline (status-quo gamma).** Cheap. Fails any pro-grade
  user expectation. The established media servers are all ACES-internal;
  shipping without a real color pipeline is shipping a tech demo.
- **Pure OCIO with no bundled config.** Forces every user to set up their
  own config. Drops the zero-config out-of-box experience.

## References

- OCIO project: <https://opencolorio.org>
- ACES Studio Config 1.3 (bundled): the OCIO library 2.4+ ships it via
  `Config::CreateFromBuiltinConfig`.
- Working plan: `~/.claude/plans/quick-thought-before-we-pure-thompson.md`
- Superseded plan: `~/.claude/plans/jaunty-launching-tulip.md`
- ODT correctness gate: `tests/unit/OcioOdtTests.cpp` (7 enabled tests +
  PQ HDR codeword check).
- End-to-end gate: `integration_aces_smoke`, `integration_hap_hdr_smoke`,
  + 22 rebaked compose-target goldens.
