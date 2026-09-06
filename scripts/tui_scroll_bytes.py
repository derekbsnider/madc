#!/usr/bin/env python3
# madcide scroll-cost meter (IDE-9c perf half): drive the SAME session shape
# as scripts/tui_scroll_recon.py (real pty, tab-indented document, 45 lines
# down + 25 up) and report the BYTES madcide emits per scroll step, plus the
# rows repainted per step (CUP count; the final caret CUP subtracted).
#
# This is the measurement that gates the scroll-op design (model-side shift
# detection + CSI r/M/L + EL trailing-space suppression): run it at HEAD for
# the baseline, again after, compare. Screen correctness stays the recon
# gate's job — this meter still feeds the interpreter so a corrupt stream
# would be caught by eye in the dump, but it asserts nothing about content.
import os, re, signal, statistics, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tui_scroll_recon as recon

CUP = re.compile(rb'\x1b\[[0-9]*;[0-9]*H|\x1b\[H')

def measure(fd, key, n, label, rows):
    for step in range(1, n + 1):
        os.write(fd, key)
        raw = recon.drain(fd, 0.25)
        cups = len(CUP.findall(raw))
        rows.append((label, step, len(raw), max(0, cups - 1)))

def main():
    path = os.path.join('tmp', 'tui_scroll_input.c')
    recon.make_input(path)
    # argv override for oracle runs (e.g. `... tui_scroll_bytes.py joe`);
    # MADC_METER_KEYS=ctrl drives with ^N/^P for editors in
    # application-cursor mode (joe), default is CSI arrows.
    argv = sys.argv[1:] or ['bin/madc', 'tools/madcide/madcide.mad']
    down, up = (b'\x0e', b'\x10') \
        if os.environ.get('MADC_METER_KEYS') == 'ctrl' else \
        (b'\x1b[B', b'\x1b[A')
    pid, fd = recon.spawn(argv + [path])
    samples = []
    try:
        startup = recon.drain(fd, 4.0)
        measure(fd, down, 45, 'down', samples)
        measure(fd, up, 25, 'up', samples)
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    print('startup paint: %d bytes' % len(startup))
    scroll = [s for s in samples if s[2] > 0]
    if not scroll:
        print('no output captured — did madcide open?')
        return 1
    print('scroll steps captured: %d of %d' % (len(scroll), len(samples)))
    # Two classes: a step that only moves the caret inside the window
    # (few rows repainted) vs a step where the window actually SCROLLS.
    for name, cls in (('caret-only', [s for s in scroll if s[3] <= 3]),
                      ('scrolling ', [s for s in scroll if s[3] > 3])):
        if not cls:
            continue
        per = [s[2] for s in cls]
        rws = [s[3] for s in cls]
        print('%s steps: %2d  bytes/step min %d median %d mean %d max %d'
              '  rows/step median %d  TOTAL %d'
              % (name, len(cls), min(per), statistics.median(per),
                 sum(per) // len(per), max(per), statistics.median(rws),
                 sum(per)))
    worst = sorted(scroll, key=lambda s: -s[2])[:5]
    for label, step, nbytes, nrows in worst:
        print('  worst: %s#%02d  %d bytes  %d rows' % (label, step, nbytes, nrows))
    return 0

if __name__ == '__main__':
    sys.exit(main())
