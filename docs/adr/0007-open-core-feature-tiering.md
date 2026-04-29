# ADR-0007: Open-core feature tiering & monetization model

- **Status:** Accepted
- **Date:** 2026-04-29
- **Context source:** post-roadmap-externalization strategic review;
  competitor feature/pricing research (Disguise, Watchout 7, Pixera,
  Millumin, Resolume) confirmed the tiering pattern.
- **Implements:** ADR-0005's plugin transport classification *plus* a
  commercial dimension that ADR-0005 didn't address.

## Context

ADR-0005 split plugins by *technical transport* (control-plane bus vs
hot-path C++ ABI). It did not address *commercial tier* — which features
are free and which are paid. Without a written principle, every Phase D/E
feature reopens the "is this free or paid?" debate. We need rules.

The competitor research (recorded under the previous roadmap-migration
plan) shows two distinct patterns:

- **Closed-source competitors** (Disguise, Watchout, Pixera, Millumin,
  Resolume) gate features by tier and sometimes by output resolution +
  watermark. Disguise Designer Starter is *free* but capped at 2K with
  blue-flash watermarks; Designer Pro is $159/mo unwatermarked at 4K.
  Millumin watermarks after 30 days. Resolume splits Avenue (€299)
  vs Arena (€799) with projection mapping + edge blending + DMX
  fixture output + timecode all in the paid tier.
- **Open-core competitors in adjacent markets** (GitLab, Sentry,
  Mattermost) gate by *feature*, not by capacity or watermark. The
  open-source core is fully usable; commercial value lives in
  paid-tier-only features and the support contract.

The closed-source pattern doesn't transfer to GPLv3 software. Anyone with
the source can patch out a resolution check or watermark in five minutes.
Putting such gates in the public source is theater that mostly annoys
legitimate free users while doing nothing to protect revenue.

## Decision

Three tiers, classified by repo + license, not by technical transport:

### 1. Core (`C:/Entity/Entity` — GPLv3 with linking exception)

Always free. No resolution caps, no watermarks, no time limits, no
runtime license checks. Any user can download, build from source, and run
unlimited.

What lives here:
- All currently-shipped functionality (Phases A, B, C, C.12, D-entry).
- Single-machine multi-output playback.
- Multi-track timeline + cues + undo/redo.
- Projection mapping (corner-pin, mesh, cylindrical) — all of it. We are
  defined as a projection-mapping server (per `CLAUDE.md`); gating this
  would defeat the product identity.
- Edge blending (soft-edge, gamma-correct).
- OCIO/ACES color pipeline (per ADR-0004).
- HAP codec family + transcode-on-import (per ADR-0002).
- Audio playback w/ frame-accurate sync.
- Stage 3D preview (glTF/OBJ as scene + content sources).
- Preview / Program output.
- NDI input (the receiver SDK has a generous-enough license).
- Project save/load + auto-save.
- FFGL/ISF shader-effect plugin slot (when it lands).

### 2. Public Plugin (`plugins/` — Apache 2.0)

Free, open-source, optional. Bus-based control-plane plugins per
ADR-0005. Communities can fork, contribute, or replace. What lives
here:

- OSC I/O plugin
- MIDI Show Control plugin
- DMX / Art-Net plugin
- LTC / MTC timecode plugin
- Lighting console bridges (per-console adapters)
- Telemetry / health monitoring
- Bus logger (already shipped as the canonical reference plugin)
- Specific protocol adapters (e.g., a Brompton Tessera TCP control
  plugin — see borderline rule below)

### 3. Pro Plugin (`Entity-Pro/plugins/` — proprietary, paid)

Commercial value. Closed-source, sold as part of the Entity Pro bundle.
Static-link only, toolchain-locked (per ADR-0005). Requires SDK
integrations the application actually calls. What lives here:

- Cluster — Conductor + Performers (multi-machine playback) — see ADR-0008
- EntityCal — camera-based projector calibration (Disguise OmniCal
  equivalent)
- NDI output (Advanced SDK)
- RenderStream-equivalent pull API (Unreal / Unity / TouchDesigner
  in-the-loop content)
- Notch Block runtime
- Genlock card driver (Quadro Sync II / AJA / Decklink)
- Internal Decklink/AJA SDI card SDK integration *(Phase F+; only when
  a customer asks; external converters cover the common case)*
- HDR display calibration toolchain
- Show-file redundancy + hot-failover
- Premium codec licenses (NotchLC, ProRes RAW, JPEG-XS) — when
  separately licensable
- Multi-user collaboration / show locking / role-based editing

## Inclusion principles

For each new feature, classify by these tests in order:

1. **Is the feature already in core today?** → Stays core. Don't move
   shipped functionality behind a paywall; that breaks faith with
   existing users and the open-source compact.
2. **Is it a baseline industry expectation across competitors?** → Core or
   Public Plugin. DMX, Art-Net, OSC, MIDI, LTC are universally baseline
   per the research; gating them would make Entity look strictly weaker
   than every alternative including the free-tier ones.
3. **Is the feature a thin protocol or format adapter?** → Public Plugin.
   Anything that's mostly translating a TCP/UDP/MIDI/OSC message
   shape, with no proprietary SDK or hardware fee, lands here.
4. **Does the feature require an in-app SDK call against proprietary
   hardware or a closed SDK?** → Pro Plugin. (See borderline rule below.)
5. **Does the feature require ongoing commercial relationships
   (codec licenses, hardware partnerships, vendor support contracts)
   to maintain?** → Pro Plugin. Community contributors can't shoulder
   commercial maintenance; that's what the paid tier funds.
6. **Otherwise** → Core, by default. The bias is open.

## Borderline rules

**Mesh / cylindrical projection mapping:** core. We are a
projection-mapping server. Resolume gates these (Arena-only) but
Disguise/Watchout/Pixera don't. Going core matches our identity and
the high-end pattern.

**NDI input vs output:** input = core (free SDK). Output = Pro Plugin
(NDI Advanced SDK is commercially licensed for sender-side features).

**3D scene rendering:** glTF/OBJ as a scene element or content source =
core. RenderStream-style live engine (Unreal/Touch/Unity in-the-loop) =
Pro Plugin. Pulled apart by where the rendering work happens.

**External hardware that consumes a standard signal ≠ Pro feature.** If
the user's hardware just takes our HDMI or DisplayPort output and does
its own thing externally, Entity has nothing to build:

- SDI converters (Datapath FX4, AJA HD5DA, BMD HDMI-to-SDI Mini)
- LED processors (Brompton Tessera, NovaStar MCTRL, Megapixel Helios)
- Most standalone projectors (incl. those with their own warp/blend)

The user buys the box, plugs HDMI/DP into it, done. Entity already
drives standard outputs via `OutputManager`. This is **not a Pro
feature, and not even an issue** — at most a one-line mention in user
docs that Entity works with standard signal-consuming hardware.

**SDK-required in-app integrations:** these are the real Pro features.
Internal Decklink Quad 2 / AJA Kona (frame send via SDK), Quadro Sync
II (NVIDIA SDK), Notch Block (closed SDK), NDI Advanced SDK output —
all involve actual application code calling vendor APIs.

**Adjacent control APIs (e.g., Brompton TCP for color-cal sync):** these
are thin protocol adapters even though the hardware is high-end. They
land as Public Plugins, not Pro, unless the protocol itself is
commercially-licensed (rare).

## Consequences

**Enables:**
- Clear binary classification on every new issue: pick a tier label,
  done.
- Protects open-source identity: anyone can run Entity at full
  resolution, on any hardware, indefinitely, and self-build from source
  if they want.
- Pro tier value scales with commercial integrations the open-source
  community can't realistically deliver (vendor SDKs, hardware
  partnerships, premium codecs, support contracts).
- The free Entity gets a competitive differentiator vs. closed-source
  free tiers (Disguise Starter is 2K-capped + watermarked; Entity
  isn't).

**Forbids:**
- Resolution gates, watermarks, time limits, and feature-flag
  paywalls in the GPLv3 core. Even if technically possible to insert,
  they conflict with the open-source compact.
- Re-classifying shipped core functionality as Pro. Once it ships
  free, it stays free.
- Pro plugins reaching into core internals beyond the public
  `plugin-api/` surface (per ADR-0005's boundary rules).

**Forces:**
- Discipline at issue-filing time: every roadmap epic gets a tier
  label (`tier:core`, `tier:public-plugin`, `tier:pro-plugin`).
  Project board filters depend on this.
- Some features that look "premium" by competitor convention are
  actually core for us — projection mapping is the obvious example.
  Don't paywall them just because Resolume does.
- The commercial model has to work via Pro plugins + support, since
  there are no other levers. This is the GitLab/Sentry/Mattermost
  pattern — proven to work, but requires the Pro tier to genuinely
  deliver value for customers who could otherwise self-build.

## Soft commercial levers (not gates, but real)

- **Trademark + branding** are commercial-protected. "Entity" as a
  product name and logo cannot be used by forks under GPLv3; legitimate
  forks must rebrand. This prevents drop-in commercial replacements
  while preserving the open-source freedoms.
- **Pre-built Entity Pro binary** is sold as a bundle with Pro plugins
  + support contract + commercial codec licenses. The same core source
  is public; the bundle is what's sold.
- **Support, training, deployment consulting** for production
  customers. Solo operators / hobbyists / academics use the
  open-source build for free; production houses pay for the integrated
  experience and SLA.

## Alternatives considered

- **Resolution-gated free tier** (à la Disguise Starter). Rejected:
  unenforceable on open-source. Five-minute patch removes the gate.
- **Watermark on free output**. Same problem. Plus signals
  amateurishness to professional users we want as long-term
  contributors.
- **Time-limited trial** (à la Watchout's 30-day-then-codecs-stripped).
  Same problem. Plus user-hostile to academics and hobbyists.
- **Proprietary core with Apache plugin layer (not GPLv3 + linking
  exception).** Cleaner commercial story but loses the
  community/contribution upside that motivated open-core in the first
  place (ADR-0005's premise).
- **No tiering at all — fully open-source, monetize only via support
  contracts**. Viable for some projects (e.g., Linux distros), but
  Entity's hardware-integration features (Notch, NDI Advanced, SDK
  cards) would be unbuildable — those vendors don't license to
  open-source projects. The Pro plugin tier exists because some code
  *cannot* be open-source.

## References

- ADR-0005 (open-core dual-license + plugin scaffold) — technical
  mechanism this ADR builds on.
- Competitor research recorded in the previous roadmap-migration plan;
  feature/pricing snapshots for Disguise, Watchout 7, Pixera, Millumin,
  Resolume captured 2026-04-29.
- Memory entry `feedback_external_hardware_vs_sdk.md` — origin of the
  external-hardware-vs-SDK borderline rule.
- Project board labels: `tier:core`, `tier:public-plugin`,
  `tier:pro-plugin` — apply one to every roadmap issue.
