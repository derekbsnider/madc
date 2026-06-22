#!/bin/bash
# DRIFT-PREVENTION GATE — emit_symbol unification.
#
# A call's emitted C symbol has ONE source of truth: CirBuilder::call_emit_symbol
# (cir_builder.cpp), whose precedence is
#       emit_symbol  ?:  local_emit_name  ?:  default/var_emit_name
# Historically this precedence was re-implemented inline at ~8 call sites, each
# with a DIFFERENT partial slice (some forgot emit_symbol, some forgot
# local_emit_name). That divergence shipped wrong symbols. This gate makes the
# divergence impossible to RE-introduce.
#
# The mechanical, unfakeable invariant: the FuncDef field `local_emit_name` may
# be READ AS A VALUE (i.e. used to build a symbol) ONLY inside call_emit_symbol.
# Everywhere else it may appear only as:
#   - a `.empty()` predicate (writer guards, the capture-decay structural check)
#   - the LHS of an assignment `local_emit_name = ...` (the parser/CIR writers)
#   - a comment
#   - a read that does NOT build a symbol (a lookup key, a debug print, a
#     field-to-field copy), explicitly opted out with a trailing
#     `// allowed-exception: <reason>` marker on the SAME line
# A bare value-read anywhere else means a new site is deriving the symbol by
# hand instead of delegating to call_emit_symbol — exactly the drift we forbid.
# New call sites MUST call call_emit_symbol(v, fd) or call_emit_symbol(fd, def).
#
# The `allowed-exception` marker is per-line and auditable: `grep allowed-exception
# src/*.cpp` lists every exemption and its stated reason. The gate stays strict —
# a NEW symbol-building read with no marker still fails. A marker is justified ONLY
# when the read provably does not construct an emitted symbol; if in doubt, route
# through call_emit_symbol instead of marking.
set -u
cd "$(dirname "$0")/.."

bad=$(awk '
  # Track when we are inside a CirBuilder::call_emit_symbol definition (the one
  # legitimate home of a local_emit_name value-read). Both overloads match; the
  # delegating one contains no local_emit_name token, so this is harmless.
  /std::string[[:space:]]+CirBuilder::call_emit_symbol\(/ { infn=1 }
  infn && /^}/ { infn=0; next }
  {
    if ($0 ~ /allowed-exception/) next      # audited per-line opt-out (see header)
    line=$0
    sub(/\/\/.*/, "", line)                 # strip // line comments
    if (line !~ /local_emit_name/) next
    if (infn) next                          # inside the resolver: allowed
    tmp=line
    gsub(/local_emit_name\.empty\(\)/, "", tmp)        # predicate: allowed
    gsub(/local_emit_name[[:space:]]*=[^=]/, "", tmp)  # assignment LHS: allowed
    if (tmp ~ /local_emit_name/) printf "%s:%d: %s\n", FILENAME, FNR, $0
  }
' src/cir_builder.cpp src/parser.cpp src/madc_cir.cpp src/cir_emit_c.cpp 2>/dev/null)

n=$(printf '%s' "$bad" | grep -c . )
echo "call-emit-symbol drift gate: $n raw local_emit_name value-read(s) outside call_emit_symbol (target 0)"
if [ "$n" -ne 0 ]; then
  printf '%s\n' "$bad" | sed 's/^/  /'
  echo "  -> route the symbol through CirBuilder::call_emit_symbol instead."
  exit 1
fi
echo "GREEN — call symbols derive only via call_emit_symbol."
