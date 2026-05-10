# ADR-0015: Profiling with Tracy

- **Status:** Accepted
- **Date:** 2026-05-10
- **Context source:** Issue #43 (Tracy integration, 8 phases).
- **Implemented by:** Issue #43, eight phases:
  - Phase 1 — CMake option, vcpkg port, wrapper header
    (`include/entity/profile/Tracy.hpp`).
  - Phase 2 — Editor/Show frame marks, CPU zones on critical render paths.
  - Phase 3 — `TracyLockable` on all hot-path mutexes; plugin-API boundary
    review.
  - Phase 4 — `FrameCache` TracyLockable mutex + hit/miss atomic counters +
    per-show-frame plots.
  - Phase 5 — D3D12 GPU zones (`TracyD3D12Context`, cross-function
    `D3D12ZoneScope` on show GPU path, `TracyD3D12Collect` after Signal).
  - Phase 6 — Thread names (Decode workers, ContentScanner, MediaProbe,
    Transcode, OSC); decode queue depth plot.
  - Phase 7 — This ADR + documentation.
  - Phase 8 — Final verification + baseline .tracy capture.

## Context

After Phase D (Director/Renderer split, editor/show thread split) the
codebase had two independent render timelines, three thread categories, and
a multi-clip decode pipeline — enough moving parts that "it's slow" stopped
being a sufficient bug report. We needed concrete evidence: frame times per
thread, GPU busy time relative to Present, decode lag, cache pressure, mutex
contention. Guessing at bottlenecks in real-time multi-layer projection work
is expensive.

The profiler choice had to satisfy:

1. **Live capture during a running show**, not post-hoc log scraping.
2. **D3D12 GPU timeline** correlated with the CPU frame, not just CPU-side.
3. **Named frame contexts** for the editor thread and show thread — the two
   advance at different rates and frame 17 on each axis is a different thing.
4. **Zero overhead when disabled** — ship builds, plugin authors, and
   cross-platform ports must not pay for instrumentation they don't use.
5. **Mutex lock view** to surface contention between decode workers and the
   frame cache.

## Decision

Use **Tracy 0.13.1** via vcpkg, `on-demand` feature only.

### vcpkg and build integration

`vcpkg.json` requests `tracy[on-demand]`. The `gui-tools` feature is
explicitly excluded: its `usearch` dependency uses POSIX `MAP_FAILED` and
fails MSVC builds.

Tracy.exe (the capture/viewer binary) is obtained separately from the Tracy
GitHub releases page for the matching version tag. It is not vendored into
the repository.

The `ENTITY_ENABLE_TRACY` CMake option (default ON) gates the entire
dependency. When OFF, the vcpkg port is not linked and all instrumentation
compiles to nothing.

### Wrapper header

`include/entity/profile/Tracy.hpp` is the single include for all Tracy
macros across the codebase. When `ENTITY_ENABLE_TRACY` is ON (and
`ENTITY_PROFILE_DISABLED` is not defined in the translation unit), it
includes `<tracy/Tracy.hpp>` and `<tracy/TracyD3D12.hpp>`. When disabled,
it provides local stub macro definitions with zero overhead, including:

```cpp
using TracyD3D12Ctx = void*;
namespace tracy { inline void SetThreadName(const char*) {} }
```

These stubs ensure the codebase compiles when Tracy is not on the include
path at all — no `#ifdef ENTITY_ENABLE_TRACY` guard required at every call
site.

### CMake propagation and plugin-API boundary

`ENTITY_ENABLE_TRACY` and `TRACY_ENABLE` are added as `PUBLIC` compile
definitions on `EntityMediaCore`. They propagate to `EntityMediaEditor` and
test targets but **not** to `entity-plugin-api`, which is linked by plugin
authors who may not have Tracy on their include path.

The GPL wrapper header (`entity/profile/Tracy.hpp`) must never be included
from `plugin-api/include/entity/plugin/`. Plugin threads that want thread
names use `#if defined(TRACY_ENABLE) / #include <tracy/Tracy.hpp>` directly,
consistent with the Apache-2.0 boundary rules documented in
`plugin-api/CLAUDE.md`.

### Frame contexts

Two named frame contexts mark the independent render timelines:

- `FrameMarkNamed("Editor")` — at the end of each editor-thread frame in
  `Engine::run`.
- `FrameMarkNamed("Show")` — at the end of each show-thread frame in
  `Engine::showThreadMain`, after Present.

These replace a potential `FrameMark` global; the global would be
meaningless with two threads running at different rates.

### D3D12 GPU zones

A single `TracyD3D12Ctx` (`m_tracyD3D12Ctx`) lives on `D3D12Renderer`. It
is initialized in `initialize()` and destroyed in `shutdown()`.

**Show-thread only.** `TracyD3D12NewFrame` and `TracyD3D12Collect` must not
be called concurrently from two threads on the same context. The editor
thread uses CPU zones only. If editor GPU zones are needed in future, a
second `TracyD3D12Ctx` on a separate allocator/queue must be created.

**Cross-function GPU zone pattern.** `tracy::D3D12ZoneScope` is
non-default-constructible, non-copyable, and non-movable, so it cannot be
stored in a `std::optional` or moved across a function boundary. The
"Show GPU" zone spans `beginShowFrame` → `endShowFrame` across two separate
functions. The pattern used:

```cpp
// File scope, guarded:
#if defined(ENTITY_ENABLE_TRACY) && defined(TRACY_ENABLE)
static constexpr tracy::SourceLocationData kShowGpuSrcLoc{
    "Show GPU", "D3D12Renderer::showFrame", __FILE__, __LINE__, 0 };
thread_local tracy::D3D12ZoneScope* tl_showGpuZone{nullptr};
#endif

// beginShowFrame — after Reset, before recording:
#if defined(ENTITY_ENABLE_TRACY) && defined(TRACY_ENABLE)
tl_showGpuZone = new tracy::D3D12ZoneScope(m_tracyD3D12Ctx,
    m_showCmdList.Get(), &kShowGpuSrcLoc, true);
#endif

// endShowFrame — FIRST thing, before any early return:
#if defined(ENTITY_ENABLE_TRACY) && defined(TRACY_ENABLE)
delete tl_showGpuZone;
tl_showGpuZone = nullptr;
#endif
```

The single delete site at the top of `endShowFrame` — before the
`m_deviceLost` early return and before the copy-list-close failure return —
ensures the destructor fires on every code path, including device-lost
recovery. Missing the delete on an early-return path leaves
`D3D12ZoneScope`'s destructor unfired and leaks the heap allocation on every
subsequent frame in that state.

### Per-frame plots

Plots sampled once per show frame (in `Engine::showThreadMain`):

| Plot name | Source | Unit |
|-----------|--------|------|
| `FrameCache bytes used` | `FrameCache::bytesUsed()` | bytes |
| `FrameCache hit rate %` | `consumeAccessCounters()` | % (0–100) |
| `FrameCache entries` | `FrameCache::entryCount()` | count |
| `Decode queue depth` | Σ `max(0, targetFrame - currentFrame)` across initialized workers | frames |

`FrameCache::consumeAccessCounters()` atomically exchanges the internal
`m_hits` / `m_misses` counters to zero, returning the interval totals for
the plot. `[[maybe_unused]]` annotations on `total` and `hitRate` suppress
MSVC C4189 when `ENTITY_ENABLE_TRACY=OFF` compiles `TracyPlot` away.

### Thread names

All long-lived worker threads call `tracy::SetThreadName` at startup:

| Thread | Name |
|--------|------|
| Decode worker N | `"Decode #<entity uint32>"` |
| ContentScanner | `"ContentScanner"` |
| MediaProbeWorker | `"MediaProbe"` |
| TranscodeWorker | `"Transcode"` |
| OSC receiver | `"OSC"` |

The show thread name is set in `Engine::showThreadMain` immediately after
spawn. The editor/main thread name is set in `Engine::run`.

### TracyLockable

Hot-path mutexes are declared with `TracyLockable(std::mutex, m_mutex)` and
locked with `std::lock_guard<LockableBase(std::mutex)>`. This gives Tracy's
lock view timing data and deadlock detection with zero overhead when disabled.
Current coverage: `FrameCache::m_mutex`. Other mutexes are candidates for
future phases.

## Consequences

**Enables:**
- Per-frame CPU/GPU breakdown in Tracy showing editor vs. show thread
  independently.
- Decode backlog visibility: "Decode queue depth" plot shows how many frames
  behind each worker is, globally.
- Frame cache pressure visible from hit-rate plot — the primary signal for
  whether the cache budget is correctly sized.
- Mutex contention between decode workers and frame cache readers shown in
  lock view.
- Evidence-driven performance work instead of guesswork.

**Forbids / forces:**
- Any new long-lived worker thread must call `tracy::SetThreadName` at startup.
- Any new system that crosses a show-frame boundary with a zone must follow
  the heap-allocated `thread_local`  pointer pattern (or refactor to fit in
  one function).
- A second `TracyD3D12Ctx` is required if editor GPU zones are added.
- `entity/profile/Tracy.hpp` must never appear in `plugin-api/include/`.
- `ENTITY_ENABLE_TRACY=OFF` must always produce a passing build and all
  passing tests — the CI matrix includes both variants.

**Costs:**
- ~2 MB binary size increase per translation unit in Tracy-enabled builds
  (zone metadata strings).
- Capture requires a running Tracy.exe obtained separately; not vendored.
- `on-demand` mode means Tracy only captures when a viewer connects —
  zero overhead at idle but the first few frames after connection may miss
  data.

## Alternatives considered

**PIX for Windows** — excellent D3D12 GPU capture, but CPU-side is
event-based (manual begin/end markup), no automatic zone nesting, no lock
view, Windows-only, and the live capture story is weaker than Tracy. Still
useful for detailed GPU shader profiling; not a replacement.

**Custom timing infrastructure** — `std::chrono` timestamps logged to ring
buffers, rendered in the ImGui overlay. Implemented this way in early Phase A.
No cross-thread correlation, no GPU timeline, maintenance burden grows with
the codebase. Replaced by Tracy.

**Perfetto** — strong on Android and Linux. The Windows story (ETW backend)
is functional but the viewer requires Chrome or a web app, and the D3D12
integration path is less direct than Tracy's. Better fit for a multi-process
cluster deployment (Phase E+); revisit then.

**Optick** — similar feature set to Tracy, less actively maintained at the
time of this decision, no vcpkg port.

## References

- Issue #43 — Tracy integration (8 phases)
- Tracy repository: https://github.com/wolfpld/tracy (v0.13.1)
- `include/entity/profile/Tracy.hpp` — wrapper header
- `src/render/D3D12Renderer.cpp` — GPU context init/destroy, cross-function
  zone pattern
- `src/core/Engine.cpp` — frame marks, show-thread plots
- `src/systems/DecodeSystem.cpp` — decode thread names, queue-depth plot
- `src/media/FrameCache.cpp` — TracyLockable, access counters
- ADR-0014 — editor/show thread split (context for two frame contexts)
- ADR-0005 — plugin-API boundary (context for Apache-2.0 guard rule)
