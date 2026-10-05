#!/usr/bin/env python3
"""Builds the offline Vulkan shader pack the Switch build loads (superman_returns_vulkan_shaders.srvk).

Every guest shader container is translated with the patched XenosRecomp emitter (HLSL for the
`sr-vulkan-buffers-v1` ABI), compiled to SPIR-V with DXC, inspected and stored as the same
"SVR3" result blob the PC runtime service decodes. The pack is keyed by FNV-1a 64 of the
container bytes, so the console never needs the translator or a compiler.

    python tools/vkshaders/fetch_xenosrecomp.py
    cmake -S tools/vkshaders/xenosrecomp -B .tools/xenosrecomp/build -G Ninja -DBUILD_TESTING=OFF
    cmake --build .tools/xenosrecomp/build
    python tools/vkshaders/build_pack.py --containers <dir with *.bin> --output superman_returns_vulkan_shaders.srvk

Containers come from a game run with the PC project's `--sr_native_dump_shader_dir`, from its
`artifacts/shaders/raw`, or from this project's `sr_dump_shader_containers` option. The output
holds code derived from the game: keep it out of version control.
"""
import argparse
import concurrent.futures
import json
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compile_vulkan import Toolchain, compile_shader  # noqa: E402
from runtime_vulkan_shader import failure, ready  # noqa: E402
from srpaths import DXC, SHADER_COMMON, TRANSLATOR  # noqa: E402
from vulkan_contract import ABI  # noqa: E402

MAGIC = b"SRVKPK01"
VERSION = 1
BINDING_ABI_VERSION = 1
HEADER = 32
ENTRY = 32
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK = (1 << 64) - 1


def fnv1a64(data: bytes, seed: int = FNV_OFFSET) -> int:
    h = seed
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def container_info(raw: bytes, name: str):
    """(stage, exact container bytes) of a guest shader container, or ValueError."""
    if len(raw) < 36:
        raise ValueError(f"{name}: shorter than a container header")
    flags, virtual_size, physical_size = struct.unpack_from(">3I", raw, 0)
    if (flags & ~1) != 0x102A1100:
        raise ValueError(f"{name}: not a shader container (flags {flags:08X})")
    total = virtual_size + physical_size
    if virtual_size < 36 or not physical_size or total > 65536 or total > len(raw):
        raise ValueError(f"{name}: invalid container sizes {virtual_size}/{physical_size}")
    return ("vs" if flags & 1 else "ps"), raw[:total]


def write_pack(entries, output: Path) -> None:
    """entries: iterable of (key, container_size, stage 'vs'|'ps', blob). Layout: shader_pack.h."""
    rows = sorted(entries, key=lambda e: (e[0], 0 if e[2] == "vs" else 1))
    table = bytearray()
    blobs = bytearray()
    offset = HEADER + len(rows) * ENTRY
    for key, size, stage, blob in rows:
        table += struct.pack("<QIIQII", key, size, 0 if stage == "vs" else 1, offset + len(blobs), len(blob), 0)
        blobs += blob
    payload = bytes(table) + bytes(blobs)
    header = MAGIC + struct.pack("<IIIIQ", VERSION, BINDING_ABI_VERSION, len(rows), 0, fnv1a64(payload))
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".pending")
    temporary.write_bytes(header + payload)
    temporary.replace(output)


def translate(item, cache: Path, tools: Toolchain):
    name, stage, container = item
    scratch = cache / "containers"
    scratch.mkdir(parents=True, exist_ok=True)
    path = scratch / f"{fnv1a64(container):016X}.{stage}.bin"
    path.write_bytes(container)
    result = compile_shader(path, stage, cache, tools)
    if result.status != "ready":
        return name, stage, container, None, result.diagnostic or "translation failed"
    try:
        blob = ready(stage, result.binary_path.read_bytes())
    except (ValueError, OSError) as e:
        return name, stage, container, None, str(e)
    return name, stage, container, blob, ""


def collect(directories):
    seen = {}
    problems = []
    for directory in directories:
        for path in sorted(Path(directory).rglob("*.bin")):
            try:
                stage, container = container_info(path.read_bytes(), path.name)
            except ValueError as e:
                problems.append(str(e))
                continue
            seen.setdefault((stage, container), path.name)
    return [(name, stage, container) for (stage, container), name in seen.items()], problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--containers", nargs="+", required=True, type=Path, help="directories with *.bin containers")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--report", type=Path, help="JSON report (default: <output>.report.json)")
    ap.add_argument("--cache", type=Path, help="translation cache (default: a temporary directory)")
    ap.add_argument("--emitter", type=Path, default=TRANSLATOR)
    ap.add_argument("--common", type=Path, default=SHADER_COMMON)
    ap.add_argument("--dxc", type=Path, default=DXC)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--allow-failures", action="store_true",
                    help="write the pack even when some containers do not translate (the console skips those draws)")
    args = ap.parse_args(argv)
    if not 1 <= args.jobs <= 32:
        ap.error("--jobs must be 1..32")
    for tool in (args.emitter, args.common, args.dxc):
        if not tool.is_file():
            ap.error(f"missing tool: {tool} (see the module docstring)")
    if args.output.exists() and not args.output.is_file():
        ap.error("--output must be a file path")
    items, problems = collect(args.containers)
    if not items:
        ap.error("no valid shader containers found; an empty pack is not useful")
    tools = Toolchain(args.emitter, args.common, args.dxc)
    with tempfile.TemporaryDirectory(prefix="vkpack-") as scratch:
        cache = args.cache or Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda item: translate(item, cache, tools), items))
    entries, rows = [], []
    for name, stage, container, blob, diagnostic in results:
        row = {"container": name, "stage": stage, "bytes": len(container),
               "key": f"{fnv1a64(container):016X}", "status": "ready" if blob else "failed"}
        if blob:
            entries.append((fnv1a64(container), len(container), stage, blob))
        else:
            row["diagnostic"] = diagnostic[:600]
        rows.append(row)
    failed = [r for r in rows if r["status"] != "ready"]
    report = {"abi": ABI, "containers": len(rows), "ready": len(entries), "failed": len(failed),
              "skipped_inputs": problems, "shaders": rows}
    report_path = args.report or args.output.with_suffix(args.output.suffix + ".report.json")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"{len(entries)}/{len(rows)} containers translated ({len(failed)} failed, {len(problems)} inputs skipped)")
    for row in failed:
        print(f"  FAILED {row['container']} ({row['stage']}): {row['diagnostic'][:200]}")
    if failed and not args.allow_failures:
        print("no pack written (use --allow-failures to write it anyway)", file=sys.stderr)
        return 1
    if not entries:
        return 1
    write_pack(entries, args.output)
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
