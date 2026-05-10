#Requires -Version 5.1
<#
.SYNOPSIS
    Compare two .summary.json files produced by capture.ps1 and show deltas.

.DESCRIPTION
    Reads a baseline and a new summary JSON, then prints a table of p95 deltas
    for CPU zones, GPU zones, and plots. Regressions >= threshold are flagged
    with a warning marker; improvements >= threshold are flagged with a star.

.PARAMETER Baseline
    Path to the baseline .summary.json file.

.PARAMETER New
    Path to the new .summary.json file to compare against the baseline.

.PARAMETER Threshold
    Fractional change threshold for flagging improvements (star) or
    regressions (warning). Default: 0.05 (5%).

.PARAMETER Metric
    Which statistic to compare: p95 (default), median, or p5.

.EXAMPLE
    .\scripts\perf\compare.ps1 docs\perf\stress_3layer_prores_1080p-2026-05-10T120000Z.summary.json `
                               docs\perf\stress_3layer_prores_1080p-2026-05-12T143000Z.summary.json
    .\scripts\perf\compare.ps1 baseline.summary.json new.summary.json -Threshold 0.10 -Metric median
#>
param(
    [Parameter(Mandatory, Position = 0)]
    [string]$Baseline,

    [Parameter(Mandatory, Position = 1)]
    [string]$New,

    [double]$Threshold = 0.05,

    [ValidateSet('p95', 'median')]
    [string]$Metric = 'p95'
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Read-Summary {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        Write-Error "File not found: $Path"
        exit 1
    }
    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    return $text | ConvertFrom-Json
}

function Format-Delta {
    param(
        [string]$Name,
        [object]$BaseVal,
        [object]$NewVal,
        [string]$Unit,
        [double]$Threshold,
        [bool]$HigherIsBetter
    )

    $dash = '--'

    if ($null -eq $BaseVal -and $null -eq $NewVal) {
        return ("{0,-40} {1,10} {2,10}   {3}" -f $Name, $dash, $dash, '')
    }
    if ($null -eq $BaseVal) {
        return ("{0,-40} {1,10} {2,10}   (new)" -f $Name, $dash, "$NewVal $Unit")
    }
    if ($null -eq $NewVal) {
        return ("{0,-40} {1,10} {2,10}   (removed)" -f $Name, "$BaseVal $Unit", $dash)
    }

    $delta = $NewVal - $BaseVal
    $pct   = if ($BaseVal -ne 0) { $delta / [math]::Abs($BaseVal) } else { 0 }

    $sign  = if ($delta -gt 0) { '+' } else { '' }
    $deltaStr = "${sign}$([math]::Round($delta, 2)) ($($sign)$([math]::Round($pct * 100, 0))%)"

    $improved  = if ($HigherIsBetter) { $pct -ge $Threshold } else { $pct -le -$Threshold }
    $regressed = if ($HigherIsBetter) { $pct -le -$Threshold } else { $pct -ge $Threshold }

    $marker = if ($improved) { ' *' } elseif ($regressed) { ' !' } else { '' }

    return ("{0,-40} {1,10} {2,10}   {3}{4}" -f $Name, "$BaseVal $Unit", "$NewVal $Unit", $deltaStr, $marker)
}

# ---------------------------------------------------------------------------
# Load files
# ---------------------------------------------------------------------------

$base = Read-Summary -Path $Baseline
$newSum  = Read-Summary -Path $New

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

$scenarioMatch = $base.scenario -eq $newSum.scenario
$scenarioLabel = if ($scenarioMatch) { $base.scenario } else { "$($base.scenario) vs $($newSum.scenario)" }

Write-Host ""
Write-Host "Scenario : $scenarioLabel"
if (-not $scenarioMatch) {
    Write-Host "  WARNING: scenario names differ. Comparison may not be meaningful."
}
Write-Host "Baseline : $($base.captured_at)  ($Baseline)"
Write-Host "New      : $($newSum.captured_at)  ($New)"
Write-Host "Metric   : $Metric  |  Threshold: $([math]::Round($Threshold * 100, 0))%  (* = improvement, ! = regression)"
Write-Host ""

# ---------------------------------------------------------------------------
# CPU Zones
# ---------------------------------------------------------------------------

$metricKey = if ($Metric -eq 'p95') { 'p95_ms' } else { 'median_ms' }

Write-Host "CPU Zones ($Metric ms)"
Write-Host ("{0,-40} {1,10} {2,10}   delta" -f "  Zone", "baseline", "new")
Write-Host ("{0,-40} {1,10} {2,10}   -----" -f "  ----", "--------", "---")

$allCpuNames = @()
if ($base.cpu_zones) { $allCpuNames += $base.cpu_zones.PSObject.Properties | Select-Object -ExpandProperty Name }
if ($newSum.cpu_zones)  { $allCpuNames += $newSum.cpu_zones.PSObject.Properties  | Select-Object -ExpandProperty Name }
$allCpuNames = $allCpuNames | Select-Object -Unique | Sort-Object

foreach ($name in $allCpuNames) {
    $bzProp = if ($base.cpu_zones) { $base.cpu_zones.PSObject.Properties[$name] } else { $null }
    $nzProp = if ($newSum.cpu_zones)  { $newSum.cpu_zones.PSObject.Properties[$name]  } else { $null }
    $bz = if ($bzProp) { $bzProp.Value } else { $null }
    $nz = if ($nzProp) { $nzProp.Value } else { $null }
    $bv = if ($bz) { $bz.$metricKey } else { $null }
    $nv = if ($nz) { $nz.$metricKey } else { $null }
    Write-Host (Format-Delta -Name "  $name" -BaseVal $bv -NewVal $nv -Unit 'ms' -Threshold $Threshold -HigherIsBetter $false)
}

Write-Host ""

# ---------------------------------------------------------------------------
# GPU Zones
# ---------------------------------------------------------------------------

Write-Host "GPU Zones ($Metric ms)"
Write-Host ("{0,-40} {1,10} {2,10}   delta" -f "  Zone", "baseline", "new")
Write-Host ("{0,-40} {1,10} {2,10}   -----" -f "  ----", "--------", "---")

$allGpuNames = @()
if ($base.gpu_zones) { $allGpuNames += $base.gpu_zones.PSObject.Properties | Select-Object -ExpandProperty Name }
if ($newSum.gpu_zones)  { $allGpuNames += $newSum.gpu_zones.PSObject.Properties  | Select-Object -ExpandProperty Name }
$allGpuNames = $allGpuNames | Select-Object -Unique | Sort-Object

foreach ($name in $allGpuNames) {
    $bzProp = if ($base.gpu_zones) { $base.gpu_zones.PSObject.Properties[$name] } else { $null }
    $nzProp = if ($newSum.gpu_zones)  { $newSum.gpu_zones.PSObject.Properties[$name]  } else { $null }
    $bz = if ($bzProp) { $bzProp.Value } else { $null }
    $nz = if ($nzProp) { $nzProp.Value } else { $null }
    $bv = if ($bz) { $bz.$metricKey } else { $null }
    $nv = if ($nz) { $nz.$metricKey } else { $null }
    Write-Host (Format-Delta -Name "  $name" -BaseVal $bv -NewVal $nv -Unit 'ms' -Threshold $Threshold -HigherIsBetter $false)
}

Write-Host ""

# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------

$plotMetricKey = $Metric  # plots use 'median', 'p5', 'p95' (no _ms suffix)

Write-Host "Plots ($Metric)"
Write-Host ("{0,-40} {1,10} {2,10}   delta" -f "  Plot", "baseline", "new")
Write-Host ("{0,-40} {1,10} {2,10}   -----" -f "  ----", "--------", "---")

$allPlotNames = @()
if ($base.plots) { $allPlotNames += $base.plots.PSObject.Properties | Select-Object -ExpandProperty Name }
if ($newSum.plots)  { $allPlotNames += $newSum.plots.PSObject.Properties  | Select-Object -ExpandProperty Name }
$allPlotNames = $allPlotNames | Select-Object -Unique | Sort-Object

$higherIsBetterPlots = @('FrameCache hit rate %')

foreach ($name in $allPlotNames) {
    $bpProp = if ($base.plots) { $base.plots.PSObject.Properties[$name] } else { $null }
    $npProp = if ($newSum.plots)  { $newSum.plots.PSObject.Properties[$name]  } else { $null }
    $bp = if ($bpProp) { $bpProp.Value } else { $null }
    $np = if ($npProp) { $npProp.Value } else { $null }
    $bv = if ($bp) { $bp.$plotMetricKey } else { $null }
    $nv = if ($np) { $np.$plotMetricKey } else { $null }
    $hib = $higherIsBetterPlots -contains $name
    $unit = if ($name -like '*%*') { '%' } else { '' }
    Write-Host (Format-Delta -Name "  $name" -BaseVal $bv -NewVal $nv -Unit $unit -Threshold $Threshold -HigherIsBetter $hib)
}

Write-Host ""
Write-Host "  * improvement >= $([math]::Round($Threshold * 100, 0))%   ! regression >= $([math]::Round($Threshold * 100, 0))%"
Write-Host ""
