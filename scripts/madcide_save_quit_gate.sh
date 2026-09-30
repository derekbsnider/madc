#!/bin/bash
# madcide_save_quit_gate (in fulltest; B85): madcide SAVES and QUITS when
# started from a directory that is not the checkout, and refuses to start
# when its verb bodies cannot load. The real TUI on a pty
# (scripts/madcide_save_quit_pty.py), its cwd a fresh temp directory:
#   1. the source tree with its profiles: `x`, ^K D, ^K Q  => SAVED EXITED
#   2. NEGATIVE CONTROL: `x` alone                         => UNSAVED RUNNING
#      (the save and exit checks detect a missing save and a live process)
#   3. a copy with NO profiles directory (the rescue keys): `x`, ^S, ^Q
#                                                          => SAVED EXITED RESCUE
#   4. a copy with NO verbs directory: madcide refuses to start, with the
#      reason                                              => EXITED rc=1 VERBSMISSING
# Before B85's fix, 1 and 3 read UNSAVED RUNNING: the verbs loaded from a
# cwd-relative path, so save and quit did nothing outside the repo root.
cd "$(dirname "$0")/.." || exit 1
root=$(pwd)

if ! command -v python3 >/dev/null 2>&1; then
    echo "madcide_save_quit_gate: python3 missing (provision_container.sh installs it)"
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
probe="$work/probe.c"
fresh() { printf 'int main(void) { return 0; }\n' > "$probe"; }
run() {	# <keys-hex> <madcide.mad>
    ( ulimit -t 180; timeout 120 python3 scripts/madcide_save_quit_pty.py \
        "$probe" "$1" "$root/bin/madc" "$2" ) 2>&1
}
fail() { echo "madcide_save_quit_gate: FAIL — $1 (got: $2)"; exit 1; }

fresh
out=$(run 0b640b71 "$root/tools/madcide/madcide.mad")
case "$out" in "SAVED EXITED rc=0 NORESCUE VERBSOK"*) ;; *) fail "the source tree did not save and quit from a foreign cwd" "$out" ;; esac

fresh
neg=$(run "" "$root/tools/madcide/madcide.mad")
case "$neg" in "UNSAVED RUNNING"*) ;; *) fail "the negative control (no save, no quit) did not read UNSAVED RUNNING" "$neg" ;; esac

mkdir -p "$work/noprof"
cp -r tools/madcide tools/texteditor "$work/noprof/"
rm -rf "$work/noprof/madcide/profiles"
fresh
res=$(run 1311 "$work/noprof/madcide/madcide.mad")
case "$res" in "SAVED EXITED rc=0 RESCUE VERBSOK"*) ;; *) fail "with no profiles the rescue keys did not save and quit" "$res" ;; esac

mkdir -p "$work/noverbs"
cp -r tools/madcide tools/texteditor "$work/noverbs/"
rm -rf "$work/noverbs/texteditor/verbs"
fresh
nov=$(run "" "$work/noverbs/madcide/madcide.mad")
case "$nov" in *"EXITED rc=1 "*"VERBSMISSING"*) ;; *) fail "with no verbs madcide did not refuse to start with the reason" "$nov" ;; esac

echo "madcide_save_quit_gate: PASS ($out; no profiles: $res; no verbs: $nov; negative control: $neg)"
