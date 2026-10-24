#!/bin/bash
# check-system-headers-from-libmadc.sh — the release (packed) madc reads its
# SYSTEM headers from libmadc, never from /usr/include (OWNER LAW 2026-10-09,
# restated 2026-10-13; .claude/rules/embedded-headers.md, top section).
#
#   bash scripts/check-system-headers-from-libmadc.sh [packed-madc-binary]
#
# Default binary: bin/madc-release. A binary whose libmadc carries no pack is
# a different product mode (it reads system headers from disk) — refused, not
# skipped: pointing this gate at it is a mistake.
#
# MECHANISM — poison, not absence. Inside a private mount namespace
# (`unshare -rm`, no root), every include directory gcc searches (C and C++,
# plus /usr/include and /usr/local/include) is covered by a copy of itself in
# which EVERY file is `#error "...read from disk..."`. The headerless lane
# (empty dirs) proves the pack CAN serve; this proves the disk is never READ
# while it is present. Linux only (unshare); other hosts print a skip line.
#
# Checks, each a C / C++ / madc-dialect program through the release binary:
#   1. compiles and runs, output exact, no poison text (headers from libmadc);
#      one C program also reaches a header the pack's C layer does not
#      freeze (<inttypes.h>), served from the profile above it;
#   2. NEGATIVE CONTROL — gcc on the same C program hits the poison;
#   3. NEGATIVE CONTROL — `--no-embedded-headers` alone hits the poison, for
#      a C and a C++ program (the one switch that sends system headers to
#      disk must reach it — the grove bind included — or the gate is not
#      watching);
#   4. NEGATIVE CONTROL — under `--no-embedded-headers --no-sysroot-includes`
#      a core header no searched dir holds fails, where `--no-sysroot-includes`
#      alone serves it from the pack (the source arm is libmadc too).
set -u
cd "$(dirname "$0")/.." || exit 2
BIN="${1:-bin/madc-release}"

if [ "$(uname -s)" != "Linux" ]; then
	echo "check-system-headers-from-libmadc: SKIP (needs Linux unshare; host $(uname -s))"
	exit 0
fi
if [ ! -x "$BIN" ]; then
	echo "check-system-headers-from-libmadc: FAIL — no binary at $BIN" >&2
	exit 1
fi

if [ "${MADC_SYSHDR_GATE_INSIDE:-0}" != "1" ]; then
	if ! unshare -rm true 2>/dev/null; then
		echo "check-system-headers-from-libmadc: FAIL — this host cannot create a user+mount namespace (unshare -rm)" >&2
		exit 1
	fi
	MADC_SYSHDR_GATE_INSIDE=1 exec unshare -rm "$0" "$@"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The binary must carry a pack: its library ends in the container footer.
LIB=$(dirname "$BIN")/../lib/release/libmadc.so
if [ ! -f "$LIB" ] || [ "$(tail -c 8 "$LIB")" != "MADCSNAP" ]; then
	echo "check-system-headers-from-libmadc: FAIL — $BIN carries no pack ($LIB has no container footer)." >&2
	echo "  A pack-less build reads system headers from disk by design; this gate is for the release binary." >&2
	exit 1
fi

# --- Every include directory gcc searches, asked never listed. --------------
search_dirs() {
	echo | gcc -E -Wp,-v -x "$1" - 2>&1 >/dev/null \
		| sed -n '/search starts here:/,/End of search list/p' \
		| sed -n 's/^ \(\/.*\)$/\1/p'
}
DIRS=()
while IFS= read -r d; do
	[ -d "$d" ] || continue
	# A dir inside an already-listed one is covered by its parent's poison.
	for seen in ${DIRS[@]+"${DIRS[@]}"}; do
		case "$d/" in "$seen"/*) continue 2 ;; esac
	done
	DIRS+=("$d")
done < <( { printf '%s\n' /usr/include /usr/local/include; search_dirs c; search_dirs c++; } \
	  | awk '!seen[$0]++' | awk '{ print length, $0 }' | sort -n | cut -d' ' -f2- )

if [ "${#DIRS[@]}" -lt 2 ]; then
	echo "check-system-headers-from-libmadc: FAIL — found only ${#DIRS[@]} include dir(s) to poison" >&2
	exit 1
fi

# --- Poison copies: same tree, every file an #error. -------------------------
n=0
for d in "${DIRS[@]}"; do
	p="$WORK/poison$n"; n=$((n + 1))
	python3 - "$d" "$p" <<'EOF' || exit 1
import os, sys
src, dst = sys.argv[1], sys.argv[2]
for root, dirs, files in os.walk(src):
    rel = os.path.relpath(root, src)
    out = os.path.join(dst, rel) if rel != '.' else dst
    os.makedirs(out, exist_ok=True)
    for name in dirs + files:
        s = os.path.join(root, name); o = os.path.join(out, name)
        if os.path.islink(s):
            os.symlink(os.readlink(s), o)
        elif name in files:
            with open(o, 'w') as f:
                f.write('#error "madc read a system header from disk: %s"\n' % s)
EOF
	mount --bind "$p" "$d" || { echo "check-system-headers-from-libmadc: FAIL — cannot poison $d" >&2; exit 1; }
done

printf '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\nint main(void) { printf("c %%d\\n", (int)strlen("abc")); return EXIT_SUCCESS; }\n' > "$WORK/prog_c.c"
printf '#include <vector>\n#include <string>\n#include <cstdio>\nint main() { std::vector<std::string> v; v.push_back("x"); std::printf("cxx %%d\\n", (int)v.size()); return 0; }\n' > "$WORK/prog_cxx.cpp"
printf '#include <stdio.h>\nint main() { printf("mad %%d\\n", 3); return 0; }\n' > "$WORK/prog_mad.mad"
# A C program that binds the pack's C layer (<stdio.h>) and reaches a header
# the C layer does not freeze (<inttypes.h>): that header's text comes from the
# source arm of the profile above it, never the disk
# (docs/plans/2026-10-18-c-pack-layer.md, J3/J4).
printf '#include <stdio.h>\n#include <inttypes.h>\nint main(void) { printf("coff %%" PRId64 "\\n", (int64_t)7); return 0; }\n' > "$WORK/prog_c_off.c"

fail=0

# 2. gcc must hit the poison, or the poison is not live.
if gcc -fsyntax-only "$WORK/prog_c.c" > "$WORK/gcc.err" 2>&1 || ! grep -q "read a system header from disk" "$WORK/gcc.err"; then
	echo "check-system-headers-from-libmadc: CONTROL FAILED — gcc did not hit the poison:" >&2
	sed 's/^/    /' "$WORK/gcc.err" | head -5 >&2
	exit 1
fi

# 1. the release binary must not.
# The poison catches a READ; an existence probe (open / stat, then bind the
# pack's copy) never runs the #error. With strace present every file-path
# syscall under a system include dir is a failure too — the law is "never
# opens", not "never parses".
TRACE=""
command -v strace > /dev/null 2>&1 && TRACE=1
[ -n "$TRACE" ] || echo "  note: strace absent — existence probes on system dirs go unchecked"
disk_touches() {	# disk_touches <trace file> -> the system-dir paths it touched
	local d
	for d in "${DIRS[@]}"; do
		grep -o "\"$d/[^\"]*\"" "$1"
	done | sort -u
}
check() {	# check <program> <expected stdout>
	local out rc touched=""
	if [ -n "$TRACE" ]; then
		out=$(timeout 120 strace -f -qq -e trace=%file -o "$WORK/trace" "$BIN" "$1" 2>"$WORK/err")
		rc=$?
		touched=$(disk_touches "$WORK/trace")
	else
		out=$(timeout 120 "$BIN" "$1" 2>"$WORK/err")
		rc=$?
	fi
	if [ $rc -ne 0 ] || [ "$out" != "$2" ] || grep -q "read a system header from disk" "$WORK/err" || [ -n "$touched" ]; then
		echo "check-system-headers-from-libmadc: FAIL — $(basename "$1"): rc=$rc out='$out' (want '$2')" >&2
		grep -m3 "read a system header from disk\|error" "$WORK/err" | sed 's/^/    /' >&2
		[ -n "$touched" ] && echo "    touched $(echo "$touched" | wc -l) system-dir path(s), e.g.:" >&2 \
			&& echo "$touched" | head -3 | sed 's/^/      /' >&2
		fail=1
	else
		echo "  ok  $(basename "$1") — system headers from libmadc"
	fi
}
check "$WORK/prog_c.c" "c 3"
check "$WORK/prog_c_off.c" "coff 7"
check "$WORK/prog_cxx.cpp" "cxx 1"
check "$WORK/prog_mad.mad" "mad 3"

# 3. --no-embedded-headers must reach the disk (and so the poison), the grove
#    bind included: a bound root IS the core headers served from libmadc, so
#    the switch turns the bind off too (only an explicit --forest-bind=FILE
#    keeps it). Both languages — C binds the pack's C layer, C++ its madc
#    profile.
for neh in prog_c.c prog_cxx.cpp; do
	timeout 120 "$BIN" --no-embedded-headers "$WORK/$neh" > /dev/null 2> "$WORK/neh.err"
	if ! grep -q "read a system header from disk" "$WORK/neh.err"; then
		echo "check-system-headers-from-libmadc: CONTROL FAILED — --no-embedded-headers did not reach the disk ($neh):" >&2
		head -3 "$WORK/neh.err" | sed 's/^/    /' >&2
		fail=1
	else
		echo "  ok  --no-embedded-headers reads the disk (control, $neh)"
	fi
done

# 4. --no-embedded-headers serves NOTHING from libmadc — the pack's source arm
#    included. Past the toolchain cut (--no-sysroot-includes) no searched dir
#    holds <sys/sysinfo.h>: the packed binary serves it from the pack (the
#    positive twin, which proves the pack carries it), and with the switch the
#    include must fail rather than fall back to the pack.
printf '#include <sys/sysinfo.h>\nint main(void) { return sizeof(struct sysinfo) > 0 ? 0 : 1; }\n' > "$WORK/prog_tail.c"
if ! timeout 120 "$BIN" --no-sysroot-includes "$WORK/prog_tail.c" > "$WORK/tail.out" 2>&1; then
	echo "check-system-headers-from-libmadc: CONTROL FAILED — the pack did not serve <sys/sysinfo.h> past the toolchain cut:" >&2
	head -3 "$WORK/tail.out" | sed 's/^/    /' >&2
	fail=1
elif timeout 120 "$BIN" --no-embedded-headers --no-sysroot-includes "$WORK/prog_tail.c" > "$WORK/tail.out" 2>&1 \
	|| ! grep -q "Failed to open include file: sys/sysinfo.h" "$WORK/tail.out"; then
	# A failure that is not "not found" (the poison, reached through the
	# pack's copy's own includes) still means the pack served it.
	echo "check-system-headers-from-libmadc: FAIL — --no-embedded-headers served <sys/sysinfo.h> from the pack:" >&2
	head -3 "$WORK/tail.out" | sed 's/^/    /' >&2
	fail=1
else
	echo "  ok  --no-embedded-headers serves no header from the pack (control)"
fi

if [ $fail -ne 0 ]; then
	echo "check-system-headers-from-libmadc: FAIL — the release binary read system headers from disk" >&2
	exit 1
fi
echo "check-system-headers-from-libmadc: OK (${#DIRS[@]} include dir(s) poisoned)"
