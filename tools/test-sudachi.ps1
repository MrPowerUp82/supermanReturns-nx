param(
    [string]$EmulatorDir = $(if ($env:SUDACHI_DIR) { $env:SUDACHI_DIR } else { "$env:USERPROFILE/Music/sudachiemu.org-winpc-1-0-15" }),
    [string]$Nro,
    [string]$EmulatorConfig,
    [ValidateRange(10, 900)][int]$Seconds = 45
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
if ($EmulatorConfig) {
    if (-not (Test-Path -LiteralPath $EmulatorConfig)) { throw "Missing config: $EmulatorConfig" }
    $arguments += @('--config', ('"' + [IO.Path]::GetFullPath($EmulatorConfig) + '"'))
}
$process = Start-Process -FilePath $emulator -ArgumentList $arguments `
    -WorkingDirectory $EmulatorDir -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$output/stdout.log" -RedirectStandardError "$output/stderr.log"
$timedOut = $false
# Retain the native handle so Windows PowerShell can read ExitCode after exit.
$null = $process.Handle
try {
    if (-not $process.WaitForExit($Seconds * 1000)) {
        $timedOut = $true
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(3000)) { Stop-Process -Id $process.Id }
    }
} finally {
    $log = "$env:APPDATA/sudachi/log/sudachi_log.txt"
    if ((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).LastWriteTime -ge $start) {
        Copy-Item -LiteralPath $log -Destination "$output/emulator.log" -Force
    }
}
$process.Refresh()
$exitCode = $process.ExitCode
$guestLogs = "$env:APPDATA/sudachi/sdmc/switch/superman-returns-nx/logs"
if (Test-Path -LiteralPath $guestLogs) {
    Get-ChildItem -LiteralPath $guestLogs -Filter '*.log' |
        Where-Object { $_.LastWriteTime -ge $start } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $output -Force }
}
"started=$($start.ToString('o')) timeout=$timedOut exit_code=$exitCode" |
    Set-Content -LiteralPath "$output/run.log"
if (-not $timedOut -and $exitCode -ne 0) {
    throw "Sudachi exited with code $exitCode. Logs: $output"
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
