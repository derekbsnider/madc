#!/bin/bash
# STATIC GATE -- MIR's own sources never depend on the signedness of plain char.
#
# Plain char is a TARGET property: signed on x86-64, Apple arm64 and Windows,
# UNSIGNED on aarch64 / ppc64 / s390x Linux. c2mir compiles plain char unsigned
# on aarch64 Linux, so a c2m that compiles MIR itself there (upstream's
# c2mir-bootstrap tests) builds MIR's sources with unsigned char. Two faults
# this gate keeps out:
#   - a value that is -1 for "none" held in a char: `(d = hex_value (...)) >= 0`
#     in mir-gen-aarch64.c's out_insn never failed once char was unsigned, and
#     the hex scan ran off the end of the pattern;
#   - EOF in c2mir's char line buffer: cs_unget pushed EOF (-1) back and cs_get
#     popped it as a char -- 255 where char is unsigned ("syntax error on 255"),
#     and where char is signed a source byte 0xFF popped as -1 and ended the file
#     ("unfinished comment"; reducer c-tests/new/source-byte-ff.c).
#
# Rule 1: every MIR source compiles clean under -funsigned-char -Wtype-limits --
#   natively, and through the aarch64 cross compiler when present (mir-gen.c
#   includes the HOST target's backend, so that is what reaches
#   mir-gen-aarch64.c). A "limited range of data type" comparison is a
#   signedness dependency.
# Rule 2: c2mir's line buffer holds bytes only: cs_get returns its byte as
#   unsigned char, and cs_unget never stores EOF.
# Negative controls: a char compared >= 0 must warn; a bad cs_get / cs_unget
# must fail rule 2.
set -u
cd "$(dirname "$0")/.."

MIR=third_party/mir
SRCS="mir.c mir-gen.c c2mir/c2mir.c c2mir/c2mir-driver.c"
FLAGS="-std=gnu11 -O0 -fsyntax-only -funsigned-char -Wtype-limits -Wno-abi -I. -DMIR_BOOTSTRAP"
T=$(mktemp -d "${TMPDIR:-/tmp}/mirchar.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail=0

# range_hits CC FILE... : the -Wtype-limits range warnings CC reports.
range_hits() {
	local cc=$1
	shift
	( cd "$MIR" && for f in "$@"; do $cc $FLAGS "$f" 2>&1; done ) \
		| grep -E 'limited range of data type'
}

# line_buffer_violations FILE : rule 2 over one c2mir.c.
line_buffer_violations() {
	awk '
		/^static int cs_get \(/ { fn = "get" }
		/^static void cs_unget \(/ { fn = "unget"; eof = 0 }
		fn == "get" && /VARR_POP \(char, cs->ln\)/ && !/\(unsigned char\) VARR_POP/ {
			print FILENAME ":" FNR ": cs_get returns a line-buffer char unconverted: " $0 }
		fn == "unget" && /EOF/ { eof = 1 }
		fn == "unget" && /VARR_PUSH \(char, cs->ln/ && !eof {
			print FILENAME ":" FNR ": cs_unget stores its argument without excluding EOF" }
		fn != "" && /^}/ { fn = "" }
	' "$1"
}

# --- negative controls -------------------------------------------------------
printf 'int hv (int);\nint f (const char *p) { char d; int n = 0; while ((d = hv (*p++)) >= 0) n++; return n; }\n' > "$T/neg.c"
if ! gcc -std=gnu11 -fsyntax-only -funsigned-char -Wtype-limits "$T/neg.c" 2>&1 | grep -q 'limited range of data type'; then
	echo "check-mir-plain-char: CONTROL FAILED -- -Wtype-limits did not flag a char compared >= 0"
	exit 1
fi
printf 'static int cs_get (c2m_ctx_t c2m_ctx) {\n  return VARR_POP (char, cs->ln);\n}\nstatic void cs_unget (c2m_ctx_t c2m_ctx, int c) {\n  VARR_PUSH (char, cs->ln, c);\n}\n' > "$T/neg_lb.c"
if [ "$(line_buffer_violations "$T/neg_lb.c" | wc -l)" -ne 2 ]; then
	echo "check-mir-plain-char: CONTROL FAILED -- rule 2 did not flag a bad cs_get and cs_unget"
	exit 1
fi

# --- rule 1 --------------------------------------------------------------------
hits=$(range_hits gcc $SRCS)
if [ -n "$hits" ]; then
	echo "check-mir-plain-char: FAIL -- a signed-char dependency (host, -funsigned-char):"
	echo "$hits" | sed 's/^/    /'
	fail=1
fi
if command -v aarch64-linux-gnu-gcc > /dev/null 2>&1; then
	hits=$(range_hits aarch64-linux-gnu-gcc mir-gen.c)
	if [ -n "$hits" ]; then
		echo "check-mir-plain-char: FAIL -- a signed-char dependency (aarch64 backend):"
		echo "$hits" | sed 's/^/    /'
		fail=1
	fi
else
	echo "check-mir-plain-char: aarch64-linux-gnu-gcc absent -- the aarch64 backend half is not checked here"
fi

# --- rule 2 --------------------------------------------------------------------
v=$(line_buffer_violations "$MIR/c2mir/c2mir.c")
if [ -n "$v" ]; then
	echo "check-mir-plain-char: FAIL -- c2mir's line buffer holds more than bytes:"
	echo "$v" | sed 's/^/    /'
	fail=1
fi

[ "$fail" -eq 0 ] && echo "check-mir-plain-char: OK (controls fire; MIR sources sign-agnostic; line buffer holds bytes)"
exit "$fail"
