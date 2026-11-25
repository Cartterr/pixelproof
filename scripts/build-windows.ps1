# Builds sb_sender / sb_receiver / sb_selftest on Windows (MSVC + official GStreamer MSVC x86_64).
# Prereqs: Visual Studio 2022 (or Build Tools) with the C++ workload, and GStreamer 1.28.x
# ("winget install gstreamerproject.gstreamer", or the official installer with /TYPE=devel).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gst = [Environment]::GetEnvironmentVariable('GSTREAMER_1_0_ROOT_MSVC_X86_64', 'Machine')
if (-not $gst) { $gst = 'C:\Program Files\gstreamer\1.0\msvc_x86_64\' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$cmake = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
if (-not $cmake) { $cmake = 'cmake' }
& $cmake -S $root -B "$root\build\win" -G 'Visual Studio 17 2022' -A x64 "-DGSTREAMER_ROOT=$gst"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build "$root\build\win" --config Release --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "Built: $root\build\win\Release\sb_sender.exe, sb_receiver.exe, sb_selftest.exe"
Write-Host "Run with GStreamer on PATH:  `$env:PATH = '$(Join-Path $gst 'bin');' + `$env:PATH"
