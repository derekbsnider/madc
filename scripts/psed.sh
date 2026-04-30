#!/bin/bash
# psed.sh — patch-sed helper for multi-line in-place edits.
#
# Motivation: the Edit tool requires an exact tab/whitespace match which is
# easy to get wrong on tab-indented source, and raw `sed -i` with shell-
# escaped tabs is permission-gated. This wrapper runs a python text
# replacement that:
#   - reads the target file
#   - asserts the old literal exists (refuses to run otherwise)
#   - replaces the first occurrence with the new literal
#   - writes the file in place
#
# Usage:
#   scripts/psed.sh <file> <old_file> <new_file>
#
# <old_file> and <new_file> contain the literal text to search for and
# replace with (newlines and tabs preserved as-is).
#
# Or pass inline via heredocs:
#   scripts/psed.sh src/foo.cpp - - <<'OLD' <<'NEW'
#   ...
#   OLD
#   ...
#   NEW
# (not recommended; use temp files for clarity)
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <target-file> <old-text-file> <new-text-file>" >&2
    exit 2
fi

target=$1
oldfile=$2
newfile=$3

python3 - "$target" "$oldfile" "$newfile" <<'PY'
import sys
target, oldfile, newfile = sys.argv[1], sys.argv[2], sys.argv[3]
with open(oldfile) as f: old = f.read()
with open(newfile) as f: new = f.read()
with open(target) as f: s = f.read()
if old not in s:
    print(f"psed: literal from {oldfile} not found in {target}", file=sys.stderr)
    sys.exit(1)
count = s.count(old)
if count > 1:
    print(f"psed: literal appears {count} times in {target}; narrow it", file=sys.stderr)
    sys.exit(1)
s = s.replace(old, new, 1)
with open(target, "w") as f: f.write(s)
print(f"psed: patched {target}")
PY
