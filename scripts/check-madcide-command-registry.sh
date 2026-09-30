#!/usr/bin/env bash
# check-madcide-command-registry.sh — ONE action vocabulary (S1, 2026-09-08;
# re-anchored on the command ENUM, V0.5c 2026-09-09).
# The vocabulary is `enum ide_cmd` in tools/madcide/madcide_enums.inc; its
# names live in the ONE table cmd_table() beside it (`{ "name": "save",
# "code": cmdSAVE }` rows); profiles/default.menu is the command REGISTRY
# (every command a menu bar or a palette shows has exactly one row: its
# title). Five directions, each a drift a green suite cannot see:
#   1. every action the key profiles bind (unscoped lines AND @scope values,
#      profiles/*.keys) is a table name — a misspelt profile word would be
#      REFUSED at load, so the shipped profiles must all load;
#   2. every registered command is a table name (a title for a word the
#      dispatcher cannot spell is a menu item that does nothing);
#   3. every table row's enumerator exists in the enum, and every enumerator
#      has a table row — one vocabulary, two spellings, never drifting;
#   4. every enumerator is DISPATCHED somewhere in the dispatcher — a
#      `case cmdX:` label or a `== cmdX` / `!= cmdX` compare (a hidden
#      command nothing handles);
#   5. every `cmd…` spelled in the dispatcher is an enumerator (the
#      compiler enforces this too; the gate names the drift first);
#   6. the contributed range (plugins Stage B1, G4): a name shipped code
#      contributes (`plugin_command(w, "name", …)` in the dispatcher) is
#      neither a table name nor contributed twice — registration refuses
#      both, so the command would silently be absent;
#   7. every bundle's menu and keys (plugins/*/) names a table name or a
#      shipped contribution, like the registry and the profiles (which may
#      name contributions too);
#   8. the enum's codes stay below cmd_contrib_base(): the enumerators run
#      from cmdNONE = 0 with no other explicit value, and their count is
#      under the base — a contributed code never aliases a built-in one.
# The dispatcher is madcide_core.inc plus the madcide files it #includes (a
# pane's own commands live beside the pane: madcide_repl.inc).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/tools/madcide/madcide_core.inc"
ENUMS="$ROOT/tools/madcide/madcide_enums.inc"
MENU="$ROOT/tools/madcide/profiles/default.menu"
PROFILES="$ROOT/tools/madcide/profiles"
PLUGINS="$ROOT/tools/madcide/plugins"
PLUGINSINC="$ROOT/tools/madcide/madcide_plugins.inc"

# The registry: column 2 of every data line that is not a separator.
registry_ids()
{
	grep -v '^[[:space:]]*#' "$1" | awk 'NF >= 2 && $2 != "-" { print $2 }' | sort -u
}

# The table rows of cmd_table(): "name enumerator" pairs.
table_pairs()
{
	grep -o -E '\{ "name": "[A-Za-z0-9_-]*", "code": cmd[A-Z][A-Z0-9_]* \}' "$1" |
	sed 's/{ "name": "//; s/", "code": / /; s/ }$//'
}

# The enum body's enumerators (between `enum ide_cmd` and its `};`).
enum_ids()
{
	awk '/^enum ide_cmd/ { on = 1 } on { print } on && /^};/ { exit }' "$1" |
	grep -o -E 'cmd[A-Z][A-Z0-9_]*' | sort -u
}

# The dispatcher: the given core plus the sibling madcide files IT #includes
# (resolved beside the live core; the enums file comes in separately, as the
# one under test).
dispatcher_files()
{
	echo "$1"
	grep -o -E '^#include "madcide_[a-z0-9_]+\.inc"' "$1" |
	sed 's/^#include "//; s/"$//' | grep -v '^madcide_enums\.inc$' |
	while read -r f; do echo "$ROOT/tools/madcide/$f"; done
}

# Where a command is dispatched: case labels and compares against an
# enumerator — in the core AND in the enums file's converters (the motion
# commands are handled by motion_key_of's switch there).
dispatch_ids()
{
	cat "$@" |
	grep -o -E '(case[[:space:]]+cmd[A-Z][A-Z0-9_]*[[:space:]]*:|[!=]=[[:space:]]*cmd[A-Z][A-Z0-9_]*)' |
	grep -o -E 'cmd[A-Z][A-Z0-9_]*' | sort -u
}

# Every enumerator spelled anywhere in the dispatcher.
spelled_ids()
{
	cat "$@" | grep -o -E 'cmd[A-Z][A-Z0-9_]*' | sort -u
}

# The names shipped code contributes: the literal name of every
# plugin_command(<world>, "<name>", …) call, one line per call (duplicates
# kept, so a double registration shows).
contributed_calls()
{
	cat "$@" | grep -o -E 'plugin_command\([^,()]*, "[A-Za-z0-9_-]*"' |
	sed 's/.*, "//; s/"$//'
}

# The contributed range's floor: cmd_contrib_base()'s returned literal.
contrib_base()
{
	awk '/^long cmd_contrib_base\(\)/ { on = 1 }
	     on && /return [0-9]+;/ { match($0, /[0-9]+/); print substr($0, RSTART, RLENGTH); exit }' "$1"
}

# An enumerator given an explicit value, cmdNONE's excepted (the count
# would no longer bound the codes).
enum_explicit()
{
	awk '/^enum ide_cmd/ { on = 1 } on { print } on && /^};/ { exit }' "$1" |
	sed 's|//.*||' | grep -o -E 'cmd[A-Z][A-Z0-9_]*[[:space:]]*=[^=]' |
	grep -v '^cmdNONE'
}

# Every bundle's files of one kind (menu, keys), sorted.
bundle_files()
{
	find "$1" -type f -name "*.$2" | sort
}

# Profile actions: the LAST word of every data line, unscoped or @scoped
# (a scoped line's value is its last word too) — across the given files.
profile_ids()
{
	cat "$@" | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$' |
	awk '{ print $NF }' | sort -u
}

missing_from()
{
	comm -23 "$1" "$2"
}

check()
{
	local core="$1" enums="$2" menu="$3" profiles="$4" plugins="$5" pinc="$6" label="$7"
	local reg pairs tnames tenums enumids dis spelled prof bad
	local calls contrib known breg bprof base count
	reg=$(mktemp); pairs=$(mktemp); tnames=$(mktemp); tenums=$(mktemp)
	enumids=$(mktemp); dis=$(mktemp); spelled=$(mktemp); prof=$(mktemp)
	calls=$(mktemp); contrib=$(mktemp); known=$(mktemp); breg=$(mktemp)
	bprof=$(mktemp)
	registry_ids "$menu" > "$reg"
	table_pairs "$enums" > "$pairs"
	awk '{ print $1 }' "$pairs" | sort -u > "$tnames"
	awk '{ print $2 }' "$pairs" | sort -u > "$tenums"
	enum_ids "$enums" | grep -v '^cmdNONE$' > "$enumids"
	dispatch_ids $(dispatcher_files "$core") "$enums" > "$dis"
	spelled_ids $(dispatcher_files "$core") > "$spelled"
	profile_ids "$profiles"/*.keys > "$prof"
	contributed_calls $(dispatcher_files "$core") > "$calls"
	sort -u "$calls" > "$contrib"
	sort -u "$tnames" "$contrib" > "$known"
	: > "$breg"; : > "$bprof"
	for f in $(bundle_files "$plugins" menu); do registry_ids "$f"; done | sort -u > "$breg"
	for f in $(bundle_files "$plugins" keys); do profile_ids "$f"; done | sort -u > "$bprof"
	base=$(contrib_base "$pinc")
	local rc=0
	if [ ! -s "$reg" ] || [ ! -s "$pairs" ] || [ ! -s "$enumids" ] || [ ! -s "$dis" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — an extractor" \
		     "came back empty (registry $(wc -l < "$reg"), table" \
		     "$(wc -l < "$pairs"), enum $(wc -l < "$enumids"), dispatch" \
		     "$(wc -l < "$dis")); the anchors moved." >&2
		rc=1
	fi
	bad=$(missing_from "$prof" "$known")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — a key profile" \
		     "binds a word the command table does not name (the profile" \
		     "would be REFUSED at load):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$reg" "$known")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — profiles/default.menu" \
		     "titles a command the table does not name (the menu would be" \
		     "REFUSED at load):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$tenums" "$enumids")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — the command table" \
		     "names an enumerator the enum lacks:" $bad >&2
		rc=1
	fi
	# cmdBUILDROW has no table row by design: its name is "build-<n>".
	bad=$(missing_from "$enumids" "$tenums" | grep -v '^cmdBUILDROW$')
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — an enumerator has" \
		     "no command-table row (no name can reach it):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$enumids" "$dis")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — an enumerator is" \
		     "dispatched nowhere in madcide_core.inc or its includes (a hidden command):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$spelled" <(sort -u "$enumids" <(echo cmdNONE)))
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — madcide_core.inc" \
		     "or an include spells a cmd… that is not an enumerator:" $bad >&2
		rc=1
	fi
	bad=$(comm -12 "$contrib" "$tnames"; sort "$calls" | uniq -d)
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — shipped code" \
		     "contributes a built-in's name or one name twice (registration" \
		     "refuses it; the command would be absent):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$breg" "$known"; missing_from "$bprof" "$known")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — a bundle's menu or" \
		     "keys names a word neither the table nor a shipped contribution" \
		     "names (the bundle would be REFUSED at load):" $bad >&2
		rc=1
	fi
	count=$(enum_ids "$enums" | wc -l)
	bad=$(enum_explicit "$enums")
	if [ -z "$base" ] || [ -n "$bad" ] || [ "$count" -ge "$base" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — the built-in codes" \
		     "may reach the contributed range (cmd_contrib_base ${base:-unread}," \
		     "$count enumerators, explicit values: ${bad:-none})." >&2
		rc=1
	fi
	rm -f "$reg" "$pairs" "$tnames" "$tenums" "$enumids" "$dis" "$spelled" "$prof"
	rm -f "$calls" "$contrib" "$known" "$breg" "$bprof"
	return $rc
}

if ! check "$CORE" "$ENUMS" "$MENU" "$PROFILES" "$PLUGINS" "$PLUGINSINC" "live"; then
	exit 1
fi

# Negative controls: each direction must catch a synthetic drift.
tmpcore=$(mktemp); tmpenums=$(mktemp); tmpmenu=$(mktemp); tmpprof=$(mktemp -d)
tmpplug=$(mktemp -d); tmpinc=$(mktemp)
# (a) a table row whose enumerator nothing dispatches
sed 's/^    t = rows;$/    rows[] = { "name": "zzbogus", "code": cmdZZBOGUS };\n    t = rows;/; s/^    cmdBUILDROW\(.*\)$/    cmdZZBOGUS, cmdBUILDROW\1/' "$ENUMS" > "$tmpenums"
if check "$CORE" "$tmpenums" "$MENU" "$PROFILES" "$PLUGINS" "$PLUGINSINC" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — negative control: an enumerator" \
	     "nothing dispatches went undetected (the dispatch marker went blind)." >&2
	exit 1
fi
# (b) a registered command the table does not name
cat "$MENU" > "$tmpmenu"
echo "Help zzbogus Bogus" >> "$tmpmenu"
if check "$CORE" "$ENUMS" "$tmpmenu" "$PROFILES" "$PLUGINS" "$PLUGINSINC" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — negative control: a registered" \
	     "command the table lacks went undetected (the registry marker went blind)." >&2
	exit 1
fi
# (c) a profile binding a word the table does not name
cp "$PROFILES"/*.keys "$tmpprof"/
echo "^k 9 zzbogus" >> "$tmpprof/joe.keys"
if check "$CORE" "$ENUMS" "$MENU" "$tmpprof" "$PLUGINS" "$PLUGINSINC" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — negative control: a profile" \
	     "word outside the table went undetected (the profile marker went blind)." >&2
	exit 1
fi
# (d) the core spelling an enumerator that does not exist
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tif ( code == cmdZZBOGUS ) return true;" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$ENUMS" "$MENU" "$PROFILES" "$PLUGINS" "$PLUGINSINC" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — negative control: a cmd… the" \
	     "enum lacks went undetected (the spelling marker went blind)." >&2
	exit 1
fi
# (e) a pane's commands, its include dropped from the core: the dispatcher
# must follow the core's includes, so the pane's commands go hidden
grep -v '^#include "madcide_repl\.inc"' "$CORE" > "$tmpcore"
if check "$tmpcore" "$ENUMS" "$MENU" "$PROFILES" "$PLUGINS" "$PLUGINSINC" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — negative control: a dropped" \
	     "include's commands went undetected (the dispatcher stopped following" \
	     "the core's includes)." >&2
	exit 1
fi
# (f) shipped code contributing a built-in's name
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tplugin_command(w, \"save\", \"Save\", 1);" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$ENUMS" "$MENU" "$PROFILES" "$PLUGINS" "$PLUGINSINC" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — negative control: a contribution" \
	     "shadowing a built-in went undetected (the contribution marker went blind)." >&2
	exit 1
fi
# (g) a bundle menu naming an unknown word — and, positively, the same word
# accepted once shipped code contributes it (the gate learns the range)
(cd "$PLUGINS" && find . -type f) | while read -r f; do
	mkdir -p "$tmpplug/$(dirname "$f")"
	cat "$PLUGINS/$f" > "$tmpplug/$f"
done
echo "Help zzgreet Greet" >> "$tmpplug/chthonic/chthonic.menu"
if check "$CORE" "$ENUMS" "$MENU" "$PROFILES" "$tmpplug" "$PLUGINSINC" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — negative control: a bundle menu" \
	     "word outside the table went undetected (the bundle marker went blind)." >&2
	exit 1
fi
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tplugin_command(w, \"zzgreet\", \"Greet\", 1);" }' "$CORE" > "$tmpcore"
if ! check "$tmpcore" "$ENUMS" "$MENU" "$PROFILES" "$tmpplug" "$PLUGINSINC" "control"; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — positive control: a bundle menu" \
	     "naming a shipped contribution was refused (the range went unlearned)." >&2
	exit 1
fi
# (h) a contributed range that starts inside the enum's codes
sed 's/^    return 4096;$/    return 8;/' "$PLUGINSINC" > "$tmpinc"
if check "$CORE" "$ENUMS" "$MENU" "$PROFILES" "$PLUGINS" "$tmpinc" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
	echo "check-madcide-command-registry: FAIL — negative control: a contributed" \
	     "base inside the enum went undetected (the range marker went blind)." >&2
	exit 1
fi
rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof" "$tmpplug" "$tmpinc"
n=$(registry_ids "$MENU" | wc -l)
m=$(enum_ids "$ENUMS" | grep -vc '^cmdNONE$')
echo "check-madcide-command-registry: OK ($n registered commands of $m enumerators; profiles, bundles, registry, table, enum, dispatcher and the contributed range agree; controls bite)"
