<#
.SYNOPSIS
    Generate audio test fixtures for Entity integration tests.

.DESCRIPTION
    Produces a short video+audio clip (1 kHz sine tone) used by the
    audio integration tests (AudioToneTest, audio_loop_section_break,
    audio_pingpong).  Run once; re-run to regenerate.

.REQUIREMENTS
    ffmpeg must be on PATH (ships with vcpkg FFmpeg or install separately).

.EXAMPLE
    .\scripts\perf\setup-audio-fixtures.ps1
#>

$ErrorActionPreference = "Stop"

$outDir = "$PSScriptRoot\..\..\tests\fixtures\audio"
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
    Write-Host "Created $outDir"
}

$tone = "$outDir\tone_1khz.mov"
if (Test-Path $tone) {
    Write-Host "Fixture already exists: $tone  (delete to regenerate)"
} else {
    Write-Host "Generating 1 kHz tone fixture -> $tone ..."
    ffmpeg -y `
        -f lavfi -i "testsrc=size=320x240:rate=30:duration=5" `
        -f lavfi -i "sine=frequency=1000:duration=5" `
        -c:v prores_ks -profile:v 0 `
        -c:a pcm_s16le `
        "$tone"
    if ($LASTEXITCODE -ne 0) {
        throw "ffmpeg failed generating $tone"
    }
    Write-Host "Done: $tone"
}

Write-Host "Audio fixtures ready."
