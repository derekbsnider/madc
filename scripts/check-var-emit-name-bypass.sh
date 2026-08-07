#!/bin/bash
# DRIFT-PREVENTION GATE — var_emit_name unification.
#
# A Variable is named in emitted C ONLY through CirBuilder::var_emit_name
# (functions: call_emit_symbol / func_emit_name). It is the single resolver of
# storage_alias_name — the Itanium symbol a system-header class static binds
# to, an __attribute__((alias)) data alias, a function asm-label. Emitting a
# raw Variable name bypasses that resolution: the storage is DEFINED under the
# alias while the bypass site references the raw name — an undeclared
# identifier at best, a silently wrong symbol at worst. This family bit six
# times in one session (2026-07-28); the four *_realhdr regressions were the
# ctor-receiver instance.
#
# Two complementary checks, structural rather than name-keyed:
#
#  1. STRICT (target 0): a Token-held Variable read for emission —
#     `(->|.)var.name.c_str()` or `(->|.)object.name.c_str()` — outside
#     var_emit_name itself. Every TokenX::var / TokenSubscript::object /
#     TokenMember::object IS a Variable by construction, so a raw read here
#     is always a bypass unless the line carries an audited
#     `// allowed-exception: <reason>` marker (population provably
#     local/parameter/capture, a guarded !file_global path, or a member
#     SELECTOR rather than storage). `grep allowed-exception src/cir_builder.cpp`
#     lists every exemption and its reason.
#
#  2. RATCHET (baseline below, may only shrink): bare-pointer forms
#     `id(<ptr>->name.c_str())` etc. where the receiver's type is invisible to
#     an AST-blind scan (Variable* like v/sv, DataDef tag reads like sdd/cdd,
#     labels, registration records). Growth means a NEW raw emission site was
#     written — route it through var_emit_name or, if it is provably not a
#     Variable, update the baseline DOWNWARD-ONLY discipline consciously in
#     the same commit and say why.
set -u
cd "$(dirname "$0")/.."

# --- 1. strict: Token-held Variable reads ---
bad=$(awk '
  /std::string[[:space:]]+CirBuilder::var_emit_name\(/ { infn=1 }
  infn && /^}/ { infn=0; next }
  {
    if ($0 ~ /allowed-exception/) next
    line=$0
    sub(/\/\/.*/, "", line)
    if (infn) next
    # Anchor on an EMISSION call consuming the raw name on the same line —
    # comparisons, lookup keys, and debug prints read the name legitimately.
    if (line ~ /(^|[^a-zA-Z_])(id|array_ctor_call|array_storage_decl|param_decl)\([^;]*(->|\.)(var|object)\.name\.c_str/)
      printf "%s:%d: %s\n", FILENAME, FNR, $0
  }
' src/cir_builder.cpp)

n=$(printf '%s' "$bad" | grep -c . )
echo "var-emit-name bypass gate: $n raw Token-held Variable emission(s) (target 0)"
if [ "$n" -ne 0 ]; then
  printf '%s\n' "$bad" | sed 's/^/  /'
  echo "  -> route through var_emit_name / func_emit_name, or add an audited allowed-exception marker."
  exit 1
fi

# --- 2. ratchet: bare-pointer name emissions (Variable* and non-Variable alike) ---
BASELINE=19
m=$(grep -cE '\b(id|array_ctor_call|array_storage_decl)\([a-zA-Z_]+->name\.c_str\(\)' src/cir_builder.cpp)
echo "var-emit-name bare-pointer ratchet: $m site(s) (baseline $BASELINE, growth forbidden)"
if [ "$m" -gt "$BASELINE" ]; then
  echo "  -> a NEW bare-pointer name emission appeared. If it names a Variable, route it"
  echo "     through var_emit_name; if provably not (type tag, label), lower is fine but"
  echo "     growth is not — justify any baseline change in the same commit."
  grep -nE '\b(id|array_ctor_call|array_storage_decl)\([a-zA-Z_]+->name\.c_str\(\)' src/cir_builder.cpp | sed 's/^/  /'
  exit 1
fi
echo "GREEN — Variable emission names derive only via var_emit_name."
