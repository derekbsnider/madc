#!/usr/bin/env bash
# Codex Cloud setup. Runs once before the agent session starts.
#
# Installs everything the madc build needs, most importantly asmjit
# v1.14 at /usr/local/ — NOT the apt libasmjit-dev package, which is
# an older incompatible version (the Makefile explicitly rejects it).
# See docs/build.md and .claude/rules/build.md for why.
#
# The agent reads AGENTS.md at the repo root for project rules and
# architecture once the environment is ready.

set -euo pipefail

# --- Build prerequisites -------------------------------------------------
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    make \
    cmake \
    git \
    ca-certificates

# --- asmjit v1.14 from source -------------------------------------------
# Install prefix /usr/local so the Makefile's -L/usr/local/lib picks it up.
# Skip if already present from a previous session (Codex Cloud caches).
if [ ! -f /usr/local/lib/libasmjit.so ]; then
    ASMJIT_SRC=$(mktemp -d)
    git clone --depth=1 https://github.com/asmjit/asmjit.git "$ASMJIT_SRC"
    mkdir -p "$ASMJIT_SRC/build"
    cd "$ASMJIT_SRC/build"
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DASMJIT_STATIC=FALSE
    make -j"$(nproc)"
    sudo make install
    sudo ldconfig
    cd -
    rm -rf "$ASMJIT_SRC"
fi

# --- Sanity build --------------------------------------------------------
# Confirm the project builds end-to-end before the agent starts working.
make -C src clean
make -C src
make -C src fulltest

echo
echo "Codex setup complete."
echo "  asmjit:      $(ls -l /usr/local/lib/libasmjit.so 2>/dev/null || echo 'MISSING')"
echo "  madc binary: $(ls -l bin/madc 2>/dev/null || echo 'MISSING')"
echo
echo "Read AGENTS.md at the repo root for rules and architecture."
