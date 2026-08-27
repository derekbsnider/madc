#!/usr/bin/env bash
# embed_examples_gate.sh — the embedding examples COMPILE AND RUN under
# bin/madc itself: the self-hosting-flavored lane nothing else in the
# battery covers (tests/libmadc_cpp_smoke.cpp is compiled by HOST g++
# against the staged install, so madc-as-the-compiler never chews the
# embedding API headers anywhere else — which is how embed_hello.cpp's
# dependence on the deleted re-parse fallback stayed invisible while the
# whole suite was green).
#
# Legs: examples/embed_hello.c (the extern-C shim API — madc's C lane)
# and examples/embed_hello.cpp (the C++ engine/program API — joined when
# the variadic class-template instantiation arc landed, s141; its checked
# lines are the HOST-side prints — guest stdout is sandbox-captured).
set -u

BIN="${MADC_BIN:-bin/madc}"

run_capped()
{
	( ulimit -t 60; timeout 90 "$BIN" "$@" )
}

out=$(run_capped examples/embed_hello.c 2>&1)
rc=$?
if [ "$rc" -ne 0 ] || ! grep -qF "6 * 7 = 42" <<<"$out"; then
	echo "embed_examples_gate: FAIL — examples/embed_hello.c under $BIN" \
	     "(rc=$rc). Last output:" >&2
	echo "$out" | tail -5 >&2
	exit 1
fi

out=$(run_capped examples/embed_hello.cpp 2>&1)
rc=$?
if [ "$rc" -ne 0 ] || ! grep -qF "6 * 7 = 42" <<<"$out" \
   || ! grep -qF "10 + 32 = 42" <<<"$out"; then
	echo "embed_examples_gate: FAIL — examples/embed_hello.cpp under $BIN" \
	     "(rc=$rc). Last output:" >&2
	echo "$out" | tail -5 >&2
	exit 1
fi

# Negative controls: (a) the rc check must see a program that does not
# compile; (b) the output check must not match a wrong result.
bad=$(mktemp --suffix=.c)
echo "int main(void) { return undeclared_x; }" > "$bad"
if run_capped "$bad" >/dev/null 2>&1; then
	rm -f "$bad"
	echo "embed_examples_gate: FAIL — negative control compiled (the rc" \
	     "check went blind)." >&2
	exit 1
fi
rm -f "$bad"
if grep -qF "6 * 7 = 42" <<<"6 * 7 = 43"; then
	echo "embed_examples_gate: FAIL — negative control matched a wrong" \
	     "output (the checker went blind)." >&2
	exit 1
fi

echo "embed_examples_gate: OK (embed_hello.c and embed_hello.cpp" \
     "compile+run under $BIN)"
exit 0
