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
| Working drafts / in-flight thinking | `~/.claude/plans/` (private to user) |
| Detailed bug registry (legacy) | `docs/reference/CODE_ISSUES.md` |

## Phase status (high-level)

- **Phase A — Stabilization** ✅ Complete
- **Phase B — God-file decomposition** ✅ Complete
- **Phase C — Single-machine MVP** ✅ Complete (closed at C.12 #11; 143/143 ctest green)
- **Phase C cleanup** 🔄 HapM second plane, soft-edge visual, FFmpegDecoder rename, etc. — see project board
- **Phase D entry — Director/Renderer split** ✅ Complete (213/213 ctest green at current HEAD)
- **Phase D feature work** 📋 Queued — LTC, OSC, audio, NDI, preview/program (5 epics on the project board)
- **Phase E+** Long tail — see master roadmap in `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md`

## Quick `gh` commands

```bash
# What's queued and not yet started
gh project item-list 2 --owner duremovich --format json \
  | python -c "import sys,json; d=json.load(sys.stdin); \
    [print(i['title']) for i in d['items'] if i.get('status') in (None,'Todo')]"

# Open epics
gh issue list --repo duremovich/Entity --label epic --state open
```

The `entity-roadmap` skill (`~/.claude/skills/entity-roadmap/SKILL.md`) has the
full set of patterns for reading/updating the project board.
