"""Build scripts run inside Linux containers even when the checkout comes from
Windows. A CRLF or a syntax error only shows up there, after a long image build."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = (sorted((ROOT / 'tools').rglob('*.sh')) + sorted((ROOT / 'shaders').glob('*.sh')) +
           sorted((ROOT / 'emulators').rglob('*.sh')))


class ShellScriptTests(unittest.TestCase):
    def test_scripts_exist(self):
        names = {p.name for p in SCRIPTS}
        for expected in ('build-docker.sh', 'build-mesa.sh', 'build-vk-probe.sh',
                         'run-sudachi-headless.sh', 'test_registry.sh', 'build-linux.sh'):
            self.assertIn(expected, names)

    def test_scripts_have_unix_line_endings(self):
        for script in SCRIPTS:
            with self.subTest(script=script.relative_to(ROOT)):
                self.assertNotIn(b'\r\n', script.read_bytes())

    @unittest.skipIf(shutil.which('bash') is None, 'bash is not available')
    def test_scripts_parse(self):
        for script in SCRIPTS:
            with self.subTest(script=script.relative_to(ROOT)):
                result = subprocess.run(['bash', '-n', str(script)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_probe_configs_use_known_keys(self):
        # vk_probe.cpp ignores unknown keys with a log line; catch typos here.
        known = {'env', 'stop_after', 'skip', 'frames', 'guest_memory'}
        for cfg in sorted((ROOT / 'tools/switch/vk-probe/configs').glob('*.cfg')):
            for line in cfg.read_text().splitlines():
                if line.strip() and not line.startswith('#'):
                    with self.subTest(cfg=cfg.name, line=line):
                        self.assertIn(line.split(' ', 1)[0], known)

    def test_superman_mesa_patch_is_separate(self):
        # mesa-switch-superman.patch applies on top of the NFSMW patch; it must
        # stay a separate file so the vendored patch keeps its provenance.
        superman = (ROOT / 'mesa/mesa-switch-superman.patch').read_text()
        self.assertIn('NOUVEAU_HORIZON_EARLY_BIND', superman)
        self.assertNotIn('NOUVEAU_HORIZON_EARLY_BIND',
                         (ROOT / 'mesa/mesa-switch-nfsmw.patch').read_text())

    def test_probe_shaders_are_spirv(self):
        # The embedded SPIR-V must start with the magic word; regenerate it with
        # glslangValidator -V --target-env vulkan1.1 -x as documented in docs/vk-probe.md.
        for name in ('probe.vert.inc', 'probe.frag.inc'):
            text = (ROOT / 'tools/switch/vk-probe' / name).read_text()
            words = [w.strip() for w in text.replace('\n', ',').split(',')
                     if w.strip().startswith('0x')]
            self.assertGreater(len(words), 5)
            self.assertEqual(words[0], '0x07230203')


if __name__ == '__main__':
    unittest.main()
