param(
    [string]$EmulatorDir = "$env:USERPROFILE/Downloads/sudachiemu.org-winpc-1-0-15",
    [string]$Nro,
    [ValidateRange(10, 60)][int]$Seconds = 45
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Nro) { $Nro = "$root/out/probe/platform-probe.nro" }
$emulator = "$EmulatorDir/sudachi-cmd.exe"
if (-not (Test-Path -LiteralPath $emulator)) { throw "Missing emulator: $emulator" }
if (-not (Test-Path -LiteralPath $Nro)) { throw "Build the NRO first: $Nro" }
$output = "$root/out/sudachi"
New-Item -ItemType Directory -Force $output | Out-Null
# Sudachi writes its normal emulator log under APPDATA. Keep a per-run copy.
$start = Get-Date
$arguments = @('--game', ('"' + [IO.Path]::GetFullPath($Nro) + '"'))
$process = Start-Process -FilePath $emulator -ArgumentList $arguments `
    -WorkingDirectory $EmulatorDir -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$output/stdout.log" -RedirectStandardError "$output/stderr.log"
try {
    if (-not $process.WaitForExit($Seconds * 1000)) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(3000)) { Stop-Process -Id $process.Id }
    }
} finally {
    $log = "$env:APPDATA/sudachi/log/sudachi_log.txt"
    if ((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).LastWriteTime -ge $start) {
        Copy-Item -LiteralPath $log -Destination "$output/emulator.log" -Force
    }
}
$report = "$env:APPDATA/sudachi/sdmc/switch/superman-returns-nx/platform-probe.log"
if ((Split-Path -Leaf $Nro) -eq 'platform-probe.nro') {
    if (-not (Test-Path -LiteralPath $report) -or (Get-Item -LiteralPath $report).LastWriteTime -lt $start) {
        throw "No fresh guest report; inspect $output/emulator.log"
    }
    Copy-Item -LiteralPath $report -Destination "$output/platform-probe.log" -Force
    Get-Content -LiteralPath "$output/platform-probe.log"
    $text = Get-Content -LiteralPath $report -Raw
    if ($text -notmatch 'Probe complete' -or $text -notmatch '39-bit address space: YES' -or
        $text -notmatch 'nvGpuChannelCreate: 00000000') {
        throw 'Platform probe failed; inspect the guest report.'
    }
}
Write-Output "Logs: $output. This test does not establish gameplay or hardware compatibility."
