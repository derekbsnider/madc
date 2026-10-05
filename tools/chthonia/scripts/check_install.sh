#!/bin/bash
# check_install.sh — Chthonia installed into a madc installation, run as a
# user runs it (Linux, macOS).
#
#   scripts/check_install.sh PREFIX [LIBDIR]
#
#   PREFIX  the installation: a madc folder (madc's tarball) Chthonia's
#           tarball was unpacked into, or a package root's usr/
#   LIBDIR  madc's libraries, when the loader would not find them (packages
#           unpacked rather than installed); a madc folder needs none, its
#           programs find ../lib themselves
#
# Each probe runs from a directory of its own with no user configuration,
# and each control must fail:
#   1. chthonia prints its usage line;
#   2. `-c check` over a C file that includes <stdio.h> is clean: madc's
#      installed headers and engine serve it [control: a syntax error is 1
#      problem, exit status 1];
#   3. its bundle is the installed one: its menu bar has no Window menu
#      [control: the bundle hidden, chthonia runs the default profile, whose
#      Window menu is listed];
#   4. Tools ▸ Key bindings… is on its Tools menu and lists madcide's six key
#      styles [control: madcide's profiles hidden, it lists none];
#   5. on Linux, its desktop entry and its 256 px icon are installed.
set -u
. "$(dirname "$0")/common.sh"
if [ $# -lt 1 ] || [ ! -d "$1" ]; then
	echo "usage: $0 PREFIX [LIBDIR]" >&2
	exit 2
fi
prefix=$(cd "$1" && pwd)
chthonia=$prefix/bin/chthonia
bundle=$prefix/share/madcide/plugins/chthonia
profiles=$prefix/share/madcide/profiles
case "$(uname -s)" in
Darwin) libvar=DYLD_LIBRARY_PATH ;;
*) libvar=LD_LIBRARY_PATH ;;
esac
work=$(mktemp -d)
hidden=
restore() {
	[ -n "$hidden" ] && [ -e "$hidden.hidden" ] && mv "$hidden.hidden" "$hidden"
	rm -rf "$work"
}
trap restore EXIT
ok() { echo "check_install: ok — $1"; }
fail() { echo "check_install: FAIL — $1" >&2; exit 1; }
# run ARG... — chthonia from $work, no user configuration; output and status.
run() {
	local -a e=(env -u MADCIDE_PLUGIN_PATH "MADCIDE_CONFIG_DIR=$work/no-config")
	[ -n "$LIBDIR" ] && e+=("$libvar=$LIBDIR")
	(cd "$work" && capped 60 "${e[@]}" "$chthonia" "$@") 2>&1
}
hide() { hidden=$1; mv "$1" "$1.hidden"; }
unhide() { mv "$hidden.hidden" "$hidden"; hidden=; }
LIBDIR=${2:-}
[ -z "$LIBDIR" ] || LIBDIR=$(cd "$LIBDIR" && pwd)

[ -x "$chthonia" ] || fail "no executable $chthonia"
[ -f "$bundle/chthonia.plugin" ] || fail "no bundle in $bundle"

# 1. usage
out=$(run --help)
case "$out" in
*"usage: chthonia"*) ok "chthonia prints its usage line" ;;
*) fail "chthonia --help printed no usage line (got: $out)" ;;
esac

# 2. -c check
printf '#include <stdio.h>\nint main(void) { printf("%%d\\n", 5); return 0; }\n' > "$work/ok.c"
printf '#include <stdio.h>\nint main(void) { return 0 }\n' > "$work/bad.c"
out=$(run ok.c -c check)
rc=$?
case "$rc:$out" in
0:*Problems*) ok "-c check over <stdio.h> is clean (exit status 0)" ;;
*) fail "-c check over <stdio.h> was not clean (exit status $rc: $out)" ;;
esac
out=$(run bad.c -c check)
rc=$?
case "$rc:$out" in
1:*"1 problem"*) ok "control: a syntax error is 1 problem (exit status 1)" ;;
*) fail "control broken: -c check over a syntax error (exit status $rc: $out)" ;;
esac

# 3. the installed bundle
out=$(run ok.c -c "menushow Window")
case "$out" in
*"No menu 'Window'."*) ok "the installed bundle's menu bar has no Window menu" ;;
*) fail "the menu bar has a Window menu: not the installed bundle's (got: $out)" ;;
esac
hide "$bundle"
out=$(run ok.c -c "menushow Window")
unhide
case "$out" in
*"No menu 'Window'."*) fail "control broken: the bundle hidden, the menu bar still has no Window menu" ;;
*"Split Window"*) ok "control: the bundle hidden, the default profile's Window menu is listed" ;;
*) fail "control broken: the bundle hidden, no Window menu either (got: $out)" ;;
esac

# 4. key styles
styles=("Chthonia" "VS Code" "Vim" "Emacs" "JOE" "Pico")
out=$(run ok.c -c "menushow Tools")
case "$out" in
*"Key bindings"*) ok "the Tools menu has Key bindings…" ;;
*) fail "the Tools menu has no Key bindings… row (got: $out)" ;;
esac
out=$(run ok.c -c keystyle)
missing=
for s in "${styles[@]}"; do
	case "$out" in *". $s"*) ;; *) missing="$missing [$s]" ;; esac
done
[ -z "$missing" ] || fail "the Key bindings list lacks$missing (got: $out)"
ok "the Key bindings list names all ${#styles[@]} styles"
hide "$profiles"
out=$(run ok.c -c keystyle)
unhide
named=
for s in "${styles[@]}"; do
	case "$out" in *". $s"*) named="$named [$s]" ;; esac
done
[ -z "$named" ] || fail "control broken: the profiles hidden, the list still names$named"
ok "control: the profiles hidden, the Key bindings list names no style"

# 5. the desktop entry and icon
if [ "$(uname -s)" != Darwin ]; then
	[ -f "$prefix/share/applications/chthonia.desktop" ] || fail "no share/applications/chthonia.desktop"
	[ -f "$prefix/share/icons/hicolor/256x256/apps/chthonia.png" ] || fail "no share/icons/hicolor/256x256/apps/chthonia.png"
	ok "the desktop entry and the 256 px icon are installed"
fi
echo "check_install: PASS ($prefix)"
