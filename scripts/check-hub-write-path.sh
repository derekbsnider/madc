#!/bin/bash
# check-hub-write-path.sh — Track 7 Phase 1 gate G4: every world change on
# the play path flows through a verb's mutation_context. The projection and
# session layers hold const world& (the compiler already enforces reads);
# what the compiler cannot see is the session layer spelling a direct
# mutator on its own non-const world — src/ns_ui.cpp's only sanctioned
# mutation paths are world_doc_apply (ingestion, before play) and
# verb_table::invoke. A planted violation must fail this gate (negative
# control below). Plan: docs/plans/2026-08-20-track7-phase1-text-adventure.md.
set -u
cd "$(dirname "$0")/.." || exit 9
fail=0
PATTERN='(^|[^_[:alnum:]])w\.(link_add|link_remove|create)[[:space:]]*\('

hits=$(grep -nE "$PATTERN" src/ns_ui.cpp | grep -v 'world_doc' || true)
if [ -n "$hits" ]; then
    echo "check-hub-write-path: direct world mutator in src/ns_ui.cpp:"
    echo "$hits"
    fail=1
fi

# Belt over the compiler's braces: nothing in the engine headers may
# mutate through the read view. (The compiled application catalog this
# once watched was evicted in Track 7.2 R1 — see check-engine-app-purity.)
hits=$(grep -nE 'view\(\)\.(link_add|link_remove|create)' \
	    include/madcdis/*.h || true)
if [ -n "$hits" ]; then
    echo "check-hub-write-path: mutation through view() in an engine header:"
    echo "$hits"
    fail=1
fi

# Negative control: the pattern must catch a planted violation, or this
# gate is blind and must say so.
mkdir -p tmp
nc=$(mktemp tmp/hubwrite.XXXXXX)
echo '    s->w.link_add(a, r, b);' > "$nc"
if ! grep -qE "$PATTERN" "$nc"; then
    echo "check-hub-write-path: NEGATIVE CONTROL FAILED (pattern is blind)"
    fail=1
fi
rm -f "$nc"

if [ $fail -ne 0 ]; then
    exit 1
fi
echo "check-hub-write-path: OK (one write path; negative control bites)"
