#!/usr/bin/env python3
"""Reads a native-renderer boot log and classifies the run.

The result is never "pass": observing that the game really progressed (audio, boot events,
a clean exit requested by the user) is a manual step, so the best outcome is
"needs_console_review". The tool only reads the log it is given; it does not touch the SD
card or the configuration and publishes nothing.

    python tools/switch/native-report.py [--expect-build REV] LOG [LOG ...]
"""
import json
import re
import sys
from pathlib import Path

TAG = '[sr-native]'
KEY_VALUE = re.compile(r'([A-Za-z_]+)=(\S+)')
SUMMARY_KEYS = ('packets', 'swaps', 'refreshes', 'draws_omitted', 'blocked', 'invalid')
# Anything here means the native path itself failed (not merely that it is blocked).
FAILURE_PATTERNS = (
    re.compile(r'\bFAILED\b'),
    re.compile(r'\bfailure='),
    re.compile(r'device lost', re.I),
    re.compile(r'unable to create'),
    re.compile(r'could not drain'),
    re.compile(r'clear failed'),
    re.compile(r'unable to register'),
    re.compile(r'belongs to another guest memory'),
    re.compile(r'shutdown=incomplete'),
    re.compile(r'invalid renderer configuration'),
)
MANUAL_CHECKLIST = (
    'guest progress observed (boot events, audio continuing) for the 60 s window',
    'screen shows the expected black image',
    'user-requested exit ended the application with no stuck workers',
    'a Xenos run with the same configuration still behaves as before',
)


def _native_lines(text):
    for line in text.splitlines():
        position = line.find(TAG)
        if position >= 0:
            yield line[position + len(TAG):].strip()


def _fields(line):
    return dict(KEY_VALUE.findall(line))


def _is_summary(line):
    fields = _fields(line)
    return all(key in fields for key in SUMMARY_KEYS)


def _to_summary(line):
    fields = _fields(line)
    summary = {key: int(fields[key]) for key in SUMMARY_KEYS}
    for key in ('interrupts', 'vblanks', 'wptr_writes', 'progress', 'surface_paints'):
        if key in fields and fields[key].isdigit():
            summary[key] = int(fields[key])
    summary['kind'] = fields.get('kind', '')
    summary['shutdown_complete'] = 'shutdown=complete' in line
    return summary


def _last_run(text):
    """A log can hold several runs (the console appends); only the last one is judged."""
    lines = text.splitlines()
    start = None
    for index, line in enumerate(lines):
        if TAG in line and 'sr_renderer=' in line:
            start = index
    return chr(10).join(lines if start is None else lines[start:])


def parse_report(text, expect_build=None):
    """Returns a dict with status "blocked" | "failed" | "needs_console_review"."""
    text = _last_run(text)
    lines = list(_native_lines(text))
    summaries = [_to_summary(line) for line in lines if _is_summary(line)]
    mode = build = None
    for line in lines:
        fields = _fields(line)
        if 'sr_renderer' in fields:
            mode, build = fields['sr_renderer'], fields.get('build')
    blocked_events = []
    shader_rejects = []
    failures = []
    stall_lines = []
    for line in lines:
        if line.startswith('BLOCKED'):
            blocked_events.append({'line': line, **_fields(line)})
        elif line.startswith('SHADER rejected'):
            reason = re.search(r'reason=(.*?) first_words=', line)
            shader_rejects.append({**_fields(line), 'line': line, 'reason': reason.group(1) if reason else ''})
        elif 'fence wait timed out' in line:
            stall_lines.append(line)
        elif any(pattern.search(line) for pattern in FAILURE_PATTERNS):
            failures.append(line)
    if 'Invalid sr_renderer' in text:  # Logged by the app without the sr-native tag.
        failures.append('Invalid sr_renderer')
    last = summaries[-1] if summaries else None

    failed, blocked = [], []
    if not lines and not failures:
        failed.append('no_report')
    if mode is not None and mode != 'native':
        failed.append('not_native')
    if failures:
        failed.append('failure_event')
    if last and last['invalid'] > 0:
        failed.append('invalid_packets')
    if expect_build and build and build != expect_build:
        failed.append('build_mismatch')

    if last and last['blocked'] > 0:
        blocked.append('blocked_counter')
    if blocked_events:
        blocked.append('blocked_event')
    if shader_rejects:
        blocked.append('shader_rejected')
    if stall_lines:
        blocked.append('present_stall')
    if last and (last['packets'] == 0 or last.get('progress', 1) == 0):
        blocked.append('no_progress')
    if len(summaries) >= 2 and 'progress' in summaries[-1] and 'progress' in summaries[-2]             and summaries[-1]['progress'] <= summaries[-2]['progress']:
        blocked.append('stalled_progress')
    # One swap may legitimately be in flight when the final summary is taken.
    if last and last['swaps'] - last['refreshes'] > 1:
        blocked.append('swaps_not_presented')
    shutdown_missing = bool(lines) and not any(s['shutdown_complete'] for s in summaries)
    no_mode = bool(lines) and mode is None

    if failed:
        status = 'failed'
    elif blocked:
        status = 'blocked'
    elif not summaries:
        status = 'failed'
        failed.append('no_report')
    elif shutdown_missing or no_mode:
        status = 'failed'
    else:
        status = 'needs_console_review'
    reasons = failed + blocked + (['shutdown_missing'] if shutdown_missing else [])         + (['no_mode'] if no_mode else [])
    return {
        'status': status,
        'reasons': reasons,
        'mode': mode,
        'build': build,
        'summary': last,
        'summaries': len(summaries),
        'first_blocked': blocked_events[0] if blocked_events else None,
        'shader_rejects': shader_rejects,
        'failures': failures,
        'manual_review': True,
        'manual_checklist': list(MANUAL_CHECKLIST),
    }


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    expect_build = None
    if '--expect-build' in args:
        index = args.index('--expect-build')
        if index + 1 >= len(args):
            print('--expect-build needs a value', file=sys.stderr)
            return 2
        expect_build = args[index + 1]
        del args[index:index + 2]
    paths = args
    if not paths:
        print('usage: native-report.py [--expect-build REV] LOG [LOG ...]', file=sys.stderr)
        return 2
    worst = 0
    for name in paths:
        path = Path(name)
        if not path.is_file():
            print(f'{name}: not a file', file=sys.stderr)
            return 2
        report = parse_report(path.read_text(encoding='utf-8', errors='replace'), expect_build)
        print(json.dumps({'log': str(path), **report}, indent=2))
        if report['status'] != 'needs_console_review':
            worst = 1
    return worst


if __name__ == '__main__':
    sys.exit(main())
