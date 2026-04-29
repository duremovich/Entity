# ADR-0002: HAP-first codec strategy; ProRes is an import format

- **Status:** Accepted
- **Date:** 2026-04-25
- **Context source:** `~/.claude/plans/so-even-with-hap-cosmic-glacier.md` § Decision 1, Decision 5
- **Implemented by:** Phase C #6, #7, #9.1-9.4 (commits `f5cabf8`, `93c4772`,
  `525b6ee`, `973f91d`, `4998e65`, `30e3df9`); Phase C.1 #6.1-6.10
  (transcode-on-import, commits `511b367` through `90b46f9`)

## Context

Every serious media server in this category — Disguise, Pixera, Watchout 7,
Resolume Arena — has converged on GPU-decodable, i-frame-only codec families
(HAP and NotchLC). The convergence is not coincidence; it's what survived
contact with live shows. HAP frames are 4-8× smaller than RGBA, decode is
"Snappy decompress + DMA," and the GPU does the texture decompression at
sample time. Per-frame i-frame encoding makes seek free — no GOP walk-back.

Pixera explicitly *removed* GPU H.264/H.265 decode in v25.1 because of NVDEC
driver inconsistency. The lesson: don't fight NVDEC; use codecs designed for
this category.

Entity already had a working ProRes 4444 path (FFmpeg). At ~33 MB per 4K
frame, it pressured the per-clip ring buffer and the upload pipeline; HAP at
~4 MB collapses both. The transcode-on-import workflow is the standard
industry pattern (Disguise/Pixera/Watchout users drop ProRes/H.264 in,
background-transcode to HAP, playback is HAP).

## Decision

**Optimize the playback engine for HAP exclusively.** ProRes playback exists
for "while the transcode is running"; do not tune the engine past that
contract for ProRes performance.

**Ship the entire HAP family** in Phase C.9: HAP (Hap1/BC1), HAP Alpha
(Hap5/BC3), HAP Q (HapY/BC3 + YCoCg), HAP Q Alpha (HapM/BC3 + BC4), HAP
Alpha-Only (HapA/BC4), HAP R (Hap7/BC7), HAP HDR (HapH/BC6H_UF16). HAP HDR
gates on FP16 compose targets landing in the color pipeline (see ADR-0004).

**Do not invent a codec.** Pixera, Watchout, Disguise, and Resolume all run on
HAP/HAP-derivatives + NotchLC. Resolume's DXV is HAP with a different header.
The codec slot is full.

**Implementation discipline:** the HAP decoder demuxes via libavformat but
**bypasses libavcodec for the BC payload** — FFmpeg's `libavcodec/hapdec.c`
expands BCn back to RGBA on CPU before returning, defeating the entire point
of HAP. The decoder reads the chunk table, Snappy-decompresses, and hands the
post-Snappy BCn bytes directly to `TextureUploader` with the matching
`DXGI_FORMAT_BC{1,3,7}_UNORM` (or `BC6H_UF16` for HDR).

## Consequences

**Enables:**
- 4-8× memory reduction per cached frame; 8× upload bandwidth reduction.
- Free seek (i-frame-only).
- A frame cache budget of 512 MB holds ~128 4K HAP frames vs ~16 4K ProRes
  frames at the same byte budget.
- Click-to-recently-viewed-frame is zero decode work (cache hit + DMA).

**Forbids:**
- Long-GOP codec optimization. H.264 long-GOP scrubbing is poor and won't
  improve.
- Inventing an in-house codec. Engineering effort goes elsewhere.

**Forces:**
- A transcode-on-import UX so users dropping ProRes/H.264/etc. get an
  HAP-backed playback experience without thinking about codecs (shipped in
  Phase C.1 #6.x).
- Per-codec input color-space tagging for the color pipeline (see ADR-0004),
  because the working space differs from the source space.
- Hand-crafted byte fixtures for HAP variants FFmpeg's encoder can't produce
  (HapA, Hap7, HAP HDR). FFmpeg's HAP encoder rejects HDR inputs and several
  variants; gating those in CI requires writing test fixtures by hand.

## Alternatives considered

- **Stay ProRes-first.** Maintains the existing simpler pipeline but ships
  a pessimal experience: 4K alpha clips fail at scale, the cache budget gets
  consumed by a handful of frames, and nothing about the engine is optimized
  for the actual market.
- **NotchLC as primary.** Nice quality/size sweet spot but: (a) closed-source
  spec, (b) requires SDK from Notch, (c) integration story is unclear for an
  open-core project. Acceptable as a future Phase E+ addition once HAP is
  done.
- **Invent a codec.** Months of work, no customer win, every competitor has
  already settled on the same handful. Pixera's "PixCodec" is marketing
  slop — they ship HAP and NotchLC.

## References

- HAP open spec: <https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md>
- Disguise HAP/HAP-Q docs: <https://help.disguise.one/designer/content-management/video-codecs/hap-hapq-codec>
- Vidvox HAP R benchmarks: <https://vdmx.vidvox.net/blog/hap-r-benchmarks>
- Playback engine deep-dive: `~/.claude/plans/so-even-with-hap-cosmic-glacier.md`
- HAP variant status table: `docs/status/HISTORY.md` (Phase C.9 entry)
