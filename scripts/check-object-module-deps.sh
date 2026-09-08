#!/bin/bash
# check-object-module-deps.sh — the object carries its module list.
#
# `import name;` binds a module's library at parse; a native object built
# from that TU used to carry NO record of it, so the single-object lane
# resolved its imports blind and needed -l<name> on the command line (KG Gap
# obj_lane_module_dependency, DECIDED 2026-09-07: the object carries the
# list — MSVC .drectve / Rust #[link] / D pragma(lib) precedent). The CIR
# builder emits `static const char __madc_module_deps[] = "<spelling>\0..."`
# into every unit that imported a module; the loader opens each spelling
# before load. This gate holds both ends visible in the emitted C11:
#   1. a TU with `import m;` carries the table naming libm's spelling;
#   2. a TU with no import carries no table (the negative control).
set -u
cd "$(dirname "$0")/.." || exit 2
MADC=${MADC_BIN:-bin/madc}
if [ ! -x "$MADC" ]; then
	echo "check-object-module-deps: $MADC not built" >&2
	exit 2
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf 'import m;\nint main() { return (int)sqrt(16.0) - 4; }\n' > "$tmp/with.mad"
printf 'int main() { return 0; }\n' > "$tmp/without.mad"
with=$("$MADC" --std=c++20 --emit=c11 "$tmp/with.mad" 2>/dev/null)
without=$("$MADC" --std=c++20 --emit=c11 "$tmp/without.mad" 2>/dev/null)
if ! grep -q "__madc_module_deps" <<< "$with"; then
	echo "check-object-module-deps: a TU with 'import m;' emits no __madc_module_deps table" >&2
	exit 1
fi
if ! grep -q "__madc_module_deps.*libm" <<< "$with"; then
	echo "check-object-module-deps: the table does not name libm's spelling:" >&2
	grep "__madc_module_deps" <<< "$with" >&2
	exit 1
fi
if grep -q "__madc_module_deps" <<< "$without"; then
	echo "check-object-module-deps: NEGATIVE CONTROL FAILED — a TU with no import carries a module table" >&2
	exit 1
fi
echo "check-object-module-deps: OK — an importing unit carries its module list, a plain unit carries none"
