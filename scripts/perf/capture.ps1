#Requires -Version 5.1
<#
.SYNOPSIS
    Run a single perf scenario and produce a .tracy capture + summary JSON.

.DESCRIPTION
    Orchestrates: verify editor has Tracy symbols → start editor with scenario
    script → wait for Tracy server socket → start tracy-capture → guard against
    a degraded/empty capture (auto-restart + retry) → kill editor → run
    csvexport (3x) → run summarize.py → print summary table.

    Outputs under docs/perf/:
        <scenario>-<timestamp>.tracy
        <scenario>-<timestamp>-cpu.csv
        <scenario>-<timestamp>-plot.csv
        <scenario>-<timestamp>-gpu.csv
        <scenario>-<timestamp>.summary.json

.PARAMETER Scenario
    Name of the perf scenario, e.g. "stress_3layer_prores_1080p".
    Must match a file under scripts/perf/<Scenario>.json.

.PARAMETER Duration
    Capture duration passed to tracy-capture -s. Default depends on the
    scenario (per-scenario override in $ScenarioDurations below; falls back
    to 35 — 30s scenario + 5s buffer). Explicit -Duration always wins.

.EXAMPLE
    .\scripts\perf\capture.ps1 stress_3layer_prores_1080p
    .\scripts\perf\capture.ps1 stress_2layer_4k_prores -Duration 40
    .\scripts\perf\capture.ps1 stress_16screen_progressive   # uses 125s default

.NOTES
    Auto-sets ENTITY_FENCE_TIMEOUT_MS=10000 in the child editor environment
    so Tracy's initial frame-data dump doesn't trip the 2s fence timeout.
    See docs/perf/tracy-troubleshooting.md for the full diagnostic ladder.

    Degraded-capture guard: Tracy's on-demand listener serves ONE usable
    session per process lifetime. A second capture against the same editor
    process returns an empty/tiny trace (real data absent — only the
    bootstrap zones). This script detects that (the cpu export lacks the
    'endShowFrame' show-thread zone) and restarts the editor to retry, so a
    degraded capture never silently produces a hollow summary that looks
    like data. See issue #62 for the misdiagnosis this prevents.
#>
param(
    [Parameter(Mandatory)]
    [string]$Scenario,

    [int]$Duration = 0
)

# Per-scenario duration override (seconds). Used when -Duration isn't passed
# explicitly. Falls back to 35 for unlisted scenarios.
$ScenarioDurations = @{
    'stress_16screen_progressive' = 125   # 4 stages × ~30s + a buffer
    'stress_8screen_progressive'  = 65    # 4 stages × 15s + 5s settle + a buffer
}

if ($Duration -le 0) {
    if ($ScenarioDurations.ContainsKey($Scenario)) {
        $Duration = $ScenarioDurations[$Scenario]
        Write-Host "Using scenario-specific duration: ${Duration}s"
    } else {
        $Duration = 35
    }
}

# Degraded-capture guard tunables.
$MaxCaptureAttempts = 3
# A healthy show-thread zone that runs every show frame regardless of content.
# Its presence in the CPU export is the authoritative "this is a real capture"
# signal. File size is NOT a reliable signal — a lean valid capture can be
# ~0.2 MB while a degraded one can be ~0.13 MB; the zone gate distinguishes
# them where size cannot.
$ShowThreadCanaryZone = 'endShowFrame'

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Step-Error {
    param([int]$Step, [string]$Message)
    Write-Error "Step $Step failed: $Message"
    exit 1
}

function Test-PortOpen {
    param([int]$Port)
    try {
        $tcp = [System.Net.Sockets.TcpClient]::new()
        $ar  = $tcp.BeginConnect('127.0.0.1', $Port, $null, $null)
        $ok  = $ar.AsyncWaitHandle.WaitOne(200)
        if ($ok -and $tcp.Connected) { $tcp.Close(); return $true }
        $tcp.Close()
        return $false
    } catch {
        return $false
    }
}

# Start the editor under the scenario script and wait for the Tracy listener
# on $Port. Returns the process object on success, or $null (after killing the
# process) if the port never opened. Bumps ENTITY_FENCE_TIMEOUT_MS for the
# child so Tracy's initial frame-data dump doesn't false-trip the 2s default.
function Start-EditorWaitingForPort {
    param(
        [string]$EditorExe,
        [string]$RepoRoot,
        [string]$Scenario,
        [int]$Port = 8086
    )
    $prevFenceTimeout = $env:ENTITY_FENCE_TIMEOUT_MS
    $env:ENTITY_FENCE_TIMEOUT_MS = '10000'
    try {
        $proc = Start-Process -FilePath $EditorExe `
            -ArgumentList "--script", "scripts\perf\$Scenario.json" `
            -WorkingDirectory $RepoRoot `
            -PassThru
    } finally {
        if ($null -ne $prevFenceTimeout) {
            $env:ENTITY_FENCE_TIMEOUT_MS = $prevFenceTimeout
        } else {
            Remove-Item env:ENTITY_FENCE_TIMEOUT_MS -ErrorAction SilentlyContinue
        }
    }

    Write-Host "  Editor PID: $($proc.Id) - waiting for Tracy port $Port..."
    # 60 × 250ms = 15s. Heavier scenarios (16-screen progressive) need >5s to
    # spin up the listener; was previously a flake source on warm-build runs.
    for ($i = 0; $i -lt 60; $i++) {
        if (Test-PortOpen -Port $Port) { Write-Host "  Port $Port open."; return $proc }
        Start-Sleep -Milliseconds 250
    }
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    return $null
}

# ---------------------------------------------------------------------------
# Step 1 — Validate scenario
# ---------------------------------------------------------------------------

$RepoRoot    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ScriptJson  = Join-Path $RepoRoot "scripts\perf\$Scenario.json"

if (-not (Test-Path $ScriptJson)) {
    Step-Error 1 "Scenario not found: $ScriptJson`nAvailable scenarios: $(Get-ChildItem (Join-Path $RepoRoot 'scripts\perf\*.json') | Select-Object -ExpandProperty BaseName)"
}

$scenarioData = Get-Content $ScriptJson -Raw | ConvertFrom-Json
$TargetFps = if ($scenarioData.PSObject.Properties['target_fps']) { [int]$scenarioData.target_fps } else { 60 }

# ---------------------------------------------------------------------------
# Step 2 — Determine output paths
# ---------------------------------------------------------------------------

$Timestamp  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHHmmssZ')
$PerfDir    = Join-Path $RepoRoot 'docs\perf'
if (-not (Test-Path $PerfDir)) { New-Item -ItemType Directory -Path $PerfDir | Out-Null }

$Base       = Join-Path $PerfDir "$Scenario-$Timestamp"
$TracyFile  = "$Base.tracy"
$CpuCsv     = "$Base-cpu.csv"
$PlotCsv    = "$Base-plot.csv"
$GpuCsv     = "$Base-gpu.csv"
$SummaryJson= "$Base.summary.json"

Write-Host "Output base: $Base"

# ---------------------------------------------------------------------------
# Step 3 — Locate Tracy binaries
# ---------------------------------------------------------------------------

$TracyToolDirs = @(
    (Join-Path $RepoRoot 'build\vcpkg_installed\x64-windows\tools\tracy'),
    (Join-Path $RepoRoot 'tools\tracy')
)

$TracyDir = $null
foreach ($dir in $TracyToolDirs) {
    if ((Test-Path (Join-Path $dir 'tracy-capture.exe')) -and
        (Test-Path (Join-Path $dir 'tracy-csvexport.exe'))) {
        $TracyDir = $dir
        break
    }
}

if (-not $TracyDir) {
    Step-Error 3 "tracy-capture.exe / tracy-csvexport.exe not found.`nExpected locations:`n  $($TracyToolDirs -join "`n  ")`nDownload Tracy v0.13.1 binaries from https://github.com/wolfpld/tracy/releases/tag/v0.13.1 and place under tools/tracy/."
}

$CaptureExe   = Join-Path $TracyDir 'tracy-capture.exe'
$CsvExportExe = Join-Path $TracyDir 'tracy-csvexport.exe'
Write-Host "Tracy binaries: $TracyDir"

# ---------------------------------------------------------------------------
# Step 3b — Tracy version match check
# ---------------------------------------------------------------------------
#
# Tracy's wire protocol changes between minor versions. A mismatch between
# the editor's compiled-in TracyClient lib and the capture tool produces
# silent handshake failures (TCP accepts, no session). See
# docs/perf/tracy-troubleshooting.md for the canonical diagnosis.

$ExpectedTracyVersion = '0.13.1'
$VcpkgSpdxPath = Join-Path $RepoRoot 'build\vcpkg_installed\x64-windows\share\tracy\vcpkg.spdx.json'
if (Test-Path $VcpkgSpdxPath) {
    $vcpkgSpdx = Get-Content $VcpkgSpdxPath -Raw
    if ($vcpkgSpdx -match '"versionInfo":\s*"([0-9.]+)"') {
        $installedVer = $matches[1]
        if ($installedVer -ne $ExpectedTracyVersion) {
            Write-Warning "vcpkg-installed Tracy version is $installedVer, expected $ExpectedTracyVersion."
            Write-Warning "Wire-protocol mismatch is likely; capture may fail silently. See docs/perf/tracy-troubleshooting.md."
        } else {
            Write-Host "Tracy version match: $installedVer (vcpkg)"
        }
    }
} else {
    Write-Warning "vcpkg SPDX manifest not found at $VcpkgSpdxPath - skipping Tracy version check."
}

# ---------------------------------------------------------------------------
# Step 4 — Locate editor binary
# ---------------------------------------------------------------------------

$EditorExe = Join-Path $RepoRoot 'build\bin\Release\EntityMediaEditor.exe'
if (-not (Test-Path $EditorExe)) {
    Step-Error 4 "Editor binary not found: $EditorExe`nBuild first: cmake --build build --config Release -DENTITY_ENABLE_TRACY=ON"
}
Write-Host "Editor: $EditorExe"

# ---------------------------------------------------------------------------
# Step 4b — Verify the editor was actually built with Tracy symbols
# ---------------------------------------------------------------------------
#
# Fails fast if the binary has no Tracy instrumentation (e.g. a build-noprof
# binary, or ENTITY_ENABLE_TRACY=OFF). This is the reliable byte-scan check —
# NOT `strings`, which gives false negatives on MSVC binaries and is exactly
# what misdiagnosed issue #62 as a "build regression".

$VerifyScript = Join-Path $PSScriptRoot 'verify-tracy-build.ps1'
if (Test-Path $VerifyScript) {
    & $VerifyScript -EditorExe $EditorExe
    if ($LASTEXITCODE -ne 0) {
        Step-Error 4 "Editor binary has no Tracy symbols (see message above). Rebuild build/ with ENTITY_ENABLE_TRACY=ON (not build-noprof/)."
    }
} else {
    Write-Warning "verify-tracy-build.ps1 not found at $VerifyScript - skipping build-symbol preflight."
}

# ---------------------------------------------------------------------------
# Steps 5-13 — guarded by finally so the editor is always cleaned up
# ---------------------------------------------------------------------------

$editorProc     = $null
$captureProc    = $null
$EnvJson        = $null
$captureHealthy = $false

try {

# ---------------------------------------------------------------------------
# Steps 5-7 — Start editor + capture, with degraded-capture retry
# ---------------------------------------------------------------------------
#
# Each attempt uses a FRESH editor process: the Tracy on-demand listener only
# yields one usable session per process lifetime, so retrying against the same
# process can never recover. We restart the editor between attempts.

for ($attempt = 1; $attempt -le $MaxCaptureAttempts; $attempt++) {
    Write-Host "`n=== Capture attempt $attempt / $MaxCaptureAttempts ==="

    # Step 5 — start editor, wait for Tracy port 8086
    $editorProc = Start-EditorWaitingForPort -EditorExe $EditorExe -RepoRoot $RepoRoot -Scenario $Scenario
    if (-not $editorProc) {
        Write-Warning "Editor failed to open Tracy port 8086 within 15s on attempt $attempt."
        continue
    }

    # Step 6 — start tracy-capture
    Write-Host "Step 6: Starting tracy-capture (duration ${Duration}s)..."
    $captureProc = Start-Process -FilePath $CaptureExe `
        -ArgumentList "-o", $TracyFile, "-s", $Duration, "-f" `
        -WorkingDirectory $RepoRoot `
        -PassThru

    # Step 7 — wait for capture to finish
    Write-Host "  Waiting up to $($Duration + 15)s for capture to complete..."
    $captureProc.WaitForExit(($Duration + 15) * 1000) | Out-Null
    if (-not $captureProc.HasExited) {
        Stop-Process -Id $captureProc.Id -Force -ErrorAction SilentlyContinue
        Stop-Process -Id $editorProc.Id  -Force -ErrorAction SilentlyContinue
        Write-Warning "tracy-capture did not exit within the expected window on attempt $attempt; retrying."
        continue
    }
    if ($captureProc.ExitCode -ne 0) {
        Stop-Process -Id $editorProc.Id -Force -ErrorAction SilentlyContinue
        Write-Warning "tracy-capture exited with code $($captureProc.ExitCode) on attempt $attempt; retrying."
        continue
    }
    Write-Host "  Capture complete: $TracyFile"

    # The .tracy is now written; the editor is no longer needed for this attempt.
    Stop-Process -Id $editorProc.Id -Force -ErrorAction SilentlyContinue

    # Degraded-capture gate — export CPU zones (this is also Step 9's output,
    # reused below) and require the show-thread canary zone to be present.
    Write-Host "  Checking capture health (looking for '$ShowThreadCanaryZone' zone)..."
    & $CsvExportExe -u $TracyFile | Out-File -Encoding utf8 $CpuCsv
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "tracy-csvexport -u failed (exit $LASTEXITCODE) on attempt $attempt; retrying."
        continue
    }

    $tracyBytes = (Get-Item $TracyFile).Length
    $hasCanary  = Select-String -Path $CpuCsv -Pattern $ShowThreadCanaryZone -SimpleMatch -Quiet
    if ($hasCanary) {
        Write-Host ("  Capture healthy: {0:N2} MB, '{1}' zone present." -f ($tracyBytes / 1MB), $ShowThreadCanaryZone)
        $captureHealthy = $true
        break
    }

    Write-Warning ("Attempt {0} produced a DEGRADED capture ({1:N0} bytes, no '{2}' zone) - the Tracy on-demand one-session-per-process gotcha. Restarting editor." -f $attempt, $tracyBytes, $ShowThreadCanaryZone)
}

if (-not $captureHealthy) {
    Step-Error 7 "All $MaxCaptureAttempts attempts produced degraded/empty captures (no '$ShowThreadCanaryZone' show-thread zone). This is usually the Tracy on-demand 'one usable session per process lifetime' gotcha, or a runtime handshake issue. See docs/perf/tracy-troubleshooting.md."
}

# ---------------------------------------------------------------------------
# Steps 10-11 — csvexport plots + GPU (CPU export already done by the gate)
# ---------------------------------------------------------------------------

Write-Host "`nStep 10: Exporting CPU zones + plots..."
& $CsvExportExe -u -p $TracyFile | Out-File -Encoding utf8 $PlotCsv
if ($LASTEXITCODE -ne 0) { Step-Error 10 "tracy-csvexport -u -p failed (exit $LASTEXITCODE)." }

Write-Host "Step 11: Exporting GPU zones..."
& $CsvExportExe -g $TracyFile | Out-File -Encoding utf8 $GpuCsv
if ($LASTEXITCODE -ne 0) { Step-Error 11 "tracy-csvexport -g failed (exit $LASTEXITCODE)." }

# ---------------------------------------------------------------------------
# Step 12 — Collect environment fingerprint + summarize.py
# ---------------------------------------------------------------------------

Write-Host "`nStep 12: Collecting environment fingerprint..."
$cpuModel  = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
$gpuInfo   = Get-CimInstance Win32_VideoController |
                 Where-Object { $_.Name -notmatch 'Microsoft Basic|Remote Desktop|Virtual' } |
                 Select-Object -First 1
$gpuModel  = if ($gpuInfo) { $gpuInfo.Name } else { 'Unknown' }
$gpuDriver = if ($gpuInfo) { $gpuInfo.DriverVersion } else { 'Unknown' }
$ramGb     = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$osInfo    = Get-CimInstance Win32_OperatingSystem
$osBuild   = "$($osInfo.Caption) $($osInfo.BuildNumber)"
$gitSha    = (git -C $RepoRoot rev-parse --short HEAD 2>$null).Trim()
if (-not $gitSha) { $gitSha = 'unknown' }

$envData = [ordered]@{
    cpu_model    = $cpuModel
    gpu_model    = $gpuModel
    gpu_driver   = $gpuDriver
    ram_gb       = $ramGb
    os_build     = $osBuild
    git_sha      = $gitSha
    build_config = 'Release'
    tracy_enabled = $true
}

$EnvJson = "$Base-env.json"
[System.IO.File]::WriteAllText($EnvJson, ($envData | ConvertTo-Json -Depth 2), (New-Object System.Text.UTF8Encoding $false))
Write-Host "  Environment fingerprint: $EnvJson"

Write-Host "`nStep 12b: Generating summary JSON..."
$pyOutput = python (Join-Path $RepoRoot 'scripts\perf\summarize.py') `
    --cpu-csv   $CpuCsv  `
    --plot-csv  $PlotCsv `
    --gpu-csv   $GpuCsv  `
    --scenario  $Scenario `
    --tracy-file $TracyFile `
    --duration  30 `
    --env-json  $EnvJson `
    --target-fps $TargetFps
if ($LASTEXITCODE -ne 0) { Step-Error 12 "summarize.py failed (exit $LASTEXITCODE)." }

[System.IO.File]::WriteAllText($SummaryJson, ($pyOutput -join "`n"), (New-Object System.Text.UTF8Encoding $false))
Write-Host "  Summary: $SummaryJson"

# ---------------------------------------------------------------------------
# Step 13 — Print summary table
# ---------------------------------------------------------------------------

$summary = ($pyOutput -join "`n") | ConvertFrom-Json

# Belt-and-suspenders: the gate above already required the canary in the CPU
# export, but assert it survived into the summary too (catches a summarize.py
# regression that silently drops show-thread zones).
if (-not $summary.cpu_zones.PSObject.Properties[$ShowThreadCanaryZone]) {
    Step-Error 13 "Summary is missing the '$ShowThreadCanaryZone' show-thread zone despite a healthy capture - summarize.py may have dropped it. Inspect $CpuCsv and $SummaryJson."
}

Write-Host "`n=========================================="
Write-Host " Perf summary: $Scenario"
Write-Host " Captured:     $($summary.captured_at)"
Write-Host "=========================================="

# Collect all zones (CPU + GPU) into one list for ranking
$allZones = @()
foreach ($prop in $summary.cpu_zones.PSObject.Properties) {
    $z = $prop.Value
    if ($z.p95_ms -ne $null) {
        $allZones += [PSCustomObject]@{ Name = $prop.Name; p95_ms = $z.p95_ms; median_ms = $z.median_ms; Type = 'CPU' }
    }
}
foreach ($prop in $summary.gpu_zones.PSObject.Properties) {
    $z = $prop.Value
    if ($z.p95_ms -ne $null) {
        $allZones += [PSCustomObject]@{ Name = $prop.Name; p95_ms = $z.p95_ms; median_ms = $z.median_ms; Type = 'GPU' }
    }
}

$top5 = $allZones | Sort-Object p95_ms -Descending | Select-Object -First 5

Write-Host "`nTop zones by p95 (ms):"
Write-Host ("{0,-40} {1,8} {2,8} {3}" -f "Zone", "median", "p95", "type")
Write-Host ("{0,-40} {1,8} {2,8} {3}" -f "----", "------", "---", "----")
foreach ($z in $top5) {
    Write-Host ("{0,-40} {1,8:F1} {2,8:F1} {3}" -f $z.Name, $z.median_ms, $z.p95_ms, $z.Type)
}

Write-Host "`nKey plots:"
$plotNames = @('FrameCache hit rate %', 'Decode queue depth', 'FrameCache bytes used')
foreach ($name in $plotNames) {
    $p = $summary.plots.PSObject.Properties[$name]
    if ($p) {
        Write-Host ("  {0,-35} median={1,10}  p95={2,10}" -f $name, $p.Value.median, $p.Value.p95)
    }
}

Write-Host "`nFiles:"
Write-Host "  .tracy  : $TracyFile"
Write-Host "  .summary: $SummaryJson"
Write-Host ""

} finally {
    if ($null -ne $editorProc -and -not $editorProc.HasExited) {
        Stop-Process -Id $editorProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($EnvJson -and (Test-Path $EnvJson)) {
        Remove-Item $EnvJson -ErrorAction SilentlyContinue
    }
}
