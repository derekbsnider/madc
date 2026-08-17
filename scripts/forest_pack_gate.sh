#!/bin/bash
# forest_pack_gate.sh — the forest pack's SILENT-DEGRADATION gate (task #63).
#
# A pack run exits 0 while tolerating parse failures, and a BIND can lose a
# whole aggregate without a word. Task #64 was exactly that: a class-nested
# enum tag was stamped a forest type-id but never recorded, so std::ios_base
# could not be filled at bind, the fill bailed with a bare `continue`, and the
# entire darwin iostream family vanished. The pack still said OK. The symptom
# arrived a layer later, on a Mac, as `struct has no member __vptr`.
#
# The three load-side diagnostics that would have named it were added in
# v0.82.0. This script is the gate that reads them, plus the ratchet over the
# pack's own tolerated parse errors. Two halves, deliberately different
# strictness:
#
#   PRODUCER (pack log)  ratchet — tolerated parse errors may only go DOWN.
#                        Every one is a header construct madc could not parse;
#                        the class breakdown printed below IS the burndown list.
#
#   CONSUMER (load log)  HARD ZERO for the three losses that have no legitimate
#                        instance, and a ratchet for the one that does.
#
# Usage:
#   bash scripts/forest_pack_gate.sh --profile <key> [--pack-log F] [--load-log F]
#                                    [--baseline F]
#   bash scripts/forest_pack_gate.sh --selftest
#
# Profiles are the pack scripts' own names for themselves — linux (forest_pack.sh),
# win64 (forest_pack_windows.sh), darwin (forest_pack_darwin.sh). Each pack
# invocation checks its OWN log, so two arches sharing a baseline key still
# diverge loudly (each is compared separately).
#
# NO cd: forest_pack_darwin.sh is invoked from src/ with ../-relative paths, so
# log arguments are used exactly as given and only the baseline resolves
# relative to this script.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASELINE="$ROOT/docs/parity/pack-degradation-baseline.txt"
PROFILE=
PACK_LOG=
LOAD_LOG=
SELFTEST=0

# ---------------------------------------------------------------------------
# The token contract. These four strings ARE the interface between the
# compiler's diagnostics and this gate; scripts/mac_battery.sh carries the
# same list for the darwin consumer side (it must stay self-contained — it
# runs on a Mac from an unpacked tarball, with no repo), and --selftest leg 8
# fails if the two copies drift apart.
#
# MARKER is the negative control ON THE CHECK ITSELF: all four load-side
# diagnostics are DBG-gated, so a load log from a run that never bound
# anything contains zero of everything and would pass vacuously. No marker,
# no verdict.
# ---------------------------------------------------------------------------
MARKER='materialize filter:'
TOK_FILL='materialize fill: DROPPED'
TOK_DERIVED='materialize derived: UNRESOLVED'
TOK_SKIPPED='forest_restore_decls: SKIPPED'
TOK_CLOSURE='materialize closure: DROPPED'
# Producer-side loss: the MIR module cache blob failed to compile and was
# dropped with "consumers fall back". Correctness survives; every consumer
# compile then pays full price. Tolerated + exit 0 = exactly this gate's remit.
TOK_BLOB='blob skipped'

usage() {
	echo "usage: $0 --profile <key> [--pack-log FILE] [--load-log FILE] [--baseline FILE]" >&2
	echo "       $0 --selftest" >&2
}

while [ $# -gt 0 ]; do
	case "$1" in
		--profile)  [ $# -ge 2 ] || { usage; exit 2; }; PROFILE="$2"; shift 2 ;;
		--pack-log) [ $# -ge 2 ] || { usage; exit 2; }; PACK_LOG="$2"; shift 2 ;;
		--load-log) [ $# -ge 2 ] || { usage; exit 2; }; LOAD_LOG="$2"; shift 2 ;;
		--baseline) [ $# -ge 2 ] || { usage; exit 2; }; BASELINE="$2"; shift 2 ;;
		--selftest) SELFTEST=1; shift ;;
		-h|--help)  usage; exit 0 ;;
		*) usage; exit 2 ;;
	esac
done

RED=0
say()  { echo "forest_pack_gate: $*"; }
fail() { echo "forest_pack_gate: RED — $*" >&2; RED=1; }

# validate_baseline — run in the MAIN shell, before any lookup. baseline_for
# runs inside $( ), where an exit would only leave the subshell and a malformed
# file would silently read as "0 allowed" — a broken ratchet that passes.
validate_baseline() {
	local count profile metric extra
	[ -f "$BASELINE" ] || return 0
	while read -r count profile metric extra; do
		case "${count:-}" in ""|\#*) continue ;; esac
		if [ -n "${extra:-}" ] || [ -z "${metric:-}" ] \
		   || ! printf '%s' "$count" | grep -qE '^[0-9]+$'; then
			echo "forest_pack_gate: malformed baseline line in $BASELINE:" >&2
			echo "  $count ${profile:-} ${metric:-} ${extra:-}" >&2
			echo "  expected: <count> <profile> <metric>" >&2
			exit 1
		fi
	done < "$BASELINE"
}

# baseline_for <profile> <metric> — the allowed count. A missing line means
# ZERO allowed, so a new profile or a new metric cannot appear silently with
# room to spare (the warning-ratchet convention, docs/parity/warning-baseline.txt).
baseline_for() {
	local want_profile="$1" want_metric="$2" count profile metric extra
	[ -f "$BASELINE" ] || { echo 0; return; }
	while read -r count profile metric extra; do
		case "${count:-}" in ""|\#*) continue ;; esac
		if [ "$profile" = "$want_profile" ] && [ "$metric" = "$want_metric" ]; then
			echo "$count"
			return
		fi
	done < "$BASELINE"
	echo 0
}

# normalize <log> <out> — one readable stream: NULs dropped (a -v dump carries
# them, and grep would call the file binary), ANSI colour stripped (madc's
# diagnostic prints `\e[1;31merror:\e[1;37m`, so the anchored pattern below
# only exists after the strip), CR dropped (wine's CRT writes CRLF).
normalize() {
	tr -d '\000\r' < "$1" | LC_ALL=C sed -e 's/\x1b\[[0-9;]*m//g' > "$2"
}

# count_pat <file> <pattern> — plain substring count, binary-tolerant.
count_pat() {
	grep -acF -- "$2" "$1" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# PRODUCER half — the pack's tolerated parse errors.
#
# The pattern is `: error: ` on the NORMALIZED stream, not a bare `error`.
# `grep -c error` over a pack log reads 339: it also matches `filesystem_error`
# inside the deliberate `pack drop:` lines. Even `grep -c 'error:'` reads high,
# because `/usr/include/c++/13/system_error:616:6:` contains it. Anchoring on
# the diagnostic's own shape (`<file>:<line>:<col>: error: <message>`, see
# Program::print_diagnostic) is the only count that means what it says.
# ---------------------------------------------------------------------------
check_pack_log() {
	local log="$1" norm="$2" n allowed blobs blobs_allowed
	n=$(grep -ac ': error: ' "$norm" 2>/dev/null || true)
	allowed=$(baseline_for "$PROFILE" pack-errors)
	blobs=$(count_pat "$norm" "$TOK_BLOB")
	blobs_allowed=$(baseline_for "$PROFILE" mir-blob-skips)
	say "[$PROFILE] pack parse errors: $n (baseline $allowed), mir-blob-skips: $blobs/$blobs_allowed — $log"
	if [ "$blobs" -gt "$blobs_allowed" ]; then
		fail "[$PROFILE] MIR cache blob skips $blobs > baseline $blobs_allowed — the pack"
		grep -aF -- "$TOK_BLOB" "$norm" | sed -E 's/^([^:]*: mir cache: )//' | head -5 >&2
		echo "       ships WITHOUT its module cache, so every consumer compile pays" >&2
		echo "       full price. Correctness survives, which is why it exits 0." >&2
	elif [ "$blobs" -lt "$blobs_allowed" ]; then
		say "[$PROFILE] IMPROVED — lower '$PROFILE mir-blob-skips' to $blobs in $BASELINE"
	fi
	if [ -s "$norm" ] && [ "$n" -gt 0 ]; then
		echo "--- pack parse-error classes (the burndown list) ---"
		# Quoted names -> 'X' and generated instantiation spellings -> <T>:
		# without the second one, six copies of the SAME missing overload
		# read as six separate classes purely because
		# __alloc_traits_std__allocator_char16_t__char16_t differs per
		# instantiation. Long identifiers are the generated ones.
		grep -a ': error: ' "$norm" \
			| sed -E 's/^.*: error: //' \
			| sed -E "s/'[^']*'/'X'/g" \
			| sed -E 's/[A-Za-z_][A-Za-z0-9_]{19,}/<T>/g' \
			| sort | uniq -c | sort -rn | head -20
		echo "----------------------------------------------------"
	fi
	if [ "$n" -gt "$allowed" ]; then
		fail "[$PROFILE] pack parse errors $n > baseline $allowed."
		echo "       A header construct that used to parse no longer does, or a new" >&2
		echo "       one entered the corpus. Fix it, or (only with a stated reason)" >&2
		echo "       raise '$PROFILE pack-errors' in $BASELINE." >&2
	elif [ "$n" -lt "$allowed" ]; then
		say "[$PROFILE] IMPROVED — lower '$PROFILE pack-errors' to $n in $BASELINE"
	fi
}

# ---------------------------------------------------------------------------
# CONSUMER half — what the bind LOST.
#
# HARD ZERO, no baseline: fill DROPPED and restore SKIPPED. Each is a record
# the consumer ADMITTED and then could not use, and a bound unit is reported
# SERVED either way, so no live parse rescues it. Neither has a legitimate
# instance. `fill: DROPPED` is the counter task #64 would have tripped.
#
# RATCHET: closure DROPPED and the DK_NONE census (UNRESOLVED with kind=0).
#
#   closure DROPPED is designed behaviour — arena_chain_ok refuses to record a
#   chain it cannot rebuild.
#
#   kind=0 means a project id whose record is DK_NONE (get_def_at returns false
#   for pinned/system ids, and has_def() is literally "get_def_at && kind !=
#   DK_NONE"), i.e. an id stamped by forest_serialize_type_id that no record
#   walk wrote. The prior handoff called that "no legitimate instance", and
#   measuring it proved otherwise: 55 of the Linux load probe's 57 are
#   DEPENDENT operands (`ref _Tp*`, `allocator__Tp1*`,
#   `polymorphic_allocator__Up*`) — a pointer to a template parameter has no
#   concrete record by construction, and the consumer re-instantiates from the
#   pattern instead. So it is a CENSUS that may only grow smaller, not a zero.
#   A ratchet still catches #64: its nested enum ADDED one, which is a rise.
#   The other 2 were real — `ptr long double*`, an unpinned primitive, fixed
#   with tests/unit/test_datadef.cpp + forest_bind_gate's [ldouble] case.
#
# UNRESOLVED without kind=0 is not counted at all: 139 are normal on a Linux
# pack (a derived type whose operand sits outside the bound closure).
# ---------------------------------------------------------------------------
check_load_log() {
	local log="$1" norm="$2" markers fill skipped dknone closure allowed
	markers=$(count_pat "$norm" "$MARKER")
	if [ "$markers" -eq 0 ]; then
		fail "[$PROFILE] load log has no '$MARKER' — nothing materialized, so"
		echo "       every count below would be zero for the wrong reason. The probe" >&2
		echo "       did not bind the container (or ran without -v): $log" >&2
		return
	fi
	fill=$(count_pat "$norm" "$TOK_FILL")
	skipped=$(count_pat "$norm" "$TOK_SKIPPED")
	dknone=$(grep -aF -- "$TOK_DERIVED" "$norm" 2>/dev/null | grep -acF -- 'kind=0' || true)
	closure=$(count_pat "$norm" "$TOK_CLOSURE")
	allowed=$(baseline_for "$PROFILE" closure-drops)
	dknone_allowed=$(baseline_for "$PROFILE" dk-none)
	say "[$PROFILE] load: fill-dropped=$fill restore-skipped=$skipped (both hard zero) dk-none=$dknone/$dknone_allowed closure-dropped=$closure/$allowed — $log"
	if [ "$fill" -gt 0 ]; then
		fail "[$PROFILE] $fill x '$TOK_FILL' — an admitted record could not be filled;"
		grep -aF -- "$TOK_FILL" "$norm" | sort | uniq -c | sort -rn | head -10 >&2
		echo "       the class materializes as NOTHING and no live parse rescues it." >&2
	fi
	if [ "$skipped" -gt 0 ]; then
		fail "[$PROFILE] $skipped x '$TOK_SKIPPED' — a restored record was not the"
		grep -aF -- "$TOK_SKIPPED" "$norm" | sort | uniq -c | sort -rn | head -10 >&2
		echo "       DataDef kind its record claimed, so the name never registered." >&2
	fi
	if [ "$dknone" -gt "$dknone_allowed" ]; then
		fail "[$PROFILE] DK_NONE cross-references $dknone > baseline $dknone_allowed — an id was"
		grep -aF -- "$TOK_DERIVED" "$norm" | grep -aF -- 'kind=0' \
			| sed -E 's/ \(operand kind=0\)$//; s/ \(param #[0-9]+ tid=[0-9]+ kind=0.*\)$/ (param)/' \
			| sort | uniq -c | sort -rn | head -10 >&2
		echo "       stamped and never RECORDED. A DEPENDENT operand (a pointer to a" >&2
		echo "       template parameter) is legitimate and is why this ratchets; a" >&2
		echo "       CONCRETE type here is task #64's shape — find the writer that" >&2
		echo "       skips the record, not the reader that trips on it." >&2
	elif [ "$dknone" -lt "$dknone_allowed" ]; then
		say "[$PROFILE] IMPROVED — lower '$PROFILE dk-none' to $dknone in $BASELINE"
	fi
	if [ "$closure" -gt "$allowed" ]; then
		fail "[$PROFILE] closure drops $closure > baseline $allowed — the container is"
		grep -aF -- "$TOK_CLOSURE" "$norm" | sort | uniq -c | sort -rn | head -10 >&2
		echo "       carrying LESS than it did; a consumer silently live-parses instead." >&2
	elif [ "$closure" -lt "$allowed" ]; then
		say "[$PROFILE] IMPROVED — lower '$PROFILE closure-drops' to $closure in $BASELINE"
	fi
}

# ---------------------------------------------------------------------------
# --selftest — the gate's own negative control, hermetic (no madc, no pack).
#
# A gate nobody has ever seen go RED is a gate that reports success for a test
# it never ran. Each leg below asserts the verdict this script MUST reach on a
# synthetic log, including the two that must stay GREEN (a routine UNRESOLVED
# and an exactly-at-baseline count) so the checks cannot be "always fail".
# ---------------------------------------------------------------------------
selftest() {
	local dir="$ROOT/tmp" bl rc legs=0 bad=0
	mkdir -p "$dir"
	bl="$dir/fpgate_selftest_baseline.txt"
	printf '# selftest baseline\n2 self pack-errors\n1 self closure-drops\n1 self dk-none\n1 self mir-blob-skips\n' > "$bl"

	leg() {
		# leg <name> <expect pass|fail> <gate args...>
		#
		# `bash "$0"`, never `"$0"`: this script ships without the exec bit
		# (like most gates here) and a rc=126 "Permission denied" is nonzero,
		# so every expect-fail leg would have passed for the wrong reason.
		# The same reason expect-fail demands EXACTLY rc=1 — the RED verdict —
		# and not merely "nonzero", which a usage error or a crash also gives.
		local name="$1" expect="$2"; shift 2
		legs=$((legs + 1))
		bash "$0" --baseline "$bl" --profile self "$@" >"$dir/fpgate_selftest_out.txt" 2>&1
		rc=$?
		if [ "$expect" = pass ] && [ "$rc" -ne 0 ]; then
			echo "forest_pack_gate: SELFTEST FAILED [$name] — expected GREEN, got rc=$rc" >&2
			cat "$dir/fpgate_selftest_out.txt" >&2
			bad=1
		elif [ "$expect" = fail ] && [ "$rc" -ne 1 ]; then
			echo "forest_pack_gate: SELFTEST FAILED [$name] — expected RED (rc=1), got rc=$rc" >&2
			cat "$dir/fpgate_selftest_out.txt" >&2
			bad=1
		else
			echo "  ok   - selftest $name (expected $expect)"
		fi
	}

	# 1-2: the producer ratchet, both directions of the boundary.
	printf 'a.h:1:1: error: one\nb.h:2:2: error: two\n' > "$dir/fpgate_st_pack_ok.log"
	leg "pack at baseline"    pass --pack-log "$dir/fpgate_st_pack_ok.log"
	printf 'a.h:1:1: error: one\nb.h:2:2: error: two\nc.h:3:3: error: three\n' > "$dir/fpgate_st_pack_bad.log"
	leg "pack over baseline"  fail --pack-log "$dir/fpgate_st_pack_bad.log"

	# 3: the counting bug this gate exists to avoid — `filesystem_error` in a
	#    deliberate drop line and `system_error:616:6:` in a warning are NOT
	#    parse errors, and a loose pattern reads them as three.
	printf 'pack drop: x (filesystem_error consumer re-instantiates)\n/usr/include/c++/13/system_error:616:6: warning -- incompatible types\nsomething about an error: not a diagnostic\n' > "$dir/fpgate_st_pack_lax.log"
	leg "loose-pattern decoys ignored" pass --pack-log "$dir/fpgate_st_pack_lax.log"

	# 3b: the MIR-cache blob loss, both directions of its boundary.
	printf 'x.tu.cpp: mir cache: MIR error during module compile: boom — blob skipped (consumers fall back)\n' > "$dir/fpgate_st_blob_ok.log"
	leg "mir blob skips at baseline" pass --pack-log "$dir/fpgate_st_blob_ok.log"
	printf 'a.tu.cpp: mir cache: boom — blob skipped (consumers fall back)\nb.tu.cpp: mir cache: bang — blob skipped (consumers fall back)\n' > "$dir/fpgate_st_blob_bad.log"
	leg "mir blob skips over baseline" fail --pack-log "$dir/fpgate_st_blob_bad.log"

	# 4: no marker => the load check must refuse to render a verdict.
	printf 'nothing bound here\n' > "$dir/fpgate_st_blind.log"
	leg "load log without marker" fail --load-log "$dir/fpgate_st_blind.log"

	# 5: a routine UNRESOLVED (operand outside the closure) stays GREEN, and
	#    closure drops exactly at baseline stay GREEN.
	printf 'materialize filter: 10 admitted\nmaterialize derived: UNRESOLVED ptr statx (operand statx)\nmaterialize derived: UNRESOLVED fptr f (param #0 tid=9 kind=3 dirent*)\nmaterialize closure: DROPPED x (member y)\n' > "$dir/fpgate_st_load_ok.log"
	leg "load clean" pass --load-log "$dir/fpgate_st_load_ok.log"

	# 6-9: each hard-zero loss, one at a time.
	printf 'materialize filter: 10 admitted\nmaterialize fill: DROPPED ios_base (member __fn_ type)\n' > "$dir/fpgate_st_fill.log"
	leg "fill DROPPED" fail --load-log "$dir/fpgate_st_fill.log"
	# The DK_NONE census RATCHETS (dependent operands are legitimate), so both
	# directions of ITS boundary need a leg — at baseline GREEN, one over RED.
	printf 'materialize filter: 10 admitted\nmaterialize derived: UNRESOLVED ref _Tp* (operand kind=0)\n' > "$dir/fpgate_st_dknone_ok.log"
	leg "DK_NONE census at baseline" pass --load-log "$dir/fpgate_st_dknone_ok.log"
	printf 'materialize filter: 10 admitted\nmaterialize derived: UNRESOLVED ref _Tp* (operand kind=0)\nmaterialize derived: UNRESOLVED fptr __fn_ (param #0 tid=815 kind=0)\n' > "$dir/fpgate_st_dknone.log"
	leg "DK_NONE census over baseline" fail --load-log "$dir/fpgate_st_dknone.log"
	printf 'materialize filter: 10 admitted\nforest_restore_decls: SKIPPED class X (restored object is not a DataDefCLASS)\n' > "$dir/fpgate_st_skip.log"
	leg "restore SKIPPED" fail --load-log "$dir/fpgate_st_skip.log"
	printf 'materialize filter: 10 admitted\nmaterialize closure: DROPPED a (member p)\nmaterialize closure: DROPPED b (base q)\n' > "$dir/fpgate_st_closure.log"
	leg "closure drops over baseline" fail --load-log "$dir/fpgate_st_closure.log"

	# 10: the token contract has ONE owner. scripts/mac_battery.sh must stay
	#     self-contained (it runs on a Mac from a tarball), so it carries its
	#     own copy of these strings — the only way that copy cannot rot into a
	#     vacuously-green Mac leg is to check it from here.
	legs=$((legs + 1))
	local battery="$ROOT/scripts/mac_battery.sh" missing=
	if [ ! -f "$battery" ]; then
		echo "forest_pack_gate: SELFTEST FAILED [token owner] — $battery is gone" >&2
		bad=1
	else
		for tok in "$MARKER" "$TOK_FILL" "$TOK_DERIVED" "$TOK_SKIPPED"; do
			grep -qF -- "$tok" "$battery" || missing="$missing [$tok]"
		done
		if [ -n "$missing" ]; then
			echo "forest_pack_gate: SELFTEST FAILED [token owner] — mac_battery.sh lost:$missing" >&2
			echo "       Its darwin load-side leg would go vacuously green. Update both." >&2
			bad=1
		else
			echo "  ok   - selftest token contract shared with mac_battery.sh"
		fi
	fi

	if [ "$bad" -ne 0 ]; then
		echo "forest_pack_gate: SELFTEST RED" >&2
		return 1
	fi
	say "SELFTEST OK ($legs legs)"
	return 0
}

if [ "$SELFTEST" -eq 1 ]; then
	selftest
	exit $?
fi

if [ -z "$PROFILE" ]; then
	usage
	exit 2
fi
if [ -z "$PACK_LOG" ] && [ -z "$LOAD_LOG" ]; then
	echo "forest_pack_gate: nothing to check — pass --pack-log and/or --load-log" >&2
	exit 2
fi

validate_baseline
mkdir -p "$ROOT/tmp"
if [ -n "$PACK_LOG" ]; then
	if [ ! -f "$PACK_LOG" ]; then
		fail "[$PROFILE] pack log is missing: $PACK_LOG"
	else
		normalize "$PACK_LOG" "$ROOT/tmp/fpgate_${PROFILE}_pack.norm"
		check_pack_log "$PACK_LOG" "$ROOT/tmp/fpgate_${PROFILE}_pack.norm"
	fi
fi
if [ -n "$LOAD_LOG" ]; then
	if [ ! -f "$LOAD_LOG" ]; then
		fail "[$PROFILE] load log is missing: $LOAD_LOG"
	else
		normalize "$LOAD_LOG" "$ROOT/tmp/fpgate_${PROFILE}_load.norm"
		check_load_log "$LOAD_LOG" "$ROOT/tmp/fpgate_${PROFILE}_load.norm"
	fi
fi

if [ "$RED" -ne 0 ]; then
	echo "forest_pack_gate: FAILED [$PROFILE]" >&2
	exit 1
fi
say "OK [$PROFILE]"
