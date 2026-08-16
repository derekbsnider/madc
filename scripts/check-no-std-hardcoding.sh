#!/bin/bash
# FINISH-LINE GATE — retire-std-hardcoding campaign.
#
# Fails (non-zero) while ANY per-type std:: hardcoding remains, INCLUDING
# tombstone comments referencing deleted machinery ("gone without a trace —
# we have git for a reason"). "Done" means THIS prints GREEN; it cannot be
# faked by a behavioral claim.
#
# The ONLY permitted homes for std:: symbol knowledge:
#   - the mangler          src/madc_mangle.{cpp,h}   (single symbol source)
#   - the auto-include map  (symbol -> header trigger; the one config table)
#   - unit tests           tests/unit/   (NOT scanned — a doctest may legitimately
#                           cross-check the header-derived layout against the real
#                           <string>; production code may not).
# Everything else (builtin DataDef, wrapper/shim, dt*/dd* tag, hardcoded
# literal, a type-identity PREDICATE that asks "is this a std::string", OR a
# comment naming a deleted thing) must be gone. PRODUCTION CODE MUST NOT KNOW
# WHETHER A TYPE IS std::string — it asks generic questions (is_object(),
# class_needs_dtor(), overload resolution) instead. The earlier gate checked only
# for the builtin TAGS/wrappers, which let the same hardcoding live on under
# ALTERNATIVE names (is_std_string / is_string_class / the string_* + STR_*
# wrapper family); those are now counted too, so the loophole is closed.
set -u
cd "$(dirname "$0")/.."
PAT='ddSTRING|DataDefSTRING|add_string_methods|add_fstream_methods|dt[A-Z]*STREAM|dd[A-Z]*STREAM|DataDef[A-Z]*STREAM|streamout_|streamin_|ifstream_open|ofstream_good|ofstream_open|fstream_open|sizeof\(std::|__std_|SK_COUT|ostream_insert_symbol|ns_stl|tkSTRING|tkVECTOR|\bdtSTRING|is_std_string|is_string_class|\bstring_[a-z]|\bSTR_[A-Z]'
hits=$(grep -rnE "$PAT" src/ include/ \
  | grep -vE '^src/madc_mangle\.(cpp|h):' \
  | grep -vE '^include/doctest\.h:' \
  | grep -vE '^include/json\.hpp:' )   # vendored nlohmann/json — third-party, like doctest.h
n=$(printf '%s' "$hits" | grep -c . )
echo "retire-std-hardcoding finish-line: $n offending lines remain (target 0)"
if [ "$n" -ne 0 ]; then
  printf '%s\n' "$hits" | sed 's/^/  /' | head -25
  [ "$n" -gt 25 ] && echo "  … and $((n-25)) more"
  exit 1
fi
echo "GREEN — no per-type std:: hardcoding remains (not even a comment)."

# ---------------------------------------------------------------------------
# Second check: the one SANCTIONED std type-name, spelled in exactly one place.
#
# std::initializer_list is the single library type the LANGUAGE names
# ([dcl.init.list]/5 gives a braced-init-list that type by name), so unlike
# std::string there is no generic question that can replace knowing it. The
# predicate DataDef::is_std_initializer_list() therefore exists and may be
# CALLED anywhere — what must not spread is the SPELLING. Callers ask the
# predicate; only the mangler writes the name.
#
# Two things are hardcoding and are checked; a COMMENT naming the type is not,
# and neither is madc's own C++ using the feature (`std::initializer_list<node_t>`
# in a lambda parameter — madc is written in C++11):
#   1. a DEFINITION of the predicate outside the mangler (a second impl), and
#   2. a STRING LITERAL carrying the type's name (that is how a spelling escapes
#      the mangler and starts diverging).
#
# Negative controls, both verified when written (2026-08-16):
#   printf 'bool DataDef::is_std_initializer_list() const { return 0; }\n' >> src/parser.cpp   -> fails (1)
#   printf 'const char *s = "std::initializer_list<";\n' >> src/parser.cpp                     -> fails (2)
# The feature-test macro name __cpp_initializer_lists is NOT a type spelling and
# does not match (no `<`) — that is deliberate, not an exemption.
spellings=$(grep -rnE '::is_std_initializer_list|"[^"]*(std::)?initializer_list<' src/ include/ \
  | grep -vE '^src/madc_mangle\.(cpp|h):' \
  | grep -vE '^include/(doctest|json)\.h(pp)?:' \
  | grep -vE '^include/madc/' )   # embedded headers ARE C++ source text
sn=$(printf '%s' "$spellings" | grep -c . )
echo "sanctioned-std-name: $sn initializer_list definition(s)/name-literal(s) outside the mangler (target 0)"
if [ "$sn" -ne 0 ]; then
  printf '%s\n' "$spellings" | sed 's/^/  /' | head -15
  exit 1
fi
