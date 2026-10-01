param(
    [string]$EmulatorDir = "$env:USERPROFILE/Music/Ryujinx",
    [string]$Nro,
    [ValidateRange(10, 900)][int]$Seconds = 45
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Nro) { $Nro = "$root/out/probe/platform-probe.nro" }
$emulator = "$EmulatorDir/Ryujinx.exe"
if (-not (Test-Path -LiteralPath $emulator)) { throw "Missing emulator: $emulator" }
if (-not (Test-Path -LiteralPath $Nro)) { throw "Missing NRO: $Nro" }
$output = "$root/out/ryujinx"
New-Item -ItemType Directory -Force $output | Out-Null
$start = Get-Date
$process = Start-Process -FilePath $emulator `
    -ArgumentList ('"' + [IO.Path]::GetFullPath($Nro) + '"') `
    -WorkingDirectory $EmulatorDir -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$output/stdout.log" -RedirectStandardError "$output/stderr.log"
$null = $process.Handle
$timedOut = -not $process.WaitForExit($Seconds * 1000)
if ($timedOut) {
    $process.CloseMainWindow() | Out-Null
    if (-not $process.WaitForExit(3000)) { Stop-Process -Id $process.Id }
}
$process.Refresh()
$exitCode = $process.ExitCode
$guest = "$env:APPDATA/Ryujinx/sdcard/switch/superman-returns-nx"
if (Test-Path -LiteralPath "$guest/logs") {
    Get-ChildItem -LiteralPath "$guest/logs" -Filter '*.log' |
        Where-Object { $_.LastWriteTime -ge $start } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $output -Force }
}
"started=$($start.ToString('o')) timeout=$timedOut exit_code=$exitCode" |
    Set-Content -LiteralPath "$output/run.log"
if (-not $timedOut -and $exitCode -ne 0) { throw "Ryujinx exited with code $exitCode. Logs: $output" }
if ((Split-Path -Leaf $Nro) -eq 'platform-probe.nro') {
    $report = "$guest/platform-probe.log"
    if (-not (Test-Path -LiteralPath $report) -or (Get-Item -LiteralPath $report).LastWriteTime -lt $start) {
        throw "No fresh guest report; inspect $output/stdout.log"
    }
    Copy-Item -LiteralPath $report -Destination "$output/platform-probe.log" -Force
    Get-Content -LiteralPath $report
    $text = Get-Content -LiteralPath $report -Raw
    if ($text -notmatch 'Probe complete' -or $text -notmatch 'Memory probe mirror coherent: YES' -or
        $text -notmatch 'nvGpuChannelCreate: 00000000') { throw 'Platform probe failed.' }
}
Write-Output "Logs: $output. Process survival does not establish gameplay."
