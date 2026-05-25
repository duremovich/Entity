# Tracy Troubleshooting

Canonical "Tracy won't connect / Tracy is being weird" reference. Diagnosed
on 2026-05-23 by reproducing the failure modes the operator hit during the
section-break perf investigation.

If you're hitting "TCP port 8086 accepts but the handshake never
completes" — that's the **one-capture-per-process bug** (item 1 below).
Restart the editor.

---

## The big ones (likely what you're hitting)

### 1. Editor Tracy listener gives ONE usable session per process lifetime

**This is the root cause of the "handshake never completes" / "tried
multiple times, all failed" report in HISTORY.md.**

The editor's Tracy `on-demand` listener accepts a TCP connection on port
8086 the first time `tracy-capture.exe` (or Tracy.exe) connects, and
serves a clean capture. On clean disconnect the listener stays bound and
keeps accepting TCP — but every subsequent connection terminates in
~60-150 ms with exit code 0 and **no .tracy file produced**. The TCP
accept succeeds, the wire handshake doesn't.

Reproduced cleanly: fresh editor → first capture works (272 KB, 11,854
zones in 3s). Without restarting: 4 back-to-back attempts all exit in
60-155 ms with zero output. Restart the editor → first capture works
again.

**Cause:** suspected interaction between vcpkg `tracy[on-demand]` 0.13.1
and `tracy-capture.exe` 0.13.1. Not investigated upstream yet.

**Workaround: restart the editor between captures.**

- `scripts/perf/capture.ps1` already does this correctly — it launches a
  fresh editor, captures once, kills it. So **automated capture is
  unaffected by this bug**.
- **Manual users** (interactive Tracy.exe GUI sessions): close + relaunch
  the editor for each capture. There's no other workaround.

### 2. `tracy-capture.exe -a <address>` segfaults in 0.13.1

```
$ tracy-capture -o out.tracy -s 3 -f -a 127.0.0.1
Connecting to 127.0.0.1:8086.../usr/bin/bash: ... Segmentation fault
Exit: 139
```

Same with `-a ::1`. Without `-a`, the binary defaults to connecting to
`127.0.0.1:8086` and works fine. **Don't pass `-a`.** `capture.ps1` is
already correct here.

### 3. Tracy.exe (the GUI viewer) is not installed by vcpkg

vcpkg ships `tracy-capture.exe`, `tracy-csvexport.exe`, etc. via the
`cli-tools` feature. The GUI viewer **Tracy.exe** has to be downloaded
manually from the GitHub releases page:

> https://github.com/wolfpld/tracy/releases/tag/v0.13.1

**Version match matters.** Tracy's wire protocol changes between minor
versions (current: `ProtocolVersion = 76`). A Tracy.exe of any version
other than 0.13.1 will silently fail the handshake against this build.

Put it somewhere stable like `C:\Tools\Tracy-0.13.1\Tracy.exe` so the
skill can find it.

---

## Pre-flight checklist (paste-ready)

Run before every Tracy session. Most failures are one of these.

```powershell
# 1. Tracy.exe GUI present and version 0.13.1?
Test-Path "C:\Tools\Tracy-0.13.1\Tracy.exe"  # adjust path as needed

# 2. tracy-capture.exe present?
Test-Path "C:\Entity\Entity\build\vcpkg_installed\x64-windows\tools\tracy\tracy-capture.exe"

# 3. vcpkg-installed Tracy version
Get-Content "C:\Entity\Entity\build\vcpkg_installed\x64-windows\share\tracy\vcpkg.spdx.json" `
  | Select-String '"versionInfo": "[0-9.]+"'  # should match 0.13.1

# 4. Editor built with -DENTITY_ENABLE_TRACY=ON?
Get-Content "C:\Entity\Entity\build\CMakeCache.txt" `
  | Select-String "ENTITY_ENABLE_TRACY"  # should be :BOOL=ON

# 5. Port 8086 free?
Get-NetTCPConnection -LocalPort 8086 -ErrorAction SilentlyContinue
# Empty output = free. Anything else = something is squatting.

# 6. Fence timeout bumped (Tracy's initial frame dump can blow the 2s default)
$env:ENTITY_FENCE_TIMEOUT_MS = "10000"
```

---

## Diagnostic ladder for "Tracy isn't capturing"

Stop at first hit.

1. **Did the previous capture leave the editor running?** That's item 1
   above. Kill and restart the editor. (`Get-Process EntityMediaEditor |
   Stop-Process`.) This is by far the most common cause.

2. **Is `Tracy.exe`/`tracy-capture.exe` version 0.13.1 exactly?** Item 3
   above. Mismatches fail the handshake silently.

3. **Are you passing `-a` to `tracy-capture`?** Don't. Item 2.

4. **Is the editor actually listening?** `on-demand` only opens the
   listener after the first profiled frame runs. If the editor crashed or
   is sitting in a modal dialog before the first frame, no listener.
   Check:
   ```powershell
   Get-NetTCPConnection -LocalPort 8086 -State Listen
   ```
   Expected: one entry with `LocalAddress: ::` (dual-stack IPv6 wildcard,
   serves IPv4 too on Windows) and `OwningProcess` = editor PID.

5. **Is something else holding port 8086?** `Get-NetTCPConnection
   -LocalPort 8086` should be empty before launch. Common offenders:
   InfluxDB, some Steam overlays.

6. **Did the editor trip the fence timeout?** Default
   `ENTITY_FENCE_TIMEOUT_MS=2000` can fire during Tracy's first-frame
   dump on heavy scenarios (4K H.264 multi-output). Set
   `ENTITY_FENCE_TIMEOUT_MS=10000` before launching.

7. **Firewall / antivirus loopback interference.** Rare on a dev box,
   but rule out:
   ```powershell
   # Add an explicit loopback allow rule
   New-NetFirewallRule -DisplayName "Tracy loopback 8086" `
     -Direction Inbound -LocalPort 8086 -Protocol TCP -Action Allow
   ```
   Disable AV temporarily as a final ruleout.

---

## Fallback: stdout structured logging (when Tracy is unusable)

If you really can't get Tracy attached and need diagnostic signal,
several systems emit structured stdout you can grep against a wall
clock:

| Surface | Set | Useful for |
|---|---|---|
| `ENTITY_DECODE_VERBOSE=1` | env var before launch | `Seek: X → Y`, `[DECODE PACE]` from DecodeSystem |
| `[SectionDetect]` | always on | Show-thread break-crossing detection (NEW-08) |
| `[SectionScheduler]` | always on | Editor-side apply, seed, continuation advance |
| `TextureUploader: created slot N` | always on | Lazy texture allocations — visible during cold start |
| `[Command] Executing: ...` | always on | Command dispatch trace |
| `[AddScreen]`, `[Compositor]`, etc. | always on | Subsystem-tagged lifecycle events |

Not a substitute for Tracy's per-frame timing, but pinpoint enough for
"what happened at the section break" type questions. The Section-Break
Fixes 1–10 in HISTORY.md were diagnosed this way when Tracy was
unavailable.

---

## Reference: instrumentation surfaces

Full list of zones, plots, and threads is in
[`docs/perf/zone-cookbook.md`](zone-cookbook.md), keyed by problem
class. Architecture rationale in
[`docs/adr/0015-profiling-with-tracy.md`](../adr/0015-profiling-with-tracy.md).
