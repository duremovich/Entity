# Entity Media Server — Licensing

Entity Media Server is dual-licensed. Different parts of the repository are
covered by different terms; this file is a pointer, not a license itself.

## Core engine

Everything under this repository **except** for the directories listed in the
"Permissively-licensed components" section below is governed by the GNU
General Public License, version 3, with an additional permission for plugins.

See `LICENSE` in this directory for the full text.

In short: if you modify or redistribute the core engine, the result must be
distributed under GPLv3-compatible terms. Closed-source software that uses
Entity through the documented plugin API (see "Permissively-licensed
components" below) is **not** itself a derived work of the core, by virtue of
the linking exception included in `LICENSE`.

## Permissively-licensed components

The following directories are licensed under the Apache License, Version 2.0,
and may be incorporated into closed-source plugins without triggering the
GPL's copyleft requirements on the plugin code:

- `include/entity/bus/`, `src/bus/` — the message bus (`entity-bus` static
  library). Network-serializable interface that plugins use to communicate
  with the core engine.
- `plugin-api/` — the public C++ headers a plugin compiles against
  (`entity-plugin-api` interface library). Defines `IPluginContext`, the
  plugin lifecycle entry points, and the abstract base classes for hot-path
  output drivers and codec providers.

A copy of the Apache License, Version 2.0, is available at the top level as
`LICENSE-PLUGIN-API`.

## First-party plugins

Plugins under `plugins/` are licensed under the Apache License, Version 2.0
(same as `plugin-api/` and `entity-bus`) unless an individual plugin's
`manifest.json` declares otherwise. There is no per-plugin `LICENSE` file by
default — `plugins/` inherits the terms in `LICENSE-PLUGIN-API` at the repo
root.

If a community contribution ever needs different terms, that one plugin's
folder may override by adding its own `LICENSE` file and matching its
`manifest.json` declaration. This is the rare exception, not the default.

## Proprietary plugins (Entity Pro)

Proprietary plugins shipped as part of the commercial **Entity Pro** product
live in a separate, private repository (`Entity-Pro`). They communicate with
this engine exclusively through the published plugin API and are not derived
works of the core under the linking exception. Entity Pro plugins ship under
their own commercial license, distinct from this repository.

---

For questions about commercial licensing or other licensing arrangements,
contact the project maintainer.
