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
#   ... --only NAME                          one scenario (the negative control
#                                            needs one screen to prove itself)
#   MADC_TUI_GOLDEN_JOBS=N                   scenarios run N at a time, each in
#                                            its own work directory (default 4)
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
    'ctrl-k': b'\x0b',
    'f10': ESC + b'[21~',
    'alt-e': ESC + b'e',            # the Meta prefix in one burst: Alt+E
    'ctrl-b': b'\x02',
}

# The Dark+ scheme chosen by keys, as a user does: ^T Options, down to the
# Scheme row, Enter (the Theme prompt), its name, Enter.
DARKPLUS = [('options', None), ('down', None), ('enter', None),
            ('text:vscode', None), ('enter', None)]
# The terminal colour depths a target detects (ui_term.cpp
# detect_colour_depth): 8/16 (TERM=xterm), 256, truecolor.
DEPTH_256 = {'TERM': 'xterm-256color'}
DEPTH_TRUE = {'COLORTERM': 'truecolor'}
# A locale without UTF-8: the frames in ASCII (detect_glyph_set).
ASCII = {'LC_ALL': 'C'}

# ^K E <name> Enter: edit a file (open it, or switch to its buffer).
def EDIT(name, wait):
    return [('ctrl-k', None), ('text:e', None), ('text:' + name, None),
            ('enter', wait)]

# (name, [ (key name | 'text:<chars>', text to wait for | None) ... ],
#  terminal environment, sizes[, bundle]) — sizes None = every size in
# SIZES; a bundle names a fixture plugin under tests/tui_golden/plugins/,
# installed in the run's config directory and launched with --profile.
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
    # the frames (S2): the REPL's panel divider in ASCII, and in Dark+ with
    # the gutter, the caret line and the divider in the scheme's colours
    ('replrun-ascii', [('f5', 'hi 42')], ASCII, [(24, 80)]),
    ('darkplus-repl', DARKPLUS + [('esc', None), ('f5', 'hi 42')], DEPTH_TRUE,
     [(36, 120)]),
    # the editor's tab strip (S3), switched by keys: ^K E opens a second
    # file (its tab active), ^K E on the first switches back to it
    ('tabs-second', EDIT('second.c', 'second.c'), {}, [(24, 80)]),
    ('tabs-back', EDIT('second.c', 'second.c') + EDIT('golden.cpp', None),
     {}, [(24, 80)]),
    # the menu bar (S4): F10 drops File; F10, File, its Save by its letter
    # (the bar closes, the file saved); Alt+E drops Edit, where Cut is
    # disabled with no selection — its letter chooses nothing, the menu
    # stays; the dropdown in Dark+ and in ASCII
    ('menu-file', [('f10', None)], {}, None),
    ('menu-save', [('f10', None), ('text:s', None)], {}, [(24, 80)]),
    ('menu-disabled', [('alt-e', None), ('text:c', None)], {}, [(24, 80)]),
    ('menu-darkplus', DARKPLUS + [('esc', None), ('f10', None)], DEPTH_TRUE,
     [(36, 120)]),
    ('menu-ascii', [('f10', None)], ASCII, [(24, 80)]),
    # the toolbar (S5): glyphs for the icons, a divider, the drop arrow;
    # Stop disabled (no window); in ASCII the glyphs as > # v
    ('toolbar', [], {}, None, 'tbgolden'),
    ('toolbar-ascii', [], ASCII, [(24, 80)], 'tbgolden'),
    # the floating windows (S6): ^K F's Find prompt as a titled field; an
    # edit then ^K Q's question with its answers as buttons; ^B's build
    # palette as a framed pick list with its Run / Close buttons; the
    # prompt in Dark+
    ('dialog-find', [('ctrl-k', None), ('text:f', None)], {}, None),
    ('dialog-question', [('text:x', None), ('ctrl-k', None), ('text:q', None)],
     {}, [(24, 80)]),
    ('dialog-palette', [('ctrl-b', None)], {}, [(24, 80)]),
    ('dialog-darkplus', DARKPLUS + [('esc', None), ('ctrl-k', None),
                                    ('text:f', None)], DEPTH_TRUE, [(36, 120)]),
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


def run_scenario(name, steps, env, rows, cols, work, bundle=None):
    path = os.path.join(work, 'golden.cpp')
    with open(path, 'w') as f:
        f.write(SOURCE)
    cfg = os.path.join(work, 'cfg')
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(cfg, exist_ok=True)
    argv = [os.path.abspath(MADC), os.path.abspath('tools/madcide/madcide.mad'),
            'golden.cpp']
    if bundle:
        shutil.copytree(os.path.join(GOLDEN_DIR, 'plugins', bundle),
                        os.path.join(cfg, 'plugins', bundle))
        argv += ['--profile', bundle]
    # the file is opened by its RELATIVE name so the status line is stable
    pid, fd = spawn(argv, rows, cols,
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


def render_one(task):
    """One screen in its own work directory (a pool worker): its rendering,
    the negative control's flip applied first when asked, or None + why."""
    name, steps, env, rows, cols, bundle, negative = task
    label = '%s %dx%d' % (name, cols, rows)
    work = tempfile.mkdtemp(prefix='tui_golden_', dir=os.path.abspath('tmp'))
    try:
        scr, why = run_scenario(name, steps, env, rows, cols, work, bundle)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if scr is None:
        return label, name, rows, cols, None, why
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
    return label, name, rows, cols, render(scr), None


def main():
    import multiprocessing
    record = '--record' in sys.argv
    negative = bool(os.environ.get('MADC_TUI_GOLDEN_NEGATIVE'))
    only = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
    jobs = int(os.environ.get('MADC_TUI_GOLDEN_JOBS', '4'))
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    tasks = []
    for sc in SCENARIOS:
        name, steps, env, sizes = sc[:4]
        bundle = sc[4] if len(sc) > 4 else None
        if only and name != only:
            continue
        for rows, cols in (sizes or SIZES):
            tasks.append((name, steps, env, rows, cols, bundle, negative))
    if not tasks:
        print('FAIL: no scenario named %r' % only)
        return 1
    # Each screen is its own madcide on its own pty: they run side by side
    # (a fork pool — every worker forks its pty child single-threaded).
    with multiprocessing.get_context('fork').Pool(max(1, jobs)) as pool:
        results = pool.map(render_one, tasks, chunksize=1)
    failures = []
    checked = 0
    for label, name, rows, cols, got, why in results:
        if got is None:
            failures.append('%s: %s' % (label, why))
            continue
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
    if record:
        return 1 if failures else 0
    if failures:
        for f in failures:
            print('FAIL: ' + f)
        return 1
    print('OK: %d golden screens match' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
