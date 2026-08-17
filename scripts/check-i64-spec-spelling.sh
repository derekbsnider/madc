#!/bin/bash
# GATE — the i64 spelling law (Windows lane, LLP64).
#
# The rule: madc's 64-bit int kinds spell as `long long` in the C11 IR —
# TWO N_LONG specifier nodes (CirBuilder::append_i64 / i64_list), and
# 64-bit constants ride N_LL/N_ULL (u.ll/u.ull) — NEVER a lone N_LONG
# spec or an N_L/N_UL constant: c2mir models platform `long`, which is
# 32-bit on win64 (third_party/mir/c2mir/x86_64/cx86_64.h), so the lone
# spellings silently truncate every 64-bit value there. The only
# legitimate lone N_LONG is the long double pair {N_LONG, N_DOUBLE}.
#
# Markers, comments stripped first:
#   1. simple(N_LONG outside lines tagged i64-owner (the helpers) or
#      ld-pair (the long double specifier pair).
#   2. An N_LONG immediately closing a brace ({N_LONG}, {N_UNSIGNED,
#      N_LONG}, { N_LONG }) — i64 vectors must end N_LONG, N_LONG}.
#   3. make(N_L, / make(N_UL, — 64-bit constants must be N_LL/N_ULL.
set -u
cd "$(dirname "$0")/.."

strip='s_(//|/\*).*__'

h1=$(grep -n "simple(N_LONG" src/cir_builder.cpp \
  | grep -vE "i64-owner|ld-pair")
h2=$(grep -nE "N_LONG[[:space:]]*\}" src/cir_builder.cpp \
  | sed -E "$strip" \
  | grep -E "N_LONG[[:space:]]*\}" \
  | grep -vE "N_LONG,[[:space:]]*N_LONG[[:space:]]*\}")
h3=$(grep -nE "make[[:space:]]*\((N_L|N_UL)[[:space:]]*," src/cir_builder.cpp)

hits=$(printf '%s\n%s\n%s\n' "$h1" "$h2" "$h3" | grep -c . )

if [ "$hits" -ne 0 ]; then
	echo "i64-spec-spelling gate: $hits lone-long spelling(s) in cir_builder.cpp."
	echo "madc's 64-bit ints spell as long long: use CirBuilder::append_i64 /"
	echo "i64_list (specs) and N_LL/N_ULL (constants) — platform long is 32-bit"
	echo "on win64. Long double pairs carry an ld-pair tag."
	printf '%s\n%s\n%s\n' "$h1" "$h2" "$h3" | grep . | sed 's/^/  /'
	exit 1
fi

echo "i64-spec-spelling gate: GREEN — no lone-long i64 spellings in cir_builder.cpp."
exit 0
