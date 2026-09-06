#!/usr/bin/env bash
# check-live-build-owners.sh — the live-parse build/run slice's
# consolidated owners stay single (DupFamilies native_build_kind_map,
# parse_tree_backend_ready, fork_child_runtime_reset).
#
# 1. The "exe"/"obj" -> MadcNativeKind vocabulary has ONE owner:
#    native_kind_of (src/madc_program.cpp). Marker: the kind_name
#    comparison spelling appears exactly once — a lane mapping kind
#    names itself has drifted off the owner.
# 2. "May this retained tree reach the backend" has ONE owner:
#    parse_tree_backend_ready. Marker: the inline gate spelling
#    (tkProgram && !child_has_error_row) appears exactly once (the
#    owner's body).
# 3. Every fork() child in madc_program.cpp RUNS madc code (isolation
#    eval children + parse_run; there is no fork+exec here), so every
#    one must reset the cooperative scheduler: the fork-site count and
#    the __madc_task_atfork_child() call count must match. A new fork
#    lane without the reset can schedule parent task contexts.
set -u

FILE="$(dirname "$0")/../src/madc_program.cpp"

count_kind()
{
	grep -c 'kind_name == "exe"' "$1"
}

count_gate()
{
	grep -c 'tkProgram && !child_has_error_row' "$1"
}

count_forks()
{
	grep -c 'pid_t pid = fork();' "$1"
}

count_resets()
{
	grep -c '__madc_task_atfork_child();' "$1"
}

fail=0

n=$(count_kind "$FILE")
if [ "$n" -ne 1 ]; then
	echo "check-live-build-owners: FAIL — $n kind-name mapping sites" \
	     "(expected 1: native_kind_of). Route kind names through the" \
	     "owner." >&2
	fail=1
fi

n=$(count_gate "$FILE")
if [ "$n" -ne 1 ]; then
	echo "check-live-build-owners: FAIL — $n inline backend-ready gates" \
	     "(expected 1: parse_tree_backend_ready's body). Call the owner." >&2
	fail=1
fi

nf=$(count_forks "$FILE")
nr=$(count_resets "$FILE")
if [ "$nf" -ne "$nr" ]; then
	echo "check-live-build-owners: FAIL — $nf fork() children but $nr" \
	     "__madc_task_atfork_child() resets in src/madc_program.cpp." \
	     "Every forked child that runs madc code resets the scheduler" \
	     "(rt_task.h fork discipline); a fork+exec lane here would be" \
	     "new — decide its discipline explicitly." >&2
	fail=1
fi

# Negative controls: each marker must see a synthetic violation.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
{
	echo 'static void __synthetic(const std::string &kind_name, ::Program &child) {'
	echo '    if ( kind_name == "exe" ) return;'
	echo '    if ( child.tkProgram && !child_has_error_row(child) ) return;'
	echo '    pid_t pid = fork(); (void)pid;'
	echo '}'
} >> "$tmp"
if [ "$(count_kind "$tmp")" -ne 2 ] \
|| [ "$(count_gate "$tmp")" -ne 2 ] \
|| [ "$(count_forks "$tmp")" -ne $((nf + 1)) ]; then
	rm -f "$tmp"
	echo "check-live-build-owners: FAIL — a negative control did not" \
	     "detect its synthetic violation (a marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

if [ "$fail" -ne 0 ]; then
	exit 1
fi

echo "check-live-build-owners: OK (native_kind_of; parse_tree_backend_ready;" \
     "$nf fork children, $nr scheduler resets)"
exit 0
