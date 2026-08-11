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
# The fix says it in the IR instead: the hidden pointer stays a PARAMETER (so
# the callee still constructs in place — a non-trivially-copyable class must
# never be bit-copied out of a temporary), marked __attribute__((ret_addr)) so
# the fork emits it as MIR_T_RBLK and the TARGET places it.
#
# Behaviour cannot gate this. On x86-64 BOTH shapes run correctly, which is why
# the bug reached a Mac. So the gate asserts the IR shape — and asserts it as a
# MECHANISM, not as a list of known symbols:
#
#   every prototype argument named __retbuf must be rblk.
#
# CirBuilder::retbuf_param is the only thing that emits that name and it always
# marks, so the invariant is exact — and keyed on no particular class, symbol or
# header, so a new emitter that forgets the marker fails here even for a
# construct nobody thought to enumerate. That matters: the marker started life
# on ONE of madc's emitters, and the others (the referenced-FuncDef typed
# extern, func_proto, func_def) each served the same symbols under slightly
# different conditions — a grove supplying a declaration was enough to switch
# which one won and silently revert the shape.
#
# KNOWN GAP: a call through a FUNCTION POINTER carries the hidden pointer in an
# abstract (unnamed) parameter, because the fn-ptr is a type and not a
# declaration — fnptr_func_node marks it, but nothing here can see the name, so
# this gate does not cover that path.
#
# It carries two negative controls: a TRIVIALLY-copyable class returned by value
# must STILL come back in a register (so the gate cannot be satisfied by forcing
# every aggregate indirect), and a minimum marked count (so it cannot pass
# vacuously by the reducer failing to exercise the path).
#
# Both lanes are checked. LIVE and BOUND legitimately differ in WHICH symbols
# the module defines versus imports — that is materialization policy, not ABI —
# but the rule above holds in each.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 480 2>/dev/null

MADC="${MADC_BIN:-bin/madc}"

fail() { echo "sret_abi_gate: $1"; exit 1; }

[ -x "$MADC" ] || fail "no madc at $MADC"

mkdir -p tmp
src=tmp/sret_abi_gate.mad
snap=tmp/sret_abi_gate.forest
cat > "$src" <<'EOF'
#include <string>
#include <vector>
#include <sstream>
#include <stdio.h>
// A MADC-defined by-value class return, so the definition/forward-prototype
// emitters are covered too and not just the two extern ones.
std::string twice(const std::string &s) { return s + s; }
int main()
{
	std::string t = twice("z");
	// Every one of these is an EXTERNAL C++ symbol returning a
	// non-trivially-copyable class BY VALUE — the shape whose hidden result
	// pointer must ride the target's indirect-result register. They reach
	// their declarations by different routes on purpose (free operator,
	// member of a class template, member of a stream), because the routes
	// are what used to disagree.
	std::string a = "ab";
	std::string b = "cd";
	std::string c = a + b;
	std::string d = c.substr(1, 2);
	std::ostringstream os;
	os << "x";
	std::string e = os.str();
	// TRIVIALLY-copyable class returned by value from an external method
	// (__normal_iterator — one pointer, no dtor): the negative control. C's
	// size rule is correct for it and must be left alone.
	std::vector<int> v;
	v.push_back(7);
	std::vector<int>::iterator it = v.begin();
	printf("%s %s %s %s %d\n", c.c_str(), d.c_str(), e.c_str(), t.c_str(), *it);
	return 0;
}
EOF

# Every prototype carrying a __retbuf argument, split into marked (rblk) and
# unmarked. Prints "<marked> <unmarked>" followed by the offending proto lines.
audit() {
	awk '
	/^proto[0-9]+:/ {
		if ($0 !~ /__retbuf/) next
		if ($0 ~ /rblk/) { good++; next }
		bad++
		offend = offend "\n      " $0
		next
	}
	END { print good+0 " " bad+0 offend }' "$1"
}

# The proto a call to <symbol-substring> references, then that proto's own line.
proto_line_for() {
	p="$(sed -nE "s/^[[:space:]]*call[[:space:]]+(proto[0-9]+),[[:space:]]*[^,]*$2[^,]*,.*/\1/p" "$1" | head -1)"
	[ -n "$p" ] || return 1
	sed -nE "s/^$p:[[:space:]]*(.*)$/\1/p" "$1" | head -1
}

check_lane() {
	lane="$1"
	mir="$2"

	res="$(audit "$mir")"
	marked="$(printf '%s' "$res" | head -1 | cut -d' ' -f1)"
	unmarked="$(printf '%s' "$res" | head -1 | cut -d' ' -f2)"

	if [ "$unmarked" != "0" ]; then
		echo "sret_abi_gate: [$lane] $unmarked by-value class return(s) pass the result address as a PLAIN pointer argument:$(printf '%s' "$res" | tail -n +2)"
		fail "[$lane] that is the x86-64-only shape — on AArch64 the hidden pointer must ride x8, which only an rblk arg expresses. Every emitter of the hidden result parameter must go through CirBuilder::retbuf_param, which marks it."
	fi
	# Non-vacuity: the reducer must actually exercise the path.
	[ "$marked" -ge 4 ] 2>/dev/null \
		|| fail "[$lane] only $marked marked indirect return(s) — the reducer stopped exercising the path, so this gate proves nothing"

	# Negative control: the trivially-copyable iterator return stays in a register.
	bg_line="$(proto_line_for "$mir" 'begin')" \
		|| fail "[$lane] no begin() call in the MIR — the negative control did not compile, so this gate proves only half of the rule"
	case "$bg_line" in
		*rblk*) fail "[$lane] negative control broke — a TRIVIALLY-copyable class return became indirect: $bg_line (C's size rule is correct for it)";;
		*) : ;;
	esac
	echo "sret_abi_gate: [$lane] OK — $marked indirect return(s) ride an rblk result address, 0 plain; trivially-copyable class return stays in a register"
}

MADC_DUMP_MIR=1 timeout 240 "$MADC" "$src" 2>tmp/sret_abi_live.mir >/dev/null \
	|| fail "madc failed on $src"
check_lane live tmp/sret_abi_live.mir

# Same source, same binary, one flag apart. A grove that DECLARES these symbols
# changes which prototype emitter wins; it must not change the ABI shape.
rm -f "$snap"
timeout 600 "$MADC" --freeze="$snap" "$src" >/dev/null 2>&1
[ -f "$snap" ] \
	|| fail "--freeze produced no snapshot — the bound lane went unchecked, and it is the lane that regressed before"
MADC_DUMP_MIR=1 timeout 240 "$MADC" --forest-bind="$snap" "$src" 2>tmp/sret_abi_bind.mir >/dev/null \
	|| fail "madc failed on $src under --forest-bind"
check_lane bound tmp/sret_abi_bind.mir

rm -f "$src" "$snap" tmp/sret_abi_live.mir tmp/sret_abi_bind.mir
echo "sret_abi_gate: OK"
