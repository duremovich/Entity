#Requires -Version 5.1
<#
.SYNOPSIS
    Generates synthetic perf fixture media under test_media/perf/.

.DESCRIPTION
    Creates three video files used by the stress scenarios:
      - noise_1080p_prores.mov    : high-entropy noise, ProRes 4444, 1920x1080@60
      - mandelbrot_4k_prores.mov  : animated Mandelbrot, ProRes 4444, 3840x2160@60
      - checker_1080p_mjpeg.mov   : animated test pattern, MJPEG q=2, 1920x1080@60

    All files are 10-second loops. Re-running is idempotent — existing files are
    skipped unless -Force is specified.

    Disk requirement: ~9 GB for the two ProRes fixtures; ~50 MB for the MJPEG fixture.
    Ensure you have at least 10 GB free before running without -Force.

.PARAMETER Force
    Re-generate all fixtures even if they already exist.

.EXAMPLE
    .\scripts\perf\setup-fixtures.ps1
    .\scripts\perf\setup-fixtures.ps1 -Force
#>
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Error "ffmpeg not found on PATH. Install ffmpeg (e.g., 'winget install Gyan.FFmpeg' or 'choco install ffmpeg') and try again."
    exit 1
}

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$OutDir   = Join-Path $RepoRoot 'test_media\perf'

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
    Write-Host "Created $OutDir"
}

function Build-Fixture {
    param(
        [string]$Label,
        [string]$OutPath,
        [scriptblock]$Cmd
    )

    Write-Host "  build $Label -> $(Split-Path -Leaf $OutPath)"
    & $Cmd
    if ($LASTEXITCODE -ne 0) {
        throw "ffmpeg exited with code $LASTEXITCODE"
    }
    $size = (Get-Item $OutPath).Length
    Write-Host "        done  ($([math]::Round($size / 1MB, 1)) MB)"
}

$skipped = 0
$built   = 0
$failed  = 0

Write-Host "Note: ProRes fixtures total ~9 GB. Ensure sufficient disk space before proceeding."
Write-Host ""

# --- noise_1080p_prores.mov ---
$out = Join-Path $OutDir 'noise_1080p_prores.mov'
if ((Test-Path $out) -and -not $Force) {
    Write-Host "  skip  noise_1080p_prores.mov (exists; use -Force to regenerate)"
    $skipped++
} else {
    try {
        # nullsrc provides size+rate context; geq generates per-pixel random noise.
        # $noiseFilter and $out are captured by the scriptblock via PowerShell closure.
        # If adding a 4th fixture, assign its filter to a named variable BEFORE the scriptblock literal.
        $noiseFilter = "nullsrc=size=1920x1080:rate=60,geq=lum_expr='random(1)*255':cb_expr='random(2)*255':cr_expr='random(3)*255'"
        Build-Fixture -Label 'Noise 1080p ProRes 4444' -OutPath $out -Cmd {
            ffmpeg -y -f lavfi -i $noiseFilter -t 10 -c:v prores_ks -profile:v 4 -pix_fmt yuv444p10le -bits_per_mb 8000 -threads 0 $out
        }
        $built++
    } catch {
        Write-Error "Failed to generate noise_1080p_prores.mov: $_"
        $failed++
    }
}

# --- mandelbrot_4k_prores.mov ---
$out = Join-Path $OutDir 'mandelbrot_4k_prores.mov'
if ((Test-Path $out) -and -not $Force) {
    Write-Host "  skip  mandelbrot_4k_prores.mov (exists; use -Force to regenerate)"
    $skipped++
} else {
    try {
        # end_pts=600 animates 10s at 60fps (600 pts).
        $mandelFilter = 'mandelbrot=size=3840x2160:rate=60:end_pts=600'
        Build-Fixture -Label 'Mandelbrot 4K ProRes 4444' -OutPath $out -Cmd {
            ffmpeg -y -f lavfi -i $mandelFilter -t 10 -c:v prores_ks -profile:v 4 -pix_fmt yuv444p10le -bits_per_mb 8000 -threads 0 $out
        }
        $built++
    } catch {
        Write-Error "Failed to generate mandelbrot_4k_prores.mov: $_"
        $failed++
    }
}

# --- checker_1080p_mjpeg.mov ---
$out = Join-Path $OutDir 'checker_1080p_mjpeg.mov'
if ((Test-Path $out) -and -not $Force) {
    Write-Host "  skip  checker_1080p_mjpeg.mov (exists; use -Force to regenerate)"
    $skipped++
} else {
    try {
        $checkerFilter = 'testsrc=size=1920x1080:rate=60'
        Build-Fixture -Label 'Checkerboard 1080p MJPEG' -OutPath $out -Cmd {
            ffmpeg -y -f lavfi -i $checkerFilter -t 10 -c:v mjpeg -q:v 2 $out
        }
        $built++
    } catch {
        Write-Error "Failed to generate checker_1080p_mjpeg.mov: $_"
        $failed++
    }
}

Write-Host ""
Write-Host "setup-fixtures: $built built, $skipped skipped, $failed failed"

if ($failed -gt 0) {
    exit 1
}
