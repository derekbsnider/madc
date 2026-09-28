#!/usr/bin/env python3
# madcide QUITS with its REPL tab live (plan §41.10a; driven by
# scripts/madcide_quit_gate.sh in fulltest). Spawn the real madcide TUI on a
# pty over a C buffer, press F5 (the buffer runs in the REPL tab's session,
# whose pump is a task inside run_tui's serve scope), wait until main's line
# is on the screen, then ^K q: the process must EXIT, promptly. A pump the
# quit never stopped is joined by that scope forever (the 2026-09-28 hang).
# `--no-quit` sends no quit: the harness must then report the process still
# running (the gate's negative control: the exit check is not vacuous).
# MADCIDE=<path to madcide.mad> runs another copy (a pre-fix tree, to show
# the harness catches the hang).
import os, pty, select, signal, struct, sys, tempfile, time, fcntl, termios

no_quit = "--no-quit" in sys.argv
workdir = tempfile.mkdtemp(prefix="madcide_quit_")
src = os.path.join(workdir, "f5quit.c")
with open(src, "w") as f:
    f.write('#include <stdio.h>\n'
            'int main(void) { printf("main ran %d\\n", 6 * 7); return 0; }\n')

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    ide = os.environ.get("MADCIDE", "tools/madcide/madcide.mad")
    os.execvp("bin/madc", ["bin/madc", ide, src])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))

out = b""
def pump(t):
    global out
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                return
            if not d:
                return
            out += d

def wait_for(needle, t):
    end = time.time() + t
    while time.time() < end:
        if needle in out:
            return True
        pump(0.2)
    return needle in out

def finish(ok, why):
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass
    print(("madcide_quit: PASS — " if ok else "madcide_quit: FAIL — ") + why)
    sys.exit(0 if ok else 1)

if not wait_for(b"f5quit.c", 60):
    finish(False, "no first render naming the buffer")
os.write(fd, b"\x1b[15~")          # F5: replrun
# 42 is computed: the buffer's own text (on screen) spells `%d`.
if not wait_for(b"main ran 42", 30):
    finish(False, "F5 did not show main's line")
pump(0.5)
if not no_quit:
    os.write(fd, b"\x0b")          # ^K
    time.sleep(0.3)
    os.write(fd, b"q")             # quit
t0 = time.time()
while time.time() - t0 < 10:
    pump(0.2)
    p, st = os.waitpid(pid, os.WNOHANG)
    if p:
        ok = os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0
        finish(ok, "exited %.1fs after the quit, status %d" % (time.time() - t0, st))
finish(False, "still running 10s after the quit")
