#!/bin/bash
# Gate: TokenAWAIT has ONE construction owner — Program::make_await_token
# (src/parser.cpp). The MT-5 wave's own first draft grew three inline
# constructions (one per grammar position) before the pre-merge dupaudit
# folded them; a fourth claim site must adopt the helper, not hand-roll the
# node (chan parse + terminator hand-back + datadef + position all ride it).
# Marker: `new TokenAWAIT(` in src/*.cpp — exactly 1 (the helper). The
# clone() in include/tokens.h is the node's own copy, not a construction
# site, and is counted separately (exactly 1 there).
set -u
cd "$(dirname "$0")/.."

fail() { echo "check-await-one-builder: FAIL — $1"; exit 1; }

src_count=$(grep -h "new TokenAWAIT(" src/*.cpp | wc -l)
hdr_count=$(grep -h "new TokenAWAIT(" include/tokens.h | wc -l)

# Negative control: the marker must actually match the known owner.
grep -q "new TokenAWAIT(" src/parser.cpp \
    || fail "negative control broken: marker no longer matches the owner in src/parser.cpp"

[ "$src_count" -eq 1 ] \
    || fail "expected exactly 1 TokenAWAIT construction in src/ (Program::make_await_token), found $src_count — adopt the helper"
[ "$hdr_count" -eq 1 ] \
    || fail "expected exactly 1 TokenAWAIT construction in include/tokens.h (clone), found $hdr_count"

echo "check-await-one-builder: OK (one construction owner + clone)"
