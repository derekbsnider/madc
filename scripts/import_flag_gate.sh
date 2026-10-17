#!/usr/bin/env bash
# import_flag_gate.sh — `madc --import=NAME` with no source only tries the
# import (docs/plans/2026-10-17-dialect-builtin-registration.md D11; `python
# -c "import NAME"` is the precedent), so a launcher tests before it starts:
# exit 0 when every named module imports; else the import error on stderr and
# exit 69 (sysexits.h's EX_UNAVAILABLE) — a status no other failure of madc's
# uses (theirs is 1), so "not installed" is told from the rest.
#
# The module that imports is tests/modules/greet (obj/test-modules/greet.so,
# built with the tests); the one refused is a shared object that is no madc
# module (it exports no madc_module_<name>), built here.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 120 2>/dev/null

BIN=${MADC_BIN:-bin/madc}
MODS=obj/test-modules
fail() { echo "import_flag_gate: $1"; rm -rf "${tmpd:-}"; exit 1; }
[ -x "$BIN" ] || fail "missing $BIN"
[ -f "$MODS/greet.so" ] || fail "missing $MODS/greet.so (make -C src test-modules)"

# run ARGS... — madc's combined output in $out, its status in $rc.
run() { out=$(timeout 60 "$BIN" "$@" </dev/null 2>&1); rc=$?; }

# 1. A module that imports: 0, and nothing said.
run --module-path="$MODS" --import=greet
[ "$rc" -eq 0 ] || fail "--import=greet exited $rc (want 0): $out"
[ -z "$out" ] || fail "--import=greet printed: $out"

# 2. No such module: 69 and the import error.
run --module-path="$MODS" --import=no_such_module_xyz
[ "$rc" -eq 69 ] || fail "--import=no_such_module_xyz exited $rc (want 69): $out"
case "$out" in
*"import: no module named 'no_such_module_xyz'"*) ;;
*) fail "--import=no_such_module_xyz did not say the import error: $out" ;;
esac

# 3. Several: one that fails fails the test, after one that imports.
run --module-path="$MODS" --import=greet --import=no_such_module_xyz
[ "$rc" -eq 69 ] || fail "--import=greet --import=no_such_module_xyz exited $rc (want 69): $out"

# 4. A library that is no madc module: refused, 69, the reason named.
tmpd=$(mktemp -d)
printf 'int fake_answer(void) { return 42; }\n' > "$tmpd/fake.c"
cc -shared -fPIC -o "$tmpd/fake.so" "$tmpd/fake.c" 2>/dev/null || fail "cc could not build the refused module"
run --module-path="$tmpd" --import=fake
[ "$rc" -eq 69 ] || fail "--import=fake exited $rc (want 69): $out"
case "$out" in
*"import: module 'fake': "*"is not a madc module (it exports no madc_module_fake)"*) ;;
*) fail "--import=fake did not name the refusal: $out" ;;
esac
rm -rf "$tmpd"

echo "import_flag_gate: OK — --import with no source: 0 when the module imports, 69 and the import error when it is missing or no madc module"
