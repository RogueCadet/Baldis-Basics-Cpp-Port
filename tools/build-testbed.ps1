# build-testbed.ps1  -  object-compile smoke test of the port with the newest available MSVC (VS 2022 / 2019).
# Usage: powershell -ExecutionPolicy Bypass -File tools\build-testbed.ps1
$ErrorActionPreference = 'Stop'
$port = Split-Path -Parent $PSScriptRoot

# VS 2022 and 2019 live on F: (custom install locations, discovered 2026-10-21)
$vcvars = $null
foreach ($p in @(
  'F:\VS2022\community\VC\Auxiliary\Build\vcvarsall.bat',
  'F:\programs\vs2019\community\VC\Auxiliary\Build\vcvarsall.bat')) {
  if (Test-Path $p) { $vcvars = $p; break }
}
if (-not $vcvars) { throw "no VC tools found at F:\VS2022\community or F:\programs\vs2019\community; adjust this script" }
Write-Host "vcvars: $vcvars"

New-Item -ItemType Directory -Force -Path (Join-Path $port 'build') | Out-Null

$bat = Join-Path $env:TEMP 'baldi-port-smoke.cmd'
@"
@echo off
call "$vcvars" x64 >nul 2>&1
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\smoke.obj" "$port\src\smoke.cpp"
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\quat-euler.obj" "$port\src\quat-euler.cpp"
if errorlevel 1 exit /b 2
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\eng-core.obj" "$port\src\eng-core.cpp"
if errorlevel 1 exit /b 3
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\eng-rewired.obj" "$port\src\eng-rewired.cpp"
if errorlevel 1 exit /b 3
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\eng-json.obj" "$port\src\eng-json.cpp"
if errorlevel 1 exit /b 4
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\eng-scene.obj" "$port\src\eng-scene.cpp"
if errorlevel 1 exit /b 5
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\eng-mesh.obj" "$port\src\eng-mesh.cpp"
if errorlevel 1 exit /b 6
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\game-actions.obj" "$port\src\game-actions.cpp"
if errorlevel 1 exit /b 10
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\bindings.obj" "$port\src\bindings.cpp"
if errorlevel 1 exit /b 7
cl /nologo /O2 /EHsc /W3 /permissive- /std:c++14 /c /I"$port\src" /Fo"$port\build\sim.obj" "$port\src\sim.cpp"
if errorlevel 1 exit /b 8
rem smoke.obj is deliberately NOT linked: it is an interface-only parse gate carrying its own
rem main(), which would collide with sim.obj's (LNK2005). Stage 1 is its whole purpose.
link /nologo /OUT:"$port\build\sim.exe" /SUBSYSTEM:CONSOLE ^
  "$port\build\quat-euler.obj" "$port\build\eng-core.obj" "$port\build\eng-rewired.obj" ^
  "$port\build\eng-json.obj" "$port\build\eng-scene.obj" "$port\build\eng-mesh.obj" ^
  "$port\build\bindings.obj" "$port\build\game-actions.obj" "$port\build\sim.obj"
if errorlevel 1 exit /b 9
exit /b 0
"@ | Set-Content -Encoding ASCII -Path $bat

cmd /c $bat
$ec = $LASTEXITCODE
Remove-Item $bat -ErrorAction SilentlyContinue
if ($ec -eq 0) { Write-Host 'SMOKE OK - headers + engine core parse under MSVC' }
else { Write-Host "SMOKE FAILED - stage exit code $ec" }
exit $ec
