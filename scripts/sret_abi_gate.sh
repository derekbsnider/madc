#!/usr/bin/env bash
# ABI gate: a by-value NON-TRIVIAL class return crosses the ABI boundary as a
# real indirect return, never as a hand-rolled leading pointer argument.
#
# C classifies a return by SIZE. C++ returns a non-trivially-copyable class
# INDIRECTLY at any size — std::locale is a single pointer. madc used to bridge
# that by declaring the callee `void f(void *sret, void *this)`, which is right
# only where the hidden pointer happens to be the first argument register:
# x86-64 SysV. AArch64 passes it in x8, OUTSIDE the argument sequence, so every
# real argument landed one register late and the callee read the caller's result
# slot as `this`. On an Apple Mac that made `std::cout << "hi"` segfault inside
# ios_base::getloc while `std::cout << 42` (a member overload, no by-value
# return) was fine — and std::string (32B) and std::vector (24B) worked, because
# they clear AArch64's 16-byte threshold and were already returned by address.
#
# The fix says it in the IR instead: the extern is declared with its real
# by-value struct return plus the fork's __attribute__((indirect_return)), and
# the TARGET places the pointer. Behaviour cannot gate this — on x86-64 both
# shapes run correctly — so the gate asserts the IR shape.
#
# It carries its own negative control: a TRIVIALLY-copyable class returned by
# value must STILL come back in a register, so the gate cannot be satisfied by
# forcing every aggregate indirect.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 240 2>/dev/null

MADC="${MADC_BIN:-bin/madc}"

fail() { echo "sret_abi_gate: $1"; exit 1; }

[ -x "$MADC" ] || fail "no madc at $MADC"

mkdir -p tmp
src=tmp/sret_abi_gate.mad
cat > "$src" <<'EOF'
#include <string>
#include <vector>
#include <stdio.h>
int main()
{
	// std::string operator+ is an EXTERNAL C++ symbol returning a
	// non-trivially-copyable class BY VALUE — the shape whose hidden result
	// pointer must ride the target's indirect-result register.
	std::string a = "ab";
	std::string b = "cd";
	std::string c = a + b;
	// TRIVIALLY-copyable class returned by value from an external method
	// (__normal_iterator — one pointer, no dtor): the negative control. C's
	// size rule is correct for it and must be left alone.
	std::vector<int> v;
	v.push_back(7);
	std::vector<int>::iterator it = v.begin();
	printf("%s %d\n", c.c_str(), *it);
	return 0;
}
EOF

mir="$(MADC_DUMP_MIR=1 timeout 240 "$MADC" "$src" 2>&1 >/dev/null)" \
    || fail "madc failed on $src"

# The proto a call to <symbol-substring> references, then that proto's own line.
call_proto_of() {
    printf '%s\n' "$mir" \
        | sed -nE "s/^[[:space:]]*call[[:space:]]+(proto[0-9]+),[[:space:]]*[^,]*$1[^,]*,.*/\1/p" \
        | head -1
}
proto_line_of() {
    printf '%s\n' "$mir" | sed -nE "s/^$1:[[:space:]]*(.*)$/\1/p" | head -1
}

gl="$(call_proto_of 'plIcSt11char_traits')"
[ -n "$gl" ] || fail "no std::operator+ call in the MIR (reducer stopped compiling?)"
gl_line="$(proto_line_of "$gl")"

case "$gl_line" in
    *rblk*) : ;;
    *) fail "a by-value NON-TRIVIAL class return is not an indirect return: $gl_line — on AArch64 the hidden pointer must ride x8, which only an rblk result expresses; a leading pointer PARAMETER is the x86-64-only answer";;
esac

# Negative control: the trivially-copyable iterator return stays in a register.
bg="$(call_proto_of 'begin')"
if [ -n "$bg" ]; then
    bg_line="$(proto_line_of "$bg")"
    case "$bg_line" in
        *rblk*) fail "negative control broke — a TRIVIALLY-copyable class return became indirect: $bg_line (C's size rule is correct for it)";;
        *) : ;;
    esac
else
    fail "no begin() call in the MIR — the negative control did not compile, so this gate proves only half of the rule"
fi

rm -f "$src"
echo "sret_abi_gate: OK (by-value non-trivial class return rides an rblk result address; trivially-copyable class return stays in a register)"
