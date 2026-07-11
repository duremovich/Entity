<#
.SYNOPSIS
    Generate video fixture media under test_media/ for Entity integration
    and repro scripts.

.DESCRIPTION
    Produces the small synthetic clips that scripted tests import but that
    are not committed to the repo (issue #86 — CI runners need a generator
    for every fixture a ctest-wired script references):

      - test_media/basic_loop.mov : 320x240 @ 30 fps, 5 s, ProRes proxy.
                                    Used by integration_park_then_import_visible
                                    (parks at timeline frame 90 and asserts the
                                    30 fps source frame 90 lands in FrameCache,
                                    so the clip must be 30 fps and span >= 91
                                    frames; 5 s = 150 frames).
      - test_media/loop_60fps.mov : 320x240 @ 60 fps, 5 s, ProRes proxy.
                                    Used by repro scripts that need a 60 fps
                                    source (scripts/repro/section_loop_60fps.json).

    Idempotent: existing files are skipped (delete to regenerate).
    Companion to setup-audio-fixtures.ps1 (audio tone fixture) and
    setup-fixtures.ps1 (large perf stress fixtures, NOT needed for ctest).

.REQUIREMENTS
    ffmpeg must be on PATH.

.EXAMPLE
    .\scripts\perf\setup-test-media.ps1
#>

$ErrorActionPreference = "Stop"

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Error "ffmpeg not found on PATH. Install ffmpeg (e.g., 'winget install Gyan.FFmpeg') and try again."
    exit 1
}

$outDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'test_media'
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
    Write-Host "Created $outDir"
}

function New-TestClip {
    param(
        [string]$OutPath,
        [int]$Fps
    )

    if (Test-Path $OutPath) {
        Write-Host "Fixture already exists: $OutPath  (delete to regenerate)"
        return
    }

    Write-Host "Generating $Fps fps test clip -> $OutPath ..."
    ffmpeg -y `
        -f lavfi -i "testsrc=size=320x240:rate=${Fps}:duration=5" `
        -c:v prores_ks -profile:v 0 `
        "$OutPath"
    if ($LASTEXITCODE -ne 0) {
        throw "ffmpeg failed generating $OutPath"
    }
    Write-Host "Done: $OutPath"
}

New-TestClip -OutPath (Join-Path $outDir 'basic_loop.mov') -Fps 30
New-TestClip -OutPath (Join-Path $outDir 'loop_60fps.mov') -Fps 60

Write-Host "Test media fixtures ready."
