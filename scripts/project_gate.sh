#!/usr/bin/env bash
# Multi-TU --project gate: a heterogeneous C++ project (each TU includes a
# DIFFERENT header set) compiles, links, and runs with output identical to
# g++ building the same sources.
#
# Guards two --project-specific regressions:
#   1. Per-TU pool activation (Program::activate_token_pools): the interned
#      spelling / value / type pools are process-global active-owner statics,
#      and madc_project_execute parses every TU before building any tree.
#      Without re-activation at build, every TU but the last resolves its
#      spelling ids against the WRONG pool — heterogeneous TUs (different
#      headers => different intern order) produce garbage identifiers.
#      Near-identical TUs (SMAUG) false-green this; heterogeneity is the test.
#   2. ODR duplicates across TU modules (MIR_set_func_redef_permission):
#      two TUs both instantiate map<string,int>, so both modules export the
#      same instantiation symbols; MIR_link must tolerate the redefinition.
#
# Run from the repo root (fulltest does). Fixtures are generated into tmp/
# (gitignored), like scripts/forest_bind_gate.sh — the gate is self-contained.
set -u
cd "$(dirname "$0")/.."

ulimit -t 300 2>/dev/null

BIN=bin/madc
if [ ! -x "$BIN" ]; then
    echo "project_gate: missing $BIN"
    exit 1
fi

mkdir -p tmp

fail() { echo "project_gate: $1"; exit 1; }

cat > tmp/pjgate_main.cpp <<'EOF'
#include <cstdio>
int parta_run();
int partb_run();
int main()
{
	printf("a=%d\n", parta_run());
	printf("b=%d\n", partb_run());
	return 0;
}
EOF

cat > tmp/pjgate_parta.cpp <<'EOF'
#include <map>
#include <string>
int parta_run()
{
	std::map<std::string, int> counts;
	counts["alpha"] = 1;
	counts["beta"] = 2;
	counts["beta"] += 10;
	int total = 0;
	for (std::map<std::string, int>::iterator it = counts.begin();
	     it != counts.end(); ++it)
		total += it->second;
	return total;
}
EOF

cat > tmp/pjgate_partb.cpp <<'EOF'
#include <vector>
#include <map>
#include <string>
int partb_run()
{
	std::vector<std::string> words;
	words.push_back("one");
	words.push_back("three");
	std::map<std::string, int> lens;
	for (size_t i = 0; i < words.size(); i++)
		lens[words[i]] = (int)words[i].size();
	int total = 0;
	for (std::map<std::string, int>::iterator it = lens.begin();
	     it != lens.end(); ++it)
		total += it->second;
	return total + (int)words.size();
}
EOF

here=$(pwd)
cat > tmp/pjgate_cc.json <<EOF
[
  { "directory": "$here/tmp", "file": "pjgate_main.cpp",  "arguments": ["c++", "-c", "pjgate_main.cpp"] },
  { "directory": "$here/tmp", "file": "pjgate_parta.cpp", "arguments": ["c++", "-c", "pjgate_parta.cpp"] },
  { "directory": "$here/tmp", "file": "pjgate_partb.cpp", "arguments": ["c++", "-c", "pjgate_partb.cpp"] }
]
EOF

if ! g++ -O0 -o tmp/pjgate_gcc tmp/pjgate_main.cpp tmp/pjgate_parta.cpp \
	tmp/pjgate_partb.cpp 2> tmp/pjgate_gcc.log; then
    fail "g++ reference build FAILED (see tmp/pjgate_gcc.log)"
fi
tmp/pjgate_gcc > tmp/pjgate_ref.out || fail "g++ reference run FAILED"

if ! timeout 300 "$BIN" --project tmp/pjgate_cc.json \
	> tmp/pjgate_madc.out 2> tmp/pjgate_madc.err; then
    echo "--- stderr ---"; head -5 tmp/pjgate_madc.err
    fail "--project build/run FAILED"
fi

if ! diff -u tmp/pjgate_ref.out tmp/pjgate_madc.out > tmp/pjgate_diff.log; then
    cat tmp/pjgate_diff.log
    fail "output differs from g++"
fi

echo "project_gate: GREEN — heterogeneous 3-TU --project output == g++ (pool activation + ODR redef both exercised)"
