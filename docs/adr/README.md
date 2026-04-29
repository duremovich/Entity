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
