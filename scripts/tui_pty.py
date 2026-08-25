# pty harness for the VT100 TUI target (R5): spawn bin/madc on a real
# pty, feed a keystroke script (a coalescible run, an arrow, focus
# cycling, menu navigation, a choose, a pty resize, ^q), and assert both
# the escape stream (alt screen in/out, drawing, attributes, cursor) and
# the exact semantic event log the program observed. Driven by
# scripts/tui_smoke_gate.sh in fulltest; usage: tui_pty.py <program.mad>.
import os, pty, time, sys, subprocess, fcntl, termios, struct, threading
import signal

prog = sys.argv[1] if len(sys.argv) > 1 else "scripts/tui_smoke.mad"

master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 40, 0, 0))
env = dict(os.environ, TERM="xterm")
p = subprocess.Popen(["bin/madc", prog],
                     stdin=slave, stdout=slave, stderr=slave,
                     env=env, close_fds=True)
os.close(slave)

out = b""
def reader():
    global out
    while True:
        try:
            d = os.read(master, 4096)
        except OSError:
            break
        if not d:
            break
        out += d
threading.Thread(target=reader, daemon=True).start()

def feed(b, delay=0.3):
    time.sleep(delay)
    try:
        os.write(master, b)
    except OSError:
        pass                      # child already gone: the asserts decide

# Wait for the first render (JIT warm-up) before typing, up to 10s.
deadline = time.time() + 10
while time.time() < deadline and b"smoke.txt" not in out:
    if p.poll() is not None:
        break
    time.sleep(0.05)

feed(b"hi")              # one coalesced text run
feed(b"\x0b")            # ^k — a bound chord's prefix, held pending...
feed(b"s")               # ...across READ BATCHES -> action "save-chord"
feed(b"\x1b[D")          # left arrow -> key event (edit focused)
feed(b"\t")              # tab -> focus the menu
feed(b"\x1b[C")          # right -> selection Save -> Quit
feed(b"\r")              # enter -> choose option 2 (action q)
# Resize the pty and deliver SIGWINCH ourselves: the child is not the
# pty's session leader here, so the kernel's automatic delivery does not
# apply — what this smoke pins is OUR handler -> flag -> TIOCGWINSZ
# re-query -> resize-event path.
time.sleep(0.3)
try:
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 14, 50, 0, 0))
    if p.poll() is None:
        os.kill(p.pid, signal.SIGWINCH)
except OSError:
    pass
feed(b"\x11")            # ^q -> the program leaves its loop
try:
    p.wait(timeout=15)
except subprocess.TimeoutExpired:
    p.kill()
    print("FAIL process hung")
    sys.exit(1)
time.sleep(0.3)

txt = out.decode("utf-8", "replace")
ev = ""
for line in txt.splitlines():
    i = line.find("EVENTS=")
    if i >= 0:
        ev = line[i:].strip()
checks = {
    "alt screen entered":  "\x1b[?1049h" in txt,
    "alt screen restored": "\x1b[?1049l" in txt,
    "heading drawn":       "smoke.txt" in txt,
    "document drawn":      "alpha" in txt,
    "reverse attr used":   "\x1b[7m" in txt,
    "cursor shown":        "\x1b[?25h" in txt,
}
expected = ("EVENTS=text:hi;action:save-chord:^k s;"
            "key:left;focus;focus;choose:2:q;resize;key:^q;")
checks["event log"] = ev == expected
ok = True
for k, v in checks.items():
    print(("OK   " if v else "FAIL ") + k)
    ok = ok and v
if not checks["event log"]:
    print("  got:      " + repr(ev))
    print("  expected: " + repr(expected))
print("SMOKE " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
