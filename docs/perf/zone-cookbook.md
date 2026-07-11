# Tracy Zone Cookbook

Which zones and plots to look at for which class of problem. Pair with
`docs/perf/tracy-troubleshooting.md` (for getting Tracy connected) and
`docs/adr/0015-profiling-with-tracy.md` (for the architecture rationale).

The instrumentation map below mirrors what's in the editor as of
2026-07-11 (originally 2026-05-23; `SignalOutputSystem` added with
ADR-0027). If you add a new system, append it to the relevant table —
the tables are a curated map, not an exhaustive `ZoneScopedN` inventory.

---

## Layout: how the timeline is organized

Two named frame contexts, drawn as separate lanes in Tracy:

- **`Editor`** — `Engine::run` loop. Vsync-bound to whichever display the
  editor window is on (typically 60 Hz). Several thousand fps in
  `--headless --script` mode.
- **`Show`** — `Engine::showThreadMain` loop. Vsync-bound to the primary
  output. Independent of editor health (ADR-0014).

Threads (named at startup, will show in Tracy's thread list):

| Thread | Owner | What runs |
|---|---|---|
| `Editor` | main | systems tick, ImGui, command dispatch |
| `Show` | dedicated | compositor, present, fallback ticks |
| `Decode #<entity>` | per clip | FFmpeg decode → FrameRingBuffer |
| `AudioDecode` | per clip | audio decode → ring |
| `AudioRender` | WASAPI | audio mix → device |
| `AudioLoopback` | headless | capture for assertions |
| `ContentScanner` | engine | filesystem watcher |
| `MediaProbe` | engine | media metadata probe |
| `Transcode` | engine | transcode-on-import |
| `OSC` | plugin | inbound OSC dispatch |

---

## "Output stutters during X"

Most common report. The visible glitch is a show-thread frame that
missed vsync.

Open: `Show` frame context → look for any `FrameMarkNamed("Show")`
interval >16.7 ms (60 Hz budget). Click it to open the GPU + CPU stack.

| Check | Tracy surface | Threshold / what to read |
|---|---|---|
| Frame ms inflated | plot `Show frame ms` | spikes >16.7 ms |
| GPU bound | GPU zone `beginShowFrame → endShowFrame` p95 vs CPU `Show iter` p95 | if GPU > CPU it's the renderer |
| LRU cache thrash | plot `Cache-miss recoveries / tick` | sustained ≥1/tick is the thrash signal |
| Decode lag | plot `Decode queue depth` | sustained >0 means workers are behind |
| Compositor blowup | zones `Compositor::pass1_generative`, `pass1_5_effects`, `pass2_screens` | dominant pass = where to look |
| Output bandwidth | zone `OutputManager::renderOutputs` | grows with output count + resolution |
| Texture upload cost | zone `PP::upload_record` | per-upload memcpy + D3D12 map cost; spikes when a new clip first hits the show thread |
| Cache lookup cost | zone `PP::cache_lookup` | LRU lock contention under 8+ active clips; should be <0.1 ms each |
| Upload count spike | plot `Uploads / show frame` | normal = active-clip count; spike above that = nearest-fallback storm (workers behind) |

If the spike is at a known event (section break, GO, scrub) jump to the
relevant section below.

## "Editor stalls during X"

User reports the UI freezes for hundreds of ms. Project load, drag-resize,
modal dialogs.

Open: `Editor` frame context → find the giant `FrameMarkNamed("Editor")`
interval. The dominant child zone tells you which subsystem.

| Suspect | Zone | Notes |
|---|---|---|
| Snapshot bake | `buildSceneSnapshot` | grows with active-clip count |
| Animation eval | `AnimationSystem` | grows with keyframe count per active clip |
| Decode steering | `DecodeSystem::update` | grows with prefetch sweep size |
| Section state machine | `SectionScheduler::tick` + `handleBreakAt` | spikes at break crossings |
| Routing reconciliation | `RoutingLibrarySystem::reconcile` | grows with Screen + Asset count |
| Filesystem deltas | `ContentScanner::drainDeltas` | spikes on large project loads |
| ImGui itself | gaps between Tracy zones | usually the resize / docking branch |

Reminder: editor stalls **do not** stop the show thread anymore
(ADR-0014). If the output also froze, look at the systems with
show-thread fallbacks (Timeline, Decode, Audio, SectionDetect) — they
should keep ticking, and animation re-evaluates from the baked snapshot
show-side. Signal output (`SignalOutputSystem`) is show-native and
unaffected by editor stalls. CODE_ISSUES.md NEW-07 / NEW-08 are closed.

## "Cache thrash"

The dynamic the perf history calls out repeatedly: two clips' working
sets together exceed `frameCacheBytes`, each evicts the other, decoders
re-seek constantly.

| Signal | Tracy surface | Target |
|---|---|---|
| Hit rate falling | plot `FrameCache hit rate %` | >90% steady state |
| Recovery seeks firing | plot `Cache-miss recoveries / tick` | <1/tick |
| Cache size | plot `FrameCache bytes used` | should plateau near `frameCacheBytes` setting |
| Working-set sum | plot `FrameCache entries` | proxy for # of cached frames |
| Per-clip seeks | `Decode` zone density per thread | back-to-back `Decode` zones with no idle = clip is re-decoding |

Fix is usually `frameCacheBytes` — default is 3 GiB since the Phase 3b
perf sweep (2026-05-24; history: 512 MiB → 2 GiB in Fix 10 → 3 GiB in
Phase 3b; the sweep evidence lives in the comment block on
`Settings::frameCacheBytes`, `include/entity/core/Settings.hpp`). On
laptops with <16 GB RAM, may need to drop. Per HISTORY.md: ~33 MB per
4K RGBA frame × 9 (current + DECODE_AHEAD_FRAMES) = ~297 MB per active
4K clip.

## "Section break glitch"

Hitch when the playhead crosses a section break. NEW-08 split the
detector to the show thread (snaps the playhead immediately) and the
apply to the editor thread (mutates the registry).

| Zone | Where | What it tells you |
|---|---|---|
| `SectionDetect` | Show thread | break-crossing detected this frame (look here for delay between break time and detection) |
| `SignalOutputSystem` | Show thread | signal-layer evaluation + emit posts (ADR-0027); at-break suppresses momentary fires without consuming the arm |
| `SectionScheduler::handleBreakAt` | Editor thread | editor applied the crossing — registry mutation cost |
| `SectionScheduler::seedContinuationAt` | Editor thread | Loop/PingPong clips set up for continuation |
| `SectionScheduler::go` | Editor thread | Section GO command fired |
| `SectionScheduler::advanceContinuation` | Editor thread | continuation phase tick — should be fast |
| Plot `Decode queue depth` | global | spikes here mean decoders couldn't keep up across the boundary |
| Plot `Cache-miss recoveries / tick` | global | spikes here mean the section-fade window evicted the working set |

Compare two adjacent frames: the one immediately before the break and
the one with `SectionDetect`. If the gap between break time and detect
is more than one frame, the show thread itself is dropping frames
upstream of detection — look at `Show frame ms` plot.

Live verification of the section-break pipeline is documented in
`docs/perf/capture-section-break-stutter.md`.

## "GPU vs CPU bound"

Quick yes/no.

- **CPU bound:** `Show iter` p95 (CPU zone) ≥ GPU zone p95. Editor frame
  time also probably elevated.
- **GPU bound:** GPU zone p95 ≥ `Show iter` p95. CPU `Show iter` looks
  flat-ish, GPU column dominates.
- **Vsync bound (everything is fine):** both well under 16.7 ms, spikes
  are exactly at multiples of the refresh interval = waiting for the
  next vsync.

Cross-reference with `Editor frame ms` plot for whether the editor is
also blocked vs idle.

## "Audio glitches / desync"

| Surface | What to read |
|---|---|
| `AudioSystem` zone | per-tick audio steering cost |
| `AudioMix` zone (in `AudioRender` thread) | mixer cost — should be tiny |
| `AudioDecode` thread density | similar shape to video Decode threads |
| Reference: ADR-0026 | seek-sync preroll gate keeps video + audio aligned post-seek |

For sub-millisecond desync investigations, get FFmpeg PTS via the
verbose decode logs (`ENTITY_DECODE_VERBOSE=1`) — Tracy timing alone
won't disambiguate decoder PTS from frame-cursor bookkeeping. See
`feedback_debug_measure_ground_truth` for why this matters.

## "Texture upload stutter"

Usually shows as a single multi-ms spike on `Show` when a fresh clip's
texture first hits the renderer.

| Surface | Notes |
|---|---|
| `PlaybackPresenter::present` zone width | spikes = a texture allocation just happened |
| stdout `TextureUploader: created slot N` | confirms a fresh allocation |
| Plot `FrameCache bytes used` step | confirms the new clip joined the working set |

Fix 9 (eager video texture slot allocation at project load) moves most
of these off the show-thread hot path; if you still see one, the
allocation either happened lazily (project still loading) or the slot
got destroyed and recreated.

---

## When the plots are flat (no signal)

If Tracy connects but the plots and zones look empty:

- The `Engine::update` zone is intentionally near-zero — most of its
  work happens in sub-zones. Look at the children.
- The `DecodeSystem::update` zone is also near-zero when no clips are
  active or all decoders are caught up.
- `FrameCache bytes used` is zero until clips are placed AND playing.
- `Cache-miss recoveries / tick` should be zero in normal operation —
  non-zero is the bug signal, not a missing-data signal.

If the named frame contexts (`Editor`, `Show`) themselves don't show,
the editor didn't actually start its loops. Check stdout for
"Starting main loop (show thread spawned)..." — if absent, the script
hit a hard error before reaching it.

---

## Adding a new zone or plot

Per the "Tracy instrumentation checklist for new systems" (originally in
the maintainer-local `CLAUDE.md`; steps reproduced here in full):

1. `tracy::SetThreadName("Name")` at thread startup
2. `ZoneScopedN("SystemName")` at the top of per-frame `update()`/`tick()`
3. Declare hot-path mutexes `TracyLockable(std::mutex, m_mutex)`,
   lock with `std::lock_guard<LockableBase(std::mutex)>`
4. `[[maybe_unused]]` on variables consumed only by `TracyPlot`

And: add the new zone/plot to this cookbook under the relevant problem
class so the next person can find it.
