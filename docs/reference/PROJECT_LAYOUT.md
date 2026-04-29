# Entity Project Layout

This is the on-disk shape of an Entity project (schema v7+). The full
rationale lives in
[ADR-0009](../adr/0009-structured-projects-with-reference-mode.md) — this
file is the operator-oriented reference. Constants are defined on
`ProjectManager` (`include/entity/project/ProjectManager.hpp`).

## Folder shape

```
ShowName/                         <- project root
├── ShowName.entity               <- project JSON (schema v7+)
├── content/                      <- managed assets, user-organized
│   ├── unsorted/                 <- default landing zone for new imports
│   ├── act1/                     <- user-created subfolders are first-class
│   │   ├── intro.mov             <- HAP-transcoded (canonical, playable)
│   │   └── .archive/             <- per-folder, only when transcoded
│   │       └── intro.mov         <- pre-transcode original
│   └── vj/loops/...              <- nest however you like
├── presets/                      <- mapping presets, color profiles, OSC maps
├── objects/                      <- 3D models for projection mapping
├── exports/                      <- recordings/captures
├── snapshots/                    <- project save snapshots
└── .cache/                       <- machine-local, regenerable
    └── thumbnails/
```

## Travel-with-the-project vs. machine-local

Two tiers of state live inside a project folder. The line between them
matters for backup, version control, cluster sync, and bundling.

| Sub-tree | Tier | Travels with the project? |
|---|---|---|
| `<name>.entity` | Project state | **Yes** — required |
| `content/` (incl. `.archive/`) | Managed assets | **Yes** — load-bearing |
| `presets/`, `objects/`, `exports/`, `snapshots/` | Project state | **Yes** |
| `.cache/` | Machine-local, regenerable | **No** — exclude |

`.cache/` holds derivatives that any machine can regenerate from
`content/`: thumbnails today, proxies later. Deleting it is always safe;
it will rebuild as the project is used. Two reasons it's separated:

1. **Cluster sync.** Pushing a project from a Conductor to a Performer
   (ADR-0006) is "rsync the project directory." Excluding `.cache/`
   avoids shipping per-machine derivatives that the Performer will
   rebuild anyway.
2. **Bundling.** `Save Project As Bundle...` writes a portable copy of
   the project for archive or hand-off. `.cache/` is omitted by design
   (`ProjectManager::saveAsBundle`) so the bundle stays lean and the
   recipient's first open just rebuilds locally.

## Linked vs. Managed (the `.archive/` story)

`content/` is for **managed** imports. Each `mediaLibrary` entry has a
`pathKind`:

- `Managed` — path is project-relative, file lives under `content/`.
  Transcodes replace the source at the canonical content path, with the
  pre-transcode original moved to a sibling `<sub>/.archive/<filename>`.
  Restorable via the MediaBin right-click "Restore Original" action.
- `Linked` — absolute path, file stays where the operator put it. The
  QLab/Watchout-style escape hatch. Transcodes (when produced) land in
  `<project>/.cache/hap/` rather than rewriting the source.

A project can mix both freely. `Collect Linked Media into Project
Folder...` (File menu) is the one-shot conversion that walks all Linked
entries, copies them into `content/<sub>/`, archives any cache-dir
transcodes, and flips the entries to Managed.

## Sync-exclusion patterns

When syncing or version-controlling project folders, exclude `.cache/`:

```sh
# rsync (e.g. for cluster push, manual sync)
rsync -av --exclude='.cache/' ShowName/ performer:/projects/ShowName/

# .gitignore inside a project folder committed to a repo
.cache/
```

Entity's own repository `.gitignore` excludes `.cache/` so a small demo
project committed alongside the source tree doesn't drag the cache.

## Quick reference

| Need | Do |
|---|---|
| Hand the show off | File > Save Project As Bundle... |
| Convert a legacy/free-path project | File > Collect Linked Media into Project Folder... |
| Fix a "naked" .entity (orphan showfile) | File > Rebuild Project Structure, then Find Missing Media... |
| Roll back a transcode | MediaBin right-click > Restore Original |
| Free disk on a project | Delete `.cache/` — Entity rebuilds on next open |
