# build-shell.ps1 -- build the Windows testbed shell (build\baldi.exe).
#
# Separate from build-testbed.ps1 on purpose: that script is the correctness gate (smoke + sim,
# 125 checks) and must stay independent of the GUI. The shell links the SAME engine objects that
# gate already produced, plus its own Win32/WGL translation unit.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-shell.ps1
$ErrorActionPreference = 'Stop'
$port = Split-Path -Parent $PSScriptRoot

$vcvars = $null
foreach ($p in @(
  'F:\VS2022\community\VC\Auxiliary\Build\vcvarsall.bat',
  'F:\programs\vs2019\community\VC\Auxiliary\Build\vcvarsall.bat')) {
  if (Test-Path $p) { $vcvars = $p; break }
}
if (-not $vcvars) { throw "no VC tools found; adjust this script" }
Write-Host "vcvars: $vcvars"

New-Item -ItemType Directory -Force -Path (Join-Path $port 'build') | Out-Null

$bat = Join-Path $env:TEMP 'baldi-port-shell.cmd'
@"
@echo off
call "$vcvars" x64 >nul 2>&1
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\shell-win32.obj" "$port\src\shell-win32.cpp"
if errorlevel 1 exit /b 1
rem render-scene (portable passes, shared with iOS) + render-gl (the rgl GPU layer)
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\render-scene.obj" "$port\src\render-scene.cpp"
if errorlevel 1 exit /b 3
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\render-gl.obj" "$port\src\render-gl.cpp"
if errorlevel 1 exit /b 4
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\app-core.obj" "$port\src\app-core.cpp"
if errorlevel 1 exit /b 5
rem the genuine BALDI.exe icon (shell-res\baldi.rc -> baldi.res, linked below)
rc /nologo /fo"$port\build\baldi.res" "$port\shell-res\baldi.rc"
if errorlevel 1 exit /b 6
rem Engine objects come from build-testbed.ps1; smoke.obj and sim.obj are excluded (each has its own main).
link /nologo /OUT:"$port\build\baldi.exe" /SUBSYSTEM:CONSOLE ^
  "$port\build\baldi.res" "$port\build\shell-win32.obj" "$port\build\render-scene.obj" "$port\build\render-gl.obj" "$port\build\app-core.obj" "$port\build\quat-euler.obj" "$port\build\eng-core.obj" "$port\build\eng-rewired.obj" ^
  "$port\build\eng-json.obj" "$port\build\eng-scene.obj" "$port\build\eng-mesh.obj" ^
  "$port\build\bindings.obj" "$port\build\game-actions.obj" ^
  opengl32.lib user32.lib gdi32.lib winmm.lib
if errorlevel 1 exit /b 2
rem RELEASE exe (what package.ps1 ships as baldi.exe): same objects, the shell built with
rem BB_RELEASE -- a GUI program (no console window), dev options only with -debug / --debug.
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /DBB_RELEASE /c /I"$port\src" /Fo"$port\build\shell-win32-rel.obj" "$port\src\shell-win32.cpp"
if errorlevel 1 exit /b 7
link /nologo /OUT:"$port\build\baldi-release.exe" /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup ^
  "$port\build\baldi.res" "$port\build\shell-win32-rel.obj" "$port\build\render-scene.obj" "$port\build\render-gl.obj" "$port\build\app-core.obj" "$port\build\quat-euler.obj" "$port\build\eng-core.obj" "$port\build\eng-rewired.obj" ^
  "$port\build\eng-json.obj" "$port\build\eng-scene.obj" "$port\build\eng-mesh.obj" ^
  "$port\build\bindings.obj" "$port\build\game-actions.obj" ^
  opengl32.lib user32.lib gdi32.lib winmm.lib
if errorlevel 1 exit /b 8
exit /b 0
"@ | Set-Content -Encoding ASCII -Path $bat

cmd /c $bat
$ec = $LASTEXITCODE
Remove-Item $bat -ErrorAction SilentlyContinue
if ($ec -eq 0) { Write-Host 'SHELL OK - build\baldi.exe (dev) + build\baldi-release.exe (packaged: no console, dev options only with -debug)' }
else { Write-Host "SHELL FAILED - stage exit code $ec (1=compile, 2=link, 7/8=release exe)" }
exit $ec
