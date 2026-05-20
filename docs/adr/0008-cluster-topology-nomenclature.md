# ADR-0008: Cluster topology nomenclature — Conductor / Performer / Standby

- **Status:** Accepted
- **Date:** 2026-04-29
- **Context source:** strategic discussion following ADR-0007's tier
  classification; the Cluster Pro plugin needs a vocabulary distinct from
  ADR-0003's in-process Director/Renderer.
- **Implemented by:** *forward-looking* — names introduced before the
  cluster code lands. Cluster is a Phase E Pro epic per ADR-0007.

## Context

ADR-0003 settled the **in-process** service split: a single Entity
process owns one `Director` (timeline + project + time authority +
command dispatch) and one `Renderer` (compositor + outputs + decode +
cache). These names are stable and shipped.

The cluster Pro plugin (per ADR-0007's Pro tier; also see ADR-0006 for
the cluster-ready plumbing already in place) introduces a *deployment
topology* layer where one machine coordinates many. We need names for
this layer that don't collide with the in-process roles, because every
doc and conversation would otherwise need to disambiguate "in-process
Director" vs "cluster Director" forever.

An established media server uses Director / Actor / Understudy.
Borrowing those exact words would make us look derivative, and they
collide with the same naming problem we just diagnosed (their
"Director" is also two-tiered, unpleasantly).

## Decision

Cluster topology vocabulary:

- **Conductor** — the cluster lead. Owns the project file, the show
  state, and the cluster-wide time authority. Every cluster has exactly
  one Conductor at a time. Internally it runs a normal in-process
  Director + Renderer; the *Conductor* role is the cluster-facing
  responsibility layered on top.
- **Performer** — a remote machine subscribed to one Conductor.
  Internally it runs a normal in-process Director + Renderer; the
  *Performer* role is the cluster-facing responsibility (receive
  bus messages from the Conductor, render its assigned outputs, send
  health back).
- **Standby** — a Performer in hot-spare mode. Receives the same bus
  messages as live Performers but doesn't drive outputs. Promoted to
  active Performer when one drops.
- **Cluster** — the set of Performers (and Standbys) coordinated by
  one Conductor.

A single-machine deployment is just "a Conductor with no remote
Performers" — same code, simpler topology. The Conductor's local
in-process Renderer drives that machine's outputs, exactly like
single-machine today.

## Why these words

- **Conductor** is active and accurate to what the role does at
  cluster scale: beats out time, signals cues, distributes per-frame
  `RenderFrame` messages. The orchestral metaphor is correct — every
  Performer locks to the Conductor's clock, just like every musician
  locks to the conductor's downbeat. There is no overlap with anything
  else in the Entity codebase or vocabulary.
- **Performer** is theatrical, fits Entity's existing Stage / Screen /
  Surface naming, single-word, and active. Doesn't directly copy an
  established media server's "Actor" while occupying the same
  conceptual slot.
- **Standby** is industry-standard for hot-failover. Doesn't borrow
  the "Understudy" used by a competing media server (which is a fine
  word but specifically theirs).
- **Cluster** is the standard distributed-systems term and reads
  unambiguously.

## Consequences

**Enables:**
- The cluster Pro plugin's class names use the new vocabulary
  consistently: `class ConductorService`, `class PerformerHost`,
  `class StandbyPerformer`, `class ClusterTopology`.
- Marketing / docs talk in cluster terms ("Add a Performer to your
  Cluster"; "Promote the Standby to active") without overloading the
  internal Director/Renderer language.
- Single-machine deployment is conceptually consistent: a Conductor
  *can* run with zero Performers; that's just current Entity.

**Forbids:**
- Renaming the in-process `Director` and `Renderer` services. ADR-0003
  is unchanged; only *new* code at the cluster layer uses Conductor /
  Performer.
- Using "Director" or "Renderer" as a cluster-level role name. They
  refer specifically to in-process services.

**Forces:**
- Discipline in docs about which layer a name applies to. The pattern
  to use:
  - "the in-process Director" / "the local Renderer" → ADR-0003 layer
  - "the Conductor" / "Performer N" / "the Cluster" → ADR-0008 layer
  - "the Conductor's in-process Director" → explicit cross-layer
    reference (rare; usually you mean one or the other)

## Layering, made explicit

```
                       ┌──────────────────────────────┐
                       │         Conductor            │  ← cluster lead
                       │  (one machine; show owner)   │
                       │                              │
                       │   ┌────────────────────┐     │
                       │   │ in-process Director│     │  ← ADR-0003
                       │   └─────────┬──────────┘     │
                       │             │                │
                       │   ┌─────────▼──────────┐     │
                       │   │ in-process Renderer│     │  ← ADR-0003
                       │   └────────────────────┘     │
                       └──────────────┬───────────────┘
                                      │  entity-bus (UDP, Phase E)
            ┌─────────────────────────┼─────────────────────────┐
            ▼                         ▼                         ▼
┌──────────────────┐      ┌──────────────────┐      ┌──────────────────┐
│   Performer 1    │      │   Performer 2    │      │     Standby      │
│                  │      │                  │      │  (hot spare for  │
│   in-proc Dir +  │      │   in-proc Dir +  │      │   any Performer) │
│   in-proc Rend   │      │   in-proc Rend   │      │                  │
└──────────────────┘      └──────────────────┘      └──────────────────┘
       │                          │
    outputs                    outputs
```

The Conductor *is* a Performer for its own local outputs (it has its own
in-process Renderer). The Cluster role is purely additional
responsibility, not a separate process type.

## Alternatives considered

- **Director / Actor / Understudy** (an established media server's
  vocabulary). Rejected: direct copy of a competitor in our exact
  niche; reinforces "knockoff" perception; doesn't actually solve the
  in-process / cluster layering problem.
- **Director / Runner** (a competing media server's vocabulary).
  Rejected: same copying problem.
- **Director / Aspect / Cluster.** Rejected: "Aspect" is a passive
  noun and doesn't pair with the active "Director" verb. You can't
  meaningfully *direct* an Aspect.
- **Producer / Performer / Standby.** Considered. "Producer" is a
  theatrical role one level above Director, which would work
  conceptually — but at runtime, the cluster lead actively *conducts*
  (beats time, cues entries) far more than it *produces* (financial,
  managerial). Conductor is the more accurate verb.
- **Control / Output / Backup** or **Host / Client / Spare** (boring
  pragmatic). Rejected: "Output" overlaps with the existing
  `OutputManager` and `OutputDisplay` names; "Render" overlaps with
  the existing Renderer service. Generic terms also actively erase
  Entity's brand voice.
- **Director / Performer with no special name for the cluster lead**
  (just call it "the Conductor's Director"). Rejected: the cluster
  lead's *cluster role* deserves a distinct name; conflating it with
  in-process Director defeats the purpose of layering.

## Open questions / future work

- Multi-Conductor topologies (active/active failover at the
  Conductor level, not just Performer level) — out of scope for the
  initial Cluster epic; revisit if a customer needs it. Vocabulary
  would extend with "Co-Conductor" or similar.
- Naming for sub-cluster groupings (e.g., "left-LED-wall Performers"
  vs "right-LED-wall Performers"). Probably "Performer Group" or
  "Section" (orchestral metaphor scales nicely). Defer until needed.

## References

- ADR-0003 (Director/Renderer in-process split) — the layer this ADR
  sits on top of.
- ADR-0006 (cluster-ready plumbing from day one) — the bus, content-hash
  AssetIds, and per-Renderer cache that make this topology possible.
- ADR-0007 (open-core feature tiering) — the Cluster Pro plugin lives
  in the Pro tier.
- The Cluster Phase E epic on the project board — implementation
  tracker (issue created alongside this ADR).
