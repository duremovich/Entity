# Roadmap & Documentation Truth Review — 2026-07-11

**Purpose.** Point-in-time audit of the roadmap and documentation against the
actual codebase and issue tracker, so that future contributors and agents
inherit an accurate picture. Three parallel audits were run (docs/ADR
internal consistency; code-vs-docs reality check; issue-tracker
cross-check), findings were re-verified against source, and the fixes landed
on branch `claude/codebase-roadmap-review-1jlm2s` alongside a tracker
cleanup. This file is the durable record of what was found, what was fixed,
and — just as important — what was deliberately **not** fixed here.

Precedent/format: `docs/archive/CODE_REVIEW_2025-11-27.md`.

---

## 1. Ground truth (verified against source at snapshot date)

| Fact | Value | Source of truth |
|---|---|---|
| Unit/golden test cases (gtest) | 783 | `tests/unit/*.cpp` + `test_Decoder.cpp`, registered via `gtest_discover_tests` |
| Integration tests | 56 | `tests/CMakeLists.txt` (`entity_add_integration_test` + raw `add_test`) |
| Project schema version | 28 | `include/entity/project/ProjectSerializer.hpp` (v27 = RemotePatch, v28 = TrackUiState — changelog in the header) |
| Plugin API version | 2 | `plugin-api/include/entity/plugin/Plugin.hpp` (v2 = `setRemoteParam`/`setRemoteParamString`) |
| `MAX_COMPOSE_TARGETS` | 64 | `include/entity/render/IRenderer.hpp` (8 → 32 in ADR-0018, → 64 in ADR-0019) |
| FrameCache default | 3 GiB | `include/entity/core/Settings.hpp` (512 MiB → 2 GiB Fix 10 → 3 GiB Phase 3b, 2026-05-24) |
| ADR count | 27 accepted (0028 unwritten — see §6) | `docs/adr/` |
| Layer kinds | 4: Clip, ObjectAnimation, Generative, Signal | `include/entity/components/Layer.hpp` |
| First-party plugins | 4: bus-logger, dmx-artnet, osc-receiver, osc-sender | `plugins/` |
| Bus transports | 1: in-memory only | `src/bus/InMemoryMessageTransport.cpp` |

**Maintenance rule this review keeps re-learning:** never hard-code test
counts (or any monotonically drifting number) in a doc as "current". Docs
found citing 143, 213, 408, 422, 510, 611, 615, and 624 tests "at current
HEAD" were all simultaneously wrong. Say `ctest -N` instead.

## 2. Contradictions found (and fixed on this branch)

| # | Contradiction | Fix |
|---|---|---|
| C1 | README said "510/510 ctest green at current HEAD"; CURRENT.md said "213/213"; both months stale | Non-brittle phrasing in README; CURRENT.md marks counts as phase-close historical |
| C2 | CODE_ISSUES.md carried "1 High open" (HIGH-13/NEW-01: "HAP decoder is a scaffolded stub") while the HAP decoder has shipped with golden-test coverage since Phase C.9 | Marked fixed with precise residual-stub wording (non-FFmpeg `#else` branches; unused software BC fallback); header now 0 High |
| C3 | TROUBLESHOOTING.md + ECS_PRINCIPLES.md described NEW-07/NEW-08 stall-fallback gaps as open; they closed 2026-05-11 / 2026-05-20 | Rewritten to describe shipped snapshot-bake / detector-on-show behavior |
| C4 | FrameCache default: code says 3 GiB; zone-cookbook said 2 GiB "current", SettingsWindow tooltip told users 512 MiB, Clip.hpp comment said 2 GiB | All updated to 3 GiB (= issue #63); historical HISTORY/ADR-0012 text annotated, not rewritten |
| C5 | README advertised OSC as shipping while CURRENT.md listed OSC (and audio) as Phase D "Queued"; both are substantially shipped | CURRENT.md Phase D split into shipped vs queued with issue links |

## 3. Stale-doc fixes (by commit on this branch)

1. **Status snapshot** — README (milestones, plugin list, four layer kinds,
   ADR count, DMX/outbound-OSC coverage) + CURRENT.md (phase table, epic
   links, private-path annotations, snapshot date).
2. **Issue-registry closeout** — CODE_ISSUES.md (HIGH-13/NEW-01, header
   counts, "unverified Mediums" caveat), TROUBLESHOOTING.md (NEW-07/08,
   `docs/phases/` dangling ref, 2024 date stamp), ECS_PRINCIPLES.md.
3. **ADR-0027 catch-up** — ENTITY_ARCHETYPES.md (Signal archetype section;
   Generative marked shipped), SYSTEM_ORDERING.md (SignalOutputSystem as
   show step 1.2 + fallback-table row), Layer.hpp doc block.
4. **FrameCache 3 GiB** — zone-cookbook (+ SignalOutputSystem zone, snapshot
   date), SettingsWindow tooltip, Clip.hpp comment, bracketed editor's notes
   in HISTORY.md / ADR-0012.
5. **ADR corrections** — ADR-0018 32→64 supersession notes; ADR-0012
   amendment-ordering note; adr/README known-gaps section (ADR-0028, schema
   v27/v28, plugin API v2) + private-source note; HISTORY Fix-3
   forward-pointer (prefetch was a no-op, code since removed).
6. **Dangling links** — BUILD.md troubleshooting path; archived phase-04 doc.

## 4. Tracker actions (2026-07-11)

**Created:**

| Issue | What / why |
|---|---|
| #93 | Epic: NDI output — advertised in CURRENT.md's Phase D list but had no tracking issue; grounded in the `OutputDriver.hpp` stub + `OutputType::NDI` scaffolding; flags the SDK-license/tier question |
| #94 | Epic: Phase E cluster build-out (Conductor/Performer) — umbrella that previously didn't exist; only prep cards (#84 as sub-issue, #85 shared with #48) were filed; honest about ADR-0006 "cluster-ready" meaning seams, not transport (in-memory bus only; `Engine.cpp:705` transport TODO folds in) |
| #95 | Epic (deferred): Linux/Vulkan port — #48 claimed this was "tracked separately"; now it actually is |
| #96 | Write ADR-0028 (RemotePatch remote-control plane) — 40+ code cites to an ADR that doesn't exist |
| #97 | Bug: all four plugin manifests declare `requiredApiVersion: 0` while osc-receiver calls v2 API methods — real loader-compat trap; deliberately not fixed on this docs branch |
| #98 | Bug: output fullscreen toggle is a no-op (`OutputManager.cpp:376` TODO) — user-facing, previously untracked |

**Closed:** #42 (show/UI thread split — shipped per ADR-0014 "Implemented
by: Issue #42"), #43 (Tracy — shipped per ADR-0015), #45 (renderer snapshot
invariant — superseded by ADR-0014's documentation; HIGH-02 already recorded
closed), #49 (duplicate of epic #70; unique subtasks noted there), #2 (OSC
epic — every subtask shipped: osc-receiver/osc-sender plugins,
ShowControlWindow mapping editors, per-project JSON persistence,
`integration_remote_layer_osc`), #63 (FrameCache doc refs — commit 4 above).

**Edited/commented:** #26 body rescoped (auto-sync MVP shipped; remainder =
#28/#29/#30), #1 (audio dependency now satisfied; acceptance normalized),
#5/#14 (acceptance normalized), #53 (fix landed via `RESOURCE_LOCK
d3d12_warp`, commit `c2f613c`; left open pending 5 consecutive green
parallel runs), #86 (cross-linked as the blocker for any "CI green"
acceptance criterion).

**Acceptance-criteria rule adopted:** until #86 (CI has never been green —
fixtures never generated on runners) lands, epic acceptance criteria must
say **local `ctest` green**, not "CI green". CI-green criteria are currently
unsatisfiable and several epics carried them.

## 5. Project-board limitation (and mitigation)

Remote agent sessions cannot mutate the user project board
(`users/duremovich/projects/2`): the GitHub MCP toolset has no Projects-v2
item tools, and repo-level issue fields are empty for this repo. New issues
created remotely therefore do NOT appear on the board by themselves.

- **Standing fix (recommended):** enable the board's built-in **"Auto-add to
  project"** workflow (filter `is:issue is:open`, repo `duremovich/Entity`)
  so every new issue lands on the board automatically.
- **For the issues created in this review**, run locally:

```bash
for n in 93 94 95 96 97 98; do
  gh project item-add 2 --owner duremovich \
    --url "https://github.com/duremovich/Entity/issues/$n"
done
# then set fields per issue, e.g.:
#   #93 Phase=D-feature  #94 Phase=E  #95 Phase=E  (Priority per taste)
#   #96 Phase=D-feature (docs)  #97 Phase=Bug  #98 Phase=Bug
```

## 6. Known gaps NOT fixed here (the honest list)

1. **Gitignored `CLAUDE.md` rule sheets** (`include/entity/components/`,
   `include/entity/systems/`, `plugin-api/`, `include/entity/bus/`,
   `include/entity/director/`, `scripts/`) exist only on the maintainer's
   machine, yet ADRs and reference docs cite them as authoritative. The
   maintainer intends to commit them separately; until then, docs updated by
   this review annotate each reference as maintainer-local. The bus
   wire-format rules ("additive-only, enums as strings") cited as living in
   `include/entity/bus/CLAUDE.md` have **no in-repo statement** — worth
   folding into ADR-0028 or a bus README when written.
2. **The private master-roadmap plans file** referenced by the old
   CURRENT.md is unavailable to agents/contributors. Phase E scope was
   reconstructed into #94 from ADR-0006/0008; if the private file holds
   more, it should be folded into the epic.
3. **ADR-0028 remains unwritten** (#96). Schema v27/v28 and plugin API v2
   are documented only in header changelogs.
4. **Phase B "god-file decomposition ✅ Complete" vs reality:** at snapshot
   date `src/command/Commands.cpp` = 7,332 lines, `src/render/D3D12Renderer.cpp`
   = 7,052, `src/core/Engine.cpp` = 5,757, `include/entity/command/Commands.hpp`
   = 3,790. The phase closed against its original scope, but the three
   largest files are god-files by any definition. No issue filed — decompose
   opportunistically (e.g. #44's command-recording seam, #92's
   deferred-destruction unification) rather than as a big-bang phase.
5. **ADR-0006 "cluster-ready plumbing from day one" is aspirational in
   code:** the seams exist (bus messages, Director/Renderer split), but the
   only transport is in-memory and no node/identity/versioning layer exists
   (#84, #94 now track this honestly).
6. **CI has never been green** (#86): integration fixtures are never
   generated on runners; `test_media/Basic loop.mov` has no generator. All
   "N/N green" claims in HISTORY are local runs. CI = Windows/WARP build +
   ctest (with the fixture problem) + a ubuntu license-boundary grep; no
   Linux/macOS build legs.
7. **CODE_ISSUES.md Medium/Low entries unverified since 2026-04-19** — now
   labeled as such in the file; a re-verification pass is unclaimed work.
8. **HapM alpha plane (#6), HapH import enable (#9), soft-edge visual
   verification (#7), FFmpegDecoder rename (#8)** — pre-existing Phase C
   cleanup items, confirmed still real, unchanged by this review.
9. **`SettingsWindow` FrameCache tooltip frame-count arithmetic** is an
   estimate (770 @ 4K HAP-Q assumes ~4 MiB/frame DXT-class); update if the
   HAP-Q footprint assumption changes.

## 7. Maintenance recommendations

- **Never hard-code drifting numbers** (test counts, "latest milestone",
  size defaults) in more than one place. State where the source of truth
  lives and point at it.
- **Docs that mirror code must name their mirror** (SYSTEM_ORDERING ↔
  `Engine::showThreadMain`) and get updated in the same PR that changes the
  mirrored code — that's the deal that keeps them trustworthy.
- **Every shipped feature epic gets its checklist ticked or the issue
  closed** in the same week it ships. #2 and #42 sat open for two months
  after shipping and poisoned every downstream status summary.
- **ADR before (or with) the feature, not after**: ADR-0028 shows what
  happens otherwise — 40+ dangling citations.
- **When a new agent session starts**, `docs/status/CURRENT.md` +
  `docs/adr/README.md` + the issue tracker are the accurate entry points;
  anything citing `~/.claude/` is maintainer-local provenance.
