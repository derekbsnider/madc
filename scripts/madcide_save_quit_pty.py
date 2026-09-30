#!/usr/bin/env python3
# Does madcide SAVE and QUIT when started outside the repo root (B85)? The
# real TUI on a pty, its cwd a fresh temp directory, never the checkout:
# data found relative to the cwd is exactly what this catches. After the
# first paint it types `x`, then the given key bytes one at a time, and
# waits for the process to exit. Driven by scripts/madcide_save_quit_gate.sh
# (the source tree, in fulltest) and scripts/package_install_gate.sh (the
# installed package).
#
# usage: madcide_save_quit_pty.py <file> <keys-hex> <program> [args...]
#   runs `program args... file`; <keys-hex> is the key bytes after the `x`
#   ("" types only the `x`: the negative control).
# prints: "SAVED|UNSAVED EXITED|RUNNING rc=<n> RESCUE|NORESCUE VERBSMISSING|VERBSOK"
#   SAVED = the file now holds `x` + its old text; VERBSMISSING = the
#   editor refused to start because its verb bodies did not load.
import os, pty, select, signal, struct, sys, tempfile, time, fcntl, termios

path, keyhex, prog = sys.argv[1], sys.argv[2], sys.argv[3:]
path = os.path.abspath(path)
before = open(path).read()
keys = bytes.fromhex(keyhex)
cwd = tempfile.mkdtemp(prefix="madcide_cwd_")

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    os.chdir(cwd)
    os.execvp(prog[0], prog + [path])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))

out = b""
status = None
def pump(t):
    global out, status
    end = time.time() + t
    while time.time() < end:
        if status is None:
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                status = st
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                return
            if not d:
                return
            out += d
        elif status is not None:
            return

# The first paint names the file on the status line (the JIT warm-up first).
name = os.path.basename(path).encode()
end = time.time() + 60
while time.time() < end and name not in out and status is None:
    pump(0.2)
if status is None and name in out:
    pump(0.5)
    for b in b"x" + keys:
        os.write(fd, bytes([b]))
        pump(0.3)
    end = time.time() + 10
    while time.time() < end and status is None:
        pump(0.2)
if status is None:
    try:
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            status = st
    except OSError:
        pass
exited = status is not None
if not exited:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
rc = os.waitstatus_to_exitcode(status) if exited else -1
after = open(path).read()
saved = after == "x" + before
print("%s %s rc=%d %s %s" % (
    "SAVED" if saved else "UNSAVED",
    "EXITED" if exited else "RUNNING",
    rc,
    "RESCUE" if b"RESCUE" in out else "NORESCUE",
    "VERBSMISSING" if b"cannot load the editor's verbs" in out else "VERBSOK"))
