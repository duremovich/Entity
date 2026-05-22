# Architecture Decision Records

This directory holds the stable architecture decisions that shape Entity. Each
ADR captures the **why** behind a load-bearing choice — context, decision,
consequences, alternatives considered — so future contributors (and future
Claude sessions) don't have to re-derive the reasoning from working plans
that have moved on.

## Index

| # | Title | Status | Date |
|---|---|---|---|
| 0001 | [D3D12 first, Metal as Phase E+ second backend](0001-d3d12-first-metal-later.md) | Accepted | 2026-04-19 |
| 0002 | [HAP-first codec strategy; ProRes is an import format](0002-hap-first-codec-strategy.md) | Accepted | 2026-04-25 |
| 0003 | [Director/Renderer service split with serializable bus](0003-director-renderer-split.md) | Accepted | 2026-04-25 |
| 0004 | [OCIO over hand-rolled ACES for the color pipeline](0004-ocio-over-handrolled-aces.md) | Accepted | 2026-04-26 |
| 0005 | [Open-core dual-license + plugin scaffold](0005-open-core-dual-license.md) | Accepted | 2026-04-29 |
| 0006 | [Cluster-ready plumbing from day one](0006-cluster-ready-plumbing-day-one.md) | Accepted | 2026-04-25 |
| 0007 | [Open-core feature tiering & monetization model](0007-open-core-feature-tiering.md) | Accepted | 2026-04-29 |
| 0008 | [Cluster topology nomenclature — Conductor / Performer / Standby](0008-cluster-topology-nomenclature.md) | Accepted | 2026-04-29 |
| 0009 | [Structured projects (folder-as-project) with reference-mode escape hatch](0009-structured-projects-with-reference-mode.md) | Accepted | 2026-04-29 |
| 0010 | [`content/` is the source of truth; filename-based versioning](0010-content-as-source-of-truth.md) | Accepted | 2026-04-29 |
| 0011 | [Projector calibration — manual correspondence + LM solve + post-fit IDW residual warp](0011-projector-calibration-architecture.md) | Accepted | 2026-05-03 |
| 0012 | [Timeline structural playback — break-point sections, cue tags, and continuation-phase decoupling](0012-timeline-sections-and-cues.md) | Accepted | 2026-05-06 |
| 0013 | [Control-plane plugins route via CommandDispatcher (defer bus-routing to Phase E)](0013-control-plane-plugins-route-via-command-dispatcher.md) | Accepted | 2026-05-08 |
| 0014 | [Editor/Show thread split — snapshot-driven show thread with zero registry writes](0014-editor-show-thread-split.md) | Accepted | 2026-05-08 |
| 0015 | [Profiling with Tracy — live CPU/GPU instrumentation](0015-profiling-with-tracy.md) | Accepted | 2026-05-10 |
| 0016 | [Timeline Layer abstraction](0016-timeline-layer-abstraction.md) | Accepted | 2026-05-11 |
| 0017 | [Generative layers — procedural-content timeline layers](0017-generative-layers.md) | Accepted (amended by 0018) | 2026-05-11 |
| 0018 | [Content-layer unification — one compositor path for clip + generative + future kinds](0018-content-layer-unification.md) | Accepted | 2026-05-12 |
| 0019 | [Per-layer effects — ordered shader chain with stack + graph editors over one data model](0019-per-layer-effects.md) | Accepted | 2026-05-12 |
| 0020 | [Object Animation layers — absolute values + Hold-by-default end behavior](0020-object-animation-end-behavior.md) | Accepted | 2026-05-15 |
| 0021 | [Two-tier mapping — Content Routing vs Feed Output](0021-two-tier-mapping.md) | Accepted | 2026-05-16 |
| 0022 | [Content Routing library + Feed Maps](0022-content-routing-library-and-feedmaps.md) | Accepted | 2026-05-17 |
| 0023 | [Control-plane plugin outbound via process-shared bridge](0023-control-plane-plugin-outbound-bridge.md) | Accepted | 2026-05-19 |
| 0024 | [getStringSetting bridge for project-scoped plugin state](0024-string-settings-bridge-for-project-scoped-plugin-state.md) | Accepted | 2026-05-19 |
| 0025 | [Timeline clock — separate rate authority from position authority](0025-clock-rate-vs-position-authority.md) | Accepted | 2026-05-21 |
| 0026 | [Seek-sync preroll gate — hold playhead until all decoders reach the target frame](0026-seek-sync-preroll-gate.md) | Accepted | 2026-05-22 |

## Format

ADRs use the short [MADR](https://adr.github.io/madr/) form. Every file has:

- **Status** — Accepted, Superseded by ###, Deprecated, Proposed.
- **Context** — what forced the decision.
- **Decision** — what we chose.
- **Consequences** — what this enables / forbids / forces. Honest about
  costs, not just benefits.
- **Alternatives considered** — the ones that almost won, and why they didn't.
- **References** — links to the original working plan(s) and the commits that
  implemented the decision.

## When to add an ADR

Add one when the decision is:
- **Load-bearing.** Reversing it would touch many files or take weeks.
- **Non-obvious.** A future reader (or future you) would otherwise wonder
  "why did we do it this way?"
- **Stable enough to write down.** Working plans in `~/.claude/plans/` are
  the right place for in-flight thinking. ADRs are for decisions that have
  settled.

If a working plan distills into something that should outlive it, write an
ADR and update the plan to point at the ADR instead of carrying the rationale
inline.

## When to update vs supersede

- **Update in place** for clarifications, additional consequences discovered
  later, references to follow-up work.
- **Supersede** when the decision itself reverses or fundamentally changes.
  Add a new ADR with the new decision; mark the old one
  `Status: Superseded by NNNN-...md`. Don't delete or rewrite the original —
  the historical reasoning matters for anyone walking back through git
  history.

## Authoring

Copy the most recent ADR as a template. Number is monotonic, four digits,
zero-padded. Keep ADRs focused — one decision per file. If you find yourself
writing two decisions, that's two ADRs.
