#!/bin/bash
# check-one-host-interrupt-guard.sh — the host-interrupt-guard gate.
#
# While a child runs in the foreground — a session entry in its backend, a
# fork Run of a parse handle, a project run — the terminal's interrupt keys
# are the child's: the host ignores Ctrl-C (SIGINT) and Ctrl-\ (SIGQUIT), as
# system(3) does, and restores them after. ONE owner holds that rule and the
# session interrupt's own signal handling: src/madc_session_interrupt.cpp
# (madc::HostIgnoresInterrupt, madc::session_interrupt_arm_backend). On
# 2026-10-05 the rule had three implementations (KG DupFamily
# host_yields_terminal_interrupt): the session's guard ignored SIGINT alone,
# the two run sites in src/madc_program.cpp ignored both, and Ctrl-\ during a
# REPL entry ended the REPL itself (tests/testrepl_terminalkeys).
#
# Rule: no source or header outside the owner installs a disposition for
# SIGINT or SIGQUIT (sigaction), ignores either with signal(), or sets a
# console control handler (SetConsoleCtrlHandler). Restoring the default in a
# child (signal(SIGINT, SIG_DFL)) is not the rule, and is not matched.
#
# Negative control: a synthetic violation must FAIL the scan, else the gate
# itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER=src/madc_session_interrupt.cpp
PATTERN='sigaction\(SIG(INT|QUIT)|signal\(SIG(INT|QUIT), *SIG_IGN|SetConsoleCtrlHandler\('

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative control -------------------------------------------------------
tmp=$(mktemp)
printf 'void f(void) { struct sigaction ign, old_int; sigaction(SIGINT, &ign, &old_int); }\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-host-interrupt-guard: NEGATIVE CONTROL FAILED — the scan did not catch a hand-rolled SIGINT ignore" >&2
	exit 2
fi
printf 'void g(void) { SetConsoleCtrlHandler(NULL, TRUE); }\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-host-interrupt-guard: NEGATIVE CONTROL FAILED — the scan did not catch a console control handler" >&2
	exit 2
fi
printf 'void h(void) { signal(SIGINT, SIG_DFL); }\n' > "$tmp"
if ! scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-host-interrupt-guard: POSITIVE CONTROL FAILED — a child's restore of the default was flagged" >&2
	exit 2
fi
rm -f "$tmp"

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'src/*.c' 'include/*.h' 'include/**/*.h' \
	| grep -v "^$OWNER\$" | grep -v '^include/doctest\.h$')
# shellcheck disable=SC2086
if ! out=$(scan $files 2>&1); then
	echo "check-one-host-interrupt-guard: a SIGINT/SIGQUIT disposition outside the one owner ($OWNER):" >&2
	echo "$out" >&2
	echo "  -> hold madc::HostIgnoresInterrupt (include/madc_session_interrupt.h) for the child's lifetime" >&2
	exit 1
fi
echo "check-one-host-interrupt-guard: OK — SIGINT/SIGQUIT dispositions and console control handlers live only in $OWNER (controls bite)"
