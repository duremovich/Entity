# Current Status

**Source of truth for active work:** [Entity Roadmap project board](https://github.com/users/duremovich/projects/2).

This file is a **thin pointer**, not a status log. It does not get updated as
work progresses. The project board does — that's the whole point.

## Where to look

| Question | Where |
|---|---|
| What's in progress right now? | Project board, filter `Status = In Progress` |
| What's queued for Phase D? | Project board, filter `Phase = D-feature` |
| What bugs are open? | Project board, filter `Phase = Bug`; or `gh issue list --label bug` |
| Why did we pick approach X? | `docs/adr/` (start at `docs/adr/README.md`) |
| What was done in past phases? | `docs/status/HISTORY.md` + git log |
| Working drafts / in-flight thinking | `~/.claude/plans/` — **private to the maintainer's machine; not readable by agents or contributors.** Durable decisions are summarized in ADRs; if a doc points here, treat the ADRs and issue tracker as the accessible record. |
| Detailed bug registry (legacy) | `docs/reference/CODE_ISSUES.md` |
| Roadmap review / doc-truth audit | `docs/archive/ROADMAP_REVIEW_2026-07-11.md` |

## Phase status (high-level)

Snapshot date: **2026-07-11**. Test counts below are the counts at each
phase's close; the suite has grown since (run `ctest -N` for the live
number — never trust a hard-coded count in a doc).

- **Phase A — Stabilization** ✅ Complete
- **Phase B — God-file decomposition** ✅ Complete (largest offenders decomposed; `Commands.cpp`, `D3D12Renderer.cpp`, and `Engine.cpp` remain large by design — see the 2026-07-11 roadmap review)
- **Phase C — Single-machine MVP** ✅ Complete (closed at C.12 #11; 143/143 ctest at close)
- **Phase C cleanup** 🔄 HapM second plane (#6), soft-edge visual (#7), FFmpegDecoder rename (#8), etc. — see project board
- **Phase D entry — Director/Renderer split** ✅ Complete (213/213 ctest at close)
- **Phase D feature work** 🔄 In progress:
  - ✅ Shipped: OSC in/out (#2), audio (#3), DMX/Art-Net (#13), per-layer effects (#54), Signal Output Layer (ADR-0027), remote-control plane (schema v27, ADR pending — #96)
  - 📋 Queued: LTC/MTC timecode (#1), preview/program (#5), MIDI Show Control (#14), NDI output (#93)
- **Phase E+** Long tail — macOS/Metal port (#48), cluster build-out (#94, prereq decision #84), Linux/Vulkan (#95, deferred). The original master-roadmap file is private to the maintainer's machine; Phase E scope is reconstructed in the epics above from ADR-0006/0008.

## Quick `gh` commands

```bash
# What's queued and not yet started (gh returns lowercase plain-string fields)
gh project item-list 2 --owner duremovich --format json \
  | python -c "import sys,json; d=json.load(sys.stdin); \
    [print(f\"[{i.get('phase','-')}] [{i.get('priority','-')}] {i['title']}\") \
     for i in d['items'] if i.get('status') in (None,'Todo')]"

# Open epics
gh issue list --repo duremovich/Entity --label epic --state open
```

The `entity-roadmap` skill (`~/.claude/skills/entity-roadmap/SKILL.md`) has the
full set of patterns for reading/updating the project board. **Note:** that
skill file lives on the maintainer's machine only — remote/CI agents won't have
it. Without it, use `gh` (or the GitHub MCP tools) directly: issues and epics
carry the same state as the board, and the board's Phase/Status fields are the
only board-exclusive data.
