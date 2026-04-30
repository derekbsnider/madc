#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <command> [args...]" >&2
    echo "example: $0 bin/madc tests/testint.mad" >&2
    echo "example: $0 bash scripts/run_tests.sh tests/testint.mad" >&2
    exit 1
fi

make -C "$ROOT/src"

cd "$ROOT"
exec "$@"
