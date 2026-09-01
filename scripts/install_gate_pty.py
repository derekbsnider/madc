# pty probe for the package-install gate (PK4, scripts/package_install_gate.sh):
# spawn an INSTALLED madcide binary on a real pty, capture a few seconds of
# its first paint, and CLASSIFY (the calling shell owns pass/fail):
#   RESCUE|NORESCUE  — "RESCUE" appearing on screen is madcide's
#                      no-profile-found banner (madcide_core.inc: bindings
#                      are pure data, so a missing profile dir is announced,
#                      never silently continued)
#   FILESEEN|NOFILE  — argv[3]'s marker rendered (the probe file's name and
#                      content both carry it), proving the editor opened the
#                      file and painted, rather than dying before first draw
#
# Distinct from scripts/tui_pty.py, the TUI-semantics harness (keystroke
# scripts, resize/SIGWINCH, alt-screen asserts, hardwired to run a .mad
# under bin/madc): this one runs an arbitrary installed BINARY and only
# classifies. Promoted from the s150 PK7 ad-hoc proof (container
# tmp/pk7_pty.py).
#
# cwd is forced to /tmp deliberately:
#   (a) Popen's cwd= re-roots RELATIVE argv paths — callers pass ABSOLUTE
#       binary/file paths only;
#   (b) the shipped madcide's first profile-search arm is
#       {dirname(__FILE__)}/profiles, and the packager bakes __FILE__ as the
#       build-tree-RELATIVE source path — from /tmp that arm misses, so the
#       probe exercises the INSTALLED share/... (or beside-exe) layout, the
#       same search an end-user box performs.
#
# usage: python3 install_gate_pty.py <abs-binary> <abs-file> <marker>
# prints: "RESCUE|NORESCUE FILESEEN|NOFILE <bytes-captured>"; exit 0 unless
# the spawn itself fails.
import os, pty, time, subprocess, struct, fcntl, termios, sys, select

binp, filep, marker = sys.argv[1], sys.argv[2], sys.argv[3]
m, s = pty.openpty()
fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
env = dict(os.environ, TERM="xterm")
p = subprocess.Popen([binp, filep], stdin=s, stdout=s, stderr=s, env=env,
                     close_fds=True, cwd="/tmp")
os.close(s)
out = b""
t0 = time.time()
while time.time() - t0 < 6:
    r, _, _ = select.select([m], [], [], 0.3)
    if r:
        try:
            d = os.read(m, 65536)
        except OSError:
            break
        if not d:
            break
        out += d
p.terminate()
p.wait()
tag = "RESCUE" if b"RESCUE" in out else "NORESCUE"
seen = "FILESEEN" if marker.encode() in out else "NOFILE"
print(tag, seen, len(out))
