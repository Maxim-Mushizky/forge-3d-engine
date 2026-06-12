param(
    [ValidateSet("debug", "release")]
    [string]$Config = "release",
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

cmake --preset "mingw-$Config"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build --preset "mingw-$Config"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $NoLaunch) {
    & ".\build\$Config\bin\ForgeEditor.exe"
}
