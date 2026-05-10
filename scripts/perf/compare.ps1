#Requires -Version 5.1
<#
.SYNOPSIS
    Compare two .summary.json files produced by capture.ps1 and show deltas.

.DESCRIPTION
    Reads a baseline and a new summary JSON, then prints a table of p95 deltas
    for CPU zones, GPU zones, and plots. Regressions >= threshold are flagged
    with a warning marker; improvements >= threshold are flagged with a star.

    Text mode (default): zones sorted by impact (|delta_ms * baseline_ms|)
    descending. Near-zero rows (both baseline and new <= 0.1 ms) are hidden
    unless --ShowAll is specified.

    JSON mode (--Json): emits a structured diff to stdout instead of the table.

.PARAMETER Baseline
    Path to the baseline .summary.json file.

.PARAMETER New
    Path to the new .summary.json file to compare against the baseline.

.PARAMETER Threshold
    Fractional change threshold for flagging improvements (star) or
    regressions (warning). Default: 0.05 (5%).

.PARAMETER Metric
    Which statistic to compare: p95 (default) or median.

.PARAMETER Json
    Emit structured JSON diff to stdout instead of the text table.

.PARAMETER ShowAll
    In text mode, show near-zero rows (both baseline and new <= 0.1 ms)
    that are otherwise hidden.

.EXAMPLE
    .\scripts\perf\compare.ps1 docs\perf\stress_3layer_prores_1080p-2026-05-10T120000Z.summary.json `
                               docs\perf\stress_3layer_prores_1080p-2026-05-12T143000Z.summary.json
    .\scripts\perf\compare.ps1 baseline.summary.json new.summary.json -Threshold 0.10 -Metric median
    .\scripts\perf\compare.ps1 baseline.summary.json new.summary.json --Json
    .\scripts\perf\compare.ps1 baseline.summary.json new.summary.json --ShowAll
#>
param(
    [Parameter(Mandatory, Position = 0)]
    [string]$Baseline,

    [Parameter(Mandatory, Position = 1)]
    [string]$New,

    [double]$Threshold = 0.05,

    [ValidateSet('p95', 'median')]
    [string]$Metric = 'p95',

    [switch]$Json,

    [switch]$ShowAll
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

function Get-Marker {
    param([double]$Pct, [double]$Threshold, [bool]$HigherIsBetter)
    $improved  = if ($HigherIsBetter) { $Pct -ge $Threshold } else { $Pct -le -$Threshold }
    $regressed = if ($HigherIsBetter) { $Pct -le -$Threshold } else { $Pct -ge $Threshold }
    if ($improved)  { return 'improvement' }
    if ($regressed) { return 'regression' }
    return 'none'
}

function Get-ImpactScore {
    param([object]$BaseVal, [object]$NewVal)
    if ($null -eq $BaseVal -or $null -eq $NewVal) { return 0 }
    $delta = [math]::Abs([double]$NewVal - [double]$BaseVal)
    return $delta * [math]::Abs([double]$BaseVal)
}

function Is-NearZero {
    param([object]$BaseVal, [object]$NewVal)
    $bNear = ($null -eq $BaseVal) -or ([double]$BaseVal -le 0.1)
    $nNear = ($null -eq $NewVal)  -or ([double]$NewVal  -le 0.1)
    return ($bNear -and $nNear)
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

function Build-ZoneDiffList {
    param(
        [object]$BaseZones,
        [object]$NewZones,
        [string]$MetricKey,
        [bool]$HigherIsBetter,
        [double]$Threshold
    )
    $allNames = @()
    if ($BaseZones) { $allNames += $BaseZones.PSObject.Properties | Select-Object -ExpandProperty Name }
    if ($NewZones)  { $allNames += $NewZones.PSObject.Properties  | Select-Object -ExpandProperty Name }
    $allNames = $allNames | Select-Object -Unique

    $rows = foreach ($name in $allNames) {
        $bzProp = if ($BaseZones) { $BaseZones.PSObject.Properties[$name] } else { $null }
        $nzProp = if ($NewZones)  { $NewZones.PSObject.Properties[$name]  } else { $null }
        $bz = if ($bzProp) { $bzProp.Value } else { $null }
        $nz = if ($nzProp) { $nzProp.Value } else { $null }
        $bv = if ($bz) { $bz.$MetricKey } else { $null }
        $nv = if ($nz) { $nz.$MetricKey } else { $null }
        $delta   = if ($null -ne $bv -and $null -ne $nv) { [math]::Round([double]$nv - [double]$bv, 3) } else { $null }
        $deltaPct = if ($null -ne $bv -and $null -ne $nv -and $bv -ne 0) {
            [math]::Round(($delta / [math]::Abs([double]$bv)) * 100, 1)
        } else { $null }
        $markerStr = if ($null -ne $deltaPct) {
            Get-Marker -Pct ($deltaPct / 100.0) -Threshold $Threshold -HigherIsBetter $HigherIsBetter
        } else { 'none' }
        $impact = Get-ImpactScore -BaseVal $bv -NewVal $nv
        [PSCustomObject]@{
            name         = $name
            baseline_ms  = $bv
            new_ms       = $nv
            delta_ms     = $delta
            delta_pct    = $deltaPct
            marker       = $markerStr
            _impact      = $impact
            _higherBetter = $HigherIsBetter
        }
    }
    return @($rows)
}

function Build-PlotDiffList {
    param(
        [object]$BasePlots,
        [object]$NewPlots,
        [string]$PlotMetricKey,
        [double]$Threshold,
        [string[]]$HigherIsBetterNames
    )
    $allNames = @()
    if ($BasePlots) { $allNames += $BasePlots.PSObject.Properties | Select-Object -ExpandProperty Name }
    if ($NewPlots)  { $allNames += $NewPlots.PSObject.Properties  | Select-Object -ExpandProperty Name }
    $allNames = $allNames | Select-Object -Unique

    $rows = foreach ($name in $allNames) {
        $bpProp = if ($BasePlots) { $BasePlots.PSObject.Properties[$name] } else { $null }
        $npProp = if ($NewPlots)  { $NewPlots.PSObject.Properties[$name]  } else { $null }
        $bp = if ($bpProp) { $bpProp.Value } else { $null }
        $np = if ($npProp) { $npProp.Value } else { $null }
        $bv = if ($bp) { $bp.$PlotMetricKey } else { $null }
        $nv = if ($np) { $np.$PlotMetricKey } else { $null }
        $hib = $HigherIsBetterNames -contains $name
        $delta   = if ($null -ne $bv -and $null -ne $nv) { [math]::Round([double]$nv - [double]$bv, 3) } else { $null }
        $deltaPct = if ($null -ne $bv -and $null -ne $nv -and $bv -ne 0) {
            [math]::Round(($delta / [math]::Abs([double]$bv)) * 100, 1)
        } else { $null }
        $markerStr = if ($null -ne $deltaPct) {
            Get-Marker -Pct ($deltaPct / 100.0) -Threshold $Threshold -HigherIsBetter $hib
        } else { 'none' }
        $impact = Get-ImpactScore -BaseVal $bv -NewVal $nv
        [PSCustomObject]@{
            name         = $name
            baseline_val = $bv
            new_val      = $nv
            delta        = $delta
            delta_pct    = $deltaPct
            marker       = $markerStr
            _impact      = $impact
            _higherBetter = $hib
        }
    }
    return @($rows)
}

# ---------------------------------------------------------------------------
# Load files
# ---------------------------------------------------------------------------

$base   = Read-Summary -Path $Baseline
$newSum = Read-Summary -Path $New

$metricKey     = if ($Metric -eq 'p95') { 'p95_ms' } else { 'median_ms' }
$plotMetricKey = $Metric  # plots use 'median', 'p5', 'p95' (no _ms suffix)

$higherIsBetterPlots = @('FrameCache hit rate %')

$scenarioMatch = $base.scenario -eq $newSum.scenario
$scenarioLabel = if ($scenarioMatch) { $base.scenario } else { "$($base.scenario) vs $($newSum.scenario)" }

# ---------------------------------------------------------------------------
# Build diff lists (shared between text and JSON modes)
# ---------------------------------------------------------------------------

$cpuRows  = Build-ZoneDiffList  -BaseZones $base.cpu_zones  -NewZones $newSum.cpu_zones  -MetricKey $metricKey  -HigherIsBetter $false -Threshold $Threshold
$gpuRows  = Build-ZoneDiffList  -BaseZones $base.gpu_zones  -NewZones $newSum.gpu_zones  -MetricKey $metricKey  -HigherIsBetter $false -Threshold $Threshold
$plotRows = Build-PlotDiffList  -BasePlots $base.plots -NewPlots $newSum.plots -PlotMetricKey $plotMetricKey -Threshold $Threshold -HigherIsBetterNames $higherIsBetterPlots

# ---------------------------------------------------------------------------
# JSON output mode
# ---------------------------------------------------------------------------

if ($Json) {
    $cpuOut = $cpuRows | ForEach-Object {
        [ordered]@{
            name        = $_.name
            baseline_ms = $_.baseline_ms
            new_ms      = $_.new_ms
            delta_ms    = $_.delta_ms
            delta_pct   = $_.delta_pct
            marker      = $_.marker
        }
    }
    $gpuOut = $gpuRows | ForEach-Object {
        [ordered]@{
            name        = $_.name
            baseline_ms = $_.baseline_ms
            new_ms      = $_.new_ms
            delta_ms    = $_.delta_ms
            delta_pct   = $_.delta_pct
            marker      = $_.marker
        }
    }
    $plotOut = $plotRows | ForEach-Object {
        [ordered]@{
            name         = $_.name
            baseline_val = $_.baseline_val
            new_val      = $_.new_val
            delta        = $_.delta
            delta_pct    = $_.delta_pct
            marker       = $_.marker
        }
    }
    $output = [ordered]@{
        baseline_file = $Baseline
        new_file      = $New
        scenario      = $scenarioLabel
        metric        = $Metric
        threshold     = $Threshold
        cpu_zones     = @($cpuOut)
        gpu_zones     = @($gpuOut)
        plots         = @($plotOut)
    }
    $output | ConvertTo-Json -Depth 5
    exit 0
}

# ---------------------------------------------------------------------------
# Text output mode
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "Scenario : $scenarioLabel"
if (-not $scenarioMatch) {
    Write-Host "  WARNING: scenario names differ. Comparison may not be meaningful."
}
Write-Host "Baseline : $($base.captured_at)  ($Baseline)"
Write-Host "New      : $($newSum.captured_at)  ($New)"
Write-Host "Metric   : $Metric  |  Threshold: $([math]::Round($Threshold * 100, 0))%  (* = improvement, ! = regression)"
if (-not $ShowAll) {
    Write-Host "           (rows where both baseline and new <= 0.1 ms are hidden; use --ShowAll to see them)"
}
Write-Host ""

# ---------------------------------------------------------------------------
# CPU Zones
# ---------------------------------------------------------------------------

Write-Host "CPU Zones ($Metric ms)"
Write-Host ("{0,-40} {1,10} {2,10}   delta" -f "  Zone", "baseline", "new")
Write-Host ("{0,-40} {1,10} {2,10}   -----" -f "  ----", "--------", "---")

$sortedCpu = $cpuRows | Sort-Object -Property _impact -Descending
foreach ($row in $sortedCpu) {
    if (-not $ShowAll -and (Is-NearZero -BaseVal $row.baseline_ms -NewVal $row.new_ms)) { continue }
    $unit = 'ms'
    $hib  = $row._higherBetter
    Write-Host (Format-Delta -Name "  $($row.name)" -BaseVal $row.baseline_ms -NewVal $row.new_ms -Unit $unit -Threshold $Threshold -HigherIsBetter $hib)
}

Write-Host ""

# ---------------------------------------------------------------------------
# GPU Zones
# ---------------------------------------------------------------------------

Write-Host "GPU Zones ($Metric ms)"
Write-Host ("{0,-40} {1,10} {2,10}   delta" -f "  Zone", "baseline", "new")
Write-Host ("{0,-40} {1,10} {2,10}   -----" -f "  ----", "--------", "---")

$sortedGpu = $gpuRows | Sort-Object -Property _impact -Descending
foreach ($row in $sortedGpu) {
    if (-not $ShowAll -and (Is-NearZero -BaseVal $row.baseline_ms -NewVal $row.new_ms)) { continue }
    Write-Host (Format-Delta -Name "  $($row.name)" -BaseVal $row.baseline_ms -NewVal $row.new_ms -Unit 'ms' -Threshold $Threshold -HigherIsBetter $row._higherBetter)
}

Write-Host ""

# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------

Write-Host "Plots ($Metric)"
Write-Host ("{0,-40} {1,10} {2,10}   delta" -f "  Plot", "baseline", "new")
Write-Host ("{0,-40} {1,10} {2,10}   -----" -f "  ----", "--------", "---")

$sortedPlots = $plotRows | Sort-Object -Property _impact -Descending
foreach ($row in $sortedPlots) {
    if (-not $ShowAll -and (Is-NearZero -BaseVal $row.baseline_val -NewVal $row.new_val)) { continue }
    $unit = if ($row.name -like '*%*') { '%' } else { '' }
    Write-Host (Format-Delta -Name "  $($row.name)" -BaseVal $row.baseline_val -NewVal $row.new_val -Unit $unit -Threshold $Threshold -HigherIsBetter $row._higherBetter)
}

Write-Host ""
Write-Host "  * improvement >= $([math]::Round($Threshold * 100, 0))%   ! regression >= $([math]::Round($Threshold * 100, 0))%"
Write-Host ""
