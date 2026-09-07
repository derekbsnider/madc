#!/usr/bin/env bash
# capabilities_json_gate.sh — the CLI capability manifest is valid JSON,
# versioned, build-specific, and rejects formats it does not implement.
set -u
cd "$(dirname "$0")/.."

ulimit -t 120 2>/dev/null

BIN=${MADC_BIN:-bin/madc}
[ -x "$BIN" ] || { echo "capabilities_json_gate: missing $BIN"; exit 1; }

D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT

if ! timeout 60 "$BIN" --capabilities=json >"$D/manifest.json" 2>"$D/stderr"; then
    echo "capabilities_json_gate: --capabilities=json failed"
    cat "$D/stderr"
    exit 1
fi
[ ! -s "$D/stderr" ] || { echo "capabilities_json_gate: unexpected stderr"; cat "$D/stderr"; exit 1; }

python3 - "$D/manifest.json" VERSION <<'PY'
import json
import pathlib
import sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text())
version = pathlib.Path(sys.argv[2]).read_text().strip()
assert manifest["schema"] == 1
assert manifest["compiler"]["name"] == "madc"
assert manifest["compiler"]["version"] == version
assert manifest["compiler"]["target"]
assert manifest["input"]["dialects"] == ["madc", "c", "c++"]
assert "c17" in manifest["input"]["c_standards"]
assert "c++20" in manifest["input"]["cpp_standards"]
assert manifest["input"]["project_manifest"] is True
assert "c11" in manifest["emit_targets"]
assert "executable" in manifest["native_outputs"]
assert manifest["embedding"] == {"libmadc": True, "c_api": True}
assert manifest["ir"] == {"frontend": "cir_node", "lowering": "c2mir", "backend": "MIR"}
PY

if timeout 60 "$BIN" --capabilities=xml >"$D/unknown" 2>"$D/unknown.err"; then
    echo "capabilities_json_gate: unsupported format was accepted"
    exit 1
fi
grep -q "Unknown capabilities format" "$D/unknown.err" \
    || { echo "capabilities_json_gate: unsupported format diagnostic missing"; cat "$D/unknown.err"; exit 1; }

echo "capabilities_json_gate: OK — schema 1 manifest and negative format gate"
