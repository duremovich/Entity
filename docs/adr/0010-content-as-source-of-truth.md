# ADR-0010: `content/` is the source of truth; filename-based versioning

- **Status:** Accepted
- **Date:** 2026-04-29
- **Context source:** strategic discussion on enabling external file-sync
  tools (FreeFileSync, rsync, robocopy) as first-class collaborators in
  populating a project's `content/` folder, plus mirroring the
  long-standing filename-versioning convention common in projection-mapping
  rigs.
- **Implemented by:** GitHub epic
  [#26](https://github.com/duremovich/Entity/issues/26). First sub-card
  ([#27](https://github.com/duremovich/Entity/issues/27)) lands the
  ContentScanner, version grouping, and resolver inversion. Sub-cards
  for missing-media UX (#28), per-clip pin/rollback UI (#29), and
  `ReadDirectoryChangesW` (#30) are tracked separately.

## Context

ADR-0009 established the structured project layout: media for a Managed
project lives at `<projectRoot>/content/<sub>/<filename>`, with
pre-transcode originals tucked into per-folder `.archive/` siblings.
Today, files reach `content/` only through Entity's Import flow. Two
problems with that:

1. **External co-writers are second-class.** Touring rigs typically keep a
   project folder in sync across multiple machines via rsync, robocopy,
   or FreeFileSync. Today Entity won't see anything dropped in `content/`
   until the user runs Import — which defeats the entire point of having
   a structured project folder.
2. **Filename versioning is a long-standing convention.** Designers
   iterate by saving `intro_v07.mov` rather than overwriting `intro.mov`.
   Production media servers in this space group versioned files into one
   logical media item, auto-roll the timeline to the latest version, and
   keep older versions on disk for rollback. Entity needs the same
   behaviour to be a credible drop-in.

## Decision

Three load-bearing rules:

### 1. `content/` is canonical; external writers are first-class

The project's `content/` folder is the single source of truth for what
media exists. A new system (`ContentScanner`) runs a background poll of
the folder; files dropped in by external tools appear in the MediaBin
without an Import action. Files removed externally are soft-marked
"missing" but not erased from the library — brief unlink-then-rename
windows during sync don't get to nuke user state.

### 2. `clip.filepath` is a logical reference, not a physical path

The `Clip` component's `filepath` field becomes a logical reference to
the media. Two shapes:

- No `_v<tag>` suffix → **auto-roll** mode. Resolver picks the latest
  member of the group at decode-open time.
- Has a `_v<tag>` suffix → **pinned** mode. Resolver requires an exact
  match.

This encoding is intentional: rolling back a clip to a specific version
is implemented by rewriting the clip's `filepath` to include the
suffix; reverting to auto-roll strips it. No new component fields,
no schema bump.

### 3. Filename versioning regex is fixed

The grouping pattern is `^(.+)_[vV]([A-Za-z0-9]+)$` against the filename
**stem** (no extension), anchored to the end. The marker `_v` is
case-insensitive — real-world filenames ship with both `_v01` and `_V01`
and we treat them identically. Tags are alphanumeric — no underscores
allowed inside the tag, so role suffixes like `_v01_alpha.mov` are NOT
versioned files (the whole stem is the base, and they get their own
group). This is deliberate: when the role taxonomy is unspecified,
treating role-suffixed files as distinct groups is the safer default.

Sort: smart numeric-first. If both tags parse cleanly as base-10
integers, compare numerically (`v2 < v10`); otherwise lexicographic
(handles date-style `v20210602a`).

## Consequences

### Enables

- Professional touring workflows: keep a `content/` folder in sync
  across machines via rsync; Entity picks up changes on the next scan
  tick without UI interaction.
- Designer iteration loop: save `intro_v08.mov` over wifi and the
  timeline rolls forward automatically when the file lands and stabilizes.
- Per-clip rollback (Phase 3 UI, [#29](https://github.com/duremovich/Entity/issues/29)):
  designers can pin individual instances of a clip to a known-good version
  while sibling instances continue to auto-roll.
- Decoupled scanner backend (Phase 4, [#30](https://github.com/duremovich/Entity/issues/30)):
  the polling backend can be swapped for `ReadDirectoryChangesW` without
  changing any caller.

### Forbids / forces

- `clip.filepath` may now reference a file that doesn't exist on disk
  (logical references resolve at decode-open). All paths through the
  resolver must call `decoderPathFor` instead of using `filepath`
  directly to compute disk paths.
- The library now has a transient `missingOnDisk` flag that is **not**
  serialized. UI must treat it as ground truth between scans, but the
  serializer ignores it.
- Pre-existing v7 clips whose `filepath` already includes a `_v` tag
  are treated as pinned-to-that-version. That's behaviourally correct
  (the user originally chose that file) but means auto-roll is opt-in
  via context menu (#29), not retroactive.
- The serializer **must not** rewrite `clip.filepath` at load time —
  it is the user's intent. Translation happens only at decode-open.
- TranscodeManager's in-place file replacement (per ADR-0009) intersects
  with the scanner's "modified" detection. The scanner emits no Modified
  delta for known entries by design; in-place replacement is silent and
  the decoder picks up the new content on next open.

### Costs

- Schema unchanged, but the semantics of `clip.filepath` shifted —
  documentation and any code touching the field needs to be aware.
- Periodic background polling adds a small steady-state CPU cost
  (one stat-walk every 2 s). Phase 4 (#30) replaces polling with
  notifications on Windows; SMB shares stay on polling.
- The fixed regex is a UX commitment. We can extend the regex (e.g.
  to recognize role suffixes) without breaking existing groupings, but
  changing the meaning of an existing pattern would silently re-group
  shipped projects.

## Alternatives considered

### Watch + import-prompt

Detect new files in `content/` and prompt the user with an "Import?"
modal instead of adding silently. Rejected — defeats the FreeFileSync
workflow (the user expects files to appear without UI), and the
modal-spam during a bulk sync is worse than the alternative.

### Schema v8 with explicit `pinPolicy` enum on Clip

Add a `pinPolicy: { Auto, Pinned, PinnedExact }` enum field on the Clip
component, plus a `pinnedTag` string. Cleaner data model. Rejected for
v1 — schema migrations are expensive; the filepath-shape encoding
covers every behaviour in the user's expectation matrix without a
schema bump. A future v8 can add the explicit enum if a third pin mode
appears (e.g. "always use the unversioned base file even when versions
exist").

### Pluggable / configurable version regex

Let users configure their own version pattern (e.g. `_R<rev>` instead
of `_v<tag>`). Rejected — adds significant complexity and produces
non-portable projects. The fixed convention is the load-bearing
decision; a user with a different convention can rename their files
or wait for explicit support.

### Cross-pathKind grouping

Allow a Linked entry (`/nas/show/intro_v01.mov`) to group with a
Managed entry (`content/intro_v02.mov`). Rejected — surprises nobody
expects ("why did the timeline roll forward to a file from someone
else's NAS?"). Group keys are scoped per pathKind. Linked + Managed
files with the same logical name remain distinct entries.

## References

- ADR-0009 — Structured projects (the `content/` layout this builds on).
- Working plan: `~/.claude/plans/imperative-shimmying-treehouse.md`.
- Epic: [#26](https://github.com/duremovich/Entity/issues/26).
- MVP sub-card: [#27](https://github.com/duremovich/Entity/issues/27).
