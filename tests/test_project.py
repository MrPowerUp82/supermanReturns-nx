import importlib.util
from pathlib import Path
import tempfile
import struct
import unittest
from unittest.mock import patch
from argparse import Namespace

spec = importlib.util.spec_from_file_location('project', Path(__file__).resolve().parents[1] / 'tools/project.py')
project = importlib.util.module_from_spec(spec)
spec.loader.exec_module(project)


class PackageValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'game'
        (self.game / 'DATA').mkdir(parents=True)
        (self.game / 'default.xex').write_bytes(b'test executable')
        for i in range(12):
            (self.game / 'DATA' / f'{i}.AST').write_bytes(b'test asset')
        self.hash = project.sha256(self.game / 'default.xex')
        self.nro = self.root / 'test.nro'
        self.nro.write_bytes(bytes(16) + b'NRO0' + bytes(12))

    def args(self, output=None):
        return Namespace(nro=self.nro, game_root=self.game,
                         output=output or self.root / 'package')

    def test_rejects_wrong_edition(self):
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            project.validate_game(self.game)

    def test_rejects_missing_ast(self):
        (self.game / 'DATA/0.AST').unlink()
        with patch.object(project, 'XEX_SHA256', self.hash):
            with self.assertRaisesRegex(ValueError, '12 original'):
                project.validate_game(self.game)

    def test_codegen_does_not_need_staged_ast(self):
        with patch.object(project, 'XEX_SHA256', self.hash):
            project.validate_game(self.game, require_data=False)

    def test_rejects_fake_nro(self):
        self.nro.write_bytes(bytes(32))
        with self.assertRaisesRegex(ValueError, 'invalid magic'):
            project.package(self.args())
        self.assertFalse((self.root / 'package').exists())

    def test_refuses_overwrite(self):
        output = self.root / 'package'
        output.mkdir()
        with patch.object(project, 'XEX_SHA256', self.hash):
            with self.assertRaisesRegex(ValueError, 'already exists'):
                project.package(self.args(output))

    def test_refuses_copy_into_source(self):
        with patch.object(project, 'XEX_SHA256', self.hash):
            with self.assertRaisesRegex(ValueError, 'separate'):
                project.package(self.args(self.game / 'package'))

    def test_packages_game_and_configuration(self):
        with patch.object(project, 'XEX_SHA256', self.hash):
            project.package(self.args())
        output = self.root / 'package'
        self.assertEqual((output / 'superman_returns.nro').read_bytes(), self.nro.read_bytes())
        self.assertEqual(len(list((output / 'game_root/DATA').glob('*.AST'))), 12)
        self.assertTrue((output / 'superman_returns.toml').is_file())

    def test_shader_library_is_copied(self):
        shader = self.root / 'local.srsp'
        shader.write_bytes(struct.pack('<8sIIQ', b'SRSSPV\0\0', 1, 1, 0))
        args = self.args()
        args.shader_library = shader
        with patch.object(project, 'XEX_SHA256', self.hash):
            project.package(args)
        self.assertEqual((args.output / 'superman_returns_shaders.srsp').read_bytes(), shader.read_bytes())

    def test_rejects_other_game_shader_library_before_copy(self):
        shader = self.root / 'other.srsp'
        shader.write_bytes(struct.pack('<8sIIQ', b'NFSSPV\0\0', 1, 1, 0))
        args = self.args()
        args.shader_library = shader
        with patch.object(project, 'XEX_SHA256', self.hash):
            with self.assertRaisesRegex(ValueError, 'shader library header'):
                project.package(args)
        self.assertFalse(args.output.exists())

    def test_vulkan_shader_pack_is_validated_and_copied(self):
        pack = self.root / 'local.srvk'
        pack.write_bytes(struct.pack('<8sIIIIQ', b'SRVKPK01', 1, 1, 1, 0, 0))
        args = self.args()
        args.vulkan_shader_pack = pack
        with patch.object(project, 'XEX_SHA256', self.hash):
            project.package(args)
        self.assertEqual((args.output / 'superman_returns_vulkan_shaders.srvk').read_bytes(), pack.read_bytes())

    def test_rejects_wrong_vulkan_shader_pack_before_copy(self):
        for header in (struct.pack('<8sIIIIQ', b'SRVKPK02', 1, 1, 1, 0, 0),
                       struct.pack('<8sIIIIQ', b'SRVKPK01', 1, 2, 1, 0, 0),
                       struct.pack('<8sIIIIQ', b'SRVKPK01', 1, 1, 0, 0, 0)):
            pack = self.root / 'bad.srvk'
            pack.write_bytes(header)
            args = self.args()
            args.vulkan_shader_pack = pack
            with patch.object(project, 'XEX_SHA256', self.hash):
                with self.assertRaisesRegex(ValueError, 'Vulkan shader pack'):
                    project.package(args)
            self.assertFalse(args.output.exists())


if __name__ == '__main__':
    unittest.main()
