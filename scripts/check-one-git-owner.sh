#!/bin/bash
# GATE — ONE git owner (Nexus L4a, design docs/plans/2026-09-13-nexus-L4-design.md
# §4.2).
#
# The rule: every libgit2 call (git_* from <git2.h>) lives in
# src/modules/madcgit/madcgit.cpp — the madcgit MODULE, the READ-ONLY
# madc::GitRepo and its C API (libgit2 is the SYSTEM library, a dependency of
# the IDE's nexus and never part of madc: owner ruling 2026-09-15). Nothing
# else in src/, include/ or tools/ includes <git2.h>, calls a git_* API, or
# spawns a git binary (exec://git …). Consumers use GitRepo (C++) or git::*
# (the dialect, <ns_git>) — so "read-only" is a property of ONE file, and the
# second rule below keeps it read-only: the owner calls no remote / clone /
# fetch / push / write API (with the vendored network-off configuration gone,
# this is where "the nexus only READS local history" lives). tests/ are
# exempt: unit fixtures build repositories through libgit2's own write API
# (the oracle, not a second owner).
set -u
cd "$(dirname "$0")/.."

OWNER=src/modules/madcgit/madcgit.cpp
fail=0
api='\bgit_(repository|revwalk|commit|blame|tree|blob|reference|status|revparse|object|remote|clone|index|signature|libgit2)_[a-z_]+[[:space:]]*\('
inc='#include[[:space:]]*[<"]git2(/|\.h|>)'
# The write / network surface the READ-ONLY owner must never touch.
writeapi='\bgit_(remote|clone|fetch|push|transport|credential|checkout|merge|rebase|reset|stash|submodule|worktree|index_(add|remove|write)|commit_create|reference_(create|set|rename|delete)|tag_create|branch_(create|delete|move)|repository_init|signature_now|blob_create|tree_builder|treebuilder)[a-z_]*[[:space:]]*\('
# madc's own git_* names share the prefix by design: the row shapers
# (value git_<record>_value(...)) declared in the owner's header, and the
# dialect publics (madc::git_open / git_blame_text / …) declared in
# include/madc/ns_madc. Both lists are READ from those headers, never
# listed here — a new public joins the exemption by being declared.
shapers=$( { grep -oE 'value[[:space:]]+(git_[a-z_]+_value)[[:space:]]*\(' include/madcdis/git_repo.h \
		| sed -E 's/value[[:space:]]+//; s/[[:space:]]*\($//';
	     grep -oE '\b(git_[a-z_]+)[[:space:]]*\(' include/madc/ns_madc \
		| sed -E 's/[[:space:]]*\($//'; } | sort -u | paste -sd'|' -)
[ -z "$shapers" ] && shapers='__no_shapers__'

hits=$(grep -rnE "$inc|$api" src include tools \
	--include='*.cpp' --include='*.h' --include='*.inc' --include='*.mad' 2>/dev/null \
	| grep -v "^$OWNER:" \
	| grep -vE "\b($shapers)[[:space:]]*\(")
if [ -n "$hits" ]; then
	echo "one-git-owner gate: libgit2 used outside $OWNER:"
	echo "$hits" | sed 's/^/  /'
	fail=1
fi

# The owner is READ-ONLY: no write / network API call in it.
writes=$(grep -nE "$writeapi" "$OWNER" 2>/dev/null)
if [ -n "$writes" ]; then
	echo "one-git-owner gate: $OWNER calls a WRITE or NETWORK libgit2 API (the nexus only READS local history):"
	echo "$writes" | sed 's/^/  /'
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
printf '#include <git2.h>\nvoid f(void){ git_repository_open(0, "x"); system("git log"); git_remote_fetch(r, 0, 0, 0); }\nmadc::channel c("exec://git log");\n' > "$ctrl"
if ! grep -qE "$inc" "$ctrl" || ! grep -qE "$api" "$ctrl" || ! grep -qE 'exec://git\b' "$ctrl" \
   || ! grep -qE '(system|popen)[[:space:]]*\([^)]*"git[[:space:]]' "$ctrl" \
   || ! grep -qE "$writeapi" "$ctrl"; then
	echo "one-git-owner gate: NEGATIVE CONTROL FAILED"
	rm -f "$ctrl"
	exit 1
fi
# ...and the owner's own READ calls must not trip the write rule (a positive
# control, or the rule is a tautology waiting to fire on the first refactor).
if printf 'git_repository_open(0, "x"); git_blame_buffer(0, 0, 0, 0); git_revparse_single(0, 0, 0);\n' | grep -qE "$writeapi"; then
	echo "one-git-owner gate: POSITIVE CONTROL FAILED — a read API trips the write rule"
	rm -f "$ctrl"
	exit 1
fi
rm -f "$ctrl"

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "one-git-owner gate: GREEN — $OWNER is the only libgit2 caller and calls no write/network API; no git binary is spawned."
exit 0
