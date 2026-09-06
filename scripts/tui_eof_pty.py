#!/usr/bin/env python3
# The terminal-death leg: spawn the smoke app on a pty, then CLOSE the
# master with no quit key sent — the process must EXIT, never outlive its
# dead terminal (SIGHUP is ignored in the child so the INPUT WAIT itself
# is what gets tested). A dead terminal is readable PROGRESS (POLLHUP ->
# read -> read_keys returns false -> the event loop ends). NOTE: on Linux
# a dead pty polls POLLIN|POLLHUP, so this leg does not discriminate the
# POLLIN-only bug here (macOS ptys are the HUP-only shape) — the
# fd_readable_progress rule's enforcement is check-fd-readable-progress.sh
# (the one-spelling mechanism gate); this leg pins terminal-death EXIT as
# a standing regression, whichever layer would break it. Exit code is
# irrelevant — TERMINATION is the assertion.
import os
import pty
import signal
import subprocess
import sys
import time

app = sys.argv[1] if len(sys.argv) > 1 else "scripts/tui_smoke.mad"

master, slave = pty.openpty()
env = dict(os.environ)
env["TERM"] = "xterm"


def ignore_hup():
    # The kernel HUPs the foreground group when the master closes, which
    # would end the process before the input wait is ever tested. A real
    # HUP-ignoring session (daemonized editors) is the case this leg
    # pins: EOF must surface through the WAIT itself.
    signal.signal(signal.SIGHUP, signal.SIG_IGN)


p = subprocess.Popen(["bin/madc", app], stdin=slave, stdout=slave,
                     stderr=subprocess.DEVNULL, env=env, close_fds=True,
                     preexec_fn=ignore_hup)
os.close(slave)

# Let it draw its first frame, then kill the terminal.
time.sleep(1.0)
os.close(master)

deadline = time.time() + 8.0
while time.time() < deadline:
    if p.poll() is not None:
        print("tui_eof: process exited on terminal death (rc=%s)"
              % p.returncode)
        sys.exit(0)
    time.sleep(0.1)

p.kill()
p.wait()
print("tui_eof: FAIL — the process outlived its dead terminal "
      "(the input wait never surfaced EOF)")
sys.exit(1)
