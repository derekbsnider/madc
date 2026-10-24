#!/bin/bash
# DRIFT-PREVENTION GATE -- the TARGET's long double size and alignment have ONE
# owner: c2mir's layout of TP_LDOUBLE, read by target_ldouble_size() /
# target_ldouble_align() (include/datadef.h, defined in src/madc_cir.cpp).
#
# madc's sources are compiled by the HOST compiler, so sizeof(long double)
# there is the host's: the Linux x86-64-hosted arm64-macos cross madc (which
# freezes the shipped darwin forest) folded 16 for a target whose long double
# is double (8), and laid long double members out 16-aligned.
#
# Rule over src/ and include/ (the served headers in include/madc/ are target
# text and exempt): no sizeof / alignof / _Alignof / __alignof__ applied to
# `long double` in code. Comments are stripped first, so prose naming the
# host expression stays legal. Two-sided: the negative control proves the
# pattern still bites and still skips a comment.
set -u
cd "$(dirname "$0")/.."

PAT='(sizeof|alignof|_Alignof|__alignof__)[[:space:]]*\([[:space:]]*long[[:space:]]+double[[:space:]]*\)'

# Print file:line:text for every code (non-comment) match in the given files.
code_hits() {
	PAT="$PAT" awk '
		BEGIN { pat = ENVIRON["PAT"] }
		FNR == 1 { inblk = 0 }
		{
			line = $0; out = ""
			while ( length(line) ) {
				if ( inblk ) {
					e = index(line, "*/")
					if ( !e ) { line = ""; break }
					line = substr(line, e + 2); inblk = 0
					continue
				}
				s = index(line, "/*"); c = index(line, "//")
				if ( c && (!s || c < s) ) { out = out substr(line, 1, c - 1); line = ""; break }
				if ( s ) { out = out substr(line, 1, s - 1); line = substr(line, s + 2); inblk = 1; continue }
				out = out line; line = ""
			}
			if ( out ~ pat ) print FILENAME ":" FNR ":" $0
		}' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	size_t n = sizeof(long double);
	return alignof( long  double );
	// the host's sizeof(long double) is not the target's
	/* nor is alignof(long double) */
	size_t m = target_ldouble_size();
CTL
k=$(code_hits "$ctl" | grep -c .)
if [ "$k" -ne 2 ]; then
	echo "check-one-ldouble-layout: NEGATIVE CONTROL FAILED"
	echo "  matched $k of 2 code lines (comments must not match)"
	exit 1
fi

files=$(git ls-files --cached --others --exclude-standard 'src/*.cpp' 'src/*.h' \
	'src/**/*.cpp' 'src/**/*.h' 'src/**/*.c' 'include/*.h' 'include/**/*.h' \
	| grep -v '^include/madc/' | sort -u)
bad=$(code_hits $files)
n=$(printf '%s' "$bad" | grep -c . || true)
echo "host long double size/align in madc code: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> use target_ldouble_size() / target_ldouble_align() (c2mir's layout)."
	exit 1
fi
echo "GREEN -- long double's layout has one owner, c2mir's."
