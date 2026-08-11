#!/usr/bin/env bash
# resume.sh — rehydration preflight. Run this FIRST on every session resume /
# post-compaction, BEFORE editing files or running git.
#
# A compaction summary tells you what you were DOING; it does NOT tell you the
# live ground truth — what's still running, where git actually is, or whether a
# background task committed while you were away. This prints that ground truth
# in one shot so you can re-orient instead of acting on stale assumptions.
#
# The 2026-06-01 incident: orphaned background tasks survived /compact, kept
# relaunching `make fulltest` (hanging test_cir, pegging the host), AND a
# background subagent committed to the branch mid-edit -> a commit/revert/reapply
# collision. This script surfaces all three classes before they bite.
#
# Usage:
#   bash scripts/resume.sh           # report only
#   bash scripts/resume.sh --kill    # also kill runaway test/torture processes

set -u
KILL=0
[ "${1:-}" = "--kill" ] && KILL=1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

bar() { printf '\n=== %s ===\n' "$1"; }

bar "REPO ($ROOT)"
echo "branch:      $(git rev-parse --abbrev-ref HEAD)"
echo "HEAD:        $(git log --oneline -1)"
up="$(git rev-parse --abbrev-ref --symbolic-full-name @{u} 2>/dev/null)"
if [ -n "$up" ]; then
	echo "upstream:    $up -> $(git rev-parse --short "$up" 2>/dev/null)"
	ab="$(git rev-list --left-right --count HEAD..."$up" 2>/dev/null)"
	echo "ahead/behind upstream: ${ab:-unknown}  (left=ahead, right=behind)"
fi
dirty="$(git status --short --untracked-files=no)"
if [ -n "$dirty" ]; then
	echo "UNCOMMITTED tracked changes:"; echo "$dirty"
else
	echo "working tree: clean (tracked)"
fi

bar "RECENT COMMITS + REFLOG (catch concurrent/collision commits you didn't make)"
git log --oneline -5
echo "--- reflog (last 6) ---"
git reflog -6

bar "MIR (third_party/mir — subtree, ordinary madc source)"
if [ -d third_party/mir ]; then
	echo "present; last commit touching it:"
	git log --oneline -1 -- third_party/mir
else
	echo "MISSING — the tree predates the subtree migration (2026-08-11)"
fi
# The old standalone fork checkout is PR-transport only (upstream PRs to
# vnmakarov/mir ride derekbsnider/mir); it is NOT a build input anymore.
if [ -d /workspace/mir/.git ]; then
	echo "--- legacy fork checkout (/workspace/mir, PR transport only) ---"
	git -C /workspace/mir log --oneline -1
fi

bar "RUNAWAY PROCESSES (test/torture/madc that survived a compaction)"
# Match the heavy, host-pegging offenders by command, excluding this script.
pat='test_cir|test_c2mir|run_gcc_testsuite|fulltest|loopprobe|bin/madc|[ /]c2m '
runaways="$(ps -eo pid,ppid,etimes,pcpu,args 2>/dev/null | grep -E "$pat" | grep -vE 'grep|resume\.sh')"
if [ -n "$runaways" ]; then
	echo "!! FOUND running test/build processes — likely orphans if you just resumed:"
	echo "$runaways"
	if [ "$KILL" = "1" ]; then
		echo "--kill: terminating them..."
		pkill -9 -f 'test_cir' 2>/dev/null
		pkill -9 -f 'test_c2mir' 2>/dev/null
		pkill -9 -f 'run_gcc_testsuite' 2>/dev/null
		pkill -9 -f 'loopprobe' 2>/dev/null
		pkill -9 -f 'fulltest' 2>/dev/null
		echo "done. survivors:"
		ps -eo pid,etimes,pcpu,args 2>/dev/null | grep -E "$pat" | grep -vE 'grep|resume\.sh' || echo "  (clean)"
	else
		echo "   -> re-run with --kill to terminate, OR verify they are a CURRENT run before killing."
	fi
else
	echo "clean — no runaway test/build processes."
fi
echo "load average: $(cat /proc/loadavg 2>/dev/null)"

bar "BUILD FRESHNESS"
if [ -x bin/madc ]; then
	newer="$(find src include -newer bin/madc -name '*.cpp' -o -newer bin/madc -name '*.h' 2>/dev/null | head -3)"
	if [ -n "$newer" ]; then
		echo "bin/madc is STALE — source changed since last build:"; echo "$newer"
		echo "  -> run: make -C src"
	else
		echo "bin/madc up to date with src/ + include/"
	fi
else
	echo "bin/madc missing -> run: make -C src"
fi

bar "REHYDRATION CORPUS (read these to recover full context — target >= 100k tokens)"
# A compaction can leave only ~3% context. Re-ground by READING this curated,
# prioritized set — it is ~100k+ tokens of genuinely-relevant material (not the
# whole repo). Tiers are ordered so even a partial read covers the essentials.
# Token estimate = bytes/4. Read top-to-bottom; stop when you have enough.
mem="/home/dev/.claude/projects/-workspace-madc/memory"
declare -a T1=(
  "docs/superpowers/plans/2026-06-01-HANDOFF.md"
  "claude_status.json"
  "docs/parity/root-cause-worklist.md"
  "docs/plans/madc-vision-and-invariants.md"
  "AGENTS.md"
  "docs/adr/0001-cir-c2mir-backend.md"
)
declare -a T2=( .claude/rules/*.md docs/rules/*.md )
declare -a T3=( "$mem"/*.md )
declare -a T4=(
  "docs/plans/cpp-support.md" "docs/plans/ROADMAP.md"
  "docs/plans/2026-05-30-template-instantiation.md"
  "docs/parity/cir-vs-asmjit-regressions.txt"
  "docs/superpowers/plans/2026-05-31-RESTART-HANDOFF.md"
)
total=0
tier() { # $1=label  rest=files
  local label="$1"; shift; local sum=0 c
  for f in "$@"; do [ -f "$f" ] || continue; c=$(wc -c < "$f"); sum=$((sum + c/4)); done
  printf "  %-26s ~%6d tok\n" "$label" "$sum"; total=$((total + sum))
}
tier "T1 live state (READ ALL)" "${T1[@]}"
tier "T2 rules (how to work)"    "${T2[@]}"
tier "T3 project memory"         "${T3[@]}"
tier "T4 deep dives (as needed)" "${T4[@]}"
printf "  %-26s ~%6d tok\n" "TOTAL curated corpus" "$total"
if [ "$total" -lt 100000 ]; then
  echo "  !! corpus < 100k tokens — handoff/status/memory have thinned; ENRICH before relying on it."
else
  echo "  OK: >= 100k tokens of relevant rehydration material available."
fi
echo "  (T1+T2+T3 alone is the must-read floor; T4 + key src/ files as the task needs.)"

bar "NEXT"
_cur_branch=$(git -C /workspace/madc rev-parse --abbrev-ref HEAD 2>/dev/null)
if [ "$_cur_branch" = "feature/header-partition-claude" ]; then
  echo "ACTIVE LINE (this branch): READ FIRST -> docs/plans/2026-06-09-fstream-construction-HANDOFF.md"
  echo "  THEN the design corpus it lists (clever-scribbling-dove.md + retire-std-hardcoding-design.md)."
  echo "  DO NOT re-derive plans/building-blocks — grep + read the design first (the recurring failure)."
  echo "  PROBE before theorizing; --emit=c11 is NOT layout/correctness truth; RUN to validate."
  echo "  STATE: real libstdc++ <fstream> ofstream CONSTRUCTS via real C1 ctor + WRITES 'hello42'"
  echo "  + DESTRUCTS cleanly (EXIT 0) — the dtor SIGSEGV is FIXED (ecfc856). Root cause WAS"
  echo "  undersizing, confirmed in the POST-CHECK c2mir tree (NOT --emit=c11): member_node's"
  echo "  is_class_object path emitted an EMPTY declarator, dropping N_ARR dims, so ios_base's"
  echo "  _Words _M_local_word[8] collapsed to 1 elem (-112B) -> libstdc++ C1 overflowed the slot."
  echo "  Keystone landed: mangled-direct construction/destruction for externally-defined classes"
  echo "  (6b5d4ea) + combined-typedef hoist + external complete-dtor (38d9152) + W2 derived-stream"
  echo "  operator binding (22c5b53) + instantiation-cache/is_complete (39878b7) + by-value struct"
  echo "  ordering (e6beebc) + vptr-in-typedef (e4fca20) + array-of-class-object dims (ecfc856)."
  echo "  GREEN TIP = d7344ac (fulltest 543/4; cout+ofstream realhdr OK). 30 commits ahead, UNPUSHED."
  echo "  LIVE FRONT = real <string> (gates getline -> testfstream/testloop). The __void_t SFINAE"
  echo "  DETECTION IDIOM for iterator_traits is now IMPLEMENTED + PROVEN (g++ oracle reproduced:"
  echo "  iterator_traits<__normal_iterator<char*,string>>::iterator_category = random_access_iterator_tag)."
  echo "  That WIP lives on branch feature/cpp-detection-idiom-claude (off d7344ac) — NOT on the green"
  echo "  tip because it advances str1/cout-realhdr two layers but does not yet COMPILE them (regresses"
  echo "  testcout_realhdr -> fulltest 542/5 mid-chain). NEXT LAYER (handoff §12.7): 'Unknown namespace"
  echo "  pointer' at parser.cpp:11947 — std::pointer_traits<pointer>::pointer_to(...) in"
  echo "  basic_string::_M_local_data() = a template-id-qualified STATIC call in EXPRESSION context the"
  echo "  expr-arm doesn't handle (it captures the inner arg 'pointer' as the :: qualifier). TO CONTINUE:"
  echo "  git checkout feature/cpp-detection-idiom-claude; make -C src; attack site 11947 (mirror"
  echo "  resolve_typename_type_token's type-context handling). Rule #2 NO shims; g++ oracle; gate hard."
  echo "  Then <sstream> (#23). (Supersedes 2026-06-08-header-partition-W2-perf-HANDOFF.md.)"
elif [ "$_cur_branch" = "feature/cpp-detection-idiom-claude" ]; then
  echo "ACTIVE LINE (this WIP branch): READ FIRST -> docs/plans/2026-06-09-fstream-construction-HANDOFF.md"
  echo "  top ⚑ MILESTONE banner, THEN docs/plans/2026-06-09-lazy-member-body-instantiation-plan.md §9."
  echo "  HEAD ~aef0366 = green tip + detection idiom + LAZY member-body instantiation + extern-template"
  echo "  binding + Pass 1.9 re-materialization. MILESTONE: real libstdc++ <string> AND <iostream>"
  echo "  COMPILE+RUN (str1 exit 0; testcout 'This is a test, x = -1'). ALL GATES GREEN: fulltest 543/4"
  echo "  (testcout_realhdr GREEN — 542/5 regression healed), gcc.c-torture 1566/31/57/1 UNCHANGED,"
  echo "  canaries OK. THIS CHAIN IS GREEN+GATED — landing-ready onto green tip header-partition-claude"
  echo "  (fast-forward) when the user OKs. DO NOT push remote without asking."
  echo "  NEXT (task #25): richer string / getline. 'std::string s=\"x\"; s+=...' still fails at"
  echo "  basic_string.h:3486 as c2mir CHECK errors (NOT parse): operator+= madc-emitted reaches"
  echo "  _M_local_data, which returns 'pointer' = allocator_traits<allocator<char>>::pointer"
  echo "  __detected_or_t STRUCT not char* (§12.1). FIX = select allocator_traits<allocator<_Tp>>"
  echo "  partial spec (pointer=_Tp*); c80577c's unify_nested_spec_pattern_arg isn't selecting it"
  echo "  (probe tmp/at1.mad). Then getline -> testfstream/testloop. Rule #2 NO shims; g++ oracle;"
  echo "  gate fulltest+torture-ALONE+canaries."
elif [ "$_cur_branch" = "feature/realhdr-parse-gaps2-claude" ]; then
  echo "ACTIVE LINE (this branch): READ FIRST -> docs/plans/2026-06-08-smaug-project-boot-HANDOFF.md"
  echo "  SMAUG boots from a FRESH compile via the umbrella AND --project (-lcrypt)."
  echo "  HEAD 4aa0a20; fulltest 537/4; gcc.c-torture 1566/31/57/1. NEXT (deferred #1/#2):"
  echo "  productize SMAUG --project in MadSMAUG + the auto-#load toggle (see the handoff)."
  echo "  (The older 2026-06-08-codex-integration-and-smaug-revival handoff is SUPERSEDED.)"
elif [ "$_cur_branch" = "feature/retire-std-hardcoding-claude" ]; then
  echo "ACTIVE LINE (this branch): READ FIRST -> docs/superpowers/plans/2026-06-02-retire-std-hardcoding-HANDOFF.md"
  echo "  NOTE: this campaign has been merged to develop; do not resume stale 475/gate-468 worklists."
  echo "  Current develop state is authoritative: check claude_status.json and docs/test-status.md."
else
  echo "Authoritative state + next tasks: claude_status.json and docs/test-status.md"
  echo "Parity worklist: docs/parity/root-cause-worklist.md   |   canonical: claude_status.json"
fi
echo "ALWAYS cap test runs:  ( ulimit -t 120; timeout 180 <cmd> )   — one heavy job at a time."
