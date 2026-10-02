import importlib.util
import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'native_report', Path(__file__).resolve().parents[1] / 'tools/switch/native-report.py')
native_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native_report)
parse_report = native_report.parse_report

# Real lines carry the SDK's timestamp/level prefix; both summary spellings must parse.
PREFIX = '[2026-10-02 12:00:00.123] [info] '


def summary(kind='interval', packets=1000, indirects=10, swaps=60, refreshes=60, draws_omitted=500,
            blocked=0, invalid=0, progress=2000, surface=58, final=False):
    return (f'{PREFIX}[sr-native] summary kind={kind} packets={packets} indirects={indirects} swaps={swaps} '
            f'refreshes={refreshes} draws_omitted={draws_omitted} blocked={blocked} invalid={invalid} '
            f'interrupts=70 vblanks=300 wptr_writes=900 progress={progress} surface_paints={surface}'
            + (' shutdown=complete' if final else '') + '\n')


def clean_log():
    return (f'{PREFIX}[sr-native] sr_renderer=native milestone=1 build=fca37bd8c980\n'
            + summary(packets=500, progress=900) + summary(packets=1000, progress=2000)
            + summary(kind='final', packets=1100, progress=2200, final=True))


class ParseReportTests(unittest.TestCase):
    def test_clean_report_needs_console_review_never_pass(self):
        report = parse_report(clean_log())
        self.assertEqual(report['status'], 'needs_console_review')
        self.assertNotIn('pass', report['status'])
        self.assertTrue(report['manual_review'])
        self.assertEqual(report['build'], 'fca37bd8c980')
        self.assertEqual(report['mode'], 'native')

    def test_refresh_does_not_hide_blocked(self):
        text = '[sr-native] packets=10 swaps=120 refreshes=120 draws_omitted=2 blocked=1 invalid=0\n'
        self.assertEqual(parse_report(text)['status'], 'blocked')

    def test_plain_contract_without_summary_prefix_parses(self):
        report = parse_report('[sr-native] packets=10 swaps=1 refreshes=1 draws_omitted=0 blocked=0 invalid=0\n')
        self.assertEqual(report['summary']['packets'], 10)

    def test_blocked_event_blocks_even_with_clean_counters(self):
        log = clean_log() + f'{PREFIX}[sr-native] BLOCKED opcode=3C at=00001000 ring_word=12 blocked=1 ' \
                            'shader_blocked=0 last_progress=5 progress_stalled_ms=4000\n'
        report = parse_report(log)
        self.assertEqual(report['status'], 'blocked')
        self.assertEqual(report['first_blocked']['opcode'], '3C')

    def test_first_blocked_event_is_kept(self):
        log = (f'{PREFIX}[sr-native] BLOCKED opcode=22 at=00000010 ring_word=1 blocked=1 shader_blocked=0 '
               'last_progress=1 progress_stalled_ms=0\n'
               f'{PREFIX}[sr-native] BLOCKED opcode=3C at=00000020 ring_word=2 blocked=2 shader_blocked=0 '
               'last_progress=1 progress_stalled_ms=9000\n')
        self.assertEqual(parse_report(log)['first_blocked']['opcode'], '22')

    def test_vulkan_failure_fails(self):
        for line in ('[sr-native] Vulkan device lost during the clear',
                     '[sr-native] unable to create the Vulkan device',
                     '[sr-native] FAILED: invalid ring pointer 00000000 size_log2 99',
                     '[sr-native] failure=vulkan clear',
                     '[sr-native] present: the clear failed; the swap stays blocked',
                     '[sr-native] presentation shutdown could not drain GPU work; resources retained',
                     'Invalid sr_renderer: nativ'):
            with self.subTest(line=line):
                self.assertEqual(parse_report(clean_log() + PREFIX + line + '\n')['status'], 'failed')

    def test_invalid_packets_fail(self):
        self.assertEqual(parse_report(clean_log() + summary(invalid=2, final=True))['status'], 'failed')

    def test_report_without_progress_is_blocked(self):
        log = (summary(packets=0, progress=0) + summary(kind='final', packets=0, progress=0, final=True))
        report = parse_report(log)
        self.assertEqual(report['status'], 'blocked')
        self.assertIn('no_progress', report['reasons'])

    def test_stalled_progress_between_summaries_is_blocked(self):
        log = (summary(packets=500, progress=900) + summary(packets=500, progress=900)
               + summary(kind='final', packets=500, progress=900, final=True))
        self.assertEqual(parse_report(log)['status'], 'blocked')

    def test_missing_shutdown_is_failed(self):
        log = f'{PREFIX}[sr-native] sr_renderer=native milestone=1 build=x\n' + summary() + summary(packets=2000, progress=4000)
        report = parse_report(log)
        self.assertEqual(report['status'], 'failed')
        self.assertIn('shutdown_missing', report['reasons'])

    def test_no_native_lines_at_all_is_failed(self):
        report = parse_report('some unrelated log\n')
        self.assertEqual(report['status'], 'failed')
        self.assertIn('no_report', report['reasons'])

    def test_xenos_run_is_not_a_native_report(self):
        report = parse_report(f'{PREFIX}[sr-native] sr_renderer=xenos milestone=1 build=abc\n')
        self.assertEqual(report['status'], 'failed')
        self.assertIn('not_native', report['reasons'])

    def test_presentation_counts_never_pass_a_blocked_run(self):
        log = summary(refreshes=5000, surface=5000, blocked=3, final=True)
        self.assertEqual(parse_report(log)['status'], 'blocked')


class CommandLineTests(unittest.TestCase):
    def test_cli_prints_json_and_does_not_touch_the_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'run.log'
            path.write_text(clean_log(), encoding='utf-8')
            before = path.read_bytes()
            out = io.StringIO()
            with redirect_stdout(out):
                code = native_report.main([str(path)])
            self.assertEqual(code, 0)
            self.assertIn('needs_console_review', out.getvalue())
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(sorted(p.name for p in Path(tmp).iterdir()), ['run.log'])

    def test_cli_exit_code_is_nonzero_when_not_reviewable(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'run.log'
            path.write_text('nothing\n', encoding='utf-8')
            with redirect_stdout(io.StringIO()):
                self.assertNotEqual(native_report.main([str(path)]), 0)

    def test_cli_missing_file_is_an_error(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(native_report.main(['does-not-exist.log']), 2)


if __name__ == '__main__':
    unittest.main()
