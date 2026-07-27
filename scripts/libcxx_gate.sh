#!/bin/bash
# libcxx_gate.sh — the libc++ standard-library flavor gate
# (docs/plans/2026-07-26-libcxx-flavor-plan.md).
#
# libc++ is a LIBRARY, not a platform: Apple's default, the Android NDK's STL,
# FreeBSD's system C++ library, and available anywhere clang is. So the library
# work develops and gates HERE, on Linux, against libc++-18-dev — only the
# target plumbing needs a Mac.
#
# The promise under test is the include-search PARTITION:
#
#   C++ stdlib (c++/v1) -> madc's embedded freestanding set -> C library
#
# madc's embedded headers ARE its compiler resource dir, so they sit at the
# slot the generated table records as madc_compiler_owned_include_dir. A real
# C++ standard library ahead of that slot must win (libc++ ships its own
# <stddef.h>/<stdint.h>/<float.h>/<stdbool.h> wrappers and #errors when they
# are bypassed), while a name nothing on the path supplies must still resolve
# from the embedded copy — that is the header-less-Mac promise.
#
# Legs:
#   1  libc++'s <stddef.h> wrapper wins when libc++ is ahead of the slot
#   2  madc's embedded <stddef.h> still serves when nothing is ahead of it
#   3  <cstddef> compiles AND runs — the full chain, including the wrapper's
#      #include_next reaching the real freestanding header behind it
#   4  <cstdint> and <climits> likewise
#   5  clang++ -stdlib=libc++ agrees, as the oracle that owns this library
#   6  a genuine user-vs-user typedef conflict STILL errors (the max_align_t
#      fix must not have cost the diagnostic)
#
# libc++ and clang are container artifacts, so the gate SKIPS (rc 0) when
# either is missing and says which. LIBCXX_INCLUDE overrides discovery.
set -u
cd "$(dirname "$0")/.."
D=tmp/libcxxgate
rc=0
pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; rc=1; }
skip() { echo "libcxx_gate: SKIP ($1)"; exit 0; }

run() { ( ulimit -t 60; timeout 90 "$@" ); }

MADC=${MADC_BIN:-bin/madc}
[ -x "$MADC" ] || skip "$MADC not built"
CLANGXX=""
for c in clang++-18 clang++; do
	command -v "$c" >/dev/null 2>&1 && CLANGXX="$c" && break
done
[ -n "$CLANGXX" ] || skip "no clang++ (apt install clang-18 libc++-18-dev)"

# Discover libc++'s include dir from clang itself — never a hardcoded path
# (the same target-derived principle the generated search table follows).
LIBCXX=${LIBCXX_INCLUDE:-}
if [ -z "$LIBCXX" ]; then
	LIBCXX=$(printf '' | $CLANGXX -stdlib=libc++ -x c++ -E -v - 2>&1 \
		 | sed -n 's,^ \(/.*/c++/v1\)$,\1,p' | head -1)
fi
[ -n "$LIBCXX" ] && [ -d "$LIBCXX" ] || skip "libc++ headers not found (apt install libc++-18-dev)"
[ -f "$LIBCXX/stddef.h" ] || skip "$LIBCXX has no stddef.h wrapper — not a libc++ tree"

rm -rf "$D"
mkdir -p "$D"

# --- fixtures --------------------------------------------------------------
# Each probe asserts WHICH header served by testing that header's own guard
# macro, so the check is about resolution rather than about compiling text
# that both copies would accept.
cat > "$D/served_libcxx.cpp" <<'EOF'
#include <stddef.h>
#ifndef _LIBCPP_STDDEF_H
#error LIBCXX_WRAPPER_WAS_BYPASSED
#endif
int main(void) { return 0; }
EOF

cat > "$D/served_embedded.cpp" <<'EOF'
#include <stddef.h>
#ifndef __MADC_STDDEF_H
#error EMBEDDED_HEADER_DID_NOT_SERVE
#endif
int main(void) { return 0; }
EOF

# The full chain: libc++ <cstddef> -> libc++ <stddef.h> wrapper ->
# #include_next -> the real freestanding <stddef.h> behind it.
cat > "$D/chain.cpp" <<'EOF'
#include <cstddef>
#include <stdio.h>
int main(void)
{
	printf("%d %d\n", (int)sizeof(size_t), (int)sizeof(ptrdiff_t));
	return 0;
}
EOF

cat > "$D/more.cpp" <<'EOF'
#include <cstdint>
#include <climits>
#include <stdio.h>
int main(void)
{
	printf("%d %d\n", (int)sizeof(int64_t), (int)(CHAR_BIT));
	return 0;
}
EOF

# The diagnostic the max_align_t fix must NOT have cost: madc pre-registers
# max_align_t, so a real header may re-typedef it — but two USER definitions
# of the same name still conflict, exactly as gcc and clang report.
cat > "$D/conflict.c" <<'EOF'
typedef struct { int a; } Foo;
typedef struct { int b; } Foo;
int main(void) { return 0; }
EOF

L="-I$LIBCXX"

# --- leg 1: libc++'s wrapper wins ------------------------------------------
if run "$MADC" $L "$D/served_libcxx.cpp" >/dev/null 2>"$D/l1.err"; then
	pass "libc++ <stddef.h> wrapper wins when libc++ precedes the slot"
else
	fail "libc++ wrapper did not win: $(head -c 200 "$D/l1.err")"
fi

# --- leg 2: the embedded copy still serves ---------------------------------
if run "$MADC" "$D/served_embedded.cpp" >/dev/null 2>"$D/l2.err"; then
	pass "embedded <stddef.h> still serves when nothing precedes the slot"
else
	fail "embedded header stopped serving: $(head -c 200 "$D/l2.err")"
fi

# --- leg 3: the full chain compiles AND runs -------------------------------
got=$(run "$MADC" $L "$D/chain.cpp" 2>"$D/l3.err")
if [ "$got" = "8 8" ]; then
	pass "<cstddef> chain compiles and runs (wrapper + #include_next)"
else
	fail "<cstddef> chain: got '$got' $(head -c 200 "$D/l3.err")"
fi

# --- leg 4: the rest of the freestanding-backed set ------------------------
got4=$(run "$MADC" $L "$D/more.cpp" 2>"$D/l4.err")
if [ "$got4" = "8 8" ]; then
	pass "<cstdint> + <climits> compile and run under libc++"
else
	fail "<cstdint>/<climits>: got '$got4' $(head -c 200 "$D/l4.err")"
fi

# --- leg 5: the oracle that owns this library ------------------------------
if run "$CLANGXX" -stdlib=libc++ -o "$D/oracle" "$D/chain.cpp" 2>"$D/l5.err"; then
	want=$("$D/oracle")
	if [ "$want" = "$got" ]; then
		pass "clang++ -stdlib=libc++ agrees ('$want')"
	else
		fail "oracle mismatch: madc '$got' vs clang++ '$want'"
	fi
else
	fail "oracle build failed: $(head -c 200 "$D/l5.err")"
fi

# --- leg 6: the redefinition diagnostic survives ---------------------------
if run "$MADC" "$D/conflict.c" >/dev/null 2>"$D/l6.err"; then
	fail "a conflicting user typedef was accepted (diagnostic lost)"
elif grep -q "already defined" "$D/l6.err"; then
	pass "conflicting user typedef still errors, naming the identifier"
else
	fail "conflict rejected for the wrong reason: $(head -c 200 "$D/l6.err")"
fi

if [ $rc -eq 0 ]; then
	echo "libcxx_gate: OK (libc++ at $LIBCXX)"
else
	echo "libcxx_gate: FAILED"
fi
exit $rc
