param([ValidateRange(1, 16)][int]$Jobs = 4, [switch]$DriverOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$mesa = "$root/.tools/mesa-switch"
$commit = '1a8c1a66d6fd8d65f10107c4627ffc3606ba5631'
$image = 'superman-returns-nx-mesa:build'

function Run-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Run-Checked 'docker' @('info', '--format', '{{.ServerVersion}}')
if (-not (Test-Path -LiteralPath "$mesa/.git")) {
    New-Item -ItemType Directory -Force "$root/.tools" | Out-Null
    Run-Checked 'git' @('clone', '-c', 'core.autocrlf=false', '--no-checkout', '--depth', '1', 'https://github.com/danfromtico/mesa-switch.git', $mesa)
    Run-Checked 'git' @('-C', $mesa, 'fetch', '--depth', '1', 'origin', $commit)
    Run-Checked 'git' @('-C', $mesa, 'checkout', '--detach', 'FETCH_HEAD')
}
$current = & git -C $mesa rev-parse HEAD
if ($current -ne $commit) { throw "Mesa checkout must be $commit. Existing checkout was preserved." }
# Accept the exact patch already applied; otherwise require a clean checkout.
# Windows PowerShell turns native stderr into a terminating error under 'Stop'.
$ErrorActionPreference = 'Continue'
& git -C $mesa apply --reverse --check "$root/mesa/mesa-switch-nfsmw.patch" 2>$null
$alreadyApplied = $LASTEXITCODE -eq 0
$ErrorActionPreference = 'Stop'
if (-not $alreadyApplied) {
    & git -C $mesa diff --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Mesa has other changes; existing files were preserved.' }
    Run-Checked 'git' @('-C', $mesa, 'apply', '--check', "$root/mesa/mesa-switch-nfsmw.patch")
    Run-Checked 'git' @('-C', $mesa, 'apply', "$root/mesa/mesa-switch-nfsmw.patch")
}
Run-Checked 'docker' @('build', '--progress', 'plain', '-f', "$root/tools/switch/Dockerfile.mesa", '-t', $image, "$root/tools/switch")
$mount = "type=bind,source=$root,target=/project"
$volume = 'type=volume,source=superman-returns-nx-build,target=/work'
Run-Checked 'tar' @('-cf', "$root/.tools/mesa-build-source.tar", '-C', $mesa, '.')
Run-Checked 'docker' @('run', '--rm', '--mount', $mount, '--mount', $volume, '-e', "JOBS=$Jobs", $image,
    'bash', '/project/tools/switch/build-mesa.sh')
if (-not $DriverOnly) {
    Run-Checked 'tar' @('-cf', "$root/.tools/project-build-source.tar", '--exclude=app/out', '--exclude=sdk/out',
        '-C', $root, 'app', 'sdk', 'tools/switch/cmake')
    Run-Checked 'docker' @('run', '--rm', '--mount', $mount, '--mount', $volume, '-e', "JOBS=$Jobs", $image,
        'bash', '/project/tools/switch/build-game.sh')
}
