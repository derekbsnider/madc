#!/usr/bin/env bash
# Win64 POSIX runtime ownership gate.
#
# The emitted-C runtime archive is the only project library on the specimen's
# link line.  The negative leg proves that the specimen really needs a
# project-only symbol; the positive leg proves that --gc-sections extracts the
# expected runtime members, that MinGW's libmingwex does not satisfy sleep in
# their place, and that the resulting PE has no dependency on a madc DLL.
set -u
set -o pipefail

cd "$(dirname "$0")/.." || exit 1
ulimit -t 900 2>/dev/null

fail() {
	echo "win_posix_archive_gate: $1" >&2
	exit 1
}

require_tool() {
	command -v "$1" >/dev/null 2>&1 \
		|| fail "required tool not found: $1"
}

# Print the madc-owned DLL imports parsed from an objdump -p report.  Parsing
# the PE import records (rather than grepping every occurrence of "madc")
# keeps paths and symbol-table text out of the decision.
audit_madc_dll_imports() {
	awk '
	$1 == "DLL" && $2 == "Name:" {
		name = tolower($3)
		if (name ~ /madc/)
			print name
	}' "$1"
}

trace_has_definition() {
	awk -v owner="$2" -v symbol="$3" '
	index($0, owner) && index($0, "definition of " symbol) { found = 1 }
	END { exit(found ? 0 : 1) }
	' "$1"
}

repo_root="$(pwd -P)"
win_cc="${WIN_CC:-x86_64-w64-mingw32-gcc-posix}"
win_ar="${WIN_AR:-x86_64-w64-mingw32-ar}"
win_nm="${WIN_NM:-x86_64-w64-mingw32-nm}"
win_objdump="${WIN_OBJDUMP:-x86_64-w64-mingw32-objdump}"
wine_bin="${WINE_BIN:-wine}"
wineboot_bin="${WINEBOOT_BIN:-wineboot}"
wineserver_bin="${WINESERVER_BIN:-wineserver}"
make_bin="${MAKE_BIN:-make}"

for tool in timeout "$make_bin" "$win_cc" "$win_ar" "$win_nm" \
	"$win_objdump" "$wine_bin" "$wineboot_bin" "$wineserver_bin"; do
	require_tool "$tool"
done

mkdir -p "$repo_root/tmp" || fail "cannot create tmp/"
work="$(mktemp -d "$repo_root/tmp/win_posix_archive_gate.XXXXXX")" \
	|| fail "cannot create the gate work directory"
wine_prefix="$work/wine-prefix"
mkdir -p "$wine_prefix" || fail "cannot create the isolated Wine prefix"
gate_ok=0
cleanup() {
	WINEDEBUG=-all WINEPREFIX="$wine_prefix" "$wineserver_bin" -k \
		>/dev/null 2>&1 || true
	if [ "$gate_ok" -eq 1 ]; then
		rm -rf "$work"
	else
		echo "win_posix_archive_gate: diagnostics retained in $work" >&2
	fi
}
trap cleanup EXIT

build_log="$work/hosted-build.log"
if ! timeout 1200 "$make_bin" -C src -j4 hosted-x86-64-windows \
	>"$build_log" 2>&1; then
	fail "hosted Win64 build failed or timed out: $(tail -n 12 "$build_log")"
fi

archive="$repo_root/lib/libmadc_rt-hosted-x86-64-windows.a"
host_exe="$repo_root/bin/madc-hosted-x86-64-windows.exe"
specs="$repo_root/obj/hosted-x86-64-windows/ucrt.specs"
[ -s "$archive" ] || fail "hosted runtime archive is absent or empty: $archive"
[ -s "$host_exe" ] || fail "hosted executable is absent or empty: $host_exe"
[ -s "$specs" ] || fail "hosted UCRT specs file is absent or empty: $specs"

# One table owns every project-only binding checked below.  Later POSIX
# slices extend this table with their symbol/member pairs.
runtime_bindings=(
	"strndup:rt_posix_str.o"
	"sleep:rt_posix_time.o"
	"setenv:rt_posix_env.o"
	"dlopen:rt_posix_dl.o"
)

source_file="$work/specimen.c"
object_file="$work/specimen.o"
specimen_exe="$work/specimen.exe"
cat >"$source_file" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <madc/posix/string.h>
#include <madc/posix/stdlib.h>
#include <madc/posix/dlfcn.h>

int main(void)
{
	char *copy = strndup("abcdef", 3U);
	const char *readback;
	void *self;

	if (copy == NULL)
		return 10;
	if (strcmp(copy, "abc") != 0) {
		free(copy);
		return 11;
	}
	free(copy);
	if (sleep(0U) != 0U)
		return 12;

	/* setenv/getenv must round-trip through the CRT's own view. */
	if (setenv("MADC_ARCHIVE_GATE", "set", 1) != 0)
		return 13;
	readback = getenv("MADC_ARCHIVE_GATE");
	if (readback == NULL || strcmp(readback, "set") != 0)
		return 14;
	/* overwrite == 0 must not replace an existing value. */
	if (setenv("MADC_ARCHIVE_GATE", "other", 0) != 0)
		return 15;
	readback = getenv("MADC_ARCHIVE_GATE");
	if (readback == NULL || strcmp(readback, "set") != 0)
		return 16;
	if (unsetenv("MADC_ARCHIVE_GATE") != 0)
		return 17;
	if (getenv("MADC_ARCHIVE_GATE") != NULL)
		return 18;

	/* dlopen(NULL) names the main program; RTLD_DEFAULT reaches the CRT. */
	self = dlopen(NULL, RTLD_LAZY);
	if (self == NULL)
		return 19;
	if (dlsym(RTLD_DEFAULT, "getenv") == NULL)
		return 20;
	if (dlsym(RTLD_DEFAULT, "madc_no_such_symbol_exists") != NULL)
		return 21;
	if (dlerror() == NULL)		/* the failed lookup must be reportable */
		return 22;
	if (dlerror() != NULL)		/* and consumed by the read */
		return 23;
	if (dlclose(self) != 0)
		return 24;

	puts("win-posix-archive-ok");
	return 0;
}
EOF

target_flags=(
	-D_UCRT
	-D__USE_MINGW_ANSI_STDIO=1
	-D__USE_MINGW_SETJMP_NON_SEH
	-D_USE_MATH_DEFINES
	"-specs=$specs"
)
compile_log="$work/compile.log"
if ! timeout 120 "$win_cc" -std=c11 -O0 -Wall -Wextra -Werror \
	-pedantic-errors -ffunction-sections -fdata-sections \
	"${target_flags[@]}" "-I$repo_root/include" -c "$source_file" \
	-o "$object_file" >"$compile_log" 2>&1; then
	fail "strict-C specimen compile failed: $(head -n 12 "$compile_log")"
fi

object_undefined="$work/object.undefined"
if ! timeout 30 "$win_nm" -u "$object_file" >"$object_undefined" 2>&1; then
	fail "could not inspect the specimen's undefined symbols"
fi
for binding in "${runtime_bindings[@]}"; do
	symbol="${binding%%:*}"
	count="$(grep -Ec "^[[:space:]]*U[[:space:]]+$symbol$" "$object_undefined")"
	[ "$count" -eq 1 ] \
		|| fail "specimen does not carry exactly one undefined $symbol reference"
done

# Negative control: sleep is present in MinGW's libmingwex, so sleep alone is
# not evidence.  strndup is deliberately the project-only unresolved symbol.
negative_log="$work/no-archive-link.log"
timeout 120 "$win_cc" "${target_flags[@]}" "$object_file" \
	-static-libgcc -Wl,--gc-sections -o "$work/no-archive.exe" \
	>"$negative_log" 2>&1
negative_rc=$?
[ "$negative_rc" -ne 0 ] \
	|| fail "negative control linked without the project runtime archive"
[ "$negative_rc" -ne 124 ] && [ "$negative_rc" -ne 137 ] \
	|| fail "negative-control link hit its resource cap"
grep -Eq 'undefined reference to .*strndup' "$negative_log" \
	|| fail "negative link failed, but not on project-only strndup: $(head -n 8 "$negative_log")"

archive_members="$work/archive.members"
archive_symbols="$work/archive.symbols"
selected_sources="$work/selected.sources"
expected_members="$work/expected.members"
if ! timeout 30 "$repo_root/scripts/select_ledger_sources.sh" win64 \
	>"$selected_sources" 2>&1; then
	fail "could not select canonical Win64 runtime members: $(head -n 8 "$selected_sources")"
fi
awk -F/ '
{
	member = $NF
	sub(/\.c$/, ".o", member)
	print member
}
' "$selected_sources" >"$expected_members" \
	|| fail "could not project selected sources to archive members"
if ! timeout 30 "$win_ar" t "$archive" >"$archive_members" 2>&1; then
	fail "could not list the hosted runtime archive"
fi
if ! cmp -s "$expected_members" "$archive_members"; then
	fail "archive membership differs from select_ledger_sources.sh win64: $(diff -u "$expected_members" "$archive_members")"
fi
if ! timeout 30 "$win_nm" -A -g --defined-only "$archive" \
	>"$archive_symbols" 2>&1; then
	fail "could not inspect the hosted runtime archive symbols"
fi
for binding in "${runtime_bindings[@]}"; do
	symbol="${binding%%:*}"
	member="${binding#*:}"
	member_count="$(grep -Fxc "$member" "$archive_members")"
	[ "$member_count" -eq 1 ] \
		|| fail "archive contains $member $member_count times (expected exactly once)"
	symbol_count="$(grep -Ec "(^|:)$member:.*[[:space:]]T[[:space:]]$symbol$" "$archive_symbols")"
	[ "$symbol_count" -eq 1 ] \
		|| fail "$member does not define exactly one strong lowercase $symbol"
done

trace_flags=()
for binding in "${runtime_bindings[@]}"; do
	symbol="${binding%%:*}"
	trace_flags+=("-Wl,--trace-symbol=$symbol")
done
link_map="$work/specimen.map"
link_log="$work/archive-link.log"
# This explicit archive path is the only project library on the link line.
if ! timeout 120 "$win_cc" "${target_flags[@]}" "$object_file" "$archive" \
	-static-libgcc -Wl,--gc-sections "-Wl,-Map,$link_map" \
	"${trace_flags[@]}" -o "$specimen_exe" >"$link_log" 2>&1; then
	fail "archive-only link failed: $(head -n 12 "$link_log")"
fi

for binding in "${runtime_bindings[@]}"; do
	symbol="${binding%%:*}"
	member="${binding#*:}"
	owner="$archive($member)"
	grep -Fq "$owner" "$link_map" \
		|| fail "link map does not show extraction of $owner"
	trace_has_definition "$link_log" "$owner" "$symbol" \
		|| fail "linker trace does not attribute $symbol to $owner"
done

# MinGW may load libmingwex for unrelated CRT helpers.  It must not extract a
# sleep member or claim the traced sleep definition.
if grep -Eiq 'libmingwex[^[:space:]]*\.a\([^)]*sleep[^)]*\.o\)' \
	"$link_map" "$link_log"; then
	fail "sleep was extracted from libmingwex instead of the project archive"
fi
if awk '
	{ line = tolower($0) }
	index(line, "libmingwex") && index(line, "definition of sleep") { found = 1 }
	END { exit(found ? 0 : 1) }
	' "$link_log"; then
	fail "linker trace assigns sleep to libmingwex"
fi

linked_symbols="$work/specimen.symbols"
if ! timeout 30 "$win_nm" -g --defined-only "$specimen_exe" \
	>"$linked_symbols" 2>&1; then
	fail "could not inspect the archive-only executable symbols"
fi
for binding in "${runtime_bindings[@]}"; do
	symbol="${binding%%:*}"
	count="$(grep -Ec "[[:space:]]T[[:space:]]$symbol$" "$linked_symbols")"
	[ "$count" -eq 1 ] \
		|| fail "archive-only executable does not define exact lowercase $symbol once"
done

# D3's runtime-resolution contract also requires the hosted madc artifact to
# carry and export lowercase sleep; inspecting the archive or Makefile alone
# cannot prove that an archive member survived the executable link.
host_symbols="$work/host.symbols"
host_pe="$work/host.pe"
if ! timeout 30 "$win_nm" -g --defined-only "$host_exe" \
	>"$host_symbols" 2>&1; then
	fail "could not inspect hosted madc symbols"
fi
host_sleep_count="$(grep -Ec '[[:space:]]T[[:space:]]sleep$' "$host_symbols")"
[ "$host_sleep_count" -eq 1 ] \
	|| fail "hosted madc does not define exact lowercase sleep once"
if ! timeout 30 "$win_objdump" -p "$host_exe" >"$host_pe" 2>&1; then
	fail "could not inspect hosted madc PE exports"
fi
grep -Eq '^[[:space:]]*\[[[:space:]]*[0-9]+\][[:space:]]+sleep[[:space:]]*$' "$host_pe" \
	|| fail "hosted madc PE export table does not contain exact lowercase sleep"

pe_dump="$work/specimen.pe"
if ! timeout 30 "$win_objdump" -p "$specimen_exe" >"$pe_dump" 2>&1; then
	fail "could not inspect archive-only PE imports"
fi
grep -Eq '^[[:space:]]*DLL[[:space:]]+Name:' "$pe_dump" \
	|| fail "objdump import report contains no DLL records; import audit would be vacuous"
madc_imports="$(audit_madc_dll_imports "$pe_dump")"
[ -z "$madc_imports" ] \
	|| fail "archive-only executable imports a madc DLL: $madc_imports"

# Negative control for the parser itself: the same audit must identify a
# synthetic import record in an otherwise-real objdump report.
negative_pe="$work/import-audit-negative.pe"
cp "$pe_dump" "$negative_pe" \
	|| fail "could not prepare import-audit negative control"
printf '\tDLL Name: libmadc-negative-control.dll\n' >>"$negative_pe" \
	|| fail "could not inject import-audit negative control"
negative_imports="$(audit_madc_dll_imports "$negative_pe")"
[ "$negative_imports" = "libmadc-negative-control.dll" ] \
	|| fail "import audit missed its synthetic madc DLL negative control"

wine_server_log="$work/wineserver.log"
WINEDEBUG=-all WINEPREFIX="$wine_prefix" timeout 30 "$wineserver_bin" -p \
	>"$wine_server_log" 2>&1
wineserver_rc=$?
# The isolated prefix has no prior lock owner, so success proves that this
# gate's server accepted the persistent lifetime before either Wine process.
[ "$wineserver_rc" -eq 0 ] \
	|| fail "could not start a persistent wineserver: $(head -n 8 "$wine_server_log")"
wineboot_log="$work/wineboot.log"
if ! WINEDEBUG=-all WINEPREFIX="$wine_prefix" timeout 120 \
	"$wineboot_bin" -u >"$wineboot_log" 2>&1; then
	fail "could not initialize the isolated Wine prefix: $(head -n 8 "$wineboot_log")"
fi

# Production-delivery leg: only the real public headers are named.  The
# canary proves hosted madc injected the generated POSIX supplement after the
# native <string.h>; the runtime call proves its declaration reaches codegen.
delivery_source="$work/delivery.mad"
delivery_stdout="$work/delivery.stdout"
delivery_stderr="$work/delivery.stderr"
cat >"$delivery_source" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef __MADC_POSIX_STRING_H
#error hosted madc did not inject the POSIX string supplement
#endif

int main(void)
{
	char *copy = strndup("abcdef", 3U);

	if (copy == NULL || strcmp(copy, "abc") != 0)
		return 20;
	free(copy);
	if (sleep(0U) != 0U)
		return 21;
	puts("win-posix-delivery-ok");
	return 0;
}
EOF
if ! WINEDEBUG=-all WINEPREFIX="$wine_prefix" timeout 120 "$wine_bin" \
	"$host_exe" --no-config --std=c11 "$delivery_source" \
	>"$delivery_stdout" 2>"$delivery_stderr"; then
	fail "hosted header-delivery probe failed under Wine: $(head -n 8 "$delivery_stderr")"
fi
[ ! -s "$delivery_stderr" ] \
	|| fail "hosted header-delivery probe emitted diagnostics: $(head -n 8 "$delivery_stderr")"
delivery_output="$(tr -d '\r' <"$delivery_stdout")"
[ "$delivery_output" = "win-posix-delivery-ok" ] \
	|| fail "hosted header-delivery probe printed [$delivery_output]"

wine_stdout="$work/wine.stdout"
wine_stderr="$work/wine.stderr"
if ! WINEDEBUG=-all WINEPREFIX="$wine_prefix" timeout 60 "$wine_bin" \
	"$specimen_exe" \
	>"$wine_stdout" 2>"$wine_stderr"; then
	fail "archive-only executable failed under Wine: $(head -n 8 "$wine_stderr")"
fi
wine_output="$(tr -d '\r' <"$wine_stdout")"
[ "$wine_output" = "win-posix-archive-ok" ] \
	|| fail "archive-only executable printed [$wine_output] under Wine"

gate_ok=1
binding_count="${#runtime_bindings[@]}"
selected_count="$(wc -l <"$expected_members")"
echo "win_posix_archive_gate: OK — $selected_count selected members exact, $binding_count POSIX symbols project-owned, headers delivered, no madc DLL import, Wine output matches"
