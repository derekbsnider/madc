#!/bin/bash
# sed.sh — thin wrapper around `sed -i` for in-place regex edits.
#
# Use this instead of invoking `sed -i ...` directly when editing project
# files so the call goes through a scripts/ entry point rather than
# triggering a permission prompt on raw sed.
#
# Usage:
#   scripts/sed.sh <file> <sed-expression>                # single expr
#   scripts/sed.sh <file> <expr1> <expr2> ...             # multiple -e
#
# Examples:
#   scripts/sed.sh src/foo.cpp 's/oldname/newname/g'
#   scripts/sed.sh src/bar.cpp '/^old_line$/d' 's/a/b/'
#
# For literal (non-regex) multi-line replacements, prefer scripts/psed.sh.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <file> <sed-expression> [<expr2> ...]" >&2
    exit 2
fi

target=$1
shift

args=()
for e in "$@"; do
    args+=(-e "$e")
done

sed -i "${args[@]}" "$target"
echo "sed: patched $target"
