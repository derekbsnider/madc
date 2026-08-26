#!/usr/bin/env python3
# madcide scroll-repaint harness (IDE-9c, driven by scripts/tui_scroll_gate.sh
# in fulltest): drive madcide on a real pty over a TAB-indented document,
# scroll past the window and back, reconstruct the screen with a minimal
# VT100 interpreter (CUP/ED/EL/IL/DL, scroll region, RI/IND, SGR skipped,
# and REAL tab-stop semantics: a raw 0x09 MOVES the cursor without erasing
# the skipped cells), and assert every non-chrome row is either blank or
# EXACTLY one expected tab-expanded document line. The 2026-08-26 defect —
# raw '\t' bytes in grid cells desynchronizing grid columns from screen
# columns (stale tail fragments + doubled brace glyphs while scrolling) —
# fails that equality 800+ times over this key script.
#
# Exit 0 = clean; exit 1 = corruption (reconstruction + offending rows on
# stdout). MADC_TUI_RECON_NEGATIVE=1 corrupts one reconstructed document
# row before checking — the gate's proof that the checker detects.
import os, pty, sys, time, signal

ROWS, COLS = 24, 80
TABSTOP = 8

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

def spawn(argv):
    env = dict(os.environ)
    env['TERM'] = 'xterm'
    env['LINES'] = str(ROWS)
    env['COLUMNS'] = str(COLS)
    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe(argv[0], argv, env)
    import fcntl, termios, struct
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', ROWS, COLS, 0, 0))
    return pid, fd

def drain(fd, secs):
    import select
    out = b''
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
    return out

class Screen:
    def __init__(self):
        self.rows = [[' '] * COLS for _ in range(ROWS)]
        self.r = 0
        self.c = 0
        self.top = 0
        self.bot = ROWS - 1
        self.carry = b''   # an escape sequence split across feed() chunks
    def clear(self, r):
        self.rows[r] = [' '] * COLS
    def scroll_up(self):
        del self.rows[self.top]
        self.rows.insert(self.bot, [' '] * COLS)
    def scroll_down(self):
        del self.rows[self.bot]
        self.rows.insert(self.top, [' '] * COLS)
    def feed(self, data):
        data = self.carry + data
        self.carry = b''
        i = 0
        n = len(data)
        while i < n:
            b = data[i]
            if b == 0x1b and i + 1 >= n:        # ESC at the chunk edge
                self.carry = data[i:]
                return
            if b == 0x1b and data[i+1] == 0x5d:  # OSC ... BEL / ESC-backslash
                j = i + 2
                while j < n and data[j] != 0x07 \
                        and not (data[j] == 0x1b and j + 1 < n
                                 and data[j+1] == 0x5c):
                    j += 1
                if j >= n:
                    self.carry = data[i:]
                    return
                i = j + (2 if data[j] == 0x1b else 1)
                continue
            if b == 0x1b and data[i+1] == 0x5b:  # CSI
                j = i + 2
                while j < n and not (0x40 <= data[j] <= 0x7e):
                    j += 1
                if j >= n:
                    self.carry = data[i:]
                    return
                body = data[i+2:j].decode('latin1')
                fin = chr(data[j])
                ps = [int(x) if x else 0
                      for x in body.replace('?', '').split(';')] if body else []
                p1 = ps[0] if ps else 0
                p2 = ps[1] if len(ps) > 1 else 0
                if fin == 'H' or fin == 'f':
                    self.r = max(0, min(ROWS-1, (p1 or 1) - 1))
                    self.c = max(0, min(COLS-1, (p2 or 1) - 1))
                elif fin == 'J':
                    if p1 == 2:
                        for r in range(ROWS):
                            self.clear(r)
                    elif p1 == 0:
                        for c in range(self.c, COLS):
                            self.rows[self.r][c] = ' '
                        for r in range(self.r+1, ROWS):
                            self.clear(r)
                elif fin == 'K':
                    if p1 == 0:
                        for c in range(self.c, COLS):
                            self.rows[self.r][c] = ' '
                    elif p1 == 2:
                        self.clear(self.r)
                elif fin == 'L':
                    for _ in range(max(1, p1)):
                        if self.top <= self.r <= self.bot:
                            del self.rows[self.bot]
                            self.rows.insert(self.r, [' '] * COLS)
                elif fin == 'M':
                    for _ in range(max(1, p1)):
                        if self.top <= self.r <= self.bot:
                            del self.rows[self.r]
                            self.rows.insert(self.bot, [' '] * COLS)
                elif fin == 'r':
                    self.top = (p1 or 1) - 1
                    self.bot = (p2 or ROWS) - 1
                    self.r = self.c = 0
                elif fin == 'S':
                    for _ in range(max(1, p1)):
                        self.scroll_up()
                elif fin == 'T':
                    for _ in range(max(1, p1)):
                        self.scroll_down()
                i = j + 1
                continue
            if b == 0x1b:
                nxt = data[i+1]
                if nxt == 0x4d:  # RI
                    if self.r == self.top:
                        self.scroll_down()
                    else:
                        self.r -= 1
                    i += 2
                    continue
                if nxt == 0x44:  # IND
                    if self.r == self.bot:
                        self.scroll_up()
                    else:
                        self.r += 1
                    i += 2
                    continue
                if nxt in (0x28, 0x29):  # charset (3 bytes)
                    if i + 2 >= n:
                        self.carry = data[i:]
                        return
                    i += 3
                    continue
                i += 2
                continue
            if b == 0x0d:
                self.c = 0
            elif b == 0x0a:
                if self.r == self.bot:
                    self.scroll_up()
                else:
                    self.r = min(ROWS-1, self.r + 1)
            elif b == 0x08:
                self.c = max(0, self.c - 1)
            elif b == 0x09:
                # a raw tab MOVES the cursor; skipped cells KEEP old glyphs —
                # exactly the desync the cell invariant forbids
                self.c = min(COLS-1, (self.c // TABSTOP + 1) * TABSTOP)
            elif b >= 0x20:
                self.rows[self.r][self.c] = chr(b) if b < 0x7f else '?'
                if self.c < COLS - 1:
                    self.c += 1
            i += 1
    def dump(self):
        return '\n'.join('%2d|%s' % (r, ''.join(row).rstrip())
                         for r, row in enumerate(self.rows))

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
    expected_rows = set(expand(l) for l in doc)
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
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
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
