#!/bin/bash
# GATE — one process-SPAWN owner (Windows lane W1).
#
# The rule: spawning a child program (fork/vfork + exec family, posix_spawn,
# popen) belongs to the Process owner in src/madc_process.cpp — the piped
# channel arm (start) and the inherited-stdio arm (run_and_wait). Call sites
# use those; only the owner touches the platform primitives, so the Win32
# backend (CreateProcess + handle-inheritance lists) lands in ONE file.
#
# NOT this gate's concern: system() and popen() — CRT-portable on every
# target (mingw provides both; the script-facing madc_system builtin and the
# unit-test harness's popen-the-CLI pattern keep them; a Win32 port never
# touches those sites); and FORK-AS-ISOLATION — the
# libmadc sandbox model clones THIS process to run JIT code in a child and
# never execs (madc_program.cpp exec_compiled_in_child / call_in_child).
# That is a different rule with a different Windows answer (fork has no
# Win32 equivalent — its port is a design decision recorded in the lane
# plan, not a spawn-seam migration), so bare fork lines in madc_program.cpp
# are exempt; an exec-family call appearing there would still fail the gate.
#
# Marker note: call-shaped uses only, `//`/`/*` comment tails stripped, so
# prose mentions don't count. Identifiers merely containing the words
# (exec_compiled_in_child, madc_executable, forest_selfexe) do not match.
set -u
cd "$(dirname "$0")/.."

pat='\b(fork|vfork)[[:space:]]*\(|\bexec(l|lp|le|v|ve|vp|vpe)[[:space:]]*\(|\bposix_spawn'
hits=$(grep -rnE --include='*.cpp' --include='*.h' "$pat" \
    src/ include/ tests/unit/ \
  | sed -E 's_(//|/\*).*__' \
  | grep -E "$pat" \
  | grep -v '^src/madc_process\.cpp:' \
  | grep -vE '^src/madc_program\.cpp:[0-9]+:.*\b(fork|vfork)[[:space:]]*\(' )
n=$(printf '%s' "$hits" | grep -c . )

if [ "$n" -ne 0 ]; then
	echo "one-spawn-owner gate: $n raw spawn call(s) outside the Process owner."
	echo "Use madc::Process (channel arm) or Process::run_and_wait (inherited stdio)"
	echo "(include/madcdis/process.h) — the Win32 backend lands behind that owner only."
	printf '%s\n' "$hits" | sed 's/^/  /'
	exit 1
fi

echo "one-spawn-owner gate: GREEN — src/madc_process.cpp is the only spawn site (fork-as-isolation exempt, see header)."
exit 0
