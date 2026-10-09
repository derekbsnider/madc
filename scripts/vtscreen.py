#!/usr/bin/env python3
# vtscreen — THE terminal-screen interpreter the TUI gates read madcide's
# output through (the scroll gate, tui_scroll_recon.py; the facelift's golden
# screens, tui_golden.py). One interpreter, so two gates never disagree about
# what the terminal showed.
#
# What it models: CUP/HVP, CUU/CUD/CUF/CUB, CHA, VPA, ED, EL, IL, DL, SU, SD,
# DECSTBM (scroll region), RI / IND, REAL tab stops (a raw 0x09 MOVES the
# cursor without erasing the cells it skips), UTF-8 text, and SGR: the
# attributes (bold, dim, italic, underline, blink, inverse) and the colours
# in every depth a renderer may emit — 8/16 (30-37, 90-97), 256 (38;5;n)
# and RGB (38;2;r;g;b), foreground and background. OSC, charset designators
# and DEC private modes are consumed and ignored.
#
# Each cell holds a character and a STYLE, a small tuple:
#   (fg, bg, attrs) — fg / bg are None (default), ('i', n) for an indexed
#   colour 0..255 (0..15 = the 16 named), or ('rgb', r, g, b); attrs is a
#   frozenset of attribute names.
# style_name() spells one as text for a golden file.
#
# Thread contract: a Screen is plain data owned by its caller.
import os, pty, select, signal, struct, time

TABSTOP = 8

ATTR_SGR = {1: 'bold', 2: 'dim', 3: 'italic', 4: 'underline', 5: 'blink',
            7: 'inverse'}
ATTR_OFF = {22: ('bold', 'dim'), 23: ('italic',), 24: ('underline',),
            25: ('blink',), 27: ('inverse',)}

NORMAL = (None, None, frozenset())


def style_name(st):
    """One style as golden text: `-` for normal, else words joined by `,`:
    the attributes, `fg=` / `bg=` with an index (`fg=2`) or `#rrggbb`."""
    fg, bg, attrs = st
    if fg is None and bg is None and not attrs:
        return '-'
    words = sorted(attrs)

    def colour(c):
        if c[0] == 'i':
            return str(c[1])
        return '#%02x%02x%02x' % (c[1], c[2], c[3])
    if fg is not None:
        words.append('fg=' + colour(fg))
    if bg is not None:
        words.append('bg=' + colour(bg))
    return ','.join(words)


class Screen:
    def __init__(self, rows=24, cols=80):
        self.ROWS = rows
        self.COLS = cols
        self.rows = [self._blank() for _ in range(rows)]
        self.styles = [[NORMAL] * cols for _ in range(rows)]
        self.r = 0
        self.c = 0
        self.top = 0
        self.bot = rows - 1
        self.style = NORMAL
        self.carry = b''   # an escape sequence / UTF-8 split across feed()s

    def _blank(self):
        return [' '] * self.COLS

    def _erased(self):
        # an erased cell takes the current BACKGROUND only (xterm's bce)
        return (None, self.style[1], frozenset()) if self.style[1] else NORMAL

    def clear(self, r):
        self.rows[r] = self._blank()
        self.styles[r] = [self._erased()] * self.COLS

    def _erase_cells(self, r, c0, c1):
        for c in range(c0, c1):
            self.rows[r][c] = ' '
            self.styles[r][c] = self._erased()

    def _insert_row(self, at, delete_at):
        del self.rows[delete_at]
        del self.styles[delete_at]
        self.rows.insert(at, self._blank())
        self.styles.insert(at, [NORMAL] * self.COLS)

    def scroll_up(self):
        self._insert_row(self.bot, self.top)

    def scroll_down(self):
        self._insert_row(self.top, self.bot)

    def _sgr(self, ps):
        fg, bg, attrs = self.style
        attrs = set(attrs)
        if not ps:
            ps = [0]
        k = 0
        while k < len(ps):
            p = ps[k]
            if p == 0:
                fg, bg, attrs = None, None, set()
            elif p in ATTR_SGR:
                attrs.add(ATTR_SGR[p])
            elif p in ATTR_OFF:
                for a in ATTR_OFF[p]:
                    attrs.discard(a)
            elif 30 <= p <= 37:
                fg = ('i', p - 30)
            elif 90 <= p <= 97:
                fg = ('i', p - 90 + 8)
            elif 40 <= p <= 47:
                bg = ('i', p - 40)
            elif 100 <= p <= 107:
                bg = ('i', p - 100 + 8)
            elif p == 39:
                fg = None
            elif p == 49:
                bg = None
            elif p in (38, 48) and k + 1 < len(ps):
                if ps[k + 1] == 5 and k + 2 < len(ps):
                    c = ('i', ps[k + 2])
                    k += 2
                elif ps[k + 1] == 2 and k + 4 < len(ps):
                    c = ('rgb', ps[k + 2], ps[k + 3], ps[k + 4])
                    k += 4
                else:
                    c = None
                    k += 1
                if p == 38:
                    fg = c
                else:
                    bg = c
            k += 1
        self.style = (fg, bg, frozenset(attrs))

    def _put(self, ch):
        self.rows[self.r][self.c] = ch
        self.styles[self.r][self.c] = self.style
        if self.c < self.COLS - 1:
            self.c += 1

    def feed(self, data):
        data = self.carry + data
        self.carry = b''
        i = 0
        n = len(data)
        ROWS, COLS = self.ROWS, self.COLS
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
                i = j + 1
                if body.startswith('?') or body.startswith('>'):
                    continue                    # a DEC private mode: no screen effect
                ps = [int(x) if x.isdigit() else 0 for x in body.split(';')] if body else []
                p1 = ps[0] if ps else 0
                p2 = ps[1] if len(ps) > 1 else 0
                if fin == 'm':
                    self._sgr(ps)
                elif fin == 'H' or fin == 'f':
                    self.r = max(0, min(ROWS-1, (p1 or 1) - 1))
                    self.c = max(0, min(COLS-1, (p2 or 1) - 1))
                elif fin == 'A':
                    self.r = max(0, self.r - (p1 or 1))
                elif fin == 'B':
                    self.r = min(ROWS-1, self.r + (p1 or 1))
                elif fin == 'C':
                    self.c = min(COLS-1, self.c + (p1 or 1))
                elif fin == 'D':
                    self.c = max(0, self.c - (p1 or 1))
                elif fin == 'G':
                    self.c = max(0, min(COLS-1, (p1 or 1) - 1))
                elif fin == 'd':
                    self.r = max(0, min(ROWS-1, (p1 or 1) - 1))
                elif fin == 'J':
                    if p1 == 2:
                        for r in range(ROWS):
                            self.clear(r)
                    elif p1 == 0:
                        self._erase_cells(self.r, self.c, COLS)
                        for r in range(self.r+1, ROWS):
                            self.clear(r)
                elif fin == 'K':
                    if p1 == 0:
                        self._erase_cells(self.r, self.c, COLS)
                    elif p1 == 1:
                        self._erase_cells(self.r, 0, self.c + 1)
                    elif p1 == 2:
                        self._erase_cells(self.r, 0, COLS)
                elif fin == 'L':
                    for _ in range(max(1, p1)):
                        if self.top <= self.r <= self.bot:
                            self._insert_row(self.r, self.bot)
                elif fin == 'M':
                    for _ in range(max(1, p1)):
                        if self.top <= self.r <= self.bot:
                            self._insert_row(self.bot, self.r)
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
                # a raw tab MOVES the cursor; skipped cells KEEP old glyphs
                self.c = min(COLS-1, (self.c // TABSTOP + 1) * TABSTOP)
            elif b >= 0xc0:
                # a UTF-8 lead byte: the sequence decodes to ONE cell
                need = 2 if b < 0xe0 else 3 if b < 0xf0 else 4
                if i + need > n:
                    self.carry = data[i:]
                    return
                try:
                    ch = data[i:i+need].decode('utf-8')
                except UnicodeDecodeError:
                    ch = '?'
                self._put(ch)
                i += need
                continue
            elif b >= 0x80:
                self._put('?')                  # a stray continuation byte
            elif b >= 0x20 and b != 0x7f:
                self._put(chr(b))
            i += 1

    def text_rows(self):
        return [''.join(row).rstrip() for row in self.rows]

    def dump(self):
        return '\n'.join('%2d|%s' % (r, t) for r, t in enumerate(self.text_rows()))

    def style_runs(self, r):
        """Row `r`'s styles as runs `col+len:style`, normal runs left out."""
        runs = []
        row = self.styles[r]
        c = 0
        while c < self.COLS:
            st = row[c]
            e = c
            while e < self.COLS and row[e] == st:
                e += 1
            if st != NORMAL:
                runs.append('%d+%d:%s' % (c, e - c, style_name(st)))
            c = e
        return runs


def spawn(argv, rows=24, cols=80, env_extra=None, cwd=None):
    """`argv` on a fresh pty of `rows` x `cols` (in `cwd` when given);
    returns (pid, master fd)."""
    env = dict(os.environ)
    env['TERM'] = 'xterm'
    env['LINES'] = str(rows)
    env['COLUMNS'] = str(cols)
    env.pop('COLORTERM', None)
    # The locale decides the glyphs (box drawing in UTF-8, else ASCII —
    # ui_term.cpp detect_glyph_set): pinned, never the host's.
    for k in ('LANG', 'LC_CTYPE'):
        env.pop(k, None)
    env['LC_ALL'] = 'C.UTF-8'
    if env_extra:
        env.update(env_extra)
    pid, fd = pty.fork()
    if pid == 0:
        if cwd:
            os.chdir(cwd)
        os.execvpe(argv[0], argv, env)
    import fcntl, termios
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    return pid, fd


def drain(fd, secs):
    """Everything the pty writes for `secs` seconds."""
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


def settle(fd, scr, quiet=0.6, limit=20.0):
    """Feed `scr` until output has come and the pty has then been quiet for
    `quiet` seconds (a repaint done), or `limit` passes — a slow start (the
    JIT compiling madcide) writes nothing for seconds, which is not "done".
    Returns the seconds waited."""
    start = time.time()
    last = None
    while time.time() - start < limit:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            scr.feed(chunk)
            last = time.time()
        elif last is not None and time.time() - last >= quiet:
            break
    return time.time() - start


def kill(pid):
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
