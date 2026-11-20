# Builds VitaTwitch.vpk. vita.cmake does not quote paths with spaces when
# packing the VPK, so the build runs through a space-free junction (C:\vt).
$ErrorActionPreference = "Stop"
if (-not $env:VITASDK) { $env:VITASDK = "C:/vitasdk" }
$env:PATH = "$($env:VITASDK -replace '/', '\')\bin;" + [Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" + [Environment]::GetEnvironmentVariable("PATH", "User")

$src = $PSScriptRoot
$root = $src
if ($src -match ' ') {
    $root = "C:\vt"
    if (-not (Test-Path $root)) { New-Item -ItemType Junction -Path $root -Target $src | Out-Null }
}

cmake -S $root -B "$root\build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=-Wno-format-truncation"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build "$root\build"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Copy-Item "$root\build\VitaTwitch.vpk" "$src\VitaTwitch.vpk" -Force
Write-Host "OK -> $src\VitaTwitch.vpk"
