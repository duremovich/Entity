#Requires -Version 5.1
<#
.SYNOPSIS
    Sweep FrameCache budget at 2 / 3 / 4 / 6 GiB and capture stress_16screen_progressive.

.DESCRIPTION
    For each budget in the sweep list:
      1. Writes %APPDATA%\Entity\settings.json with frameCacheBytes set to the target value.
      2. Runs capture.ps1 stress_16screen_progressive.
      3. Copies the produced .summary.json to a fixed name
         docs/perf/stress_16screen_progressive-<budget>GiB.summary.json
         so results are easy to compare without time-correlating timestamps.

    Restores the original settings.json (or removes it if it did not exist) at the end.

.NOTES
    Each capture is ~90s + editor startup; total wall time ~10 minutes.
    The editor is killed between captures by capture.ps1 - do not override.
    Tracy gotcha: capture.ps1 handles the single-session-per-process-lifetime
    limit by killing the editor after each capture. This script relies on that.
#>

$ErrorActionPreference = 'Stop'

$RepoRoot      = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$SettingsPath  = Join-Path $env:APPDATA 'Entity\settings.json'
$SettingsDir   = Split-Path -Parent $SettingsPath
$PerfDir       = Join-Path $RepoRoot 'docs\perf'
$CaptureScript = Join-Path $RepoRoot 'scripts\perf\capture.ps1'

# GiB values to sweep
$Budgets = @(2, 3, 4, 6)

# ---------------------------------------------------------------------------
# Save original settings so we can restore after the sweep
# ---------------------------------------------------------------------------

$originalSettingsJson = $null
if (Test-Path $SettingsPath) {
    $originalSettingsJson = Get-Content $SettingsPath -Raw
    $origKb = [math]::Round($originalSettingsJson.Length / 1024, 1)
    Write-Host ("Saved original settings.json (" + $origKb + " kibibytes)")
} else {
    Write-Host "No existing settings.json - will remove after sweep"
}

# Ensure settings dir exists
if (-not (Test-Path $SettingsDir)) {
    New-Item -ItemType Directory -Path $SettingsDir | Out-Null
}

$results = @()

try {
    foreach ($gib in $Budgets) {
        $bytes     = [uint64]$gib * 1024 * 1024 * 1024
        $bytesStr  = $bytes.ToString()

        Write-Host "`n======================================================="
        Write-Host (" Budget: " + $gib + " gibibytes (" + $bytesStr + " octets)")
        Write-Host "======================================================="

        # Write settings.json for this budget
        $settingsObj = @{ frameCacheBytes = $bytes }
        $settingsJson = $settingsObj | ConvertTo-Json -Depth 2
        [System.IO.File]::WriteAllText($SettingsPath, $settingsJson,
            (New-Object System.Text.UTF8Encoding $false))
        Write-Host ("Wrote settings.json: frameCacheBytes = " + $bytesStr)

        # Run the capture
        & powershell -NoProfile -File $CaptureScript -Scenario stress_16screen_progressive
        if ($LASTEXITCODE -ne 0) {
            Write-Error "capture.ps1 failed for ${gib} GiB (exit $LASTEXITCODE)"
        }

        # Find the newest .summary.json for this scenario
        $summaries = Get-ChildItem (Join-Path $PerfDir 'stress_16screen_progressive-*.summary.json') |
                         Sort-Object LastWriteTime -Descending
        if ($summaries.Count -eq 0) {
            Write-Error "No summary JSON found after capture for ${gib} GiB"
        }
        $newest = $summaries[0].FullName

        # Copy to fixed name
        $fixedName = Join-Path $PerfDir "stress_16screen_progressive-${gib}GiB.summary.json"
        Copy-Item $newest $fixedName -Force
        Write-Host "Copied $newest -> $fixedName"

        # Extract key metrics
        $summary = Get-Content $newest -Raw | ConvertFrom-Json
        $showIter = $summary.cpu_zones.'Show iter'
        $hitRate  = $summary.plots.'FrameCache hit rate %'
        $cacheMiss = $summary.plots.'Cache-miss recoveries / tick'

        $row = [PSCustomObject]@{
            Budget_GiB          = $gib
            HitRate_median      = if ($hitRate)   { $hitRate.median }   else { 'n/a' }
            ShowIter_p95_ms     = if ($showIter)  { $showIter.p95_ms }  else { 'n/a' }
            CacheMiss_p95       = if ($cacheMiss) { $cacheMiss.p95 }    else { 'n/a' }
        }
        $results += $row

        Write-Host ("  Show iter p95 = {0} ms   FrameCache hit rate median = {1}%   Cache-miss p95 = {2}" -f
            $row.ShowIter_p95_ms, $row.HitRate_median, $row.CacheMiss_p95)
    }
} finally {
    # Restore original settings
    if ($null -ne $originalSettingsJson) {
        [System.IO.File]::WriteAllText($SettingsPath, $originalSettingsJson,
            (New-Object System.Text.UTF8Encoding $false))
        Write-Host "`nRestored original settings.json"
    } else {
        Remove-Item $SettingsPath -ErrorAction SilentlyContinue
        Write-Host "`nRemoved settings.json (did not exist before sweep)"
    }
}

# ---------------------------------------------------------------------------
# Print results table
# ---------------------------------------------------------------------------

Write-Host "`n"
Write-Host "===== FrameCache sweep results ====="
Write-Host ("{0,-12} {1,25} {2,20} {3,30}" -f "Budget", "FrameCache hit rate %", "Show iter p95 ms", "Cache-miss recoveries p95")
Write-Host ("{0,-12} {1,25} {2,20} {3,30}" -f "------", "---------------------", "----------------", "------------------------")
foreach ($r in $results) {
    Write-Host ("{0,-12} {1,25} {2,20} {3,30}" -f
        "$($r.Budget_GiB) GiB",
        $r.HitRate_median,
        $r.ShowIter_p95_ms,
        $r.CacheMiss_p95)
}
Write-Host ""
