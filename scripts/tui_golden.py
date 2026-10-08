#!/usr/bin/env python3
# tui_golden — the TUI facelift's golden screens (docs/plans/
# 2026-10-07-tui-facelift-plan.md, slice S0): madcide on a real pty at
# 120x36 and 80x24, driven by KEYS (a screenshot is not verification),
# the screen read back through the one interpreter (scripts/vtscreen.py) as
# text AND style runs, compared with tests/tui_golden/<scenario>-<WxH>.golden.
#
#   python3 scripts/tui_golden.py            check every scenario
#   python3 scripts/tui_golden.py --record   (re)write the goldens
#   MADC_TUI_GOLDEN_NEGATIVE=1 ...           flip one cell's colour first: the
#                                            check must FAIL (the gate's proof)
#
# Scenarios are DATA (SCENARIOS below): a name and its key steps; each step
# sends its bytes and waits for the repaint (or for a text the step names).
# Every run gets a fresh madcide config directory, so a user's settings and
# recent files never reach a golden. Content that changes with the tree
# (help pages, the project window's file list) is left out on purpose.
import os, shutil, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vtscreen import Screen, spawn, settle, kill, NORMAL

GOLDEN_DIR = os.path.join('tests', 'tui_golden')
SIZES = [(36, 120), (24, 80)]
MADC = os.environ.get('MADC_BIN', 'bin/madc')

SOURCE = ('#include <stdio.h>\n'
          '// a comment\n'
          'int main(void)\n'
          '{\n'
          '\tprintf("hi %d\\n", 42);\n'
          '\treturn 0;\n'
          '}\n')

ESC = b'\x1b'
KEY = {
    'options': b'\x14',             # ^T
    'esc': ESC,
    'enter': b'\r',
    'down': ESC + b'[B',
    'f5': ESC + b'[15~',
}

# The Dark+ scheme chosen by keys, as a user does: ^T Options, down to the
# Scheme row, Enter (the Theme prompt), its name, Enter.
DARKPLUS = [('options', None), ('down', None), ('enter', None),
            ('text:vscode', None), ('enter', None)]
# The terminal colour depths a target detects (ui_term.cpp
# detect_colour_depth): 8/16 (TERM=xterm), 256, truecolor.
DEPTH_256 = {'TERM': 'xterm-256color'}
DEPTH_TRUE = {'COLORTERM': 'truecolor'}

# (name, [ (key name | 'text:<chars>', text to wait for | None) ... ],
#  terminal environment, sizes) — sizes None = every size in SIZES.
SCENARIOS = [
    ('startup', [], {}, None),
    ('options', [('options', None)], {}, None),
    ('replrun', [('f5', 'hi 42')], {}, None),
    # a popup takes the keyboard: ^T Options, down to Scheme, Enter = the
    # Theme prompt (the arrows and Enter reach the list, not the buffer)
    ('options-scheme', [('options', None), ('down', None), ('enter', None)],
     {}, None),
    ('darkplus-16', DARKPLUS, {}, [(36, 120)]),
    ('darkplus-256', DARKPLUS, DEPTH_256, [(36, 120)]),
    ('darkplus-truecolor', DARKPLUS, DEPTH_TRUE, [(36, 120)]),
]


def render(scr):
    lines = ['text:']
    lines += ['%2d|%s' % (r, t) for r, t in enumerate(scr.text_rows())]
    lines.append('styles:')
    for r in range(scr.ROWS):
        runs = scr.style_runs(r)
        if runs:
            lines.append('%2d %s' % (r, ' '.join(runs)))
    return '\n'.join(lines) + '\n'


def wait_for(fd, scr, text, limit=30.0):
    import time
    end = time.time() + limit
    while time.time() < end:
        settle(fd, scr, quiet=0.5, limit=2.0)
        if any(text in row for row in scr.text_rows()):
            return True
    return False


def run_scenario(name, steps, env, rows, cols, work):
    path = os.path.join(work, 'golden.cpp')
    with open(path, 'w') as f:
        f.write(SOURCE)
    cfg = os.path.join(work, 'cfg')
    os.makedirs(cfg, exist_ok=True)
    # the file is opened by its RELATIVE name so the status line is stable
    pid, fd = spawn([os.path.abspath(MADC), os.path.abspath('tools/madcide/madcide.mad'),
                     'golden.cpp'], rows, cols,
                    env_extra=dict(env, MADCIDE_CONFIG_DIR=cfg), cwd=work)
    scr = Screen(rows, cols)
    try:
        settle(fd, scr, quiet=1.5, limit=40)
        for key, text in steps:
            os.write(fd, key[5:].encode() if key.startswith('text:') else KEY[key])
            if text is not None:
                if not wait_for(fd, scr, text):
                    return None, 'never showed %r' % text
            settle(fd, scr, quiet=1.0, limit=20)
    finally:
        kill(pid)
    return scr, None


def golden_path(name, rows, cols):
    return os.path.join(GOLDEN_DIR, '%s-%dx%d.golden' % (name, cols, rows))


def main():
    record = '--record' in sys.argv
    negative = bool(os.environ.get('MADC_TUI_GOLDEN_NEGATIVE'))
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    work = tempfile.mkdtemp(prefix='tui_golden_', dir=os.path.abspath('tmp'))
    failures = []
    checked = 0
    try:
        for name, steps, env, sizes in SCENARIOS:
            for rows, cols in (sizes or SIZES):
                label = '%s %dx%d' % (name, cols, rows)
                scr, why = run_scenario(name, steps, env, rows, cols, work)
                if scr is None:
                    failures.append('%s: %s' % (label, why))
                    continue
                if negative:
                    # flip the first styled cell (or the first cell) to red
                    done = False
                    for r in range(rows):
                        for c in range(cols):
                            if scr.styles[r][c] != NORMAL or r == rows - 1:
                                scr.styles[r][c] = (('i', 1), None, frozenset())
                                done = True
                                break
                        if done:
                            break
                got = render(scr)
                gp = golden_path(name, rows, cols)
                if record:
                    with open(gp, 'w') as f:
                        f.write(got)
                    print('recorded %s' % gp)
                    continue
                if not os.path.exists(gp):
                    failures.append('%s: no golden %s (run --record)' % (label, gp))
                    continue
                want = open(gp).read()
                checked += 1
                if got != want:
                    import difflib
                    diff = ''.join(difflib.unified_diff(
                        want.splitlines(True), got.splitlines(True),
                        gp, 'screen', n=1))
                    failures.append('%s differs:\n%s' % (label, diff))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if record:
        return 0
    if failures:
        for f in failures:
            print('FAIL: ' + f)
        return 1
    print('OK: %d golden screens match' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
