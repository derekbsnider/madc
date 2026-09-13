#!/bin/bash
# GATE — ONE git owner (Nexus L4a, design docs/plans/2026-09-13-nexus-L4-design.md
# §4.2).
#
# The rule: every libgit2 call (git_* from <git2.h>) lives in
# src/madcdis_git_repo.cpp, the READ-ONLY madc::GitRepo; nothing else in
# src/, include/ or tools/ includes <git2.h>, calls a git_* API, or spawns a
# git binary (exec://git …). Consumers use GitRepo (C++) or madc::git_* (the
# dialect) — so "network off" and "read-only" are properties of ONE file.
# tests/ are exempt: unit fixtures build repositories through libgit2's own
# write API (the oracle, not a second owner).
set -u
cd "$(dirname "$0")/.."

fail=0
api='\bgit_(repository|revwalk|commit|blame|tree|blob|reference|status|revparse|object|remote|clone|index|signature|libgit2)_[a-z_]+[[:space:]]*\('
inc='#include[[:space:]]*[<"]git2(/|\.h|>)'
# madc's own row shapers (value git_<record>_value(...)) share the git_ prefix
# by design; their names are READ from the owner's header, never listed here.
shapers=$(grep -oE 'value[[:space:]]+(git_[a-z_]+_value)[[:space:]]*\(' include/madcdis/git_repo.h \
	| sed -E 's/value[[:space:]]+//; s/[[:space:]]*\($//' | paste -sd'|' -)
[ -z "$shapers" ] && shapers='__no_shapers__'

hits=$(grep -rnE "$inc|$api" src include tools \
	--include='*.cpp' --include='*.h' --include='*.inc' --include='*.mad' 2>/dev/null \
	| grep -v '^src/madcdis_git_repo\.cpp:' \
	| grep -vE "\b($shapers)[[:space:]]*\(")
if [ -n "$hits" ]; then
	echo "one-git-owner gate: libgit2 used outside src/madcdis_git_repo.cpp:"
	echo "$hits" | sed 's/^/  /'
	fail=1
fi

# A git BINARY spawned: an exec:// channel naming git, or a CRT spawn whose
# command text starts with git. (Error prose such as "git log: …" is not a
# spawn — the shapes below are call-shaped.)
spawn_pat='exec://git\b|(system|popen)[[:space:]]*\([^)]*"git[[:space:]]'
spawn=$(grep -rnE "$spawn_pat" \
	src include tools --include='*.cpp' --include='*.h' --include='*.inc' --include='*.mad' 2>/dev/null)
if [ -n "$spawn" ]; then
	echo "one-git-owner gate: a git BINARY is spawned (use madc::GitRepo / madc::git_*):"
	echo "$spawn" | sed 's/^/  /'
	fail=1
fi

# Negative control: a synthetic violation of each marker must be caught, or
# the gate is dead and the verdict means nothing.
ctrl=$(mktemp)
printf '#include <git2.h>\nvoid f(void){ git_repository_open(0, "x"); system("git log"); }\nmadc::channel c("exec://git log");\n' > "$ctrl"
if ! grep -qE "$inc" "$ctrl" || ! grep -qE "$api" "$ctrl" || ! grep -qE 'exec://git\b' "$ctrl" \
   || ! grep -qE '(system|popen)[[:space:]]*\([^)]*"git[[:space:]]' "$ctrl"; then
	echo "one-git-owner gate: NEGATIVE CONTROL FAILED"
	rm -f "$ctrl"
	exit 1
fi
rm -f "$ctrl"

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "one-git-owner gate: GREEN — src/madcdis_git_repo.cpp is the only libgit2 caller; no git binary is spawned."
exit 0
