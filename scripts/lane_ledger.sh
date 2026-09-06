#!/bin/bash
# lane_ledger.sh — the lane-freshness ledger (owner directive 2026-08-28).
#
# THE PROBLEM IT GATES: /promote's "tests everything" was a checklist, not
# a mechanism — three lanes drifted silently in one month (SMAUG red five
# weeks in every release, the wine lane 75 red since the July 24 S4 arc,
# macOS untested across five releases). A rule without a gate decays.
#
# THE MECHANISM: every lane run records its green tally against the
# CONTENT it ran on (docs/lane-status.tsv, tracked). A lane is FRESH for
# promotion iff no code content changed between its recorded commit and
# HEAD — `git diff --quiet <recorded> HEAD -- $CODE_PATHS` — so docs-only
# commits never stale a lane, and any src/tests/scripts change stales
# every lane that has not re-run. `check --promote` fails LOUDLY on any
# stale promote-gated lane; the pre-push hook runs it on a develop push, and
# `check --release` — the same check over the develop set PLUS the release
# tier (promote=release) — on a master push and in /promote.
#
# Usage:
#   lane_ledger.sh record <lane> <tally...>   stamp HEAD+date for a GREEN run
#   lane_ledger.sh check [--promote|--release]
#                                             list staleness; --promote exits 1
#                                             on any stale push-gated lane
#                                             (promote=yes: the develop push
#                                             gate); --release ALSO gates the
#                                             release tier (promote=release —
#                                             owner law 2026-09-04: every
#                                             platform lane's FULL suite is
#                                             green before a master release;
#                                             the master push runs it)
#   lane_ledger.sh selftest                   the negative control (check must
#                                             FAIL a stale row and PASS a fresh
#                                             one); check runs it first itself
set -u
cd "$(dirname "$0")/.."

LEDGER="${MADC_LANE_LEDGER:-docs/lane-status.tsv}"
# Content that constitutes "the code a lane validates". Docs and status
# files are excluded on purpose.
CODE_PATHS="src include third_party tests scripts tools examples"

HEADER=$'# lane\tpromote\tlast_green_commit\tdate\ttally'

ensure_ledger() {
	if [ ! -f "$LEDGER" ]; then
		printf '%s\n' "$HEADER" > "$LEDGER"
	fi
}

record() {
	local lane="$1"; shift
	local tally="$*"
	local sha date promote
	sha=$(git rev-parse --short=12 HEAD)
	date=$(date +%Y-%m-%d)
	ensure_ledger
	# Preserve the lane's promote flag; new lanes default to yes —
	# a lane worth recording is a lane worth gating (opt OUT in the
	# file, visibly, not by omission).
	promote=$(awk -F'\t' -v l="$lane" '$1==l{print $2}' "$LEDGER")
	[ -z "$promote" ] && promote=yes
	grep -v -P "^$lane\t" "$LEDGER" > "$LEDGER.tmp" || true
	printf '%s\t%s\t%s\t%s\t%s\n' \
		"$lane" "$promote" "$sha" "$date" "$tally" >> "$LEDGER.tmp"
	mv "$LEDGER.tmp" "$LEDGER"
	echo "lane_ledger: recorded $lane green at $sha ($date): $tally"
}

# stale_reason <sha> -> prints why the recorded content is stale (empty =
# fresh). Unknown shas are stale by definition (rewritten history, shallow
# clone): the lane must simply re-run.
stale_reason() {
	local sha="$1"
	if ! git rev-parse --quiet --verify "$sha^{commit}" > /dev/null; then
		echo "recorded commit $sha not found"
		return
	fi
	# shellcheck disable=SC2086
	if ! git diff --quiet "$sha" HEAD -- $CODE_PATHS; then
		echo "code content changed since $sha"
	fi
}

# gate_applies <mode> <promote-flag>: does a STALE row with this flag block
# under this check mode? --promote (the develop push) blocks on `yes`;
# --release (the master push / promotion) blocks on `yes` AND `release` —
# the platform lanes whose FULL suite must be green before a master release
# (owner law 2026-09-04) but whose cost or hardware keeps them off every
# develop push (the libc++ flavor lane, the darwin runner suite, genuine
# Windows). `no` never blocks; it is recorded for the record.
gate_applies() {
	case "$1" in
		--promote) [ "$2" = "yes" ];;
		--release) [ "$2" = "yes" ] || [ "$2" = "release" ];;
		*) return 1;;
	esac
}

check() {
	local promote_gate="${1:-}"
	ensure_ledger
	local rc=0 lane promote sha date tally reason
	printf '%-16s %-8s %-14s %-12s %s\n' LANE PROMOTE STATE LAST-GREEN TALLY
	while IFS=$'\t' read -r lane promote sha date tally; do
		case "$lane" in ''|'#'*) continue;; esac
		reason=$(stale_reason "$sha")
		if [ -z "$reason" ]; then
			printf '%-16s %-8s %-14s %-12s %s\n' \
				"$lane" "$promote" FRESH "$date" "$tally"
		else
			printf '%-16s %-8s %-14s %-12s %s (%s)\n' \
				"$lane" "$promote" STALE "$date" "$tally" "$reason"
			if gate_applies "$promote_gate" "$promote"; then
				rc=1
			fi
		fi
	done < "$LEDGER"
	if [ "$rc" -ne 0 ]; then
		echo "lane_ledger: ${promote_gate#--} BLOCKED — stale gated" \
		     "lane(s) above must re-run on current content" >&2
	fi
	return $rc
}

# The negative control: a stale row MUST fail --promote and a fresh row
# MUST pass. A gate that cannot fail is not a gate. IN-PROCESS on
# purpose: it calls the check FUNCTION in a subshell — never "$0", whose
# check entrypoint would re-run selftest and recurse without bound (the
# 2026-08-28 QNAP fork spiral; this comment is the tombstone).
selftest() {
	local tmp head
	tmp=$(mktemp)
	head=$(git rev-parse --short=12 HEAD)
	# A commit that certainly differs in code content from HEAD: HEAD's
	# parent chain — walk back to the first commit that changed code.
	local stale_sha
	# shellcheck disable=SC2086
	stale_sha=$(git log --format=%h -n 1 --skip=1 HEAD -- $CODE_PATHS)
	{
		printf '%s\n' "$HEADER"
		printf 'selftest-stale\tyes\t%s\tnever\t0/0\n' "$stale_sha"
	} > "$tmp"
	if ( LEDGER="$tmp"; check --promote ) > /dev/null 2>&1; then
		echo "lane_ledger: SELFTEST FAILED — a stale row passed" >&2
		rm -f "$tmp"
		return 1
	fi
	{
		printf '%s\n' "$HEADER"
		printf 'selftest-fresh\tyes\t%s\ttoday\t1/0\n' "$head"
	} > "$tmp"
	if ! ( LEDGER="$tmp"; check --promote ) > /dev/null 2>&1; then
		echo "lane_ledger: SELFTEST FAILED — a fresh row blocked" >&2
		rm -f "$tmp"
		return 1
	fi
	# The release tier: a stale `release` row must NOT block a develop push
	# (--promote) and MUST block a master push (--release).
	{
		printf '%s\n' "$HEADER"
		printf 'selftest-release-stale\trelease\t%s\tnever\t0/0\n' "$stale_sha"
	} > "$tmp"
	if ! ( LEDGER="$tmp"; check --promote ) > /dev/null 2>&1; then
		echo "lane_ledger: SELFTEST FAILED — a stale release-tier row blocked a develop push" >&2
		rm -f "$tmp"
		return 1
	fi
	if ( LEDGER="$tmp"; check --release ) > /dev/null 2>&1; then
		echo "lane_ledger: SELFTEST FAILED — a stale release-tier row passed --release" >&2
		rm -f "$tmp"
		return 1
	fi
	rm -f "$tmp"
	echo "lane_ledger: selftest OK (stale row blocks, fresh row passes, release tier gates master only)"
}

case "${1:-}" in
	record)   shift; record "$@";;
	check)    shift || true
		  selftest || exit 1	# the gate proves it can fail, every use
		  check "${1:-}";;
	selftest) selftest;;
	*) echo "usage: $0 record <lane> <tally...> | check [--promote|--release] | selftest" >&2
	   exit 2;;
esac
