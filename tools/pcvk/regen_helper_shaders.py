#!/usr/bin/env python3
"""Regenerates (or checks) the SPIR-V headers of the project-owned Vulkan helper shaders.

The Switch build has no DXC, so the headers under
app/src/pcvk/graphics/vulkan/generated/ are committed. They are small, derived only
from the HLSL next to them (no game data), and `--check` fails if they are stale.

    python tools/pcvk/regen_helper_shaders.py --dxc .tools/dxc/bin/dxc [--check]
"""
import argparse, os, pathlib, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
VK = ROOT / "app/src/pcvk/graphics/vulkan"
OUT = VK / "generated"
# (output header, HLSL source, profile, entry, symbol)
SHADERS = [
    ("depth_resolve_shader.h", "depth_resolve.hlsl", "cs_6_0", "CSMain", "depth_resolve_shader"),
    ("edram_alias_shader.h", "edram_alias_compute.hlsl", "cs_6_0", "CSMain", "edram_alias_shader"),
    ("composition_vs.h", "composition.hlsl", "vs_6_0", "VSMain", "composition_vs"),
    ("composition_ps.h", "composition.hlsl", "ps_6_0", "PSMain", "composition_ps"),
]


def embed(spv: bytes, name: str) -> str:
    if len(spv) % 4 or len(spv) < 20:
        raise SystemExit("invalid helper SPIR-V length")
    words = [int.from_bytes(spv[i:i + 4], "little") for i in range(0, len(spv), 4)]
    lines = "".join(f"0x{w:08x}u,\n" for w in words)
    return ("#pragma once\n#include <cstdint>\nnamespace superman_returns::graphics::vulkan {\n"
            f"inline constexpr uint32_t {name}[]={{\n{lines}}};\n}}\n")


def build(dxc: str, source: str, profile: str, entry: str) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "out.spv"
        subprocess.run([dxc, "-spirv", "-fspv-target-env=vulkan1.1", "-T", profile, "-E", entry,
                        "-Fo", str(out), str(VK / "shaders" / source)], check=True)
        return out.read_bytes()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dxc", default=os.environ.get("DXC", "dxc"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    OUT.mkdir(exist_ok=True)
    stale = []
    for header, source, profile, entry, symbol in SHADERS:
        text = embed(build(args.dxc, source, profile, entry), symbol)
        target = OUT / header
        if args.check:
            if not target.exists() or target.read_text() != text:
                stale.append(header)
        else:
            target.write_text(text)
    if stale:
        print("stale generated headers: " + ", ".join(stale), file=sys.stderr)
        return 1
    print("helper shaders up to date" if args.check else "helper shaders regenerated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
