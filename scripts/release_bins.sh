#!/bin/bash
# release_bins.sh — the archive of every released madc-release, and the
# regression matrix that runs tests across it.
#
# Owner (2026-08-09, 2026-10-02): keep EVERY released madc-release binary, so
# releases can be timed against each other and a failing test can be run
# against the past releases to find the last one where it passed.
#
# One self-contained directory per release. Since v0.98.0 bin/madc-release is
# a thin driver over libmadc.so.0 (RUNPATH $ORIGIN/../lib/release:$ORIGIN/../lib),
# so an entry carries its own lib/ and runs from where it sits; a bare copy of
# the driver binds whatever libmadc the host has, or none:
#   tmp/release-bins/vX.Y.Z/bin/madc-release
#   tmp/release-bins/vX.Y.Z/lib/            libmadc.so.0 & co. (thin drivers)
#   tmp/release-bins/vX.Y.Z/PROVENANCE      version, commit, source, libmadc, when
#   tmp/release-bins/vX.Y.Z/MISSING         a release that cannot be recovered:
#                                           one line saying why (a formal skip)
#
# The release list is git's: the tag vX.Y.Z when it exists, otherwise the
# newest commit whose subject begins "Release vX.Y.Z " (several releases were
# never tagged). The archive starts at FIRST, the first archived release.
#
#   release_bins.sh list                  every release and its archive state
#   release_bins.sh check                 every release has an entry (no exec;
#                                         the master push gate runs this,
#                                         after its selftest)
#   release_bins.sh selftest              the check's negative control: an
#                                         archive missing a release fails
#   release_bins.sh archive [DIR]         archive DIR's (default: this tree's)
#                                         built madc-release; DIR's VERSION
#                                         names the release, and DIR must hold
#                                         that release's content (HEAD at its
#                                         commit, nothing modified under src/
#                                         include/ third_party/)
#   release_bins.sh backfill [vX.Y.Z...]  recover missing entries: the shipped
#                                         Linux tarball in dist/ when present,
#                                         else build the release commit (build
#                                         host only)
#   release_bins.sh verify                run each entry's --version and check
#                                         its libmadc binds inside the entry
#   release_bins.sh migrate               convert flat madc-release-vX.Y.Z
#                                         files to entries; drop thin-driver
#                                         stubs that cannot run
#   release_bins.sh sync                  (NAS) mirror the build container's
#                                         entries here (never overwrites)
#   release_bins.sh run [--since vX.Y.Z] [--last N] [--head] <test>...
#                                         the regression matrix: each named
#                                         test (run_tests.sh globs) on each
#                                         archived release, JIT; --head adds
#                                         this tree's bin/madc-release
#
# The archive lives on the build container, where binaries run (QNAP law: the
# NAS builds and runs nothing); the NAS checkout mirrors it with `sync`.
# list/check are file operations and run anywhere.
set -u
cd "$(dirname "$0")/.." || exit 9
ROOT=$(pwd)
ARCH="${MADC_RELEASE_BINS_DIR:-$ROOT/tmp/release-bins}"
FIRST=v0.72.0
REMOTE="${MADC_RELEASE_BINS_REMOTE:-dev@localhost}"
REMOTE_PORT="${MADC_RELEASE_BINS_PORT:-2299}"
REMOTE_ROOT="${MADC_RELEASE_BINS_REMOTE_ROOT:-/workspace/madc}"

die() { echo "release_bins: $*" >&2; exit 2; }

on_nas() { case "$(uname -r)" in *qnap*) return 0;; esac; return 1; }

need_build_host() {
	on_nas && die "$1 runs binaries; run it on the build container (scripts/remote_build.sh, or ssh -p $REMOTE_PORT $REMOTE)"
	return 0
}

ver_ge() {	# ver_ge <a> <b>: a >= b, both vX.Y.Z
	[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$2" ]
}

# releases: "vX.Y.Z <commit>" per release >= FIRST, ascending. A tag wins;
# otherwise the newest "Release vX.Y.Z " commit on any ref. A candidate is a
# madc release only when its commit's VERSION file says so — the MIR subtree
# history brings its own vX.Y.Z tags and release commits along.
releases() {
	{
		git -C "$ROOT" log --all --format='%H %s' |
			sed -n 's/^\([0-9a-f]*\) Release \(v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)\( .*\)\{0,1\}$/\2 \1 log/p'
		git -C "$ROOT" tag --list 'v*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' |
			while read -r t; do echo "$t $(git -C "$ROOT" rev-parse "$t^{commit}") tag"; done
	} | awk '
		$3 == "tag" { tag[$1] = $2; next }
		!($1 in rl) { rl[$1] = $2 }
		END { for (v in rl) if (!(v in tag)) print v, rl[v]; for (v in tag) print v, tag[v] }' |
		sort -V | while read -r v c; do
			ver_ge "$v" "$FIRST" || continue
			[ "$(git -C "$ROOT" show "$c:VERSION" 2>/dev/null)" = "${v#v}" ] && echo "$v $c"
		done
}

release_commit() {	# release_commit <vX.Y.Z>
	releases | awk -v v="$1" '$1 == v { print $2 }'
}

# state <vX.Y.Z>: ok | skip | legacy | stub | none
state() {
	local e="$ARCH/$1"
	if [ -f "$e/MISSING" ]; then echo skip
	elif [ -x "$e/bin/madc-release" ] && [ -f "$e/PROVENANCE" ]; then
		if grep -q '^libmadc: bundled' "$e/PROVENANCE" && [ ! -e "$e/lib/libmadc.so.0" ]; then echo stub
		else echo ok; fi
	elif [ -f "$ARCH/madc-release-$1" ] || [ -f "$e/madc-release" ]; then echo legacy
	else echo none; fi
}

# libmadc_kind <binary>: bundled (NEEDED libmadc.so.0) | static
libmadc_kind() {
	if readelf -d "$1" 2>/dev/null | grep -q 'NEEDED.*\[libmadc\.so'; then echo bundled; else echo static; fi
}

# put_entry <v> <commit> <source line> <bin> <libdir or ""> — stage, check, publish
put_entry() {
	local v="$1" c="$2" how="$3" bin="$4" libdir="$5"
	local e="$ARCH/$v" stage="$ARCH/.stage-$v" kind
	if [ "$(state "$v")" = ok ] && [ "${MADC_RELEASE_BINS_FORCE:-}" != 1 ]; then
		echo "release_bins: $v already archived (releases are immutable; MADC_RELEASE_BINS_FORCE=1 replaces)"; return 0
	fi
	rm -rf "$stage"; mkdir -p "$stage/bin" || return 1
	cp -p "$bin" "$stage/bin/madc-release" || return 1
	kind=$(libmadc_kind "$stage/bin/madc-release")
	if [ "$kind" = bundled ]; then
		[ -n "$libdir" ] && [ -e "$libdir/libmadc.so.0" ] || { echo "release_bins: $v: thin driver but no libmadc.so.0 beside it ($libdir)" >&2; rm -rf "$stage"; return 1; }
		mkdir -p "$stage/lib"
		cp -a "$libdir/." "$stage/lib/" || return 1
	fi
	{
		echo "version: $v"
		echo "commit: $c"
		echo "source: $how"
		echo "libmadc: $kind"
		echo "archived: $(date -u +%Y-%m-%dT%H:%MZ) on $(hostname)"
	} > "$stage/PROVENANCE"
	if ! on_nas; then
		entry_runs "$stage" "$v" || { rm -rf "$stage"; return 1; }
	fi
	rm -rf "$e"; mv "$stage" "$e" || return 1
	rm -f "$ARCH/madc-release-$v"
	echo "release_bins: archived $v ($kind libmadc; $how)"
}

# entry_runs <entry dir> <vX.Y.Z>: the binary is the release (its --version,
# or — before v0.84.0 had --version — the version string it carries) and runs
# a program; a bundled libmadc is the one the loader binds
entry_runs() {
	local e="$1" v="$2" bin="$1/bin/madc-release" out probe="$ROOT/tmp/rbprobe/p.mad" rc
	out=$(env -u LD_LIBRARY_PATH "$bin" --version 2>&1 | head -1)
	case "$out" in
		"madc ${v#v}") ;;
		"madc "*) echo "release_bins: $v: --version says '$out'" >&2; return 1;;
		*) strings "$bin" | grep -qx "${v#v}" ||
			{ echo "release_bins: $v: no --version and no '${v#v}' string in the binary" >&2; return 1; };;
	esac
	mkdir -p "${probe%/*}"; printf 'int main() { return 7; }\n' > "$probe"
	env -u LD_LIBRARY_PATH timeout 30 "$bin" "$probe" > /dev/null 2>&1; rc=$?
	[ $rc -eq 7 ] || { echo "release_bins: $v: a 'return 7' program exits $rc" >&2; return 1; }
	if [ "$(libmadc_kind "$bin")" = bundled ]; then
		local real bound
		real=$(cd "$e/lib" && pwd -P)
		bound=$(env -u LD_LIBRARY_PATH ldd "$bin" | sed -n 's/^[[:space:]]*libmadc\.so[^ ]* => \([^ ]*\) .*/\1/p' | head -1)
		[ -n "$bound" ] && [ "$(dirname "$(readlink -f "$bound")")" = "$real" ] ||
			{ echo "release_bins: $v: libmadc binds '$bound', not the entry's lib/" >&2; return 1; }
	fi
	return 0
}

cmd_list() {
	local v c
	releases | while read -r v c; do printf '%-10s %s  %s\n' "$v" "${c:0:9}" "$(state "$v")"; done
}

cmd_check() {
	[ -d "$ARCH" ] || { echo "release_bins: no $ARCH on this host — nothing to check (a fresh clone)"; return 0; }
	local v c s bad=0
	while read -r v c; do
		s=$(state "$v")
		case "$s" in
			ok|skip) ;;
			*) echo "release_bins: $v is $s — run: scripts/release_bins.sh backfill $v (build container), then sync" >&2; bad=$((bad+1));;
		esac
	done < <(releases)
	[ $bad -eq 0 ] && echo "release_bins: every release since $FIRST is archived"
	[ $bad -eq 0 ]
}

# The negative control: the check over an archive that lacks the newest
# release MUST fail, and the same archive with it MUST pass. A gate that
# cannot fail is not a gate.
cmd_selftest() {
	local t v c newest
	newest=$(releases | tail -1)
	[ -n "$newest" ] || die "selftest: no releases found"
	t=$(mktemp -d)
	while read -r v c; do
		mkdir -p "$t/$v"; echo "selftest skip" > "$t/$v/MISSING"
	done < <(releases)
	v=${newest%% *}
	rm -rf "${t:?}/$v"
	if MADC_RELEASE_BINS_DIR="$t" bash "$0" check > /dev/null 2>&1; then
		rm -rf "$t"; echo "release_bins: SELFTEST FAILED — an archive missing $v passed" >&2; return 1
	fi
	mkdir -p "$t/$v/bin"; printf '#!/bin/sh\n' > "$t/$v/bin/madc-release"; chmod +x "$t/$v/bin/madc-release"
	printf 'version: %s\nlibmadc: static\n' "$v" > "$t/$v/PROVENANCE"
	if ! MADC_RELEASE_BINS_DIR="$t" bash "$0" check > /dev/null 2>&1; then
		rm -rf "$t"; echo "release_bins: SELFTEST FAILED — a complete archive failed" >&2; return 1
	fi
	rm -rf "$t"
	echo "release_bins: selftest OK (a missing release blocks, a complete archive passes)"
}

cmd_archive() {
	need_build_host archive
	local dir="${1:-$ROOT}" v c head
	dir=$(cd "$dir" && pwd) || die "no such tree: $1"
	v="v$(cat "$dir/VERSION")"
	c=$(release_commit "$v")
	[ -n "$c" ] || die "$v has no release commit (tag $v, or a 'Release $v ' commit)"
	head=$(git -C "$dir" rev-parse HEAD 2>/dev/null)
	if [ "$head" != "$c" ]; then
		# The release content, not its commit hash, is what an entry must hold:
		# the code paths at HEAD must be the release commit's.
		git -C "$dir" diff --quiet "$c" HEAD -- src include third_party 2>/dev/null ||
			die "$dir HEAD ${head:0:9} differs from release $v (${c:0:9}) under src/ include/ third_party/ — not the release's content"
	fi
	git -C "$dir" diff --quiet HEAD -- src include third_party 2>/dev/null ||
		die "$dir has uncommitted changes under src/ include/ third_party/ — not the release's content"
	[ -x "$dir/bin/madc-release" ] || die "$dir/bin/madc-release is not built (make -C src release)"
	put_entry "$v" "$c" "the release tree $dir (HEAD ${head:0:9})" "$dir/bin/madc-release" "$dir/lib/release"
}

# recover <vX.Y.Z> <commit>: the shipped tarball, else a build of the commit.
# The build is the dev build THEN release, as every release tree was built:
# through v0.100.1 the release link resolved -lmadc in lib/, where only a
# dev build leaves a libmadc.so (src/Makefile CLI_LINK).
recover() {
	local v="$1" c="$2" tgz="$ROOT/dist/madc-${1#v}-linux-x86_64.tar.gz" x b log
	if [ -f "$tgz" ]; then
		x="$ROOT/tmp/relbuild/$v-tgz"; rm -rf "$x"; mkdir -p "$x"
		tar -xzf "$tgz" -C "$x" || return 1
		local top; top=$(ls -d "$x"/*/ | head -1)
		put_entry "$v" "$c" "the shipped tarball dist/$(basename "$tgz") (sha256 $(sha256sum "$tgz" | cut -c1-16)…)" \
			"$top/bin/madc" "$top/lib"
		local rc=$?; rm -rf "$x"; return $rc
	fi
	b="$ROOT/tmp/relbuild/$v"; log="$ROOT/tmp/relbuild/$v.log"
	rm -rf "$b"; mkdir -p "$b"
	echo "release_bins: building $v from ${c:0:9} (log: tmp/relbuild/$v.log)"
	{
		git -C "$ROOT" archive --format=tar "$c" | tar -x -C "$b" &&
		( cd "$b" && autoheader && autoconf && ./configure ) &&
		( ulimit -t 14400; timeout 5400 make -C "$b/src" -j"${MADC_RELBUILD_JOBS:-8}" ) &&
		( ulimit -t 14400; timeout 5400 make -C "$b/src" -j"${MADC_RELBUILD_JOBS:-8}" release )
	} > "$log" 2>&1 || { echo "release_bins: $v: build failed — tail of $log:" >&2; tail -5 "$log" >&2; rm -rf "$b"; return 1; }
	put_entry "$v" "$c" "built from the release commit (git archive ${c:0:9}; autoheader; autoconf; ./configure; make -C src; make -C src release)" \
		"$b/bin/madc-release" "$b/lib/release" || return 1
	rm -rf "$b"
}

cmd_backfill() {
	need_build_host backfill
	cmd_migrate
	local v c want=" $* " bad=0
	while read -r v c; do
		[ $# -eq 0 ] || [[ "$want" == *" $v "* ]] || continue
		case "$(state "$v")" in ok|skip) continue;; esac
		recover "$v" "$c" || bad=$((bad+1))
	done < <(releases)
	[ $bad -eq 0 ]
}

cmd_verify() {
	need_build_host verify
	local v c bad=0
	while read -r v c; do
		case "$(state "$v")" in
			ok) entry_runs "$ARCH/$v" "$v" && echo "ok   $v" || bad=$((bad+1));;
			skip) echo "skip $v: $(head -1 "$ARCH/$v/MISSING")";;
			*) echo "MISS $v ($(state "$v"))"; bad=$((bad+1));;
		esac
	done < <(releases)
	[ $bad -eq 0 ]
}

# A flat madc-release-vX.Y.Z (and v0.92.0/madc-release, an older dir form)
# becomes an entry when it is self-contained (static libmadc) and proves to be
# the release it is named for; a thin-driver copy cannot run without the
# libmadc it was built with, so it is dropped and `backfill` rebuilds the
# release. Entries are made only where binaries run (the build container):
# on the NAS, migrate only removes flat copies whose entry has arrived.
cmd_migrate() {
	[ -d "$ARCH" ] || mkdir -p "$ARCH"
	local f v c src kind
	for f in "$ARCH"/madc-release-v* "$ARCH"/.legacy-v* "$ARCH"/v*/madc-release; do
		[ -f "$f" ] || continue
		case "$f" in
			*/madc-release-v*) v=${f##*/madc-release-};;
			*/.legacy-v*) v=${f##*/.legacy-};;	# an interrupted migrate
			*) v=$(basename "$(dirname "$f")");;
		esac
		[ "$(state "$v")" = ok ] && { rm -f "$f"; continue; }
		on_nas && continue
		kind=$(libmadc_kind "$f")
		if [ "$kind" = bundled ]; then
			echo "release_bins: $v: dropping $(basename "$f") — a thin driver archived without its libmadc"
			rm -f "$f"; continue
		fi
		c=$(release_commit "$v")
		src="$ARCH/.legacy-$v"; [ "$f" = "$src" ] || mv "$f" "$src"
		if put_entry "$v" "${c:-unknown}" "the legacy flat archive (self-contained)" "$src" ""; then
			rm -f "$src"
		else
			mv "$src" "$ARCH/madc-release-$v"	# kept as found; never lost
		fi
	done
	rmdir "$ARCH"/v*/ 2>/dev/null
	return 0
}

# The build container holds the archive (entries are verified where they
# run); the NAS checkout mirrors its entries. Entries are immutable, so the
# copy never replaces one (--ignore-existing) and carries only entry dirs.
cmd_sync() {
	on_nas || die "sync runs on the NAS checkout (it pulls the container's archive over ssh -p $REMOTE_PORT)"
	ssh -p "$REMOTE_PORT" -o BatchMode=yes "$REMOTE" "bash $REMOTE_ROOT/scripts/release_bins.sh migrate" || return 1
	mkdir -p "$ARCH"
	# --no-perms/--no-owner/--no-group: the NAS share refuses chmod (ACLs) —
	# remote_build.sh's pull convention; new files keep the source's mode
	# masked by the share's defaults, so the binaries stay executable.
	rsync -az --no-perms --no-owner --no-group --ignore-existing \
		--include='/v*/***' --exclude='*' -e "ssh -p $REMOTE_PORT" \
		"$REMOTE:$REMOTE_ROOT/tmp/release-bins/" "$ARCH/" || return 1
	cmd_migrate
	cmd_check
}

cmd_run() {
	need_build_host run
	local since="$FIRST" last=0 head=0 v c
	while [ $# -gt 0 ]; do
		case "$1" in
			--since) since="$2"; shift 2;;
			--last) last="$2"; shift 2;;
			--head) head=1; shift;;
			*) break;;
		esac
	done
	[ $# -gt 0 ] || die "run needs at least one test name or glob"
	local -a bins=() labels=()
	while read -r v c; do
		ver_ge "$v" "$since" && [ "$(state "$v")" = ok ] && { labels+=("$v"); bins+=("$ARCH/$v/bin/madc-release"); }
	done < <(releases)
	if [ "$last" -gt 0 ] && [ ${#labels[@]} -gt "$last" ]; then
		labels=("${labels[@]: -$last}"); bins=("${bins[@]: -$last}")
	fi
	[ $head -eq 1 ] && { labels+=("HEAD"); bins+=("$ROOT/bin/madc-release"); }
	[ ${#labels[@]} -gt 0 ] || die "no archived release to run"
	local tmpd="$ROOT/tmp/release-bins-run"; rm -rf "$tmpd"; mkdir -p "$tmpd"
	local i
	for i in "${!labels[@]}"; do
		# env -u: the run must bind the entry's own libmadc, never the tree's
		env -u LD_LIBRARY_PATH MADC_BIN="${bins[$i]}" bash scripts/run_tests.sh --report=json "$@" \
			> "$tmpd/${labels[$i]}.json" 2>/dev/null
	done
	# matrix: one row per test, one column per release; P pass, F fail,
	# T timeout, S skip
	local tests; tests=$(cat "$tmpd"/*.json | sed -n 's/^{"test":"\([^"]*\)","family":"mad","result".*/\1/p' | sort -u)
	printf '%-36s' test; for v in "${labels[@]}"; do printf ' %-9s' "$v"; done; echo
	local t r
	for t in $tests; do
		printf '%-36s' "$t"
		for v in "${labels[@]}"; do
			r=$(sed -n "s/^{\"test\":\"$t\",\"family\":\"mad\",\"result\":\"\([a-z]*\)\".*/\1/p" "$tmpd/$v.json" | head -1)
			case "$r" in pass) r=P;; fail) r=F;; timeout) r=T;; skip) r=S;; *) r=-;; esac
			printf ' %-9s' "$r"
		done
		echo
	done
	rm -rf "$tmpd"
}

case "${1:-}" in
	list) shift; cmd_list "$@";;
	check) shift; cmd_check "$@";;
	selftest) shift; cmd_selftest "$@";;
	archive) shift; cmd_archive "$@";;
	backfill) shift; cmd_backfill "$@";;
	verify) shift; cmd_verify "$@";;
	migrate) shift; cmd_migrate "$@";;
	sync) shift; cmd_sync "$@";;
	run) shift; cmd_run "$@";;
	*) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2;;
esac
