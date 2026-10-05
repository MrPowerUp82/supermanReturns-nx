import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools' / 'vkshaders'
sys.path.insert(0, str(TOOLS))
import build_pack  # noqa: E402
import srpaths  # noqa: E402


def container(vertex=True, alu=0x000F0000, virtual=72, physical=24):
    """A fabricated container: one control-flow slot and one `r0 = r0 + r0` ALU instruction."""
    data = bytearray(virtual + physical)
    struct.pack_into('>7I', data, 0, 0x102A1100 | int(vertex), virtual, physical, 24, 36, 0, 24)
    struct.pack_into('>2I', data, virtual, 0x1001, 0x2000)
    struct.pack_into('>3I', data, virtual + 12, alu, 0, 0xE0000000)
    return bytes(data)


class PackFormatTests(unittest.TestCase):
    def test_fnv_matches_reference_values(self):
        self.assertEqual(build_pack.fnv1a64(b''), 14695981039346656037)
        self.assertEqual(build_pack.fnv1a64(b'a'), 0xAF63DC4C8601EC8C)
        self.assertEqual(build_pack.fnv1a64(b'foobar'), 0x85944171F73967E8)

    def test_container_info_checks_stage_and_exact_size(self):
        stage, data = build_pack.container_info(container(True) + b'padding', 'a.bin')
        self.assertEqual(stage, 'vs')
        self.assertEqual(len(data), 96)  # trailing padding is not part of the container
        self.assertEqual(build_pack.container_info(container(False), 'b.bin')[0], 'ps')
        for bad in (b'x' * 8, container()[:-1], b'\0' * 96):
            with self.assertRaises(ValueError):
                build_pack.container_info(bad, 'bad.bin')

    def test_pack_layout_is_sorted_and_self_checking(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'p.srvk'
            blob_a, blob_b = b'A' * 24, b'B' * 40
            build_pack.write_pack([(9, 96, 'ps', blob_b), (3, 96, 'vs', blob_a)], out)
            data = out.read_bytes()
            self.assertEqual(data[:8], b'SRVKPK01')
            version, abi, count, reserved, digest = struct.unpack_from('<IIIIQ', data, 8)
            self.assertEqual((version, abi, count, reserved), (1, 1, 2, 0))
            self.assertEqual(digest, build_pack.fnv1a64(data[32:]))
            first = struct.unpack_from('<QIIQII', data, 32)
            second = struct.unpack_from('<QIIQII', data, 64)
            self.assertEqual((first[0], first[2]), (3, 0))
            self.assertEqual((second[0], second[2]), (9, 1))
            self.assertEqual(data[first[3]:first[3] + first[4]], blob_a)
            self.assertEqual(data[second[3]:second[3] + second[4]], blob_b)
            self.assertEqual(second[3], first[3] + first[4])  # blobs are contiguous in table order


def toolchain_ready():
    return all(p.is_file() for p in (srpaths.TRANSLATOR, srpaths.SHADER_COMMON, srpaths.DXC))


@unittest.skipUnless(toolchain_ready(), 'run tools/vkshaders/fetch_xenosrecomp.py and build the emitter first')
class EndToEndTests(unittest.TestCase):
    def test_translated_vertex_shader_lands_in_a_pack_the_console_loader_accepts(self):
        env = dict(os.environ)
        dxc_lib = srpaths.DXC.parent.parent / 'lib'
        if dxc_lib.is_dir():
            env['LD_LIBRARY_PATH'] = f"{dxc_lib}:{env.get('LD_LIBRARY_PATH', '')}"
        with tempfile.TemporaryDirectory() as tmp:
            containers = Path(tmp) / 'containers'
            containers.mkdir()
            (containers / 'a.vs.bin').write_bytes(container(True))
            (containers / 'junk.bin').write_bytes(b'not a container')
            out = Path(tmp) / 'p.srvk'
            old = dict(os.environ)
            os.environ.update(env)
            try:
                code = build_pack.main(['--containers', str(containers), '--output', str(out), '--jobs', '1'])
            finally:
                os.environ.clear()
                os.environ.update(old)
            self.assertEqual(code, 0)
            data = out.read_bytes()
            self.assertEqual(struct.unpack_from('<I', data, 16)[0], 1)
            checker = os.environ.get('SR_PCVK_PACK_CHECK')
            if checker:
                result = subprocess.run([checker, str(out)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('1 entries, 0 failed', result.stdout)


if __name__ == '__main__':
    unittest.main()
