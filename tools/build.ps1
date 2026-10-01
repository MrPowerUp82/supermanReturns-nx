param(
    [Parameter(Mandatory = $true)][string]$MesaSdk,
    [ValidateRange(1, 64)][int]$Jobs = 4,
    [string]$DevkitPro = $env:DEVKITPRO,
    [string]$CMake = 'cmake',
    [switch]$Lto
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# Visual Studio bundles CMake/Ninja even when they are not on the normal PATH.
if ($CMake -eq 'cmake' -and -not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $vsRoot = & $vswhere -latest -products '*' -property installationPath
        if ($vsRoot) {
            $cmakeRoot = "$vsRoot/Common7/IDE/CommonExtensions/Microsoft/CMake"
            $env:PATH = "$cmakeRoot/CMake/bin;$cmakeRoot/Ninja;$env:PATH"
        }
    }
}
if (-not $DevkitPro) { $DevkitPro = 'C:/devkitPro' }
$DevkitPro = [IO.Path]::GetFullPath($DevkitPro).Replace('\', '/')
$MesaSdk = [IO.Path]::GetFullPath($MesaSdk).Replace('\', '/')
if (-not (Test-Path -LiteralPath "$DevkitPro/devkitA64/bin/aarch64-none-elf-g++.exe")) {
    throw "devkitA64 is missing under $DevkitPro. See docs/building.md."
}
if (-not (Test-Path -LiteralPath "$MesaSdk/lib/libvulkan.a")) {
    throw 'MesaSdk must point to the Mesa Horizon opt/devkitpro/portlibs/switch folder.'
}
if (-not (Test-Path -LiteralPath "$root/app/generated/default/sources.cmake")) {
    throw 'Run tools/project.py prepare and codegen first.'
}
$out = "$root/app/out/switch"
$ltoOption = if ($Lto) { 'ON' } else { 'OFF' }
# Always configure, so changed SDK paths and options take effect.
& $CMake -S "$root/app" -B $out -G Ninja '-DCMAKE_BUILD_TYPE=Release' `
    "-DCMAKE_TOOLCHAIN_FILE=$root/tools/switch/cmake/switch-devkitA64.cmake" `
    "-DDEVKITPRO=$DevkitPro" "-DREXSDK_DIR=$root/sdk" `
    "-DREXGLUE_SWITCH_NVK_SDK=$MesaSdk" "-DSR_LTO=$ltoOption"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $CMake --build $out --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Switch compilation failed.' }
Write-Output "Built $out/superman_returns.nro (experimental; console validation required)."
