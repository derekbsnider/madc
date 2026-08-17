#!/bin/sh
# Select the strict-C runtime ledger sources for one native target.
#
# Usage: select_ledger_sources.sh <linux|darwin|win64> [manifest]
#
# Output is the manifest's canonical-order, repo-root-relative source list.
# Validation is deliberately all-or-nothing: no paths are printed when any
# manifest line is malformed, so Make cannot silently consume a partial list.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
	echo "usage: $0 <linux|darwin|win64> [manifest]" >&2
	exit 2
fi

target=$1
script_dir=$(dirname "$0")
repo_root=$script_dir/..
manifest=${2:-"$script_dir/ledger_sources.txt"}

case "$target" in
	linux|darwin|win64) ;;
	*)
		echo "select_ledger_sources: unknown target '$target'" >&2
		exit 2
		;;
esac

if [ ! -r "$manifest" ]; then
	echo "select_ledger_sources: cannot read '$manifest'" >&2
	exit 1
fi

awk -v target="$target" -v repo_root="$repo_root" '
function reject(message)
{
	printf "%s:%d: %s\n", FILENAME, FNR, message > "/dev/stderr"
	invalid = 1
}

{
	line = $0
	sub(/\r$/, "", line)
	gsub(/^[[:space:]]+/, "", line)
	gsub(/[[:space:]]+$/, "", line)
	if (line == "" || substr(line, 1, 1) == "#")
		next

	field_count = split(line, field, /[[:space:]]+/)
	if (field_count != 2) {
		reject("expected: <target[,target...]> <src/rt/source.c>")
		next
	}

	target_spec = field[1]
	source = field[2]
	if (source !~ /^src\/rt\/[[:alnum:]_.+-]+\.c$/) {
		reject("ledger source must be a repo-root-relative src/rt/*.c path")
		next
	}
	if (seen_source[source]++) {
		reject("duplicate ledger source: " source)
		next
	}
	component_count = split(source, component, /\//)
	object_name = component[component_count]
	sub(/\.c$/, ".o", object_name)
	if (object_name in object_owner) {
		reject("ledger object basename collision: " source " and " object_owner[object_name])
		next
	}
	object_owner[object_name] = source
	source_path = repo_root "/" source
	source_status = (getline source_line < source_path)
	close(source_path)
	if (source_status < 0) {
		reject("ledger source does not exist or is unreadable: " source)
		next
	}

	target_count = split(target_spec, source_target, /,/)
	matched = 0
	tags_valid = 1
	for (i = 1; i <= target_count; i++) {
		tag = source_target[i]
		tag_key = FNR SUBSEP tag
		if (tag != "all" && tag != "linux" && tag != "darwin" && tag != "win64") {
			reject("unknown source target: " tag)
			tags_valid = 0
		} else if (seen_tag[tag_key]++) {
			reject("duplicate source target: " tag)
			tags_valid = 0
		} else if (tag == "all" && target_count != 1) {
			reject("all must be the only target in its target field")
			tags_valid = 0
		}
		if (tag == "all" || tag == target)
			matched = 1
	}
	if (tags_valid && matched)
		selected[++selected_count] = source
}

END {
	if (invalid)
		exit 1
	if (selected_count == 0) {
		printf "%s: no ledger sources selected for target %s\n", FILENAME, target > "/dev/stderr"
		exit 1
	}
	for (i = 1; i <= selected_count; i++)
		print selected[i]
}
' "$manifest"
