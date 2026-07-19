#!/bin/bash
# B2 class-pattern language/ABI equivalence gate.

set -u
cd "$(dirname "$0")/.."

MADC="${MADC:-bin/madc}"
PER_TEST_TIMEOUT="${PER_TEST_TIMEOUT:-30}"
MADC_CPU_LIMIT="${MADC_CPU_LIMIT:-30}"
SOURCE="tests/testclasspatternbasic.mad"
EXPECTED="tests/testclasspatternbasic.expect"
WORK="tmp/class_pattern_equivalence"

fail()
{
	echo "class_pattern_equivalence: $1" >&2
	exit 1
}

[ -x "$MADC" ] || fail "missing executable $MADC"
command -v g++ >/dev/null 2>&1 || fail "g++ is required"
command -v clang++ >/dev/null 2>&1 || fail "clang++ is required"
command -v nm >/dev/null 2>&1 || fail "nm is required"

rm -rf "$WORK"
mkdir -p "$WORK"

env MADC_CPU_LIMIT="$MADC_CPU_LIMIT" timeout "$PER_TEST_TIMEOUT" \
	"$MADC" --show-stats "$SOURCE" \
	>"$WORK/pattern.out" 2>"$WORK/pattern.stats" \
	|| fail "structural runtime failed"
env MADC_CPU_LIMIT="$MADC_CPU_LIMIT" MADC_CLASS_PATTERN_FORCE_LEGACY=1 \
	timeout "$PER_TEST_TIMEOUT" "$MADC" --show-stats "$SOURCE" \
	>"$WORK/legacy.out" 2>"$WORK/legacy.stats" \
	|| fail "forced-legacy runtime failed"
cmp -s "$EXPECTED" "$WORK/pattern.out" \
	|| fail "structural runtime output differs from the fixture"
cmp -s "$WORK/pattern.out" "$WORK/legacy.out" \
	|| fail "structural and forced-legacy runtime output differ"
grep -Eq 'class instantiate[[:space:]]+\.[[:space:]]+3 pattern / 0 parse / 0 cache / 1 opaque' \
	"$WORK/pattern.stats" || fail "structural lane did not engage 3/0/0/1"
grep -Eq 'class instantiate[[:space:]]+\.[[:space:]]+0 pattern / 3 parse / 0 cache / 1 opaque' \
	"$WORK/legacy.stats" || fail "forced-legacy lane did not report 0/3/0/1"

env MADC_CPU_LIMIT="$MADC_CPU_LIMIT" timeout "$PER_TEST_TIMEOUT" \
	"$MADC" --emit=c11 "$SOURCE" \
	>"$WORK/pattern.c" 2>"$WORK/pattern.c.err" \
	|| fail "structural C11 emission failed"
env MADC_CPU_LIMIT="$MADC_CPU_LIMIT" MADC_CLASS_PATTERN_FORCE_LEGACY=1 \
	timeout "$PER_TEST_TIMEOUT" "$MADC" --emit=c11 "$SOURCE" \
	>"$WORK/legacy.c" 2>"$WORK/legacy.c.err" \
	|| fail "forced-legacy C11 emission failed"
cmp -s "$WORK/pattern.c" "$WORK/legacy.c" \
	|| fail "structural and forced-legacy C11 output differ"

env MADC_CPU_LIMIT="$MADC_CPU_LIMIT" MADC_DUMP_MIR=1 \
	timeout "$PER_TEST_TIMEOUT" "$MADC" "$SOURCE" \
	>"$WORK/pattern.mir.out" 2>"$WORK/pattern.mir" \
	|| fail "structural MIR dump failed"
env MADC_CPU_LIMIT="$MADC_CPU_LIMIT" MADC_DUMP_MIR=1 \
	MADC_CLASS_PATTERN_FORCE_LEGACY=1 timeout "$PER_TEST_TIMEOUT" \
	"$MADC" "$SOURCE" \
	>"$WORK/legacy.mir.out" 2>"$WORK/legacy.mir" \
	|| fail "forced-legacy MIR dump failed"
cmp -s "$WORK/pattern.mir.out" "$WORK/legacy.mir.out" \
	|| fail "structural and forced-legacy MIR runtime output differ"
cmp -s "$WORK/pattern.mir" "$WORK/legacy.mir" \
	|| fail "structural and forced-legacy MIR dumps differ"

g++ -x c++ -std=c++11 -Wall -Wextra -Werror -O0 \
	-include stdio.h -include stdint.h "$SOURCE" -o "$WORK/gcc" \
	|| fail "GCC rejected the fixture"
clang++ -x c++ -std=c++11 -Wall -Wextra -Werror -O0 \
	-include stdio.h -include stdint.h "$SOURCE" -o "$WORK/clang" \
	|| fail "Clang rejected the fixture"
timeout "$PER_TEST_TIMEOUT" "$WORK/gcc" >"$WORK/gcc.out" \
	|| fail "GCC fixture runtime failed"
timeout "$PER_TEST_TIMEOUT" "$WORK/clang" >"$WORK/clang.out" \
	|| fail "Clang fixture runtime failed"
cmp -s "$WORK/pattern.out" "$WORK/gcc.out" \
	|| fail "madc runtime/layout differs from GCC"
cmp -s "$WORK/pattern.out" "$WORK/clang.out" \
	|| fail "madc runtime/layout differs from Clang"

cat >"$WORK/symbol_oracle.cpp" <<'EOF'
template<typename T>
class PatternSymbol {
public:
	T read(T *value) const;
	static T choose(T value);
};

long probe(PatternSymbol<long> *object, long *value)
{
	return object->read(value) + PatternSymbol<long>::choose(*value);
}
EOF

g++ -std=c++11 -Wall -Wextra -Werror -O0 -c \
	"$WORK/symbol_oracle.cpp" -o "$WORK/symbol_oracle.gcc.o" \
	|| fail "GCC rejected the symbol oracle"
clang++ -std=c++11 -Wall -Wextra -Werror -O0 -c \
	"$WORK/symbol_oracle.cpp" -o "$WORK/symbol_oracle.clang.o" \
	|| fail "Clang rejected the symbol oracle"
nm -u "$WORK/symbol_oracle.gcc.o" >"$WORK/symbol_oracle.gcc.nm" \
	|| fail "nm failed on the GCC oracle"
nm -u "$WORK/symbol_oracle.clang.o" >"$WORK/symbol_oracle.clang.nm" \
	|| fail "nm failed on the Clang oracle"
grep -qF '_ZNK13PatternSymbolIlE4readEPl' "$WORK/symbol_oracle.gcc.nm" \
	|| fail "GCC const-method symbol differs"
grep -qF '_ZN13PatternSymbolIlE6chooseEl' "$WORK/symbol_oracle.gcc.nm" \
	|| fail "GCC static-method symbol differs"
grep -qF '_ZNK13PatternSymbolIlE4readEPl' "$WORK/symbol_oracle.clang.nm" \
	|| fail "Clang const-method symbol differs"
grep -qF '_ZN13PatternSymbolIlE6chooseEl' "$WORK/symbol_oracle.clang.nm" \
	|| fail "Clang static-method symbol differs"

env MADC_CPU_LIMIT="$MADC_CPU_LIMIT" timeout "$PER_TEST_TIMEOUT" \
	"$MADC" --emit=c11 "$WORK/symbol_oracle.cpp" \
	>"$WORK/symbol_oracle.c" 2>"$WORK/symbol_oracle.err" \
	|| fail "madc rejected the symbol oracle"
grep -qF '_ZNK13PatternSymbolIlE4readEPl' "$WORK/symbol_oracle.c" \
	|| fail "madc const-method symbol differs from GCC/Clang"
grep -qF '_ZN13PatternSymbolIlE6chooseEl' "$WORK/symbol_oracle.c" \
	|| fail "madc static-method symbol differs from GCC/Clang"
if grep -qF '_ZN13PatternSymbolIlE6chooseEv' "$WORK/symbol_oracle.c"; then
	fail "madc emitted the obsolete void-parameter static symbol"
fi

rm -rf "$WORK"
echo "class_pattern_equivalence: structural == legacy == GCC == Clang; C11/MIR/symbols match"
