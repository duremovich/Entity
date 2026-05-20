# ADR-0009: Structured projects (folder-as-project) with reference-mode escape hatch

- **Status:** Accepted
- **Date:** 2026-04-29
- **Context source:** strategic discussion comparing the rigid
  prescribed-layout model used by established media servers vs. the
  free-floating-workspace model used by cue-based and workspace-file
  tools.
  Working plan: `~/.claude/plans/i-have-a-question-async-ripple.md`.
- **Implemented by:** GitHub epic
  [#20](https://github.com/duremovich/Entity/issues/20). Shipped
  across ten commits on `master`:
  - `850dd45` — schema v7 + `ProjectManager::createNew()`
  - `4ae559b` — Project Launcher + Recent Projects
  - `9a53d3e` — Copy/Link import flow + Managed path resolution
  - `7dc2828` — TranscodeManager repoint + archive-on-transcode
  - `f31273f` — Collect Linked → Managed
  - `62b4c9b` — Save Project As Bundle
  - `9056719` — Orphan-showfile recovery (Rebuild + Find Missing)
  - `d79121d` — Restore Original from archive
  - `83a3ed4` — `.cache/` discipline + `docs/reference/PROJECT_LAYOUT.md`
  - `fe5b52b` — File > Close Project returns to launcher

## Context

Entity today follows the free-floating-workspace model: a `.entity`
project file references absolute paths anywhere on disk. The only foothold toward
project-as-folder is the HAP transcode cache, which lives at
`<projectDir>/.cache/hap/`. Otherwise, paths are fully free-floating.

Two industry models exist for live-show project state:

- **The prescribed-layout model** (established media servers): rigid
  prescribed folder layout per show (`content/`, `objects/`,
  `recordings/`, etc.). Importing media copies/links into the project
  tree. You cannot reference arbitrary external paths.
- **The free-floating-workspace model** (cue-based and workspace-file
  tools, most others): a workspace file references files at arbitrary
  paths. Move the workspace, get a "missing media" relink dialog.
  "Bundle/Collect" is an opt-in export step.

Entity sits in the professional-media-server market segment — projection
mapping for live shows, with cluster ambitions (ADR-0006) and a Pro tier
targeting touring/install (ADR-0007). That market cares about
portability and cluster sync more than the single-machine audience.
But Entity is also open-core and wants to win indie/small-venue users
who live in the free-floating-workspace flexibility model.

Cluster sync (the ADR-0006 follow-on epics, currently unqueued) is
dramatically simpler when projects are folders. Pushing a project to a
Performer becomes "rsync the directory" instead of "rewrite every
absolute path + transfer every referenced file individually + handle
missing-file fallbacks per asset." Locking in the folder-as-project
model *before* cluster sync starts is the right ordering.

## Decision

**Adopt structured projects as the default model. Per-asset "link in
place" is an explicit opt-in escape hatch.** A project is a folder
on disk with a prescribed subdirectory layout; the `.entity` file
sits at the root.

```
ShowName/                        <- project root
├── ShowName.entity              <- project JSON (schema v7+)
├── content/                     <- managed assets, user-organized
│   ├── act1/                    <- user-created subfolders, first-class
│   │   ├── intro.mov            <- the playable (HAP-transcoded) file
│   │   └── .archive/            <- per-folder, only when needed
│   │       └── intro.mov        <- pre-transcode original
│   └── unsorted/                <- default landing zone for new imports
├── presets/                     <- mapping presets, color profiles, OSC maps
├── objects/                     <- 3D models for projection mapping
├── exports/                     <- recordings/captures
├── snapshots/                   <- project save snapshots
└── .cache/                      <- machine-local, regenerable, sync-excluded
    └── thumbnails/
```

Three load-bearing design choices:

1. **The transcode replaces the source at the canonical path.** When
   HAP is required, the file at `content/act1/intro.mov` *is* the HAP
   transcode. The original (ProRes, H.264, whatever) moves to a
   sibling `content/act1/.archive/intro.mov`. Entity always plays
   from the user-organized location; there is no parallel `transcodes/`
   tree.

2. **Archives are per-folder and loose-coupling.** Because users will
   subdivide `content/` however they organize their show
   (`act1/scene2/`, `vj/loops/`, etc.), each folder containing
   transcoded content gets its own sibling `.archive/`. When the user
   moves a content subfolder, its archive moves with it. The dotfile
   prefix keeps `.archive/` visually quiet but it's still browseable.
   Missing `.archive/` is fine — Entity carries on, you just can't
   recover the pre-transcode source.

3. **The Project Launcher is the entry point, not a blank workspace.**
   Launching Entity opens a Project Manager dialog with Recent / New /
   Open. There is no "untitled, in-memory" state that you can import
   files into before saving. New Project asks for name + parent
   directory, creates the folder tree on disk, then opens the editor
   pointed at it. This matches the project-browser entry point used by
   established media servers and by Unity Hub / Unreal.

`mediaLibrary` entries gain `path_kind` (`"managed"` = project-relative
under `content/`; `"linked"` = absolute, the free-floating-workspace
escape hatch),
`archived_original` (project-relative path under `.archive/` when
transcoded), and `original_codec`.

## Why this shape, not strict prescribed-layout

Locking out reference-mode loses too much of the open-core target
market. Single-machine theater shops with `\\nas\events\...` already
organized want to *reference* what's already there, not be forced into
a per-project copy step. Reference-mode keeps that workflow open while
the structured default serves the touring/cluster market.

## Why transcode replaces source (not a parallel tree)

A `media/sources/` + `media/transcodes/` parallel-tree alternative was
considered and rejected. Two reasons:

1. The user organizes `content/` for their show. If they put the
   playable file under `content/act1/`, that's where they expect to
   find it — not in a hash-named blob under `media/transcodes/`.
2. A parallel tree means every "what's the playable path?" lookup
   becomes a join across two structures. Replace-at-canonical-path
   collapses that to one lookup.

The cost is that the original codec is no longer at the path the user
imported from. The `.archive/` directory mitigates this; restore is a
right-click action.

## Why per-folder archives (not one global archive)

A single root-level `.archive/` was considered. Rejected because users
will reorganize their content/ subtree (move `act1/scene2/` to
`scenes/finale/`), and a global archive would either break or require
the project file to track every original->archive mapping by absolute
project-relative path. Per-folder `.archive/` directories travel with
their content automatically — file-browser drag-drop just works.

## Consequences

**Enables:**
- Cluster sync (ADR-0006) becomes "rsync the project directory" for
  managed assets. Linked assets fail loudly when the same path
  doesn't resolve on a Performer; that's a feature, not a bug.
- Bundle/archive is trivial: zip the folder.
- Save-As that actually copies the show's content, not just the JSON.
- `.cache/` discipline: cleanly machine-local, never bundled, free to
  regenerate.

**Forbids:**
- Treating the `.entity` file as a single-file save target unless the
  project is wholly free-path (all `path_kind: "linked"`). Structured
  projects move/copy `content/` along with the JSON.
- Transcode output in `.cache/hap/` (the previous home). Transcodes
  now live at the canonical content path and travel with the project.

**Forces:**
- A v6 → v7 schema migration. v6 entries become `"linked"` on load.
  A "Collect into project folder" command exists to convert legacy
  projects to managed by copying linked assets into `content/` and
  re-saving as v7.
- Editor entry-point rework. Engine no longer initializes into a
  blank state; the launcher provides a valid project root before the
  editor window opens.
- TranscodeManager output target changes from the cache dir to the
  canonical content path. Workers archive the source first.

## Alternatives considered

- **Stay free-path; add a Bundle/Collect export step** (the
  free-floating-workspace model). Rejected because it punts the
  cluster-sync problem and
  every asset-touching system continues to accrete free-path
  assumptions. Doing the structural work now is cheaper than after
  LTC/OSC/audio/DMX bolt onto absolute-path conventions.

- **Strict prescribed-layout (no link-in-place escape hatch).**
  Rejected because it loses the free-floating-workspace user.
  Open-core needs to win both segments; a single-default-with-opt-out
  covers both.

- **Parallel `media/sources/` + `media/transcodes/` trees.** See
  "Why transcode replaces source" above.

- **Top-level `media/` instead of `content/`.** Rejected: "media" is
  already overloaded in Entity (media library, media bin, media file
  formats). `content/` is unambiguous and conventional.

- **Global root-level `.archive/`.** See "Why per-folder archives"
  above.

- **Defer to Phase E alongside cluster work.** Defensible — the
  High-priority Phase D items don't strictly conflict with free
  paths. But the longer this waits, the more code accretes free-path
  assumptions. Doing it first is cheaper than retrofitting.

## References

- ADR-0003 (Director/Renderer in-process split) — the project file
  is owned by the in-process Director.
- ADR-0006 (cluster-ready plumbing from day one) — the load-bearing
  reason structured projects matter beyond single-machine
  portability.
- ADR-0007 (open-core feature tiering) — touring/Pro market is the
  primary beneficiary; free-floating-workspace single-machine users
  keep link-in-place as escape hatch.
- Working plan:
  `~/.claude/plans/i-have-a-question-async-ripple.md` — the full
  analysis and implementation sketch.
- Phase D epic on the project board (filed alongside this ADR) —
  implementation tracker.
