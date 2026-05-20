# ADR-0005: Open-core dual-license + plugin scaffold

- **Status:** Accepted
- **Date:** 2026-04-29
- **Context source:** working plan `~/.claude/plans/i-would-like-to-foamy-shannon.md`,
  recorded in memory `architecture_open_core_plugins.md`
- **Implemented by:** open-core scaffold (commit `1cdd8c7` on origin/master,
  post-history-scrub; original commit was `dbeaf29`). Sibling private
  repo Entity-Pro at `f48c27e` on origin/main.

## Context

Entity's commercial vision is a replacement for the established media servers,
which means: (a) some plugins are commercially load-bearing and must stay
proprietary, (b) the core engine wants community contribution and ecosystem
plugins, (c) we need a clean licensing story before the project goes public
or any third-party touches the code.

The constraint is strong: GPL-style copyleft on the core protects against
fork-and-close-source competitors, but a strict GPL plugin layer would prevent
proprietary plugins (including Entity Pro). We need a structure that's
honest about which parts are which.

## Decision

**Two repositories, hybrid plugin transport, dual licensing:**

### Repos

- **Public** (`C:/Entity/Entity`, `github.com/duremovich/Entity`): GPLv3 core
  with a linking exception, plus Apache 2.0 covering `entity-bus/`,
  `plugin-api/`, and `plugins/`.
- **Private** (`C:/Entity/Entity-Pro`, `github.com/duremovich/Entity-Pro`):
  proprietary Entity Pro plugins. Sibling clone of public repo; no top-level
  `CMakeLists.txt`. Folded into public build via
  `-DEXTRA_PLUGIN_DIRS=C:/Entity/Entity-Pro/plugins`. CI never references
  it.

### Licensing

- Root `LICENSE` carries full GPLv3 text + project-specific linking-exception
  preamble (so GitHub auto-detect picks up GPL-3.0 cleanly).
- Root `LICENSE-PLUGIN-API` carries full Apache 2.0 text. One file at root —
  no per-plugin LICENSE ceremony. Each plugin's `manifest.json` declares its
  license explicitly.
- `LICENSING.md` is the human-readable explainer.

### Plugin transport: hybrid by data class

- **Control-plane plugins** (OSC, timecode, DMX, MIDI, telemetry,
  output-driver lifecycle) subscribe/publish on `entity-bus`. Already
  network-serializable; future static→dynamic upgrade is purely additive.
- **Hot-path plugins** (NDI/SDI output, codec providers, custom shaders)
  implement `OutputDriver` / `CodecProvider` C++ ABIs from
  `plugin-api/include/entity/plugin/`. Static-link only forever.
  Toolchain-locked (same MSVC/STL as core). Acceptable because hot-path
  plugins are first-party Entity Pro work, not third-party.

### Plugin loading: static, generated dispatcher

- `cmake/EntityPlugins.cmake` provides `entity_register_plugin()`,
  `entity_discover_plugins()`, `entity_emit_static_registry()`.
- `EXTRA_PLUGIN_DIRS` is the cross-repo seam — semicolon-separated cache
  variable, folds external plugins into the build with zero reference in
  public code.
- Generated `build/generated/StaticRegistry.cpp` defines
  `entity::plugin::registerStaticPlugins(IPluginContext*)` and is linked
  into `EntityMediaEditor` (not `EntityMediaCore`, so test binaries skip
  plugin libs).

### Plugin-API boundary (CI-enforceable)

`plugin-api/` headers must not include:
- `<entt/entt.hpp>`
- `<d3d12.h>`
- `entity/{core,render,timeline,director,renderer}/*`

`entity-bus` headers same rules. Hot-path plugins (`OutputDriver`,
`CodecProvider`) get an explicit exception for specific core types
(`TextureRef`, `DecodedFrame`, `FrameLease`).

## Consequences

**Enables:**
- Entity Pro can ship as proprietary closed-source via sibling private repo
  without violating the public GPL.
- Third-party community plugins can be Apache 2.0 (the plugin API is Apache,
  so they're not derivative of GPL core in the linking-exception sense).
- Public + private build with one CMake configure command — no monorepo,
  no submodules.
- Phase D plugin classification (`plugins/` for OSC/timecode/audio/LTC,
  `Entity-Pro/plugins/` for NDI/Dante/multi-sync) attaches features to the
  right side of the boundary from day one.

**Forbids:**
- Linking proprietary code into the GPLv3 core itself. The linking exception
  applies to plugins crossing the API boundary, not to fork-and-close-the-core
  scenarios.
- Bus-only data in `entity-bus` headers from including engine internals.
- Hot-path plugin distribution as third-party DLLs (toolchain-locked,
  first-party only).

**Forces:**
- Discipline on `plugin-api/` and `entity-bus/` headers. CI grep guard not
  yet wired (`.github/workflows/ci.yml` TODO before first outside
  contributor).
- Future dynamic loader for control-plane plugins (`extern "C"
  entity_plugin_register`, scan `<exe>/plugins/*.dll`) deferred until first
  commercial Entity-Pro plugin needs binary distribution. Architecture
  supports it; not implemented yet.
- Docs split: project's architecture documentation (`CLAUDE.md`) is
  gitignored and lives only locally — backup at
  `~/.entity-claude-backup-2026-04-29/CLAUDE-files/`. Future contributors
  cloning fresh won't see it without a manual share.

## Alternatives considered

- **Single GPLv3 repo, no proprietary plugins.** Doesn't support the Entity
  Pro commercial path. Either we ship everything open or nothing open — no
  middle ground.
- **MIT/Apache-only on core.** Permits fork-and-close-source competitors.
  For a project entering an established commercial market, the copyleft
  protection on the core matters.
- **Full monorepo with proprietary subdirectory.** GitHub-supported via
  private monorepo but: (a) makes auto-detect license confused, (b) couples
  CI for public + private code paths, (c) every PR potentially exposes
  private code in CI logs.
- **Dynamic plugin loader from day one.** More flexible but the first
  commercial requirement is for binary-distributable plugins, which is
  hot-path territory (NDI/SDI/codec providers) — and those are static-link
  forever per the transport classification. Premature.

## References

- Working plan: `~/.claude/plans/i-would-like-to-foamy-shannon.md`
- License files: `LICENSE`, `LICENSE-PLUGIN-API`, `LICENSING.md` (repo root)
- Plugin-API: `plugin-api/include/entity/plugin/`
- Reference plugin: `plugins/bus-logger/` — copy this to author a new plugin
- CMake plugin scaffolding: `cmake/EntityPlugins.cmake`, `plugins/StaticRegistry.cpp.in`
- Plugin-API boundary docs: `plugin-api/CLAUDE.md`
- Authoring flow: `plugins/README.md`
- See also ADR-0003 (Director/Renderer split) for the bus the control-plane
  plugins ride on.
