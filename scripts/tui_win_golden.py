#!/usr/bin/env python3
# tui_win_golden — the TUI facelift's golden screens on GENUINE Windows
# (docs/plans/2026-10-07-tui-facelift-plan.md, slice S9): the Windows build
# of madc runs madcide on the owner's box, driven by the SAME keys and mouse
# bytes as scripts/tui_golden.py, and its screens are compared with the SAME
# goldens. Run on the container: ssh reaches the box's WSL, whose interop
# hands a Win32 program pipes, never a console — so scripts/conpty_host.c
# (cross-built here) runs madc.exe inside a ConPTY, the pseudo-console Windows
# Terminal hosts programs in, and the bytes cross ssh on plain pipes. A screen
# that matches proves the Windows console target paints, reads keys and reads
# the mouse as the POSIX one does. Both renders go through visible() first:
# ConPTY re-encodes a blank cell with whatever foreground is cheapest.
#
#   python3 scripts/tui_win_golden.py              every scenario that applies
#   python3 scripts/tui_win_golden.py --only NAME  one scenario
#   MADC_WIN_DUMP=DIR   also write each Windows render to DIR/NAME-COLSxROWS.win
#   MADC_WIN_KEEP=1     leave the stage on the box
#
# MADC_WIN_SSH (derek@host.docker.internal), MADC_WIN_DIR
# (/mnt/c/Users/Public/madcwin), MADC_BIN (bin/release-windows/madc.exe) as
# scripts/win_suite.sh reads them. Scenarios that pin a POSIX locale's ASCII
# frames (LC_ALL=C) are left out: a Windows console's glyphs follow its code
# page, which the target sets to UTF-8. Screens run one at a time (the box is
# the owner's desktop machine).
import difflib, os, shlex, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tui_golden as tg
from vtscreen import Screen, settle

WIN = os.environ.get('MADC_WIN_SSH', 'derek@host.docker.internal')
BASE = os.environ.get('MADC_WIN_DIR', '/mnt/c/Users/Public/madcwin')
PRODUCT = os.environ.get('MADC_BIN', 'bin/release-windows/madc.exe')
SSH = ['ssh', '-o', 'BatchMode=yes']
# WSL's non-login PATH carries no Windows directories.
POWERSHELL = '/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe'
HOST_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'conpty_host.c')
HOST_EXE = 'tmp/conpty_host.exe'


def remote(cmd, data=None):
    return subprocess.run(SSH + [WIN, cmd], input=data, capture_output=True)


def stage():
    """madc.exe, its DLLs and the tools/tests trees in a fresh directory."""
    st = '%s/tui.%d' % (BASE, os.getpid())
    if remote("mkdir -p '%s/bin'" % st).returncode != 0:
        sys.exit('tui_win_golden: channel down (%s)' % WIN)
    if subprocess.run(['x86_64-w64-mingw32-gcc', '-O2', '-o', HOST_EXE,
                       HOST_SRC, '-lshell32']).returncode != 0:
        sys.exit('tui_win_golden: conpty_host build failed')
    files = [PRODUCT, HOST_EXE] + [os.path.join(os.path.dirname(PRODUCT), f)
                         for f in os.listdir(os.path.dirname(PRODUCT))
                         if f.endswith('.dll')]
    if subprocess.run(['scp', '-q', '-o', 'BatchMode=yes'] + files
                      + ['%s:%s/bin/' % (WIN, st)]).returncode != 0:
        sys.exit('tui_win_golden: binary copy failed')
    tree = subprocess.run('git ls-files -z -co --exclude-standard tools tests/tui_golden'
                          ' | tar -c --null -T - -f -', shell=True,
                          capture_output=True).stdout
    if remote("tar -x -C '%s'" % st, tree).returncode != 0:
        sys.exit('tui_win_golden: tree copy failed')
    return st


def run_remote(st, n, name, steps, env, rows, cols, bundle):
    work = '%s/w%d' % (st, n)
    cfg = work + '/cfg'
    setup = "rm -rf '%s' && mkdir -p '%s'" % (work, cfg)
    if bundle:
        setup += " && mkdir -p '%s/plugins' && cp -r '%s/tests/tui_golden/plugins/%s' '%s/plugins/'" % (
            cfg, st, bundle, cfg)
    remote(setup)
    remote("cat > '%s/golden.cpp'" % work, tg.SOURCE.encode())
    # The terminal's environment crosses into the Win32 process only through
    # WSLENV (MADCIDE_CONFIG_DIR as a translated path).
    # TERM as vtscreen.spawn pins it for the POSIX run.
    env = dict({'TERM': 'xterm'}, **env)
    names = ['MADCIDE_CONFIG_DIR/p'] + sorted(env)
    assign = ' '.join('%s=%s' % (k, shlex.quote(v)) for k, v in sorted(env.items()))
    # Paths a Win32 program reads are RELATIVE (a /mnt/c spelling means nothing
    # to it); the file is opened by its relative name so the status line is
    # the POSIX run's.
    argv = ['../bin/conpty_host.exe', str(cols), str(rows),
            '../bin/madc.exe', '../tools/madcide/madcide.mad', 'golden.cpp']
    if bundle:
        argv += ['--profile', bundle]
    cmd = ("cd '%s' && WSLENV=%s MADCIDE_CONFIG_DIR='%s' %s %s"
           % (work, ':'.join(names), cfg, assign, ' '.join(argv)))
    # No tty on either end: ssh carries the bytes over plain pipes and
    # conpty_host is the terminal (a pty's line discipline would echo and
    # line-buffer the keys).
    proc = subprocess.Popen(SSH + [WIN, cmd], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    fd = proc.stdout.fileno()
    scr = Screen(rows, cols)

    def send(data):
        proc.stdin.write(data)
        proc.stdin.flush()
    try:
        settle(fd, scr, quiet=3.0, limit=60)
        for key, text in steps:
            send(tg.step_bytes(key))
            if text is not None and not tg.wait_for(fd, scr, text, limit=40):
                return None, 'never showed %r' % text
            settle(fd, scr, quiet=1.5, limit=30)
    finally:
        proc.kill()
        proc.wait()
        # Only this stage's processes: the box may run madc of its own.
        remote("%s -NoProfile -Command \"Get-Process madc,conpty_host -ErrorAction"
               " SilentlyContinue | Where-Object { \\$_.Path -like '*\\%s\\*' } |"
               " Stop-Process -Force\"; true" % (POWERSHELL, os.path.basename(st)))
    return scr, None


# Words a blank cell shows no trace of unless it is inverse: ConPTY re-encodes
# a space with whatever foreground is cheapest to emit.
GLYPH_ONLY = ('bold', 'dim', 'italic', 'blink')


def visible(render):
    """A tui_golden render with each blank cell's invisible style words
    dropped (foreground and glyph-only attributes, not inverse), runs
    re-joined — both sides of the comparison go through it."""
    lines = render.split('\n')
    cut = lines.index('styles:')
    text = {}
    for ln in lines[1:cut]:
        text[int(ln[:2])] = ln[3:]
    out = lines[:cut + 1]
    for ln in lines[cut + 1:]:
        if not ln:
            out.append(ln)
            continue
        r = int(ln[:2])
        row = text.get(r, '')
        cells = []
        for run in ln[3:].split(' '):
            pos, name = run.split(':', 1)
            c0, n = map(int, pos.split('+'))
            for c in range(c0, c0 + n):
                words = name.split(',')
                if (c >= len(row) or row[c] == ' ') and 'inverse' not in words:
                    words = [w for w in words if w not in GLYPH_ONLY
                             and not w.startswith('fg=')]
                cells.append((c, ','.join(words)))
        runs, prev = [], None
        for c, name in cells:
            if name and prev and prev[1] == name and prev[0] + prev[2] == c:
                prev[2] += 1
                continue
            if name:
                prev = [c, name, 1]
                runs.append(prev)
            else:
                prev = None
        if runs:
            out.append('%2d %s' % (r, ' '.join('%d+%d:%s' % (c, n, name)
                                                for c, name, n in runs)))
    return '\n'.join(out)


def main():
    only = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
    if not os.path.exists(PRODUCT):
        sys.exit('tui_win_golden: %s missing — make -C src release-windows' % PRODUCT)
    st = stage()
    failures, checked, n = [], 0, 0
    try:
        for sc in tg.SCENARIOS:
            name, steps, env, sizes = sc[:4]
            bundle = sc[4] if len(sc) > 4 else None
            if (only and name != only) or env.get('LC_ALL') == 'C':
                continue
            for rows, cols in (sizes or tg.SIZES):
                n += 1
                label = '%s %dx%d' % (name, cols, rows)
                scr, why = run_remote(st, n, name, steps, env, rows, cols, bundle)
                if scr is None:
                    failures.append('%s: %s' % (label, why))
                    continue
                got = tg.render(scr)
                if os.environ.get('MADC_WIN_DUMP'):
                    with open(os.path.join(os.environ['MADC_WIN_DUMP'], '%s-%dx%d.win'
                                           % (name, cols, rows)), 'w') as f:
                        f.write(got)
                want = visible(open(tg.golden_path(name, rows, cols)).read())
                got = visible(got)
                checked += 1
                if got != want:
                    failures.append('%s differs:\n%s' % (label, ''.join(difflib.unified_diff(
                        want.splitlines(True), got.splitlines(True), 'golden', 'windows', n=1))))
                else:
                    print('ok   %s' % label, flush=True)
    finally:
        if os.environ.get('MADC_WIN_KEEP') != '1':
            # A killed process lets go of its files some seconds later.
            if remote("for i in 1 2 3 4 5 6 7 8 9 10; do rm -rf '%s' 2>/dev/null && exit 0;"
                      " sleep 3; done; exit 1" % st).returncode != 0:
                print('tui_win_golden: stage %s left on the box' % st)
    for f in failures:
        print('FAIL: ' + f)
    print('%s: %d of %d Windows screens match their goldens'
          % ('OK' if not failures else 'FAIL', checked - len([f for f in failures if 'differs' in f]),
             checked + len([f for f in failures if 'differs' not in f])))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
