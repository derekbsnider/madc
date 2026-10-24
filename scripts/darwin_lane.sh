#!/bin/bash
# darwin_lane.sh — the darwin release lanes ON a Mac (the arm64 Mini, the Intel
# MacBook), run from a ~/madc tree that scripts/mac_release.sh syncs in.
# It runs .github/workflows/darwin-probe.yml's release-gate recipe:
# - fetch the pinned input trees, configure, and stage the module deps;
# - make release-macos (self-frozen, verified), then a hello;
# - the Mac battery on the staged driver;
# - the FULL suite on the -O2 madc-release-<arch>-macos (the darwin-suite
#   lane), with the darwin domains and the advisory EXE pass.
#
#   bash ~/madc/scripts/darwin_lane.sh <arm64|x86-64> [env-file]
#   LANE_PREGENERATED=1  configure + include/config.h.in were shipped in (a
#                        host whose autoconf is broken and may not be upgraded:
#                        the MacBook's 2.72 keg points at a retired perl)
#   LANE_PACKAGE=1       the owner's test artifact instead of the suite: this
#                        arch's tarball, built natively against the pinned zstd
set -u
ARCH="$1"
ENVF="${2:-}"
[ -n "$ENVF" ] && . "$ENVF"
cd "$HOME/madc" || exit 2
mkdir -p tmp bin obj lib dist
step() { echo "=== $1 ($(date -u +%T))"; }

step inventory
sw_vers -productVersion; uname -m; sysctl -n hw.ncpu
make --version | head -1; bash --version | head -1; command -v timeout sed grep

step fetch
bash scripts/fetch_darwin_open_headers.sh > tmp/fetch-darwin.log 2>&1 || { tail -8 tmp/fetch-darwin.log; exit 3; }
bash scripts/fetch_libcxx_headers.sh > tmp/fetch-libcxx.log 2>&1 || { tail -8 tmp/fetch-libcxx.log; exit 3; }
cat "$DARWIN_OPEN_HEADERS_HOME/sysroot-any-darwin-any/.PROVENANCE" "$LIBCXX_HEADERS_HOME/.PROVENANCE"

step configure
if [ "${LANE_PREGENERATED:-}" = 1 ]; then
	echo "autoreconf skipped: pregenerated $(grep -m1 '^PACKAGE_VERSION=' configure)"
else
	autoreconf -i > tmp/autoreconf.log 2>&1 || { tail -8 tmp/autoreconf.log; exit 4; }
fi
./configure --enable-madcdat=no \
	CPPFLAGS="-I$(brew --prefix)/include" LDFLAGS="-L$(brew --prefix)/lib" \
	> tmp/configure.log 2>&1 || { tail -20 tmp/configure.log; exit 4; }

step stage-deps
bash scripts/stage_libgit2.sh "$ARCH-macos" > tmp/libgit2.log 2>&1 || { tail -8 tmp/libgit2.log; exit 5; }
bash scripts/stage_cmark_gfm.sh "$ARCH-macos" > tmp/cmark.log 2>&1 || { tail -8 tmp/cmark.log; exit 5; }
# The pinned static PCRE2 the hosted MODE links (darwin-probe.yml's stage step;
# src/Makefile's PCRE2_TAG) — without it the build falls back to brew's.
export PCRE2_DIR="$HOME/pcre2"
bash scripts/stage_pcre2.sh "$ARCH-macos" > tmp/pcre2.log 2>&1 || { tail -8 tmp/pcre2.log; exit 5; }
ls "$(bash scripts/stage_pcre2.sh --path "$ARCH-macos")"
# The shipped artifact links the PINNED zstd stage (release.yml's mac job),
# not brew's; a variable change is no file dependency, so the hosted mode
# rebuilds from clean against it.
if [ "${LANE_PACKAGE:-}" = 1 ]; then
	export DARWIN_ZSTD_DIR="$HOME/zstd"
	bash scripts/stage_darwin_zstd.sh "$ARCH" > tmp/zstd.log 2>&1 || { tail -8 tmp/zstd.log; exit 5; }
	echo "DARWIN_ZSTD_LIB = $(make -C src -s MODE=hosted-$ARCH-macos print-DARWIN_ZSTD_LIB)"
	rm -rf obj/hosted-"$ARCH"-macos bin/madc-release-"$ARCH"-macos bin/madc-hosted-"$ARCH"-macos
fi

step release-macos
make -C src -j"$(sysctl -n hw.ncpu)" release-macos > tmp/release-make.log 2>&1
rc=$?
grep -E "forest_pack_gate:|forest_pack_darwin: OK|verify_macho_release:|Froze " tmp/release-make.log
[ "$rc" -eq 0 ] || { tail -30 tmp/release-make.log; echo "release-macos rc=$rc"; exit 6; }

step hello
printf 'var g = "darwin-host-gate-alive";\nprintln("{}", g);\n' > tmp/gate.mad
out=$(bin/madc-release-"$ARCH"-macos tmp/gate.mad)
echo "madc said: $out"
[ "$out" = "darwin-host-gate-alive" ] || exit 7
bin/madc-release-"$ARCH"-macos --version | head -1

if [ "${LANE_PACKAGE:-}" = 1 ]; then
	step package
	bash scripts/package_release_macos.sh "$ARCH" > tmp/package-macos.log 2>&1
	rc=$?
	tail -15 tmp/package-macos.log
	ls -l dist/
	step done
	exit "$rc"
fi

step battery
rm -rf tmp/stage; mkdir -p tmp/stage/bin tmp/stage/lib
cp bin/madc-release-"$ARCH"-macos tmp/stage/bin/madc
cp obj/hosted-"$ARCH"-macos/libmadc-0.dylib tmp/stage/lib/libmadc-0.dylib
cp lib/libmadc_rt-hosted-"$ARCH"-macos.a tmp/stage/lib/libmadc_rt.a
bash scripts/mac_battery.sh tmp/stage/bin/madc > tmp/mac-battery.log 2>&1
grep -E '^FAIL|^== battery' tmp/mac-battery.log

step suite
MADC_BIN=bin/madc-release-"$ARCH"-macos \
MADC_SKIP_EXT="darwin libcxx darwin-$ARCH macos-$(sw_vers -productVersion | cut -d. -f1)" \
MADC_EXE_FLAGS=-static-libmadc MADC_FAIL_DETAIL=12 \
MADC_EXE_ADVISORY="darwin exe lane is advisory until the runtime dylib ships (D5): -static-libmadc cannot cover the dialect runtime (Tier B)" \
	bash scripts/run_tests.sh --exe > tmp/suite.log 2>&1
rc=$?
grep -E '^(FAIL|TIMEOUT)' tmp/suite.log | sort > tmp/suite-failures.txt
tail -4 tmp/suite.log
echo "failure lines: $(wc -l < tmp/suite-failures.txt)   suite rc=$rc"
step done
exit "$rc"
