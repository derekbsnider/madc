#!/bin/bash
# GATE — lazy template registries thaw at their owners (task #25 B2).
#
# The rule: forest-restored template definitions register as identity-only
# STUBS; payload (params / defaults / body / constraints / spec patterns /
# frozen class-pattern span) decodes at the thaw owners in src/parser.cpp —
# thaw_template_def / thaw_alias_def / thaw_fn_def and the choke accessors
# built on them (find_template / find_template_alias / template_with_body /
# match_partial_specialization / thawed_fn_templates / thawed_fn_template_decls
# / thaw_all_frozen_templates / ensure_free_overload_surfaces).
#
# A NEW direct content read of the five lazy maps —
#   template_map, partial_spec_map, template_alias_map,
#   fn_template_map, fn_template_decl_map
# — that dereferences definition payload without thawing reads EMPTY vectors
# from a stub and silently mis-resolves (wrong overload, missing body, failed
# partial-spec match) ONLY on the packed/forest lane, where the plain dev
# suite is blind to it. That is exactly the silent-wrong-answer class.
#
# Mechanism: every read-API occurrence (find_readonly / .find( / .for_each)
# on those maps must carry a same-line marker:
#   /* thaw-owner */     — the line IS a thaw owner / feeds one directly
#   /* identity-read */  — reads existence or identity fields only
#                          (never params/defaults/body/target/decl/
#                          constraints/spec_pattern/frozen_class_pattern)
# An unmarked occurrence fails the build. Do NOT mark a payload read
# identity-read to pass — route it through a thaw owner instead. .count( is
# inherently existence and exempt. Writer-side API (operator[],
# find_for_subvalue_write, transactions) is exempt: registration writes
# stubs or live defs, it never reads payload.
set -u
cd "$(dirname "$0")/.."

# \b keeps var_template_map (an EAGER lane) out of the template_map match.
PATTERN='\btemplate_map\.(find_readonly|find\(|for_each)|\bpartial_spec_map\.(find_readonly|find\(|for_each)|\btemplate_alias_map\.(find_readonly|find\(|for_each)|\bfn_template_map\.(find_readonly|find\(|for_each)|\bfn_template_decl_map\.(find_readonly|find\(|for_each)'

bad=$(grep -nE "$PATTERN" src/*.cpp | grep -v 'thaw-owner' | grep -v 'identity-read')

if [ -n "$bad" ]; then
	echo "check-template-thaw-choke: UNMARKED lazy-template-registry read(s):"
	echo "$bad"
	echo ""
	echo "Route payload reads through a thaw owner (find_template /"
	echo "thawed_fn_templates / ... in src/parser.cpp) and mark the line"
	echo "/* thaw-owner */, or mark a pure existence/identity read"
	echo "/* identity-read */. Design + traps:"
	echo "docs/plans/2026-08-08-packed-include-latency-plan.md (slice B2)."
	exit 1
fi

echo "check-template-thaw-choke: OK (all lazy-registry reads marked thaw-owner/identity-read)"
exit 0
