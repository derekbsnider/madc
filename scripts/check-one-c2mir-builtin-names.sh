#!/bin/bash
# DRIFT-PREVENTION GATE -- the call names c2mir lowers in place (alloca,
# __builtin_va_arg, __builtin_frame_address, ...) have ONE table:
# builtin_names[] in third_party/mir/c2mir/c2mir.c, read through
# builtin_kind() (and c2mir_builtin_call_name_p() for an embedding front end).
#
# Four copies grew: c2mir's __has_builtin, its call check, its definition
# check and its generator each compared the names on their own, and madc's
# is_c2mir_builtin_call_name (src/cir_builder.cpp) kept a fifth list -- the
# one that missed the frame queries and failed the win64 release pack
# (2026-10-21).
#
# Rules: in c2mir.c, a name comparison (strcmp / strncmp against a builtin
# name macro or a "__builtin_" literal) appears only inside builtin_kind();
# madc's is_c2mir_builtin_call_name holds no string literal (it asks c2mir).
# Two-sided: the negative control proves both rules still bite.
set -u
cd "$(dirname "$0")/.."

C2MIR=third_party/mir/c2mir/c2mir.c
CIR=src/cir_builder.cpp
NAMES='ADD_OVERFLOW|SUB_OVERFLOW|MUL_OVERFLOW|EXPECT|BUILTIN_ABORT|BUILTIN_MEMCMP|BUILTIN_COPYSIGNF|BUILTIN_NAN|CONVERT_VECTOR|SHUFFLE|SHUFFLE_VECTOR|VECTOR_ELEMENTS|JCALL|JRET|PROP_SET|PROP_EQ|PROP_NE|CLEANUP_ARM|FRAME_ADDRESS|RETURN_ADDRESS|ALLOCA|BUILTIN_ALLOCA|BUILTIN_VA_START|BUILTIN_VA_ARG'
CMP="(strn?cmp|str_eq_p)[[:space:]]*\\([^;]*(\\b($NAMES)\\b|\"__builtin_)"

# c2mir.c's comparisons outside builtin_kind's body
c2mir_stray() {
	awk '/^static enum builtin_kind builtin_kind \(/ && !/;[[:space:]]*$/ { inside = 1 }
	     inside && /^}/ { inside = 0; next }
	     !inside { print FILENAME ":" FNR ": " $0 }' "$1" | grep -E "$CMP"
}
# string literals in madc's predicate body
cir_literals() {
	awk '/^static bool is_c2mir_builtin_call_name\(/ && !/;[[:space:]]*$/ { inside = 1 }
	     inside { print FILENAME ":" FNR ": " $0 }
	     inside && /^}/ { inside = 0 }' "$1" | grep '"'
}

ctl=$(mktemp -d)
trap 'rm -rf "$ctl"' EXIT
cat > "$ctl/c.c" <<'CTL'
static enum builtin_kind builtin_kind (const char *name, unsigned *flags) {
  if (strncmp (name, "__builtin_", 10) != 0 && strcmp (name, ALLOCA) != 0) return BI_NONE;
}
static int other (const char *s) {
  return strcmp (s, FRAME_ADDRESS) == 0 || strcmp (s, "__builtin_expect") == 0;
}
CTL
cat > "$ctl/cir.cpp" <<'CTL'
static bool is_c2mir_builtin_call_name(const madc::dis::istring &name);
static void unrelated() { const char *s = "a"; }
static bool is_c2mir_builtin_call_name(const madc::dis::istring &name)
{
	return name == "__builtin_alloca";
}
CTL
a=$(c2mir_stray "$ctl/c.c" | grep -c . || true)
b=$(cir_literals "$ctl/cir.cpp" | grep -c . || true)
if [ "$a" -ne 1 ] || [ "$b" -ne 1 ]; then
	echo "check-one-c2mir-builtin-names: NEGATIVE CONTROL FAILED"
	echo "  a stray c2mir comparison matched $a of 1, a madc literal $b of 1"
	exit 1
fi

if ! grep -q '^static enum builtin_kind builtin_kind (' "$C2MIR"; then
	echo "check-one-c2mir-builtin-names: $C2MIR has no builtin_kind() -- the table's reader moved"
	exit 1
fi
if ! grep -q '^static bool is_c2mir_builtin_call_name(' "$CIR"; then
	echo "check-one-c2mir-builtin-names: $CIR has no is_c2mir_builtin_call_name -- update this gate"
	exit 1
fi
stray=$(c2mir_stray "$C2MIR")
lits=$(cir_literals "$CIR")
n=$(printf '%s' "$stray" | grep -c . || true)
m=$(printf '%s' "$lits" | grep -c . || true)
echo "c2mir builtin-name comparisons outside builtin_kind: $n (target 0); literals in madc's predicate: $m (target 0)"
if [ "$n" -ne 0 ] || [ "$m" -ne 0 ]; then
	[ -n "$stray" ] && printf '%s\n' "$stray"
	[ -n "$lits" ] && printf '%s\n' "$lits"
	echo "  -> add the name to builtin_names[] and ask builtin_kind / c2mir_builtin_call_name_p."
	exit 1
fi
echo "GREEN -- c2mir's builtin call names have one table."
