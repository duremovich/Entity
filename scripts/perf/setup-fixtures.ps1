#Requires -Version 5.1
<#
.SYNOPSIS
    Generates synthetic perf fixture media under test_media/perf/.

.DESCRIPTION
    Creates five video files used by the stress scenarios:
      - noise_1080p_prores.mov    : high-entropy noise, ProRes 4444, 1920x1080@60
                                    (CPU decode pressure; intra-only)
      - mandelbrot_4k_prores.mov  : animated Mandelbrot, ProRes 4444, 3840x2160@60
                                    (GPU upload bandwidth; intra-only)
      - checker_1080p_mjpeg.mov   : animated test pattern, MJPEG q=2, 1920x1080@60
                                    (light decode, used for cache-pressure scenarios)
      - testpat_1080p_hap.mov     : animated testsrc pattern, Hap1/BC1 RGB, 1920x1080@60
                                    (GPU-direct decode; intra-only; minimal CPU.
                                     Pattern source instead of noise -- HAP is block
                                     compression, random noise inflates the file to
                                     multi-GB because every block is unique.)
      - testpat_1080p_h264.mp4    : animated testsrc pattern, H.264 high@5.1, 1920x1080@60
                                    (inter-frame; exercises GOP seek + FrameCache eviction.
                                     Pattern source instead of noise -- libx264 can't compress
                                     random noise and inflates the file to ~850 MB at 1080p60.)

    All files are 10-second loops. Re-running is idempotent -- existing files are
    skipped unless -Force is specified.

    Disk requirement:
      ~9 GB total for the two ProRes fixtures
      ~50 MB MJPEG
      ~30 MB HAP (testsrc pattern -- block compression friendly)
      ~30 MB H.264 (testsrc pattern, CRF 18 + slow preset)
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

# Encoder availability check -- 'essentials' builds of Gyan ffmpeg do NOT
# include the HAP encoder; the 'full' build does (winget: Gyan.FFmpeg.Full).
# Detect once so we can skip HAP gracefully instead of aborting mid-script.
$ffmpegEncoders = & ffmpeg -hide_banner -encoders 2>$null
$hasHapEncoder  = $ffmpegEncoders -match '\bhap\b'
if (-not $hasHapEncoder) {
    Write-Host "Note: ffmpeg HAP encoder not available -- will use fallback for HAP fixture."
    Write-Host "      Install Gyan.FFmpeg.Full or build ffmpeg with --enable-libsnappy for native HAP encoding."
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
        Write-Warning "Failed to generate noise_1080p_prores.mov: $_"
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
        Write-Warning "Failed to generate mandelbrot_4k_prores.mov: $_"
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
        Write-Warning "Failed to generate checker_1080p_mjpeg.mov: $_"
        $failed++
    }
}

# --- testpat_1080p_hap.mov ---
# Hap1 (BC1 RGB). GPU-direct decode -- payload uploads straight to VRAM
# without CPU decompression. Intra-only (every frame keyframe).
#
# Source MUST be a low-entropy / structured pattern. HAP is fixed-rate BC1
# block compression (8 bytes per 4x4 block); random noise can't compress and
# inflates the file to multi-GB at 1080p60. Using `testsrc` (animated color
# bars + frame number) which is realistic-ish for show content.
$out = Join-Path $OutDir 'testpat_1080p_hap.mov'
if (-not $hasHapEncoder) {
    # Fallback: copy the checked-in gradient HAP fixture to the expected name
    # so scenarios that reference testpat_1080p_hap.mov still find a HAP file.
    # Decode pressure is reduced (the gradient is much smaller than a 10s
    # 1080p stress file would be), but the HAP code path is still exercised.
    $hapFallback = Join-Path $RepoRoot 'test_media\hap_gradient\test.mov'
    if (Test-Path $hapFallback) {
        Copy-Item -Path $hapFallback -Destination $out -Force
        Write-Host "  copy  testpat_1080p_hap.mov  <- test_media\hap_gradient\test.mov (HAP encoder not available; using checked-in gradient as fallback)"
        $built++
    } else {
        Write-Warning "HAP encoder missing AND no fallback at $hapFallback; scenarios that reference testpat_1080p_hap.mov will fail to load it."
        $skipped++
    }
} elseif ((Test-Path $out) -and -not $Force) {
    Write-Host "  skip  testpat_1080p_hap.mov (exists; use -Force to regenerate)"
    $skipped++
} else {
    try {
        $hapFilter = 'testsrc=size=1920x1080:rate=60,format=rgb24'
        Build-Fixture -Label 'Testpat 1080p HAP (Hap1/BC1)' -OutPath $out -Cmd {
            ffmpeg -y -f lavfi -i $hapFilter -t 10 -c:v hap -format hap -threads 0 $out
        }
        $built++
    } catch {
        Write-Warning "Failed to generate testpat_1080p_hap.mov: $_"
        $failed++
    }
}

# --- testpat_1080p_h264.mp4 ---
# H.264 high@5.1 with GOP=60 (1 keyframe per second @ 60 fps) for predictable
# seek behavior. CRF 18 keeps quality high; -preset slow makes the encode
# slower but trims the resulting file. Inter-frame -- exercises the
# decode-from-keyframe + FrameCache eviction paths the way ProRes/HAP don't.
#
# Source is testsrc (animated bars + frame number), not random noise: libx264
# can't compress noise and inflates the file to ~850 MB.
$out = Join-Path $OutDir 'testpat_1080p_h264.mp4'
if ((Test-Path $out) -and -not $Force) {
    Write-Host "  skip  testpat_1080p_h264.mp4 (exists; use -Force to regenerate)"
    $skipped++
} else {
    try {
        $h264Filter = 'testsrc=size=1920x1080:rate=60'
        Build-Fixture -Label 'Testpat 1080p H.264 high@5.1' -OutPath $out -Cmd {
            ffmpeg -y -f lavfi -i $h264Filter -t 10 -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -profile:v high -level 5.1 -g 60 -keyint_min 60 -sc_threshold 0 -threads 0 $out
        }
        $built++
    } catch {
        Write-Warning "Failed to generate testpat_1080p_h264.mp4: $_"
        $failed++
    }
}

Write-Host ""
Write-Host "setup-fixtures: $built built, $skipped skipped, $failed failed"

if ($failed -gt 0) {
    exit 1
}
