#!/usr/bin/env bash
# embed_examples_gate.sh — the embedding examples COMPILE AND RUN under
# bin/madc itself: the self-hosting-flavored lane nothing else in the
# battery covers (tests/libmadc_cpp_smoke.cpp is compiled by HOST g++
# against the staged install, so madc-as-the-compiler never chews the
# embedding API headers anywhere else — which is how embed_hello.cpp's
# dependence on the deleted re-parse fallback stayed invisible while the
# whole suite was green).
#
# Today: examples/embed_hello.c (the extern-C shim API — madc's C lane).
# examples/embed_hello.cpp JOINS THIS GATE when the variadic
# class-template instantiation arc lands (KG Gap
# variadic_class_template_instantiation) — it is the work order's stated
# real gate ("compiles+RUNS").
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

echo "embed_examples_gate: OK (embed_hello.c compiles+runs under $BIN;" \
     "the .cpp leg joins with the variadic class-template arc)"
exit 0
