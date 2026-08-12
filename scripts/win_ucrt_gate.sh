#!/bin/bash
# win_ucrt_gate.sh — W0.1 + W2.1 gate for the windows release lane (Track 6.4).
#
# Proves, on the container, that the cross toolchain produces correct UCRT
# win64 binaries — the two traps this lane starts from:
#
#   W0.1  Ubuntu/Debian mingw-w64 DEFAULTS TO MSVCRT.  The UCRT recipe is:
#           compile:  -D_UCRT -D__USE_MINGW_ANSI_STDIO=1
#           link:     -specs=<dumpspecs with -lmsvcrt -> -lucrt>
#                     + src/win_ucrt_compat.S (_setjmp/_setjmpex thunks for
#                       msvcrt-compiled static libs: winpthreads, libstdc++)
#         __USE_MINGW_ANSI_STDIO=1 is NOT optional: defining _UCRT alone turns
#         mingw's printf interposer OFF, and UCRT treats long double as 64-bit
#         (MSVC model) while gcc passes x87 80-bit — %Lf silently prints 0.00.
#
#   W2.1  mingw's default win64 setjmp captures __builtin_frame_address(0), so
#         longjmp runs SEH frame-consistency unwinding (RtlUnwindEx) — which
#         FAULTS crossing a JIT frame with no RUNTIME_FUNCTION entry (probed
#         2026-08-12: page fault inside ntdll, wine 9.0).  The mitigation is
#         __USE_MINGW_SETJMP_NON_SEH: setjmp becomes _setjmp(buf, NULL) and
#         longjmp restores registers without the unwinder.  madc's exception
#         lowering MUST ride the NON-SEH variant on win64.  The SEH leg here
#         is the negative control: it must KEEP failing — if a future
#         wine/ntdll makes it pass, the hazard model changed and this gate
#         says so loudly instead of silently drifting.
#
# Runner: wine by default; set MADC_WIN_RUNNER to run on real Windows over
# the W0.2 channel (e.g. an ssh-wrapper script), same convention the suite
# lanes will use.  Every run is wall-clock capped.
set -u

MINGW=x86_64-w64-mingw32
CC=$MINGW-gcc
CXX=$MINGW-g++
OBJDUMP=$MINGW-objdump
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/tmp/win-gate"
COMPAT="$ROOT/src/win_ucrt_compat.S"
UCRT_CPPFLAGS="-D_UCRT -D__USE_MINGW_ANSI_STDIO=1"
RUN="${MADC_WIN_RUNNER:-wine}"
CAP="timeout 60"

for tool in "$CC" "$CXX" "$OBJDUMP"; do
	if ! command -v "$tool" > /dev/null 2>&1; then
		echo "win_ucrt_gate: MISSING $tool — run scripts/provision_container.sh"
		exit 2
	fi
done
if [ "$RUN" = "wine" ] && ! command -v wine > /dev/null 2>&1; then
	echo "win_ucrt_gate: MISSING wine — run scripts/provision_container.sh"
	exit 2
fi

mkdir -p "$WORK"
cd "$WORK" || exit 2
fail=0

# The specs swap IS the UCRT selection: gcc's *lib spec hard-codes -lmsvcrt.
"$CC" -dumpspecs | sed 's/-lmsvcrt/-lucrt/g' > ucrt.specs

cat > gate_hello.c <<'EOF'
#include <stdio.h>
int main(void)
{
	long long big = 1234567890123456789LL;
	long double ld = 3.25L;
	printf("lld=%lld\n", big);
	printf("ld=%.2Lf\n", ld);
	printf("sizeof_ld=%d align_ld=%d\n", (int)sizeof(long double), (int)_Alignof(long double));
	return 0;
}
EOF

cat > gate_hello.cpp <<'EOF'
#include <iostream>
#include <string>
int main()
{
	std::string s = "hello from win64 C++";
	std::cout << s << " len=" << s.size() << std::endl;
	long double ld = 2.5L;
	std::cout << "ld=" << ld << " sizeof_ld=" << sizeof(long double) << std::endl;
	return 0;
}
EOF

cat > gate_setjmp_jit.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <windows.h>
static jmp_buf env;
static void helper_longjmp(void)
{
	fflush(stdout);
	longjmp(env, 42);
}
typedef void (*jitfn)(void);
static jitfn make_jit_stub(void)
{
	/* mov rax, imm64(helper) ; call rax ; ret — a JIT frame with no unwind info */
	unsigned char code[16];
	size_t n = 0;
	void *page;
	unsigned long long target = (unsigned long long)(uintptr_t)helper_longjmp;
	code[n++] = 0x48; code[n++] = 0xB8;
	memcpy(code + n, &target, 8); n += 8;
	code[n++] = 0xFF; code[n++] = 0xD0;
	code[n++] = 0xC3;
	page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!page) { printf("VirtualAlloc failed\n"); exit(2); }
	memcpy(page, code, n);
	FlushInstructionCache(GetCurrentProcess(), page, n);
	return (jitfn)page;
}
int main(void)
{
	jitfn stub = make_jit_stub();
	if (setjmp(env)) {
		printf("PROBE_OK\n");
		fflush(stdout);
		return 0;
	}
	stub();
	printf("UNREACHABLE\n");
	return 1;
}
EOF

# imports_ok <exe>: import table must be UCRT (apisets or ucrtbase), never msvcrt
imports_ok() {
	local exe="$1" dlls
	dlls=$("$OBJDUMP" -p "$exe" | awk '/DLL Name/{print $3}')
	if echo "$dlls" | grep -qi '^msvcrt\.dll'; then
		echo "  FAIL $exe imports msvcrt.dll"
		return 1
	fi
	if ! echo "$dlls" | grep -qiE '^(api-ms-win-crt|ucrtbase)'; then
		echo "  FAIL $exe imports no UCRT dll (got: $dlls)"
		return 1
	fi
	return 0
}

# expect <exe> <needle>...: run capped, every needle must appear on stdout
expect() {
	local exe="$1" out
	shift
	out=$(WINEDEBUG=-all $CAP $RUN "./$exe" 2>/dev/null)
	local rc=$?
	for needle in "$@"; do
		if ! echo "$out" | grep -qF "$needle"; then
			echo "  FAIL $exe: missing '$needle' (rc=$rc)"
			echo "$out" | sed 's/^/    | /'
			return 1
		fi
	done
	return 0
}

echo "win_ucrt_gate: [1/4] C hello — UCRT imports + %lld + 80-bit long double printf"
if "$CC" -O0 $UCRT_CPPFLAGS -specs=ucrt.specs gate_hello.c "$COMPAT" -o gate_hello_c.exe \
   && imports_ok gate_hello_c.exe \
   && expect gate_hello_c.exe "lld=1234567890123456789" "ld=3.25" "sizeof_ld=16 align_ld=16"; then
	echo "  ok"
else
	fail=1
fi

echo "win_ucrt_gate: [2/4] C++ hello — static libstdc++/winpthreads over UCRT"
if "$CXX" -O0 $UCRT_CPPFLAGS -specs=ucrt.specs -static gate_hello.cpp "$COMPAT" -o gate_hello_cpp.exe \
   && imports_ok gate_hello_cpp.exe \
   && expect gate_hello_cpp.exe "hello from win64 C++ len=20" "ld=2.5 sizeof_ld=16"; then
	echo "  ok"
else
	fail=1
fi

echo "win_ucrt_gate: [3/4] W2.1 — NON-SEH setjmp/longjmp across a JIT frame survives"
if "$CC" -O0 $UCRT_CPPFLAGS -D__USE_MINGW_SETJMP_NON_SEH -specs=ucrt.specs gate_setjmp_jit.c "$COMPAT" -o gate_sj_nonseh.exe \
   && expect gate_sj_nonseh.exe "PROBE_OK"; then
	echo "  ok"
else
	fail=1
fi

echo "win_ucrt_gate: [4/4] W2.1 negative control — DEFAULT (SEH) setjmp must still fault"
if "$CC" -O0 $UCRT_CPPFLAGS -specs=ucrt.specs gate_setjmp_jit.c "$COMPAT" -o gate_sj_seh.exe; then
	out=$(WINEDEBUG=-all $CAP $RUN ./gate_sj_seh.exe 2>/dev/null)
	if echo "$out" | grep -qF "PROBE_OK"; then
		echo "  NEGCTL-CHANGED: SEH-default longjmp crossed a JIT frame cleanly."
		echo "  The unwinder hazard this gate models no longer reproduces here —"
		echo "  re-examine W2.1 (runner: $RUN) before trusting either leg."
		fail=1
	else
		echo "  ok (still faults, as modeled)"
	fi
else
	echo "  FAIL: SEH probe did not build"
	fail=1
fi

if [ $fail -eq 0 ]; then
	echo "win_ucrt_gate: OK (runner: $RUN)"
else
	echo "win_ucrt_gate: FAIL (runner: $RUN)"
fi
exit $fail
