#!/bin/bash
# check-engine-app-purity.sh — Rule #7 gate for the interaction engine
# (Track 7.2 R1, born from the eviction of the compiled adventure catalog):
# the ENGINE — include/madcdis/ headers and the ui:: session layer — must
# carry no application vocabulary and ship no application verbs.
# Applications supply verbs as data + madc source (ui::bind_verb) and
# vocabulary as arguments. A planted violation must fail this gate
# (negative control below).
# Plan: docs/plans/2026-08-24-ui-interaction-rework-and-texteditor.md.
set -u
cd "$(dirname "$0")/.." || exit 9
fail=0

# The evicted file must never return, under any name that means it.
if ls include/madcdis/ | grep -qiE 'adventure|game'; then
    echo "check-engine-app-purity: application-layer header in include/madcdis/:"
    ls include/madcdis/ | grep -iE 'adventure|game'
    fail=1
fi

# Canary vocabulary: application/game words that have no business in the
# generic engine. A tripwire, not a proof — the reviewer owns the rest.
CANARY='adventure|grue|lantern|brass|xyzzy|world-state|blocked_msg|"noun"|"portable"|"dark"|"exit"'
ENGINE_SOURCES="src/ns_ui.cpp include/madcdis/*.h"
# Plan-doc path references in header comments are not vocabulary.
hits=$(grep -nEi "$CANARY" $ENGINE_SOURCES 2>/dev/null \
	    | grep -v 'docs/plans/' || true)
if [ -n "$hits" ]; then
    echo "check-engine-app-purity: application vocabulary in engine sources:"
    echo "$hits"
    fail=1
fi

# The engine ships no verbs of its own: no NATIVE registration (a compiled
# handler next to a name literal) anywhere in the session layer, and
# exactly ONE script registration site — the ui::bind_verb implementation.
hits=$(grep -nE 'register_verb\(' src/ns_ui.cpp || true)
if [ -n "$hits" ]; then
    echo "check-engine-app-purity: native verb registration in the session layer:"
    echo "$hits"
    fail=1
fi
nscript=$(grep -cE 'register_script_verb\(' src/ns_ui.cpp || true)
if [ "$nscript" -ne 1 ]; then
    echo "check-engine-app-purity: expected exactly 1 register_script_verb site (ui::bind_verb), found $nscript"
    fail=1
fi

# Negative control: the canary must catch a planted violation, or this
# gate is blind and must say so.
mkdir -p tmp
nc=$(mktemp tmp/apppurity.XXXXXX)
echo 'inline bool h_take_the_lantern() { return true; } // adventure' > "$nc"
if ! grep -qEi "$CANARY" "$nc"; then
    echo "check-engine-app-purity: NEGATIVE CONTROL FAILED (canary is blind)"
    fail=1
fi
rm -f "$nc"

if [ $fail -ne 0 ]; then
    exit 1
fi
echo "check-engine-app-purity: OK (engine is application-free; negative control bites)"
