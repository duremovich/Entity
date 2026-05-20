# Contributing to Entity Media Server

Thanks for taking the time to look at Entity. This document covers what kinds
of contributions are welcome and how the public/private boundary works in this
project.

## Project structure

Entity is an **open-core** project. The engine itself (this repository) is
GPLv3 with a linking exception for plugins. Commercial extensions live in a
separate, private repository (`Entity-Pro`).

What this means for contributors:

| Area                                               | Contributions accepted? |
| -------------------------------------------------- | ----------------------- |
| Core engine (everything outside `plugin-api/` and  | **Yes**, GPLv3 + CLA    |
| `plugins/`)                                        |                         |
| `entity-bus` library (`include/entity/bus/`,       | **Yes**, Apache 2.0     |
| `src/bus/`)                                        |                         |
| `plugin-api/` headers                              | **Yes**, Apache 2.0     |
| `plugins/<name>/` first-party plugins              | **Yes**, Apache 2.0     |
| Entity Pro plugins                                 | Closed; not in this     |
|                                                    | repository              |

## Contributor License Agreement

This is a solo project today. Once outside contributions arrive we will
require a CLA for changes to the core engine, so the maintainer retains the
right to relicense for commercial bundling. For now, opening a pull request
is taken as agreement to license your contribution under the file's existing
terms.

If a real CLA process needs to be put in place, it'll be linked from this
file before the first outside merge.

## What "core" can include

The core engine is GPLv3. New features for the core can freely depend on:

- Anything already in the codebase
- Standard C++20
- Existing third-party dependencies (EnTT, FFmpeg, GLFW, ImGui, OCIO, …)
- New permissively-licensed dependencies (MIT / BSD / Apache / MPL2 OK; AGPL
  not OK without explicit discussion)

The core is **not** a stable API. Internal types like `entt::registry`,
`Director*`, `Renderer*`, and the per-tick `RenderFrame` structure can change
between versions.

## What "plugin-api" can include

`plugin-api/` headers are the **only** public C++ surface that closed-source
plugins compile against. Every header here is Apache 2.0 and must remain so.

Strict rules:

1. No core internals: no `<entt/entt.hpp>`, no `<d3d12.h>`, no
   `#include "entity/core/..."` (or `render/`, `timeline/`, `director/`,
   `renderer/`).
2. No GPL-licensed third-party dependencies pulled in transitively. (Apache /
   MIT / BSD / MPL only.)
3. ABI-stability isn't required across major versions, but fields cannot be
   reordered or removed within a version. Use `PLUGIN_API_VERSION` to gate
   incompatible changes.

If the change breaks the rule, it doesn't belong in `plugin-api/`.

## What `plugins/` can include

Each subdirectory of `plugins/` is a self-contained plugin. To add one:

1. Create `plugins/<name>/` with `CMakeLists.txt`, `manifest.json`, and your
   sources. No per-plugin `LICENSE` file — first-party plugins inherit
   Apache 2.0 from the repo root.
2. Link `entity-plugin-api` (and `entity-bus` if your plugin sends/receives
   bus messages).
3. Anything in `plugins/` may use core internals *only* through the
   documented plugin API surface (`plugin-api/` headers and `entity-bus`).
4. Call `entity_register_plugin(<target> <register-fn-name>)` from your
   plugin's CMakeLists so the static dispatcher picks it up.

See `plugins/bus-logger/` for the canonical example.

If a contribution ever needs different license terms (uncommon — the
expectation is Apache 2.0 across the public plugin tree), the plugin's
`manifest.json` declares the alternate license and the folder includes its
own `LICENSE` file. That's the override path, not the default.

## What does *not* belong in this repository

- Closed-source code (lives in the private `Entity-Pro` repo).
- Vendor SDKs that can't be redistributed.
- Anything that would create a GPL/closed-source ambiguity for downstream
  plugin authors.

## Style

See `docs/` for code style, architecture rules, and the
ECS/data-oriented invariants the core enforces. Plugin code is held to the
same C++ style; ECS rules don't apply since plugins shouldn't see the
registry.
