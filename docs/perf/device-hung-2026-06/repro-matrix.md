# DEVICE_HUNG Repro Matrix — June 2026 Investigation

Controlled reproduction of the IIWY GPU faults (nvlddmkm Event 153 / `DXGI_ERROR_DEVICE_HUNG`)
under DRED instrumentation. Part of the `entity-gpu-device-hung-investigation` plan.

**Environment for all runs** (unless noted):

- Build: HEAD (`dfc9528` + uncommitted Phase 1 instrumentation: `ENTITY_FORCE_DRED` release gate, named GPU queues/command lists)
- `ENTITY_FORCE_DRED=1`
- `ENTITY_FENCE_TIMEOUT_MS=8000` (let the driver TDR first instead of our 2 s watchdog)
- Project: `D:\EnityTests\IIWY\IIWY\IIWY.entity` (7 compose targets, 4095×1920 ProRes 4444 alpha)
- Rig: RTX 5090, driver 32.0.15.9174, physical console, editor window unoccluded

**Oracle:** a run "faulted" iff a new nvlddmkm 153 System-log event AND a new crash-log dir
(`%APPDATA%\Entity\crash-logs\`) appeared during the run window.

## Pre-instrumentation fault timeline, 2026-06-11 (primary source: System event log)

All times local (rig timezone, UTC-4). Queried via
`Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'; Id=153}`
on 2026-06-11; 31 events total for the day. The rig returned to the physical console on
2026-06-11 after weeks of remote-desktop operation (occluded/throttled Present — see
`feedback_no_perf_capture_over_remote_desktop`); no nvlddmkm 153 events are recorded on
prior days.

| Cluster | nvlddmkm 153 events | Attribution |
|---|---|---|
| 05:36:15, 05:37:44 | 2 | **Unattributed** — predates the workday; no crash-log dir. Unknown whether the console hookup was already in place. Recorded for honesty; does not change the analysis. |
| 12:19:56 – 12:22:30 | 10 | Pre-plan script repro session (master + fixes-branch runs). Pre-instrumentation build: 2 s watchdog, no DRED. |
| 12:37:52 – 12:43:03 | 13 | The plan's "delete_paused 3/3 (both branches) + just_load 1/2" cluster. Same pre-instrumentation build. **These are the runs the run-6/9 notes call "historical 3/3" / "historical 1/2"** — same day, post-console-return, hours before our instrumented matrix. No surviving crash-log dirs for these runs (operator crash dirs from 13:11 are the earliest retained). |
| 13:11:34 | 1 | Operator crash (delete 2 clips during playback → DEVICE_HUNG at Present). Crash-log pair `2026-06-11T17-11-00Z` + `17-11-37Z` — note it is a **pair, 37 s apart**, matching the dual-handleDeviceLost pattern we later reproduced. |
| 14:14:08 | 1 | Matrix run 1 (instrumented: DRED + 8000 ms watchdog). |
| 14:51:32 | 1 | Matrix run 2. |
| 15:15:27 | 1 | Matrix run 11. (Run 3's CPU AV at ~15:10 produced crash dirs but no TDR event, as expected.) |

Key fact for the diagnosis: **every attributed fault — including everything called
"historical" elsewhere in this doc — occurred on 2026-06-11, after the rig returned to
the physical console.** "Historical" in the run-6/9 notes means *earlier the same day,
on the pre-instrumentation build (2 s watchdog, no DRED)*, not prior days/weeks. The
repro-rate drop between the 12:38 cluster and our instrumented runs co-occurs with
enabling DRED + raising the watchdog to 8000 ms.

## Run table

| # | Script | Exit | Faulted | Signature | Crash-log dir(s) | Notes |
|---|--------|------|---------|-----------|------------------|-------|
| 1 | delete_during_play_long | killed (wedged) | **Y** | DEVICE_HUNG at Present, PageFaultVA != 0 | `2026-06-11T18-14-07Z` (incomplete), `2026-06-11T18-14-08Z` | Faulted during **project load** (~1 s after start, before Play). App continued degraded, completed script, then hung forever in shutdown `waitForGpu()` (D3D12Renderer.cpp:314) — killed after ~30 min. See "Run 1 wedge" below. |
| 2 | delete_during_play_long | killed (wedged, 180 s guard) | **Y** | DEVICE_HUNG at Present, **use-after-free page fault** | `2026-06-11T18-51-31Z` (incomplete), `2026-06-11T18-51-32Z` | Enriched formatter (Phase 2a). Same PageFaultVA as run 1 (`0x141f4000`). `RecentFreed=<unnamed> type=RESOURCE`, no Existing node. Hung node: EditorCmdList/DirectQueue at op 15/26, all DRAWINDEXEDINSTANCED. Crash-dir pair + wedge pattern repeated exactly. |
| 3 | delete_during_play_long | exited (SEH) | **N** (per GPU oracle) — **CPU AV** | none (no TDR) | `2026-06-11T19-10-50Z` (seh 0xC0000005), `2026-06-11T19-11-01Z` (device-lost, fence timeout) | **New signature #4: CPU access violation.** SEH crash at 19:10:50; 11 s later show-fence wait timeout reported device-lost with hresult null + empty breadcrumbs (device never actually removed — the crashed/stalled show pipeline stopped signaling fences). Both 62 MB minidumps complete — SEH dump is a Phase 3 lead. Phase 2b named-resources build. |
| 4 | delete_paused | exited clean | N | clean | — | |
| 5 | delete_paused | exited clean | N | clean | — | |
| 6 | delete_paused | exited clean | N | clean | — | 0/3 vs historical 3/3 (2 s watchdog, no DRED): DRED serialization perturbs this repro. |
| 7 | just_load | exited clean | N | clean | — | |
| 8 | just_load | exited clean | N | clean | — | |
| 9 | just_load | exited clean | N | clean | — | 0/3 vs historical 1/2. |
| 10 | delete_during_play_long (extra) | exited clean | N | clean | — | Fishing for a named RecentFreed. |
| 11 | delete_during_play_long (extra) | killed (wedged, 180 s guard) | **Y** | DEVICE_HUNG at Present, use-after-free page fault | `2026-06-11T19-15-25Z` (incomplete), `2026-06-11T19-15-27Z` | **Named-resources build, fault reproduced — `RecentFreed=` STILL `<unnamed>`.** PageFaultVA=`0x141f5000` (one page from runs 1-2). Breadcrumb shape byte-for-byte identical to run 2: EditorCmdList 16/26 mid-DRAWINDEXEDINSTANCED. Exclusion result — see reading below. |

(Matrix paused after run 1 to enrich `formatDredBreadcrumbs` — run 1 showed the formatter
prints list-level progress but not op names, queue names (wide-name lookup missing), or
DRED allocation nodes. Remaining 8 runs will use the enriched formatter. Run 1 stands as
recorded with the v1 blob below.)

## Breadcrumb evidence

### Run 1 — `2026-06-11T18-14-08Z\context.json` (verbatim)

```json
{
  "timestamp": "2026-06-11T18-14-08Z",
  "kind": "device-lost",
  "exceptionCode": null,
  "hresult": "0x887A0006",
  "osVersion": "Windows 10.0.26200",
  "buildCommit": "May 23 2026 21:53:30",
  "projectPath": "D:/EnityTests/IIWY/IIWY/IIWY.entity",
  "adapterDescription": "NVIDIA GeForce RTX 5090",
  "vendorId": 0x10DE,
  "deviceId": 0x2B85,
  "dredBreadcrumbs": "[Node 0] CmdList=Internal DXGI CommandList LastOp=1/1\n[Node 1] CmdList=ShowCmdList LastOp=28/28\n[Node 2] CmdList=Internal DXGI CommandList LastOp=1/1\n[Node 3] CmdList=Internal DXGI CommandList LastOp=0/1\n[Node 4] CmdList=EditorCmdList LastOp=0/26\n[Node 5] CmdList=Internal DXGI CommandList LastOp=0/1\n[Node 6] CmdList=EditorCmdList LastOp=16/26\n[Node 7] CmdList=Internal DXGI CommandList LastOp=0/1\n[Node 8] CmdList=ShowCmdList LastOp=0/30\nPageFaultVA=0x141f4000\n",
  "extra": "Present"
}
```

`2026-06-11T18-14-07Z` contains a 0-byte `context.json` and 0-byte `dump.dmp` — the thread
writing it deadlocked (see wedge note) and the files were still open ~30 min later.

### Run 1 reading

DRED capture in Release works (first non-null `dredBreadcrumbs` of the investigation).
The fault is a **GPU page fault** (`PageFaultVA=0x141f4000`), not a runaway workload —
this supports a buffer-lifetime or out-of-bounds-read mechanism over a "workload too big"
one. Three lists were in flight and unfinished when the device died: `EditorCmdList` at
op 16/26 (the most informative partial node), a second `EditorCmdList` instance at 0/26,
and `ShowCmdList` at 0/30; a completed `ShowCmdList` node (28/28) from the prior frame is
a red herring. The fault fired ~1 s into **project load** — before any Play or Delete —
which independently confirms `just_load`-class repros: first-frame decode burst alone can
trigger it. Op-level identification (is op 16 a CopyTextureRegion?) and the faulting
resource's identity await the enriched formatter (Phase 2a) on the remaining runs.

### Run 2 — `2026-06-11T18-51-32Z\context.json` dredBreadcrumbs (verbatim, enriched formatter)

```
[Node 0] CmdList=Internal DXGI CommandList Queue=DirectQueue LastOp=0/1
  ops: op[0]=BEGIN_COMMAND_LIST
[Node 1] CmdList=EditorCmdList Queue=DirectQueue LastOp=0/26
  ops: op[0]=BEGIN_COMMAND_LIST op[1]=RESOURCEBARRIER op[2]=CLEARRENDERTARGETVIEW op[3]=DRAWINDEXEDINSTANCED
[Node 2] CmdList=Internal DXGI CommandList Queue=DirectQueue LastOp=0/1
  ops: op[0]=BEGIN_COMMAND_LIST
[Node 3] CmdList=EditorCmdList Queue=DirectQueue LastOp=16/26
  ops: op[12]=DRAWINDEXEDINSTANCED op[13]=DRAWINDEXEDINSTANCED op[14]=DRAWINDEXEDINSTANCED op[15]=DRAWINDEXEDINSTANCED<--last confirmed op[16]=DRAWINDEXEDINSTANCED op[17]=DRAWINDEXEDINSTANCED op[18]=DRAWINDEXEDINSTANCED
[Node 4] CmdList=Internal DXGI CommandList Queue=DirectQueue LastOp=0/1
  ops: op[0]=BEGIN_COMMAND_LIST
[Node 5] CmdList=ShowCmdList Queue=DirectQueue LastOp=0/30
  ops: op[0]=BEGIN_COMMAND_LIST op[1]=RESOURCEBARRIER op[2]=CLEARRENDERTARGETVIEW op[3]=DRAWINSTANCED
[Node 6] CmdList=Internal DXGI CommandList Queue=DirectQueue LastOp=0/1
  ops: op[0]=BEGIN_COMMAND_LIST
[Node 7] CmdList=ShowCmdList Queue=DirectQueue LastOp=0/30
  ops: op[0]=BEGIN_COMMAND_LIST op[1]=RESOURCEBARRIER op[2]=CLEARRENDERTARGETVIEW op[3]=DRAWINSTANCED
[Node 8] CmdList=Internal DXGI CommandList Queue=DirectQueue LastOp=0/1
  ops: op[0]=BEGIN_COMMAND_LIST
PageFaultVA=0x141f4000
RecentFreed=<unnamed> type=RESOURCE
```

(hresult `0x887A0006` DEVICE_HUNG, `"extra": "Present"` — same as run 1.)

### Run 2 reading

**Use-after-free confirmed.** The page-fault VA lies inside a *recently freed* resource
(`RecentFreed=` node present, `Existing=` absent): the GPU read memory belonging to a
resource that had already been destroyed. The same VA in both runs (`0x141f4000`) means
the same allocation slot is recycled and faulted deterministically. The hung operation is
a **draw** (`DRAWINDEXEDINSTANCED`, EditorCmdList op 15/26 confirmed), not a copy — the
freed resource is something a draw references through descriptors (texture-class), during
project load's first-frame burst. All in-flight nodes are on `DirectQueue`; no copy-queue
work was outstanding at fault time. The freed resource is `<unnamed>` — Phase 2b adds
SetName to pooled resources (video texture slots, upload pool buffers, compose targets,
swap-chain buffers) so remaining runs name the guilty pool directly.

### Run 11 — `2026-06-11T19-15-27Z` (named-resources build) — the exclusion result

Breadcrumbs identical in shape to run 2 (EditorCmdList/DirectQueue hung at op 16/26, ops
window all DRAWINDEXEDINSTANCED after RESOURCEBARRIER + CLEARRENDERTARGETVIEW;
ShowCmdList instances at 0/30; DEVICE_HUNG at Present). `PageFaultVA=0x141f5000`,
`RecentFreed=<unnamed> type=RESOURCE`, no Existing node.

**The freed resource is still unnamed after Phase 2b named all 26
`CreateCommittedResource` sites in `src/render` (and verified src/media, src/renderer,
src/project have none).** Therefore the faulting resource is created by code outside the
engine tree. The prime candidate is the **ImGui D3D12 backend** (`imgui_impl_dx12`,
prebuilt via vcpkg — its internals are unnamed and unnameable without patching the port):

- ImGui draws are `DrawIndexedInstanced` — the exact hung op, on `EditorCmdList`, which
  is where the editor thread records ImGui (ADR-0014).
- The op pattern (clear backbuffer, then a run of indexed draws) is the editor UI frame.
- The backend keeps per-frame-in-flight vertex/index **upload-heap buffers that it
  releases and recreates larger whenever the UI's vertex count grows** — exactly what
  happens during IIWY project load (timeline + media bin populating) and after deletes.
  A released-while-still-referenced grow event matches the use-after-free signature and
  the "same VA neighborhood every run" determinism.
- Per ADR-0014 the editor thread *records* ImGui but the **show thread Presents the
  editor swap chain** — frame-ring fencing for ImGui's per-frame resources spans two
  threads. If the editor's frame-index reuse (or ImGui's buffer growth) can run before
  the show thread's fence for that frame index has actually signaled, the backend frees
  a buffer the GPU still reads.

Phase 2 decision gate: breadcrumbs name queue/op consistently (DirectQueue,
EditorCmdList, indexed draws, use-after-free of a non-engine resource) → proceed to
Phase 3 with the audit re-steered at the editor-frame ring / ImGui buffer lifetime, with
the original two suspects (upload-heap FrameCache, premultiply pitch) demoted to
secondary checks. Run 3's SEH minidump (complete, 62 MB) should be stack-walked in
Phase 3 — a CPU-side AV in the same workload may be the same lifetime bug read from the
wrong side.

## Run 1 wedge (new, third signature)

After the device-lost was reported and the script completed, shutdown hung **forever**
(not 8 s — forever; no watchdog message ever printed) at `D3D12Renderer::shutdown`'s
`waitForGpu()` (D3D12Renderer.cpp:314). Process was killed manually after ~30 min.
Two crash-log dirs were created 1 s apart; the first (`18-14-07Z`) never finished writing
(0-byte context.json + dump.dmp, file handles still held). Working hypothesis: two threads
entered the crash-logging path near-simultaneously (editor-swap-chain `Present` site at
D3D12Renderer.cpp:743 + another site), and concurrent `MiniDumpWriteDump` deadlocked
(dbghelp is process-global and not thread-safe). The `m_deviceLost` latch
(D3D12Renderer.cpp:841) loads relaxed then stores — two threads can both pass the check in
the race window, or the second dir came from a non-renderer crash path. Needs audit in
Phase 3/5; also explains why the operator saw the app freeze rather than exit after the
13:11 crash. Matrix automation now kills any run that outlives its script by >3 min.
