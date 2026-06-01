#Requires -Version 5.1
<#
.SYNOPSIS
    Verify that an editor binary was actually compiled with Tracy
    instrumentation baked in.

.DESCRIPTION
    Asserts that a known Tracy zone-name literal (default "Show iter", the
    canonical show-thread frame zone from Engine.cpp) is present in the
    binary. When ENTITY_ENABLE_TRACY=ON the ZoneScopedN("Show iter") macro
    bakes that string into the .exe; when OFF (or when a stale build-noprof
    binary is used) it is absent.

    DO NOT use `strings` for this check. The git-bash / GNU `strings` build
    silently fails on MSVC COFF/PE binaries — it returns ZERO strings for a
    fully-populated 486 KB .obj — so `strings EntityMediaEditor.exe | grep
    tracy` is a FALSE NEGATIVE and will tell you the build is broken when it
    is fine. (This exact false negative is what sent issue #62 down a wrong
    diagnosis.) We read the bytes and do a literal substring scan instead,
    which is reliable on Windows binaries.

.PARAMETER EditorExe
    Path to the editor binary to check. Defaults to the Release build.

.PARAMETER Zone
    Zone-name literal to look for. Defaults to "Show iter".

.EXAMPLE
    pwsh scripts/perf/verify-tracy-build.ps1
    pwsh scripts/perf/verify-tracy-build.ps1 -EditorExe build-noprof\bin\Release\EntityMediaEditor.exe

.NOTES
    Exit 0 = Tracy symbols present. Exit 1 = absent or file missing.
    Intended both for humans and as a capture.ps1 / CI preflight.
#>
param(
    [string]$EditorExe,
    [string]$Zone = 'Show iter'
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $EditorExe) {
    $EditorExe = Join-Path $RepoRoot 'build\bin\Release\EntityMediaEditor.exe'
}

if (-not (Test-Path $EditorExe)) {
    Write-Error "Editor binary not found: $EditorExe"
    exit 1
}

# Read the whole binary and decode as Latin-1 (ISO-8859-1) so every byte maps
# 1:1 to a char — this lets us do a reliable literal substring scan without
# the COFF-parsing failure that breaks `strings`.
$bytes = [System.IO.File]::ReadAllBytes($EditorExe)
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$text = $latin1.GetString($bytes)

if ($text.Contains($Zone)) {
    Write-Host "Tracy symbols: present ('$Zone' found in $(Split-Path -Leaf $EditorExe))"
    exit 0
} else {
    Write-Host @"
Tracy symbols: ABSENT - zone literal '$Zone' not found in:
  $EditorExe

The editor binary was NOT compiled with Tracy instrumentation. Rebuild the
profiling tree with Tracy enabled (it is the default):

  cmake --build build --config Release

Make sure you are using build/ (ENTITY_ENABLE_TRACY=ON) and NOT build-noprof/.

NOTE: if you previously checked this with 'strings | grep tracy' and got 0
hits, that tool gives false negatives on MSVC binaries - this script is the
reliable check. See docs/perf/tracy-troubleshooting.md.
"@ -ForegroundColor Red
    exit 1
}
