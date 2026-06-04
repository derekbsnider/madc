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
  | grep -vE '^include/doctest\.h:' )
n=$(printf '%s' "$hits" | grep -c . )
echo "retire-std-hardcoding finish-line: $n offending lines remain (target 0)"
if [ "$n" -ne 0 ]; then
  printf '%s\n' "$hits" | sed 's/^/  /' | head -25
  [ "$n" -gt 25 ] && echo "  … and $((n-25)) more"
  exit 1
fi
echo "GREEN — no per-type std:: hardcoding remains (not even a comment)."
