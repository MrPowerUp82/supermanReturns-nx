"""Import validated local PC containers for offline Switch SPIR-V generation.

Inputs and outputs contain private game data; outputs belong in ignored out/.
The reference is read-only and an existing output directory is never replaced.
This is a new structural parser, not an import of native-kit implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def validate_container(data: bytes, filename: str) -> str:
    if len(data) < 36 or len(data) > 65536:
        raise ValueError(f'{filename}: invalid container extent')
    flags, virtual, physical, _, ctab, literals, shader = struct.unpack_from('>7I', data)
    if flags not in (0x102A1100, 0x102A1101):
        raise ValueError(f'{filename}: unsupported signature')
    stage = 'vs' if flags & 1 else 'ps'
    if filename.endswith(('.vs.bin', '.ps.bin')) and not filename.endswith(f'.{stage}.bin'):
        raise ValueError(f'{filename}: stage differs from header')
    if virtual < 36 or not physical or virtual + physical != len(data):
        raise ValueError(f'{filename}: inconsistent virtual/physical sizes')

    def in_virtual(offset, size):
        return 36 <= offset <= virtual and 0 <= size <= virtual - offset

    if not in_virtual(shader, 36 if stage == 'vs' else 32):
        raise ValueError(f'{filename}: shader descriptor outside virtual part')
    code_offset, code_size = struct.unpack_from('>2I', data, shader)
    if not code_size or code_size % 12 or code_offset > physical or code_size > physical - code_offset:
        raise ValueError(f'{filename}: invalid instruction extent')
    if ctab:
        if not in_virtual(ctab, 32):
            raise ValueError(f'{filename}: constant table outside virtual part')
        count, info = struct.unpack_from('>2I', data, ctab + 16)
        if not in_virtual(ctab + 4 + info, count * 20):
            raise ValueError(f'{filename}: constant entries outside virtual part')
    if literals and not in_virtual(literals, 24):
        raise ValueError(f'{filename}: literal table outside virtual part')
    return stage


def import_corpus(reference: Path, output: Path) -> dict:
    reference = reference.resolve(strict=True)
    output = output.resolve()
    if output == reference or reference in output.parents:
        raise ValueError('Output must be outside the reference checkout')
    if output.exists():
        raise FileExistsError(output)
    raw = reference / 'artifacts/shaders/raw'
    candidates = sorted(raw.glob('*.bin'))
    if not candidates:
        raise ValueError(f'No raw containers in {raw}')
    unique = {}
    for candidate in candidates:
        if reference not in candidate.resolve(strict=True).parents:
            raise ValueError(f'Input escapes reference: {candidate}')
        data = candidate.read_bytes()
        stage = validate_container(data, candidate.name)
        digest = hashlib.sha256(data).hexdigest()
        key = (stage, digest)
        if key in unique and unique[key]['data'] != data:
            raise ValueError('Container digest collision')
        record = unique.setdefault(key, {'data': data, 'sources': []})
        record['sources'].append(candidate.relative_to(reference).as_posix())
    revision = subprocess.run(['git', '-C', str(reference), 'rev-parse', 'HEAD'],
                              capture_output=True, text=True)
    manifest = {'version': 1, 'reference': str(reference),
                'reference_revision': revision.stdout.strip() if revision.returncode == 0 else None,
                'importer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'entries': []}
    # Validate all inputs before creating any output; mkdir without exist_ok
    # also refuses a directory created concurrently after the earlier check.
    (output / 'containers').mkdir(parents=True, exist_ok=False)
    for (stage, digest), record in sorted(unique.items()):
        name = f"{'v' if stage == 'vs' else 'p'}_{digest}.bin"
        with (output / 'containers' / name).open('xb') as destination:
            destination.write(record['data'])
        manifest['entries'].append({'stage': stage, 'sha256': digest,
                                    'size': len(record['data']), 'file': f'containers/{name}',
                                    'sources': record['sources']})
    with (output / 'manifest.json').open('x', encoding='utf-8') as destination:
        json.dump(manifest, destination, indent=2)
        destination.write('\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = import_corpus(args.reference, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Import failed: {error}\n')
    print(f"Imported {len(manifest['entries'])} distinct containers into {args.output}")


if __name__ == '__main__':
    main()
