#!/usr/bin/env bash
# capabilities_json_gate.sh — `madc --capabilities=json` is a valid, versioned,
# build-specific manifest, and its DERIVED lists cannot drift from the compiler.
#
# The manifest exists so tooling can query the compiler without a source file.
# Its two drift-prone lists are not typed into the manifest source — they come
# from the same owners the compiler itself uses:
#   - c_standards / cpp_standards come from Program's one --std= table
#     (Program::supported_*_standard_names). A hand-kept copy in the original
#     PR had already dropped c95; this gate asserts c95 is present AND that
#     EVERY standard the manifest advertises is actually accepted by --std=
#     (manifest ⊆ recognizer — the anti-drift guarantee, rule #7).
#   - emit_targets comes from CIR_EMIT_TARGETS; this gate compares the manifest
#     against that macro read straight out of src/cir_emit_c.h.
#
# The negative control is built in: aliases the recognizer ACCEPTS but the
# manifest must NOT advertise (c90, c, c++, cpp20) are asserted absent, and an
# unknown --capabilities format must be rejected loudly.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 120 2>/dev/null

BIN=${MADC_BIN:-bin/madc}
[ -x "$BIN" ] || { echo "capabilities_json_gate: missing $BIN"; exit 1; }

fail() { echo "capabilities_json_gate: $1"; exit 1; }

D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT

# 1. The manifest itself — exit 0, no stderr, valid JSON.
if ! timeout 60 "$BIN" --capabilities=json >"$D/manifest.json" 2>"$D/stderr"; then
    echo "capabilities_json_gate: --capabilities=json failed"; cat "$D/stderr"; exit 1
fi
[ ! -s "$D/stderr" ] || { echo "capabilities_json_gate: unexpected stderr"; cat "$D/stderr"; exit 1; }

# The emit-target macro, straight from the owning header (no re-listing here).
EMIT_MACRO=$(sed -n 's/.*#define CIR_EMIT_TARGETS "\([^"]*\)".*/\1/p' src/cir_emit_c.h)
[ -n "$EMIT_MACRO" ] || fail "could not read CIR_EMIT_TARGETS from src/cir_emit_c.h"

# 2. Structural + derived-content assertions. Emit the canonical standard list
#    on stdout so the shell can drive the recognizer-acceptance loop below.
python3 - "$D/manifest.json" VERSION "$EMIT_MACRO" >"$D/std_names" <<'PY'
import json, pathlib, sys
m = json.loads(pathlib.Path(sys.argv[1]).read_text())
want_version = pathlib.Path(sys.argv[2]).read_text().strip()
emit_macro = sys.argv[3].split("|")

def die(msg):
    sys.stderr.write("capabilities_json_gate: " + msg + "\n"); sys.exit(1)

if m.get("schema") != 1: die("schema is not 1")
if m["compiler"]["name"] != "madc": die("compiler.name != madc")
if m["compiler"]["version"] != want_version:
    die("compiler.version %r != VERSION %r" % (m["compiler"]["version"], want_version))
if not m["compiler"]["target"]: die("compiler.target empty")
if m["input"]["dialects"] != ["madc", "c", "c++"]: die("input.dialects wrong")
if m["input"]["project_manifest"] is not True: die("project_manifest not true")
if m["embedding"] != {"libmadc": True, "c_api": True}: die("embedding wrong")
if m["ir"] != {"frontend": "cir_node", "lowering": "c2mir", "backend": "MIR"}: die("ir wrong")
if not isinstance(m["execution"]["jit"], bool): die("execution.jit not a JSON bool")
if m["execution"]["aot"] is not True: die("execution.aot not true")

c = m["input"]["c_standards"]
cpp = m["input"]["cpp_standards"]

# The exact drift the derive-from-table refactor fixes: c95 was dropped.
if "c95" not in c: die("c95 missing from c_standards (the drift this gate guards)")
for s in ("c17", "c23"):
    if s not in c: die("%s missing from c_standards" % s)
for s in ("c++20", "c++26"):
    if s not in cpp: die("%s missing from cpp_standards" % s)

# The canonical set is exactly 10 C + 8 C++ (declared count — a new standard is
# a deliberate bump of these numbers alongside the table row).
if len(c) != 10: die("expected 10 canonical c_standards, got %d: %r" % (len(c), c))
if len(cpp) != 8: die("expected 8 canonical cpp_standards, got %d: %r" % (len(cpp), cpp))

# Negative control: accepted-but-non-canonical aliases must NOT be advertised.
for alias in ("c90", "c"):
    if alias in c: die("alias %r leaked into c_standards" % alias)
for alias in ("c++", "cpp", "cpp20"):
    if alias in cpp: die("alias %r leaked into cpp_standards" % alias)
if "madc" in c or "madc" in cpp: die("madc (a dialect) leaked into a standards list")

# emit_targets is exactly CIR_EMIT_TARGETS, in order.
if m["emit_targets"] != emit_macro:
    die("emit_targets %r != CIR_EMIT_TARGETS %r" % (m["emit_targets"], emit_macro))

for s in ("object", "executable", "shared", "relocatable"):
    if s not in m["native_outputs"]: die("%s missing from native_outputs" % s)
if "show-stats" not in m["introspection"]: die("show-stats missing from introspection")

# Hand the canonical standards to the shell for the recognizer loop.
print("\n".join(c + cpp))
PY
[ $? -eq 0 ] || exit 1

# 3. manifest ⊆ recognizer: every advertised standard must be ACCEPTED by
#    --std=. A trivial std-agnostic program compiles under all of them; a bogus
#    standard is rejected (rc 1) before compilation. This is what ties the
#    manifest to the compiler — neither can advertise what the other refuses.
mkdir -p tmp
echo 'int main(){return 0;}' >"$D/trivial.mad"
while read -r std; do
    [ -n "$std" ] || continue
    if ! timeout 60 "$BIN" "--std=$std" "$D/trivial.mad" >/dev/null 2>"$D/std.err"; then
        echo "capabilities_json_gate: manifest advertises --std=$std but the compiler rejects it"
        cat "$D/std.err"; exit 1
    fi
done <"$D/std_names"

# The recognizer really does reject the unknown — proves the loop above is a
# live check, not a rubber stamp.
if timeout 60 "$BIN" "--std=nope-not-a-standard" "$D/trivial.mad" >/dev/null 2>&1; then
    fail "the recognizer accepted a bogus --std= (acceptance loop is not meaningful)"
fi

# The recognizer refactor (strcmp chain -> one table) must keep every ACCEPTED
# alias the manifest deliberately does NOT advertise, plus the gnu-prefix
# transform that maps onto a base standard. A dropped spelling here is the
# exact regression a table rewrite risks.
for alias in c90 c c++ cpp cpp98 cpp20 cpp26 gnu17 gnu++17; do
    if ! timeout 60 "$BIN" "--std=$alias" "$D/trivial.mad" >/dev/null 2>"$D/alias.err"; then
        echo "capabilities_json_gate: recognizer no longer accepts alias --std=$alias (refactor dropped a spelling)"
        cat "$D/alias.err"; exit 1
    fi
done

# 4. Unknown capabilities format is rejected loudly.
if timeout 60 "$BIN" --capabilities=xml >"$D/unknown" 2>"$D/unknown.err"; then
    fail "unsupported --capabilities format was accepted"
fi
grep -q "Unknown capabilities format" "$D/unknown.err" \
    || { echo "capabilities_json_gate: unsupported-format diagnostic missing"; cat "$D/unknown.err"; exit 1; }

echo "capabilities_json_gate: OK — schema 1 manifest; standards+emit_targets derived from their owners; negative controls green"
