import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / 'shaders' / 'import_pc_corpus.py'


def container(vertex=True, ctab=0):
    # Fabricated header, shader descriptor and one empty physical instruction.
    data = bytearray(84)
    struct.pack_into('>7I', data, 0, 0x102A1100 | int(vertex), 72, 12, 0, ctab, 0, 36)
    struct.pack_into('>2I', data, 36, 0, 12)
    return bytes(data)


class CorpusImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('sr_corpus_import', MODULE_PATH)
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def test_empty_constant_table_accepted_and_stage_checked(self):
        self.assertEqual(self.module.validate_container(container(), 'a.vs.bin'), 'vs')
        with self.assertRaises(ValueError):
            self.module.validate_container(container(), 'a.ps.bin')
        with self.assertRaises(ValueError):
            self.module.validate_container(container()[:-1], 'a.vs.bin')
        with self.assertRaises(ValueError):
            self.module.validate_container(container(ctab=71), 'a.vs.bin')

    def test_import_deduplicates_bytes_preserves_reference_and_records_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / 'pc'
            raw = reference / 'artifacts/shaders/raw'
            raw.mkdir(parents=True)
            original = container()
            (raw / 'a.vs.bin').write_bytes(original)
            (raw / 'duplicate.vs.bin').write_bytes(original)
            (raw / 'b.ps.bin').write_bytes(container(False))
            output = root / 'new'
            manifest = self.module.import_corpus(reference, output)
            self.assertEqual(len(manifest['entries']), 2)
            self.assertEqual(len(list((output / 'containers').glob('*.bin'))), 2)
            self.assertEqual((raw / 'a.vs.bin').read_bytes(), original)
            self.assertEqual(json.loads((output / 'manifest.json').read_text()), manifest)
            with self.assertRaises(FileExistsError):
                self.module.import_corpus(reference, output)
            with self.assertRaises(ValueError):
                self.module.import_corpus(reference, reference / 'forbidden')

    def test_invalid_input_creates_no_output(self):
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / 'pc'
            raw = reference / 'artifacts/shaders/raw'
            raw.mkdir(parents=True)
            (raw / 'bad.vs.bin').write_bytes(b'bad')
            output = Path(directory) / 'new'
            with self.assertRaises(ValueError):
                self.module.import_corpus(reference, output)
            self.assertFalse(output.exists())
