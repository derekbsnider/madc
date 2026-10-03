#!/bin/bash
# check-alias-symbol-emission.sh — __attribute__((alias)) must DEFINE a symbol.
#
# madc implements the attribute as a REDIRECT: a reference to the alias
# resolves to the target's storage (how a system-header class static binds to
# its real Itanium symbol). gcc and clang do that AND emit a second symbol of
# the alias's own name — its asm label when it has one — at the target's
# address. madc emitted only the redirect, so the alias never reached the
# symbol table at all, and a madc-built libmir exported none of the eight
# `mir.*` / `__mir_*` names its own generated code imports.
#
# A RUNNING program cannot tell the two halves apart — the redirect alone makes
# every reference behave correctly — which is exactly how the gap survived a
# green suite. So this gate reads the SYMBOL TABLE with nm, against the gcc
# oracle, and tests/testasmlabelalias.mad pins the reference half beside it.
#
# c2mir is not a reference here: `c2m -fobject` ignores the attribute silently
# and rejects the asm-labelled spelling outright (measured 2026-09-22). gcc and
# clang are the oracle.
#
# See docs/plans/2026-09-22-alias-symbol-emission.md.

set -u
cd "$(dirname "$0")/.." || exit 2

BIN="${MADC_BIN:-bin/madc}"

# Object mode is x86-64 ELF only, and the gate needs the oracle + a reader.
if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
	echo "check-alias-symbol-emission: SKIP — object mode is x86-64 ELF only (this is $(uname -s)/$(uname -m))"
	exit 0
fi
for tool in gcc nm; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "check-alias-symbol-emission: SKIP — no $tool (oracle/reader unavailable)"
		exit 0
	fi
done
if [ ! -x "$BIN" ]; then
	echo "check-alias-symbol-emission: $BIN missing — build first" >&2
	exit 2
fi
BIN_ABS=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The three shapes that matter: a DATA alias, a FUNCTION alias, and the
# asm-labelled function alias MIR actually writes (the label, not the declared
# name, is the symbol).
cat > "$work/alias.c" <<'SRC'
int real_var = 42;
int real_fn (int x) { return x + 1; }
extern int data_alias __attribute__ ((alias ("real_var")));
extern int func_alias (int) __attribute__ ((alias ("real_fn")));
extern __typeof (real_fn) labelled_alias asm ("gate.alias_fn")
  __attribute__ ((alias ("real_fn"), used));
int main (void) { return 0; }
SRC

# The same file with every alias attribute removed — the negative control.
# Its object must NOT carry the alias names; if it does, this gate is reading
# something other than the attribute's effect.
sed -e 's/ __attribute__ ((alias ("real_var")))//' \
    -e 's/ __attribute__ ((alias ("real_fn")))//' \
    -e 's/ __attribute__ ((alias ("real_fn"), used))//' \
    -e 's/^extern int data_alias;$/int data_alias = 1;/' \
    -e 's/^extern int func_alias (int);$/int func_alias (int x) { return x; }/' \
    "$work/alias.c" > "$work/noalias.c"

defined_globals() {	# $1 = object file -> sorted global DEFINED symbol names
	nm -g --defined-only "$1" 2>/dev/null | awk '{print $NF}' | sort -u
}

if ! gcc -std=gnu17 -c -o "$work/gcc.o" "$work/alias.c" 2>"$work/gcc.err"; then
	echo "check-alias-symbol-emission: the ORACLE failed to compile the" >&2
	echo "reducer — the gate cannot measure anything:" >&2
	sed 's/^/  /' "$work/gcc.err" >&2
	exit 2
fi
defined_globals "$work/gcc.o" > "$work/gcc.syms"

# --- negative control: the gate must NOTICE a missing definition -------------
# Strip the alias names out of a copy of the oracle's symbol list and confirm
# the comparison below reports them. A gate that passes on an object with no
# alias symbols would never have caught the bug it exists for.
grep -Ev '^(data_alias|func_alias|gate\.alias_fn)$' "$work/gcc.syms" > "$work/ctrl.syms"
if [ -z "$(comm -23 "$work/gcc.syms" "$work/ctrl.syms")" ]; then
	echo "check-alias-symbol-emission: NEGATIVE CONTROL FAILED — the oracle" >&2
	echo "object defines none of the alias names, so the comparison below" >&2
	echo "would pass on any object at all. The gate is broken." >&2
	exit 2
fi

# --- the measurement ---------------------------------------------------------
if ! "$BIN_ABS" --std=c17 -c -o "$work/madc.o" "$work/alias.c" 2>"$work/madc.err"; then
	echo "check-alias-symbol-emission: $BIN failed to compile the reducer:" >&2
	sed 's/^/  /' "$work/madc.err" >&2
	exit 1
fi
defined_globals "$work/madc.o" > "$work/madc.syms"

# Subset, not equality: madc legitimately adds its own machinery (the
# __madc_shim_* thunks). Every symbol gcc DEFINES must be there.
missing=$(comm -23 "$work/gcc.syms" "$work/madc.syms")
if [ -n "$missing" ]; then
	echo "check-alias-symbol-emission: $BIN does not define $(echo "$missing" | wc -l) symbol(s)" >&2
	echo "gcc -std=gnu17 defines in the same translation unit:" >&2
	echo "$missing" | sed 's/^/  /' >&2
	echo "" >&2
	echo "__attribute__((alias(\"T\"))) DEFINES a symbol at T's address — an" >&2
	echo "asm-labelled alias defines the LABEL, not the declared name. See" >&2
	echo "cir_emit_alias_symbols (src/madc_cir.cpp) and" >&2
	echo "docs/plans/2026-09-22-alias-symbol-emission.md." >&2
	exit 1
fi

# --- negative control, second direction -------------------------------------
# Without the attribute the ALIAS-ONLY name must disappear. data_alias and
# func_alias become real definitions in the control source (a bare `extern`
# declaration defines nothing), so the name that must vanish is the asm label:
# it exists ONLY because the alias declaration created it.
if ! "$BIN_ABS" --std=c17 -c -o "$work/noalias.o" "$work/noalias.c" 2>"$work/noalias.err"; then
	echo "check-alias-symbol-emission: the control source failed to compile:" >&2
	sed 's/^/  /' "$work/noalias.err" >&2
	exit 2
fi
if defined_globals "$work/noalias.o" | grep -qx 'gate\.alias_fn'; then
	echo "check-alias-symbol-emission: NEGATIVE CONTROL FAILED — 'gate.alias_fn'" >&2
	echo "is defined in an object built from a source with NO alias attribute," >&2
	echo "so its presence above proves nothing about the attribute." >&2
	exit 2
fi

echo "check-alias-symbol-emission: OK — every symbol gcc defines ($(wc -l < "$work/gcc.syms")) is defined by $BIN, both controls bite"
exit 0
