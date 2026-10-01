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
# The NFSMW patch, then this port's fixes on top (mesa-switch-superman.patch).
# Accept either stage already applied; otherwise require a clean checkout.
# Windows PowerShell turns native stderr into a terminating error under 'Stop'.
$nfsmwPatch = "$root/mesa/mesa-switch-nfsmw.patch"
$supermanPatch = "$root/mesa/mesa-switch-superman.patch"
$ErrorActionPreference = 'Continue'
& git -C $mesa apply --reverse --check $supermanPatch 2>$null
$supermanApplied = $LASTEXITCODE -eq 0
& git -C $mesa apply --reverse --check $nfsmwPatch 2>$null
$nfsmwApplied = $LASTEXITCODE -eq 0
$ErrorActionPreference = 'Stop'
if (-not $supermanApplied) {
    if (-not $nfsmwApplied) {
        & git -C $mesa diff --quiet
        if ($LASTEXITCODE -ne 0) { throw 'Mesa has other changes; existing files were preserved.' }
        Run-Checked 'git' @('-C', $mesa, 'apply', '--check', $nfsmwPatch)
        Run-Checked 'git' @('-C', $mesa, 'apply', $nfsmwPatch)
    }
    Run-Checked 'git' @('-C', $mesa, 'apply', '--check', $supermanPatch)
    Run-Checked 'git' @('-C', $mesa, 'apply', $supermanPatch)
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
