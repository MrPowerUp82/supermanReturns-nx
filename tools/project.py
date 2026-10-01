"""Local game preparation, codegen, dependency checks and SD package creation."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
XEX_SHA256 = 'c8f243acd99de9a91f5ae4f409721c0e954e3d5eb96861419d3da07b8106db2b'
DEFAULT_SOURCE = ROOT.parent / 'superman_returns_recomp'


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def validate_game(game: Path, require_data: bool = True) -> Path:
    xex = game / 'default.xex'
    if not xex.is_file():
        raise ValueError(f'Missing {xex}')
    if sha256(xex) != XEX_SHA256:
        raise ValueError('Unsupported default.xex: this manifest supports only SHA-256 ' + XEX_SHA256)
    if require_data and (not (game / 'DATA').is_dir() or
                         len(list((game / 'DATA').glob('*.AST'))) != 12):
        raise ValueError(f'Expected the 12 original DATA/*.AST files in {game}')
    return xex


def prepare(args):
    source = args.source.resolve()
    xex = validate_game(source / 'game')
    staged = ROOT / 'assets/game_root'
    staged.mkdir(parents=True, exist_ok=True)
    shutil.copy2(xex, staged / 'default.xex')
    # Do not duplicate several GB of AST data just to run the recompiler.
    (ROOT / 'assets/source.json').write_text(json.dumps({
        'game_root': str(source / 'game'), 'sha256': XEX_SHA256
    }, indent=2), encoding='utf-8')
    print(f'Prepared {staged / "default.xex"}; DATA stays in {source / "game/DATA"}')


def codegen(args):
    validate_game(ROOT / 'assets/game_root', require_data=False)
    rexglue = args.rexglue.resolve()
    if not rexglue.is_file():
        raise ValueError(f'Host recompiler not found: {rexglue}')
    subprocess.run([str(rexglue), 'codegen', 'superman_returns_manifest.toml'],
                   cwd=ROOT / 'app', check=True)
    generated = ROOT / 'app/generated/default'
    if not (generated / 'sources.cmake').is_file():
        raise ValueError('Codegen produced no sources.cmake')
    for path in generated.glob('*.cpp'):
        if 'REX_FATAL(' in path.read_text(encoding='utf-8'):
            raise ValueError(f'Unresolved recompiler failure in {path}')
    (generated / 'manifest.sha256').write_text(
        sha256(ROOT / 'app/superman_returns_manifest.toml') + '\n', encoding='ascii')
    print('Codegen complete; no REX_FATAL stubs. ARM64 compilation and boot remain to be tested.')


def check(args):
    devkit = args.devkitpro or Path(os.environ.get('DEVKITPRO', 'C:/devkitPro' if os.name == 'nt' else '/opt/devkitpro'))
    suffix = '.exe' if os.name == 'nt' else ''
    checks = {
        'devkitA64': (devkit / f'devkitA64/bin/aarch64-none-elf-g++{suffix}').is_file(),
        'libnx': (devkit / 'libnx/switch.specs').is_file(),
        'nacptool': (devkit / f'tools/bin/nacptool{suffix}').is_file(),
        'elf2nro': (devkit / f'tools/bin/elf2nro{suffix}').is_file(),
        'cmake': bool(shutil.which('cmake')),
        'ninja': bool(shutil.which('ninja')),
        'Mesa NVK': bool(args.mesa_sdk and (args.mesa_sdk / 'lib/libvulkan.a').is_file()),
        'generated code': (ROOT / 'app/generated/default/sources.cmake').is_file(),
    }
    deps = ['cli11', 'libmspack', 'FFmpeg', 'tomlplusplus', 'simde', 'xxHash',
            'spdlog', 'fmt', 'utfcpp', 'imgui', 'vulkan-headers',
            'vulkan-memory-allocator', 'spirv-headers', 'spirv-tools', 'glslang']
    for name in deps:
        directory = ROOT / 'sdk/thirdparty' / name
        checks[f'dependency {name}'] = ((directory / '.rex-dependency-complete').is_file()
                                       or (directory / '.git').exists())
    for name, available in checks.items():
        print(f'{"OK" if available else "MISSING"}: {name}')
    return 0 if all(checks.values()) else 1


def package(args):
    nro = args.nro.resolve()
    if not nro.is_file() or nro.stat().st_size < 20:
        raise ValueError(f'Missing or incomplete NRO: {nro}')
    with nro.open('rb') as stream:
        stream.seek(16)
        if stream.read(4) != b'NRO0':
            raise ValueError('Input is not a Nintendo Switch NRO (invalid magic)')
    if args.game_root:
        game = args.game_root.resolve()
    else:
        state = ROOT / 'assets/source.json'
        if not state.is_file():
            raise ValueError('Run prepare or pass --game-root')
        game = Path(json.loads(state.read_text(encoding='utf-8'))['game_root'])
    validate_game(game)
    target = args.output.resolve()
    # Refuse overwrites and source nesting: do not recursively copy into game data.
    if target.exists():
        raise ValueError(f'Output already exists; choose a new --output: {target}')
    if target == game or game in target.parents or target in game.parents:
        raise ValueError('Package output must be separate from the source game directory')
    target.mkdir(parents=True)
    shutil.copy2(nro, target / 'superman_returns.nro')
    shutil.copy2(ROOT / 'config/superman_returns.toml', target / 'superman_returns.toml')
    shutil.copytree(game, target / 'game_root')
    print(f'Local package: {target}. Copy this folder to sdmc:/switch/superman-returns-nx/')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    cmd = commands.add_parser('prepare')
    cmd.add_argument('--source', type=Path, default=DEFAULT_SOURCE)
    cmd.set_defaults(action=prepare)
    cmd = commands.add_parser('codegen')
    cmd.add_argument('--rexglue', type=Path, required=True)
    cmd.set_defaults(action=codegen)
    cmd = commands.add_parser('check')
    cmd.add_argument('--devkitpro', type=Path)
    cmd.add_argument('--mesa-sdk', type=Path)
    cmd.set_defaults(action=check)
    cmd = commands.add_parser('package')
    cmd.add_argument('--nro', type=Path, default=ROOT / 'app/out/switch/superman_returns.nro')
    cmd.add_argument('--game-root', type=Path)
    cmd.add_argument('--output', type=Path, default=ROOT / 'dist/superman-returns-nx')
    cmd.set_defaults(action=package)
    args = parser.parse_args(argv)
    try:
        return args.action(args) or 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
