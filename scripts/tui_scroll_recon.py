#!/usr/bin/env python3
# madcide scroll-repaint harness (IDE-9c, driven by scripts/tui_scroll_gate.sh
# in fulltest): drive madcide on a real pty over a TAB-indented document,
# scroll past the window and back, reconstruct the screen with a minimal
# VT100 interpreter (scripts/vtscreen.py, the one the TUI gates share — REAL
# tab-stop semantics: a raw 0x09 MOVES the cursor without erasing the skipped
# cells), and assert every non-chrome row is either blank or
# EXACTLY one expected tab-expanded document line. The 2026-08-26 defect —
# raw '\t' bytes in grid cells desynchronizing grid columns from screen
# columns (stale tail fragments + doubled brace glyphs while scrolling) —
# fails that equality 800+ times over this key script.
#
# Exit 0 = clean; exit 1 = corruption (reconstruction + offending rows on
# stdout). MADC_TUI_RECON_NEGATIVE=1 corrupts one reconstructed document
# row before checking — the gate's proof that the checker detects.
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vtscreen import Screen as VtScreen, spawn as vt_spawn, drain, kill, TABSTOP

ROWS, COLS = 24, 80


def Screen():
    return VtScreen(ROWS, COLS)


def spawn(argv):
    return vt_spawn(argv, ROWS, COLS)

def expand(line):
    out = []
    for ch in line:
        if ch == '\t':
            out.append(' ')
            while len(out) % TABSTOP:
                out.append(' ')
        else:
            out.append(ch)
    return ''.join(out)

def make_input(path):
    # Realistic tab-indented code: brace-only lines at DIFFERENT tab depths
    # adjacent across scroll steps (the doubled-glyph shape), long lines
    # followed by short ones (the stale-tail shape).
    lines = []
    for k in range(1, 21):
        lines.append('int fn_%02d_with_a_long_descriptive_name(void)' % k)
        lines.append('{')
        lines.append('\tif (condition_%02d) {' % k)
        lines.append('\t\tcall_target_%02d(argument_one, argument_two);' % k)
        lines.append('\t\t}')
        lines.append('\t}')
        lines.append('}')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return lines

def check(scr, expected_rows, chrome, label, failures):
    for r in range(ROWS):
        if r in chrome:
            continue
        text = ''.join(scr.rows[r]).rstrip()
        if text == '' or text in expected_rows:
            continue
        failures.append('%s: row %d not a document line: %r' % (label, r, text))

def main():
    path = os.path.join('tmp', 'tui_scroll_input.c')
    doc = make_input(path)
    # Each document row as the editor shows it: its number in the gutter
    # (facelift S2 — right-aligned in at least three columns plus one, then
    # two blanks; the text's trailing newline makes one more, empty line),
    # then the tab-expanded text. A row is that line's, number and text.
    numbered = doc + ['']
    field = max(3, len(str(len(numbered)))) + 1
    expected_rows = set(('%*d  %s' % (field, n + 1, expand(l))).rstrip()
                        for n, l in enumerate(numbered))
    pid, fd = spawn(['bin/madc', 'tools/madcide/madcide.mad', path])
    scr = Screen()
    failures = []
    try:
        scr.feed(drain(fd, 4.0))            # startup paint (JIT warm-up)
        # chrome = non-blank rows that are NOT document lines at startup;
        # fixed lines stay at their rows while the edit region scrolls
        chrome = set()
        for r in range(ROWS):
            text = ''.join(scr.rows[r]).rstrip()
            if text and text not in expected_rows:
                chrome.add(r)
        if len(chrome) >= ROWS - 2:
            print('startup screen shows no document lines — the harness '
                  'cannot anchor (madcide failed to open?)')
            print(scr.dump())
            return 1
        check(scr, expected_rows, chrome, 'startup', failures)
        # one line at a time, checking after every step — corruption
        # accumulates across scroll repaints
        for step in range(1, 46):
            os.write(fd, b'\x1b[B')
            scr.feed(drain(fd, 0.25))
            check(scr, expected_rows, chrome, 'down#%02d' % step, failures)
        for step in range(1, 26):
            os.write(fd, b'\x1b[A')
            scr.feed(drain(fd, 0.25))
            check(scr, expected_rows, chrome, 'up#%02d' % step, failures)
        if os.environ.get('MADC_TUI_RECON_NEGATIVE'):
            # corrupt one reconstructed document row: the checker must fail
            for r in range(ROWS):
                if r not in chrome and ''.join(scr.rows[r]).strip():
                    scr.rows[r][0] = '}' if scr.rows[r][0] != '}' else 'X'
                    break
            check(scr, expected_rows, chrome, 'negative', failures)
    finally:
        kill(pid)
    if failures:
        print(scr.dump())
        print('\nFAIL: %d corrupted row observations' % len(failures))
        for f in failures[:20]:
            print('  ' + f)
        if len(failures) > 20:
            print('  ... %d more' % (len(failures) - 20))
        return 1
    print('OK: every non-chrome row matched a tab-expanded document line '
          'through 70 scroll steps (chrome rows: %s)' % sorted(chrome))
    return 0

if __name__ == '__main__':
    sys.exit(main())
