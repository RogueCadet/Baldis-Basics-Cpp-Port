# package.ps1  -  assemble a standalone, runnable folder.
#
# Produces  dist\BaldiCpp\
#              baldi.exe
#              content\data\...        (everything the game reads at runtime)
#
# The exe looks for its content next to ITSELF -- <exe>\content\data\ first, then <exe>\data\, then
# <exe>\..\data\ for the dev tree -- so this folder runs from anywhere, including a double-click in
# Explorer. That search is the fix for the black-window bug: before it, every data path resolved
# against the LAUNCH directory, so running from anywhere but the project root loaded 0 GameObjects
# and rendered nothing, with the explanation going to a console a windowed launch never shows.
#
# Content is a FOLDER, not bytes baked into the exe: the raw texture blobs alone are ~168 MB, which
# would bloat the binary and slow every build, and keeping them on disk means tools\bake-*.js can
# re-generate them without a recompile.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-OutDir <path>] [-Clean]
param(
  [string]$OutDir = '',
  [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$port = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $port 'dist\BaldiCpp' }

# The RELEASE exe (no console; dev options only with -debug) ships as baldi.exe.
$exe = Join-Path $port 'build\baldi-release.exe'
if (-not (Test-Path $exe)) { throw "no build\baldi-release.exe -- run tools\build-shell.ps1 first" }
$bake = Join-Path $port 'data\scene-MainMenu.bake.json'
if (-not (Test-Path $bake)) { throw "no data\scene-MainMenu.bake.json -- run the data pipeline first (see HANDOFF.md)" }

if ($Clean -and (Test-Path $OutDir)) { Remove-Item -Recurse -Force $OutDir }
$contentData = Join-Path $OutDir 'content\data'
New-Item -ItemType Directory -Force -Path $contentData | Out-Null

Write-Host "packaging -> $OutDir"
Copy-Item $exe (Join-Path $OutDir 'baldi.exe') -Force

# Everything under data\ EXCEPT the intermediates the game never opens: the *.graph.json scan
# output and the _prebake_backup folder are build inputs, not content, and together they are a
# large fraction of the tree.
$src = Join-Path $port 'data'
$skipDirs = @('_prebake_backup')
$files = Get-ChildItem -Recurse -File $src | Where-Object {
  $rel = $_.FullName.Substring($src.Length + 1)
  $top = ($rel -split '\\')[0]
  (-not ($skipDirs -contains $top)) -and ($_.Name -notlike '*.graph.json')
}
$n = 0; $bytes = 0
foreach ($f in $files) {
  $rel = $f.FullName.Substring($src.Length + 1)
  $dest = Join-Path $contentData $rel
  $destDir = Split-Path -Parent $dest
  if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
  Copy-Item $f.FullName $dest -Force
  $n++; $bytes += $f.Length
}
Write-Host ("  {0} content files, {1:N1} MB" -f $n, ($bytes / 1MB))

# Prove the package actually runs, from a directory that is NOT the project root. Packaging that is
# not verified is just copying: the bug this layout fixes was invisible from the project root.
Push-Location ([System.IO.Path]::GetTempPath())
try {
  $shot = Join-Path $OutDir 'package-check.raw'
  # The packaged exe ignores command-line options without -debug: give it dev options and NO
  # -debug, plus the BB_SELFTEST hook. It must still load MainMenu (not the --scene asked for).
  $env:BB_SELFTEST = $shot
  $out = & (Join-Path $OutDir 'baldi.exe') --scene School --at 5,4,60 2>&1 | Out-String
  Remove-Item Env:BB_SELFTEST
  Pop-Location
  if ($out -match "scene 'School'") { throw "packaged exe honoured --scene without -debug:`n$out" }
  if ($out -notmatch 'content root') { throw "packaged exe did not report a content root:`n$out" }
  if ($out -match 'NO CONTENT FOUND') { throw "packaged exe found no content:`n$out" }
  if ($out -notmatch "scene 'MainMenu'") { throw "packaged exe did not load MainMenu:`n$out" }
  & node (Join-Path $port 'tools\pixel-sanity.js') $shot --quiet
  if ($LASTEXITCODE -ne 0) { throw "packaged exe rendered an empty frame" }
  Remove-Item $shot, ($shot + '.txt') -ErrorAction SilentlyContinue
} catch {
  if ((Get-Location).Path -ne $port) { try { Pop-Location } catch {} }
  throw
}

Write-Host "PACKAGE OK - run $OutDir\baldi.exe from anywhere"
