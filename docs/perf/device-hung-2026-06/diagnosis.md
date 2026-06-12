# DEVICE_HUNG Diagnosis — June 2026 Investigation

The `entity-gpu-device-hung-investigation` plan, Phases 1-5. Read
[`repro-matrix.md`](repro-matrix.md) for the full run-by-run evidence; this
document is the standalone diagnosis. Fix plan:
`~/.claude/plans/entity-fix-device-hung-imgui-frame-ring.md`.

## Symptom summary

The show rig hit GPU `DXGI_ERROR_DEVICE_HUNG` faults (nvlddmkm Event 153 driver
TDRs) when loading / editing the heavy IIWY project (7 compose targets,
4095×1920 ProRes 4444 alpha) on an RTX 5090. Across the investigation the same
underlying fault showed up under **three report signatures**:

1. **`0x887A0006 DEVICE_HUNG` at Present, with a GPU page fault** (instrumented
   runs 1/2/11). The clean signature: `EditorCmdList` on `DirectQueue` hung
   mid-`DRAWINDEXEDINSTANCED`, page fault on a `RecentFreed type=RESOURCE`
   allocation node (no `Existing` node = use-after-free), `PageFaultVA` in the
   same neighborhood every run (`0x141f4000` / `0x141f5000`). Fires ~1 s into
   **project load**, before any Play/Delete.
2. **`RemovedReason: 0x0` (S_OK) at "moveToNextFrame fence wait timeout"**
   (pre-instrumentation runs). Same fault, ambiguous report — see the
   watchdog-vs-TDR race below. This is *not* a separate bug.
3. **CPU access violation `0xC0000005`** (run 3). A complete 62 MB minidump; the
   working hypothesis is the same buffer-lifetime defect observed from the CPU
   side. Not yet stack-walked (no debugger on the rig — see SEH section).

**Watchdog-vs-TDR race (why signature 2 looked different).** The
pre-instrumentation fence watchdog timed out at 2 s
(`ENTITY_FENCE_TIMEOUT_MS` default, `D3D12Renderer.cpp:63`), which is the same
order as the driver's own TDR timeout. When the GPU hangs, our
`WaitForSingleObject` can hit `WAIT_TIMEOUT` and call
`GetDeviceRemovedReason()` *before* the driver has finished declaring the device
removed — so it returns `S_OK` (`0x0`) and empty breadcrumbs. Raising the
timeout to 8000 ms for the matrix (let the driver TDR first) is exactly what
turned the ambiguous `0x0` into the clean `DEVICE_HUNG` + breadcrumbs of runs
1/2/11. The 2 s default is itself a latent diagnosability bug — see the fix
plan's follow-up.

## Evidence index

All crash-log dirs under `%APPDATA%\Entity\crash-logs\` (rig: `C:\Users\Dylan\
AppData\Roaming\Entity\crash-logs\`). Each fault wrote a *pair* of dirs — ~1 s
apart for the dual-`handleDeviceLost` GPU faults (runs 1/2/11; first dir
0-byte/wedged — see crash-logger double-entry defect below). Run 3's pair is
11 s apart (SEH crash, then the show-fence timeout reporting device-lost), and
the operator's 13:11 pair is 37 s apart.

| Run | Crash-log dir(s) | Signature | Breadcrumb highlight |
|-----|------------------|-----------|----------------------|
| 1 | `2026-06-11T18-14-07Z` (0-byte), `...18-14-08Z` | DEVICE_HUNG @ Present, `PageFaultVA=0x141f4000` | EditorCmdList 16/26; v1 formatter (no op/queue names) |
| 2 | `...18-51-31Z` (0-byte), `...18-51-32Z` | DEVICE_HUNG @ Present, use-after-free | EditorCmdList 15/26 DRAWINDEXEDINSTANCED, `RecentFreed=<unnamed> type=RESOURCE`, `VA=0x141f4000` |
| 3 | `...19-10-50Z` (SEH), `...19-11-01Z` (fence-timeout) | CPU AV `0xC0000005` | 62 MB minidump; `dredBreadcrumbs: null` |
| 11 | `...19-15-25Z` (0-byte), `...19-15-27Z` | DEVICE_HUNG @ Present, use-after-free | identical shape to run 2, `RecentFreed=<unnamed>` on the **named build**, `VA=0x141f5000` |

- **Full breadcrumb blobs:** runs 1 and 2 verbatim in
  [`repro-matrix.md`](repro-matrix.md) ("Breadcrumb evidence"); run 11 shape
  documented there too.
- **nvlddmkm Event 153 timestamps:** the oracle for each "faulted" run was a new
  System-log nvlddmkm 153 event co-occurring with a new crash-log dir; see the
  matrix run table.
- **Historical (pre-instrumentation) faults, same day:** operator crash ~13:11;
  `delete_paused` 3/3 and `just_load` 1/2 script clusters 12:38-12:43 — all
  2026-06-11, all post-console-move (see timeline below).
- **Bisect table:** none. Phase 4 was **skipped** — the fault is an environment
  trigger, not a datable code regression (see "Recommendation on Phase 4").
- **ImGui-side source facts:** v1.89.7-docking
  `backends/imgui_impl_dx12.cpp`, researcher-verified (see "ImGui-side evidence
  base").

## Mechanism statement (primary)

**The GPU faults are a use-after-free of a Dear ImGui per-frame vertex/index
buffer.** The ImGui D3D12 backend (imgui 1.89.7, vcpkg prebuilt) keeps a ring
of `NumFramesInFlight` per-frame vertex/index UPLOAD-heap buffers. When the UI's
vertex count grows past a ring slot's current capacity, the backend
`SafeRelease`s that slot's old buffer and allocates a larger one **with no fence
wait of its own** — it trusts the caller to have fenced the slot's previous use.
Our editor frame ring does fence slot reuse, but it keys its fence ring on a
*different index* than ImGui keys its buffer ring. The hypothesis (see "The exact
desync" below — the desync step is an inference, not something our code or the
ImGui source documents directly) is that these two index keys can diverge under
a specific, load-correlated condition; when they diverge, ImGui frees a buffer
the GPU's previous editor frame is still reading, the recycled allocation slot is
reissued, and the next editor frame's `DrawIndexedInstanced` page-faults on it.
That matches the breadcrumb signature exactly: `EditorCmdList` on `DirectQueue`,
mid-`DRAWINDEXEDINSTANCED`, page fault on a `RecentFreed` `type=RESOURCE` node
with no `Existing` node, same VA neighborhood every run (`0x141f4000` /
`0x141f5000`).

**Implicated commit: none — this is a latent defect, not a regression.** The
ImGui-ring vs editor-fence-ring keying has plausibly existed since the
editor/show thread split (ADR-0014) introduced the current
`GetCurrentBackBufferIndex()`-keyed editor fence ring; it was masked by
remote-desktop present throttling until the rig returned to a physical console
on 2026-06-11. The trigger is the environment change, not a code change (see the
timeline under "Why each observed fact follows" and "Recommendation on Phase 4").

### Why the freed resource is `<unnamed>` (the exclusion result)

Phase 2b named all 26 `CreateCommittedResource` sites in `src/render` and
verified `src/media`, `src/renderer`, `src/project` have none. Run 11, on that
named build, still reported `RecentFreed=<unnamed>`. The faulting resource is
therefore created outside the engine tree. ImGui's backend creates its VB/IB via
its own internal `CreateCommittedResource` inside the prebuilt static lib; those
resources are never `SetName`d and cannot be named without patching the vcpkg
port. `<unnamed> type=RESOURCE` is precisely what an ImGui frame buffer looks
like in a DRED allocation node.

### The exact desync (the off-by-one)

- `ImGui_ImplDX12_Init` is called with `NumFramesInFlight = FRAME_COUNT = 2`
  (`src/render/D3D12Renderer.cpp:1920-1927`; `FRAME_COUNT` at
  `include/entity/render/D3D12Renderer.hpp:442`). The editor swap chain
  `BufferCount = FRAME_COUNT` (`D3D12Renderer.cpp:1052`) and the editor fence
  ring is depth `FRAME_COUNT` (`D3D12Renderer.cpp:96-97`, `1256-1257`). Counts
  all match — the bug is **not** a count mismatch, it is a **key** mismatch.
- ImGui's backend keys its buffer ring on a **free-running software counter**,
  not on any swap-chain index. From the v1.89.7-docking source
  (`raw.githubusercontent.com/ocornut/imgui/v1.89.7-docking/backends/imgui_impl_dx12.cpp`,
  the version vcpkg installed — see "ImGui-side evidence base" below):
  `vd->FrameIndex++;` then
  `ImGui_ImplDX12_RenderBuffers* fr = &vd->FrameRenderBuffers[vd->FrameIndex % bd->numFramesInFlight];`.
  The counter is `ViewportData`-owned, initialized to `UINT_MAX`, and advances by
  exactly 1 per `RenderDrawData` call.
- Our editor fence ring is keyed on `m_currentBackBufferIndex =
  m_swapChain->GetCurrentBackBufferIndex()`
  (`beginEditorFrame` `D3D12Renderer.cpp:685`; `moveToNextFrame` waits on
  `m_editorFenceValues[m_currentBackBufferIndex]` at `D3D12Renderer.cpp:1402-1419`).
  This key is the **swap chain's physical back-buffer index**.
- **Inference — this is the load-bearing, unproven step of the hypothesis,
  flagged as such.** These two keys stay in lockstep only while
  `GetCurrentBackBufferIndex()` flips by exactly 1 each Present, and it does not
  always: `DXGI_STATUS_OCCLUDED` / a no-flip present can leave it returning the
  same slot twice. If that happens, ImGui's `FrameIndex` still advances (flipping
  its buffer slot 0↔1) while the editor fence index does not, so
  `moveToNextFrame` would wait on the slot ImGui is *not* about to reuse and
  leave the slot ImGui *is* about to grow-and-free unfenced — one such present
  putting the two rings out of phase. **Caveat on the supporting citation:** the
  `moveToNextFrame` comment (`D3D12Renderer.cpp:1387-1392`) does establish that
  `GetCurrentBackBufferIndex()` can return the same slot after Present on this
  swap chain — but it documents that as a *fixed* deadlock w.r.t. editor **fence
  values** (the prior code bumped the wrong slot's recorded value and then waited
  on a value the GPU never reached). It is **not** evidence of a live ImGui-ring
  hazard; it only confirms the same-slot-twice condition is real. The step from
  "same-slot-twice is real" to "this desyncs the ImGui ring and frees a
  still-referenced buffer" is my reasoning, and falsification test 1 below is
  what would confirm or kill it.

### ImGui-side evidence base (v1.89.7-docking, researcher-verified)

The prebuilt vcpkg lib's internals can't be read from the binary, so these facts
come from the matching tagged source
(`raw.githubusercontent.com/ocornut/imgui/v1.89.7-docking/backends/imgui_impl_dx12.cpp`):

- **Free-running slot selection:** `vd->FrameIndex++;` then
  `&vd->FrameRenderBuffers[vd->FrameIndex % bd->numFramesInFlight]`. `FrameIndex`
  is `ViewportData`-owned and init `UINT_MAX`.
- **Immediate release-and-grow, no deferral:** `if (fr->VertexBuffer == nullptr
  || fr->VertexBufferSize < draw_data->TotalVtxCount) { SafeRelease(fr->VertexBuffer);
  fr->VertexBufferSize = draw_data->TotalVtxCount + 5000; ... CreateCommittedResource(
  ...UPLOAD...) }`. Same for `IndexBuffer` (`+10000`). The old buffer is
  `SafeRelease`d the instant the vertex count grows.
- **Zero GPU sync inside `RenderDrawData`.** `ImGui_WaitForPendingOperations`
  exists but is only called from `DestroyWindow` / `SetWindowSize`, and for the
  **main viewport** `vd->Fence` is `nullptr`, so even that path no-ops. The
  backend relies entirely on the caller having fenced the slot before reuse.
- **Font atlas eliminated as the `RecentFreed` suspect:** created once with its
  own blocking fence wait, never freed at runtime.
- **This is pre-rework code:** the DX12 backend's frame-resource sync was
  reworked upstream on 2025-09-29 (issue #8961); v1.89.7 predates that by ~2
  years and carries the fence-it-yourself contract above.

There is no `RenderDrawData`-without-`moveToNextFrame` path or vice versa
(multi-viewport is **off** — only `ImGuiConfigFlags_DockingEnable` is set,
`D3D12Renderer.cpp:1783` — so there is exactly one `RenderDrawData` per editor
frame, and every `endImGuiFrame` is followed by `endEditorFrame` →
`moveToNextFrame` in all five `Engine::render` editor-frame branches,
`Engine.cpp:2230-2341`). The desync is purely the index-key divergence above,
not a missing wait.

## Why each observed fact follows

- **Why EditorCmdList indexed draws fault on a freed non-engine resource during
  load.** Project load is the heaviest UI-growth event in the app: deserializing
  IIWY populates the timeline, media bin, and 7 compose-target panels, which
  spikes ImGui's `TotalVtxCount` and triggers the release-and-grow on a ring
  slot. The load handshake (`Engine.cpp:2229-2305`) also runs back-to-back
  overlay frames (`beginEditorFrame`/`endEditorFrame` twice in quick
  succession) right before the multi-second blocking `loadProject` call — a
  burst of presents that is exactly when a no-flip present (and thus the index
  desync) is most likely, and when the GPU is busiest (first-frame decode +
  compose). The fault fires ~1 s in, before any Play/Delete, because the
  trigger is *UI growth during deserialize*, not playback.

- **Why delete runs trigger it too.** A clip delete is another large UI mutation
  (timeline + media-bin rows disappear/reflow, property panel rebinds), so it is
  another ImGui vertex-count growth/shrink event on the same ring. The
  `delete_during_play_long` script faulted during *load* in every captured run
  (1, 2, 11) — the delete is not actually required; it just keeps the editor UI
  churning. `delete_paused` and `just_load` faulted 0/3 each under DRED, which
  is consistent with DRED's serialization perturbing the timing window
  (matrix notes for runs 6, 9), not with a different mechanism.

- **Why the show pipeline (ShowCmdList) is collateral, not cause.** All
  `ShowCmdList` breadcrumb nodes are at `0/30` — they had only reached
  `BEGIN_COMMAND_LIST` / `RESOURCEBARRIER` / `CLEARRENDERTARGETVIEW` /
  `DRAWINSTANCED` (note: `DrawInstanced`, the show path's fullscreen-triangle
  composite, **not** `DrawIndexedInstanced`). The show thread shares the
  `DirectQueue` with the editor (`endShowFrame` ExecuteCommandLists at
  `D3D12Renderer.cpp:635`; editor at `D3D12Renderer.cpp:737`). When the editor's
  indexed draw page-faults and the device hangs, every in-flight list on that
  shared queue — including the show list — is killed mid-recording. The show
  list never references the freed ImGui buffer; it is downstream of the same
  queue death. The completed `ShowCmdList 28/28` node from the prior frame is a
  red herring.

- **Why the historical "0x0 at moveToNextFrame fence wait timeout" signature is
  the same fault, reported ambiguously.** The pre-instrumentation runs used the
  2 s fence watchdog (`fenceWaitTimeoutMs()` default, `D3D12Renderer.cpp:63`) and
  reported `RemovedReason: 0x0` (`S_OK`) at the `moveToNextFrame fence wait
  timeout` site. That `0x0` is not a different (non-GPU) bug — it is the watchdog
  racing the driver TDR. The driver's own timeout is ~2 s, the same order as our
  watchdog, so when the GPU hangs our `WaitForSingleObject` can hit
  `WAIT_TIMEOUT` and call `GetDeviceRemovedReason()` *before* the driver has
  finished declaring the device removed — at which point it still returns `S_OK`.
  Same underlying page fault; the report is ambiguous only because we asked the
  driver "what happened?" a few milliseconds too early. The Phase 2
  instrumentation's 8000 ms timeout (let the driver TDR first) is exactly what
  turned this ambiguous `0x0` into the clean `0x887A0006 DEVICE_HUNG` +
  breadcrumbs of runs 1/2/11.

- **Why the faults began on 2026-06-11 (timeline — no contradiction with the
  matrix's "historical 3/3").** All of it happened on the same day, in this
  order:
  1. **Console move (first).** Per
     [[feedback_no_perf_capture_over_remote_desktop]], for weeks before this the
     rig ran over remote desktop, where the editor window is occluded/virtual and
     DXGI throttles its `Present` to ~1 fps. A throttled, occluded editor swap
     chain (a) renders the editor UI far slower, so ImGui's vertex buffers rarely
     hit the grow path during the load window, and (b) changes the present/flip
     cadence so the same-slot-twice index condition manifests differently. On
     2026-06-11 the rig returned to a physical console with the projector
     attached and the editor window unoccluded, rendering at full rate.
  2. **First faults, same day, hours later — pre-instrumentation.** Every
     "historical" fault the matrix references happened *after* the console move,
     still on 2026-06-11: the operator crash at 13:11, and the earlier
     `delete_paused` 3/3 and `just_load` 1/2 script clusters (12:38–12:43). In
     the matrix, "historical" means "earlier today, before our instrumentation"
     — those runs used the 2 s watchdog and **no DRED**. So the matrix's
     "historical 3/3" is not a contradiction with "first sighting 2026-06-11";
     both are the same day, and both are post-console-move.
  3. **Our instrumented runs, hours later still.** Runs 1–11 in the matrix added
     `ENTITY_FORCE_DRED=1` and an 8000 ms fence timeout. DRED's serialization
     plus the longer timeout perturb the repro window, which is why the
     `delete_paused` / `just_load` classes dropped to 0/3 under instrumentation
     while `delete_during_play_long` still reproduced (3 faults). Same
     mechanism, perturbed timing.

  The net of the timeline *strengthens* the environment-trigger argument: zero
  sightings across weeks of remote-desktop operation, then faults starting the
  same day physical-console rendering resumed. Heavy IIWY content (7 compose
  targets, dense timeline) correlates because it produces the biggest load-time
  UI vertex spike — the most likely growth event.

## Secondary suspects — exonerated as the cause

**A. Upload-heap FrameCache recycle worker (`b4ba66a`).** The recycle worker
(`src/render/UploadHeapBufferPool.cpp:392-458`) waits on the **copy** fence
(`m_uploadFence`, signaled from `endShowFrame` `D3D12Renderer.cpp:620-625`) for
each deferred buffer, off-thread. Every fault breadcrumb shows **no copy-queue
work in flight** (all nodes on `DirectQueue`; the `CopyQueue` is named and absent
from the in-flight set), and the faulting resource is non-engine, so this pool is
not the observed fault. **However**, its fence-timeout path is a genuine latent
crash answering the plan's original question 2: on `WAIT_TIMEOUT`/`WAIT_FAILED`
it does `delete p` (`UploadHeapBufferPool.cpp:456`, also the device-removed
`delete p` at `:425`). If the GPU is hung rather than merely slow, that `delete`
frees an UPLOAD-heap resource the GPU may still reference — a use-after-free of
the *same class* as the primary bug, on the copy path. It should be fixed
(leak-on-timeout instead of delete-on-timeout) but it is not what produced runs
1/2/11.

**B. Premultiply pitch (`973c0e8`).** The row-pitch math in
`include/entity/media/AlphaPremultiply.hpp:36-54` is **correct**: the inner loop
iterates `width` columns and indexes `out[col*4 + {0..3}]`, which stays inside
`width*4` bytes regardless of a 256-aligned `dstPitch` (8448) vs a tight
`srcPitch` — no over-read or over-write of either buffer. And it is a CPU memory
operation writing into an UPLOAD-heap buffer destined for the **copy** queue, so
it structurally cannot produce a `DirectQueue` *draw* page fault. Exonerated.

## SEH minidump (run 3, signature #4)

Run 3 produced a CPU access violation (`0xC0000005`) with a complete 62 MB
minidump at `%APPDATA%\Entity\crash-logs\2026-06-11T19-10-50Z\dump.dmp`
(context.json carries no stack — that lives in the dump). **No command-line
debugger is installed on the rig**: `Get-Command cdb` fails, and
`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\` contains only
`dbghelp.dll` / `dbgcore.dll` / `srcsrv.dll` / `symsrv.dll` — no `cdb.exe`,
`ntsd.exe`, or `windbg.exe`. Per the task rules I did not install anything. The
dump should be stack-walked in Phase 5 once a debugger is available (the
team-lead has the rig). Working hypothesis to test against the stack: the same
ImGui buffer-lifetime bug observed from the CPU side — if the editor thread
reads or maps a freed/recycled ImGui frame resource (or the desync corrupts the
backend's `FrameRenderBuffers` bookkeeping), a CPU AV in or near
`ImGui_ImplDX12_RenderDrawData` is the expected face of the same defect.

## Related defects surfaced (not the GPU fault, worth fixing)

- **Crash-logger double-entry / MiniDumpWriteDump deadlock** (matrix "Run 1
  wedge"). Every GPU-fault run produced a pair of crash-log dirs ~1 s apart, the
  first 0-byte and wedged. `handleDeviceLost` latches `m_deviceLost` with a
  relaxed load then a release store (`D3D12Renderer.cpp:841`), so two threads
  (editor `Present` site `D3D12Renderer.cpp:744` and a show `Present` /
  fence-timeout site) can both pass the check and both enter
  `MiniDumpWriteDump`. dbghelp is process-global and not thread-safe →
  concurrent dumps deadlock. The latch needs to be a single atomic
  compare-exchange gating the whole crash path.
- **Infinite shutdown hang in `waitForGpu`** (`D3D12Renderer.cpp:315`, the
  shutdown call). After a device-lost, `shutdown`'s `waitForGpu` waited forever
  (no watchdog message ever printed). `waitForGpu`'s editor/show fence drains
  use `fenceWaitTimeoutMs()` and call `handleDeviceLost` on timeout, but the
  early-out / event-handle path can still block when the device is already gone
  and fences never signal. Shutdown needs to honor `m_deviceLost` and skip the
  GPU drain.

## Confidence and falsification

**Confidence: moderate-to-high, pending falsification test 1.** What is *firmly*
established: the fault is a use-after-free page fault of a `type=RESOURCE`
created outside the engine tree, hit by `EditorCmdList` `DRAWINDEXEDINSTANCED` on
`DirectQueue` during load — tight and self-consistent across three faults (queue,
list, op, allocation-node class, VA determinism), with every engine-owned
resource excluded by Phase 2b's naming, and the editor frame path being where
ImGui records its indexed draws. The ImGui-side facts (release-and-grow with no
own GPU sync, free-running `FrameIndex` ring key) are researcher-verified against
the v1.89.7-docking source. What is **inference, not proof**: that the editor
fence ring and ImGui's `FrameIndex` ring actually desync via a no-flip present,
and that this is what frees the live buffer. That causal step is plausible and
mechanism-complete but unconfirmed — the `moveToNextFrame` comment proves only
that same-slot-twice is possible, not that it desyncs the ImGui ring. Confidence
rises to high only after falsification test 1 (naming ImGui's buffers and reading
`RecentFreed=`) directly fingers an ImGui buffer.

**What would falsify it:**
1. The decisive test — name ImGui's buffers. Patch the vcpkg imgui port (or
   build imgui from source in-tree) to `SetName(L"ImGuiVtx<slot>" /
   L"ImGuiIdx<slot>")` on the backend's frame buffers, reproduce, and read the
   `RecentFreed=` line. If it names an ImGui buffer → confirmed. If it names
   something else → falsified. (This is the recommended Phase 5 first move.)
2. Set `ImGui_ImplDX12_Init`'s `NumFramesInFlight` to `FRAME_COUNT + 1` (or
   re-key the editor fence ring on a monotonic counter that matches ImGui's),
   reproduce. If the fault stops → confirms the index-depth/desync mechanism.
3. Stack-walk run 3's SEH dump. A CPU AV unrelated to ImGui draw data would
   weaken (not kill) the single-root-cause framing and suggest a second defect.

## Recommendation on Phase 4 (bisect)

**Skip the Phase 4 regression bisect.** The bisect framing assumes a content-
pipeline regression introduced in a datable commit window. This bug is **not**
content-pipeline and likely **not** a recent regression at all. The decisive
evidence is the timeline above: zero sightings across weeks of remote-desktop
operation, then faults beginning the *same day* the rig returned to a physical
console (console move first, operator + script-cluster faults hours later, our
instrumented runs later still — all 2026-06-11). That is the fingerprint of an
**environment trigger, not a code change** — the latent ImGui index-keying
mismatch has plausibly existed since the editor/show split (ADR-0014) shipped and
was merely masked by remote-desktop present throttling. A commit bisect would
chase a regression window that does not exist; the most it could find is the
ADR-0014 split itself, which is not a "regression" in the bisectable sense. The
higher-value path is Phase 5: prove it with falsification test 1 (name the ImGui
buffers and read `RecentFreed=`) and fix it directly (re-key the editor fence
ring to a monotonic counter, or raise `NumFramesInFlight` and fence on a counter
that matches ImGui's), then mop up the two related defects (crash-logger
double-entry, shutdown `waitForGpu` hang).

## Fix implementation status (2026-06-11, evening — `entity-fix-device-hung-imgui-frame-ring`)

**Fix shipped (uncommitted, pending operator validation).** The editor fence
ring and command-allocator ring are re-keyed on a monotonic frame counter
(`m_editorFrameCounter % FRAME_COUNT` — the same arithmetic ImGui's
`FrameRenderBuffers` ring uses), with `GetCurrentBackBufferIndex()` retained
only for RTV selection. `moveToNextFrame` signals for the just-recorded slot
and waits on `(counter+1) % FRAME_COUNT`'s previous submission. One correction
to the fix plan's verbatim code was required: the high-water write must target
`m_editorFenceValues[nextSlot]` (the plan text wrote the just-rendered slot,
which deadlocks once the CPU runs two frames ahead — caught on the WARP smoke,
verified against baseline). `scheduleMeshSlotFree` was re-keyed to the same
ring slot (consumer audit — the old back-buffer key would have desynced the
deferred mesh free the same way).

**Falsification test 1 status: armed but UNRESOLVED.** The ImGui VB/IB buffers
are now named (`ImGuiVtx` / `ImGuiIdx`) via a repo-local vcpkg overlay port
(`ports/imgui/`, fail-loud guard against silent patch drop). However, the fault
did not reproduce in 11 pre-fix attempts the same evening (8 × DRED, 3 × no
DRED, same `delete_during_play_long` script that faulted 3× that afternoon) —
the repro window is timing/state sensitive and was closed. No `RecentFreed=`
name was ever read, so the desync inference remains unproven (and unfalsified).
**Decision: the buffer names stay in** — any future fault self-identifies its
freed resource class in the DRED breadcrumbs.

**Post-fix matrix: 9/9 clean** (`delete_during_play_long` / `delete_paused` /
`just_load` × 3, DRED + 8000 ms watchdog, physical console). Evidential weight
is limited by the pre-fix runs also being clean that evening; the meaningful
validation is the operator scenario (open IIWY, play, delete during playback)
and fault-free time in service.

**New latent defect found during review (pre-existing, same class):**
`scheduleMeshSlotFree` captures its deferred-free fence target as
`EFV[slot] + 1` at record time; after a mid-session `waitForGpu()` drain
(which advances the fence past the deliberately-stale `EFV[]`), that target
can already be ≤ `GetCompletedValue()` while the referencing frame is still in
flight, so the next `onFrameBegin` reap can free a GPU-live mesh buffer.
Fix folded into the companion-fix phase (lift the floor:
`max(EFV[slot], GetCompletedValue()) + 1`).
