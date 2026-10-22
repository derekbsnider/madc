#!/bin/bash
# BEHAVIOUR GATE -- x86-64 code placement (BUGS.md B232): every function
# madc emits starts on MIR_TARGET_CODE_ALIGN (32), every innermost loop head
# on 32, every other loop head on at least 8, in the object (-c) and in the
# executable (-o).  Without it a hot loop lands wherever the code before it
# leaves it: funnkuch-reduce ran 1945 or 2300 ms with identical instructions,
# by the 16-byte position of its function.
#
# Oracle: gcc 13 -O2 aligns functions to 16 and loop heads to 16 when that
# costs at most 10 bytes, else 8 (.p2align 4,,10 / .p2align 3); clang 18 -O2
# pads loop heads to 16.  MIR's innermost loop heads go to 32 -- the fetch
# block the measurement found -- with functions aligned to match
# (third_party/mir/mir-target.h, mir-gen-x86_64.c).
#
# The reducer is tests/testloopalign.mad (C; its .expect is the gcc / clang
# output), compiled -O2.  A loop is a backward branch target; it is innermost
# when no other loop lies inside it.  The negative control runs the same
# check on gcc -O0's object, which aligns no loop head, and must see it fail.
# x86-64 Linux only (objdump); run from the repo root (make gates does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 120 2>/dev/null

MADC="${MADC:-bin/madc}"
SRC=tests/testloopalign.mad
FUNCS="sum_inner nested count_bits main"

if [ "$(uname -m)" != x86_64 ] || ! command -v objdump > /dev/null 2>&1; then
	echo "loop_align_gate: SKIP -- x86-64 with objdump only"
	exit 0
fi
if [ ! -x "$MADC" ]; then
	echo "loop_align_gate: $MADC missing -- build first" >&2
	exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# check FILE: print each violation, return 1 when any
check() {
	local f=$1
	nm "$f" > "$tmp/nm" 2> /dev/null
	objdump -d --no-show-raw-insn "$f" > "$tmp/dis" 2> /dev/null || return 2
	awk -v funcs="$FUNCS" -v nmfile="$tmp/nm" '
		function hex(s,  i, c, v) {
			v = 0; s = tolower (s);
			for (i = 1; i <= length (s); i++) {
				c = index ("0123456789abcdef", substr (s, i, 1));
				if (c == 0) break;
				v = v * 16 + c - 1;
			}
			return v;
		}
		BEGIN {
			n = split (funcs, fl, " ");
			for (i = 1; i <= n; i++) want[fl[i]] = 1;
			while ((getline line < nmfile) > 0) {
				split (line, w, " ");
				if ((w[2] == "T" || w[2] == "t") && (w[3] in want)) {
					addr = hex(w[1]);
					seen[w[3]] = 1;
					if (addr % 32 != 0) { printf "  function %s at 0x%x, not 32-aligned\n", w[3], addr; bad = 1 }
				}
			}
			for (i = 1; i <= n; i++)
				if (!(fl[i] in seen)) { printf "  function %s not found\n", fl[i]; bad = 1 }
		}
		/^ *[0-9a-f]+:\tj[a-z]+ +[0-9a-f]+ </ {
			pc = hex($1);
			t = hex($3);
			if (t < pc) { if (!(t in last) || pc > last[t]) last[t] = pc }
		}
		END {
			nl = 0;
			for (t in last) { nl++; lt[nl] = t + 0 }
			for (i = 1; i <= nl; i++) {
				inner = 1;
				for (j = 1; j <= nl; j++)
					if (j != i && lt[j] > lt[i] && lt[j] <= last[lt[i]] && last[lt[j]] <= last[lt[i]]) inner = 0;
				need = inner ? 32 : 8;
				if (lt[i] % need != 0) {
					printf "  %s loop head at 0x%x, not %d-aligned\n", inner ? "innermost" : "outer", lt[i], need;
					bad = 1
				}
			}
			if (nl == 0) { print "  no loop found"; bad = 1 }
			exit bad
		}' "$tmp/dis"
}

cp "$SRC" "$tmp/la.c"
red=0
if ! "$MADC" --std=gnu17 -O2 -c -o "$tmp/la.o" "$tmp/la.c" > "$tmp/log" 2>&1; then
	echo "loop_align_gate: RED -- madc -O2 -c failed:"; cat "$tmp/log"; exit 1
fi
if ! out=$(check "$tmp/la.o"); then echo "loop_align_gate: RED -- the object:"; echo "$out"; red=1; fi
if ! "$MADC" --std=gnu17 -O2 -o "$tmp/la" "$tmp/la.c" > "$tmp/log" 2>&1; then
	echo "loop_align_gate: RED -- madc -O2 -o failed:"; cat "$tmp/log"; exit 1
fi
if ! out=$(check "$tmp/la"); then echo "loop_align_gate: RED -- the executable:"; echo "$out"; red=1; fi
if [ "$("$tmp/la")" != "$(cat tests/testloopalign.expect)" ]; then
	echo "loop_align_gate: RED -- the executable's output differs from the gcc/clang oracle"; red=1
fi

# negative control: gcc -O0 aligns no loop head
if gcc -O0 -c -o "$tmp/g0.o" "$tmp/la.c" 2> /dev/null; then
	if check "$tmp/g0.o" > /dev/null; then
		echo "loop_align_gate: RED -- the negative control (gcc -O0's object) passed the check"; red=1
	fi
fi
[ "$red" = 0 ] || exit 1
echo "GREEN -- madc -O2: functions and innermost loop heads on 32, other loop heads on 8 (object and executable)."
