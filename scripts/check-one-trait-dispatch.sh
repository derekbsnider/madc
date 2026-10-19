#!/bin/bash
# check-one-trait-dispatch.sh — a type-trait builtin (__is_class, __is_same,
# __has_trivial_destructor, ...) is answered by ONE evaluator: its spelling is
# read once, at the name, into the TypeTrait enum (type_trait_of's table in
# src/parser.cpp), and eval_type_trait switches on the kind. The live parse
# (evaluate_type_trait) and a template argument's fold (eval_local_type_trait)
# both call it.
#
# Two hand ladders of `name == "__is_..."` used to answer the same traits, one
# per site, and had to be kept in step by hand. A string compare against a
# trait builtin's spelling on a non-comment line of src/*.cpp is a third copy.
# (The lexer's __has_include / __has_builtin / __has_attribute are #if query
# operators, not type traits; the pattern does not name them.)
# Negative control: a synthetic source with `name == "__is_class"` must FAIL;
# the table form `{ "__is_class", TypeTrait::IsClass, 1 }` must PASS.
#
#   scripts/check-one-trait-dispatch.sh [DIR]   (DIR: the sources to scan)
set -u
cd "$(dirname "$0")/.." || exit 2

trait_compare='[=!]=[[:space:]]*"__(is_[a-z_]+|has_trivial_[a-z_]+|has_virtual_destructor|has_unique_object_representations)"'

# Prints the trait-spelling compares in the sources under $1; returns 0 when none.
scan() {
	local dir=$1 f hits found=0
	for f in "$dir"/*.cpp; do
		[ -f "$f" ] || continue
		hits=$(grep -n -E "$trait_compare" "$f" | grep -v -E '^[0-9]+:[[:space:]]*//')
		if [ -n "$hits" ]; then
			found=1
			printf '%s\n' "$hits" | sed "s|^|  $f:|"
		fi
	done
	return $found
}

mkdir -p tmp
ctl=$(mktemp -d tmp/trait-dispatch-ctl.XXXXXX) || exit 2
trap 'rm -rf "$ctl"' EXIT
mkdir -p "$ctl/bad" "$ctl/good"
printf 'bool f(const istring &name) { return name == "__is_class"; }\n' > "$ctl/bad/x.cpp"
printf 'static const T t[] = { { "__is_class", TypeTrait::IsClass, 1 } };\n' > "$ctl/good/x.cpp"
if scan "$ctl/bad" > /dev/null; then
	echo "check-one-trait-dispatch: CONTROL FAILED — a hand trait compare went undetected" >&2
	exit 1
fi
if ! scan "$ctl/good" > /dev/null; then
	echo "check-one-trait-dispatch: CONTROL FAILED — the spelling table was flagged" >&2
	exit 1
fi

out=$(scan "${1:-src}")
if [ -n "$out" ]; then
	echo "check-one-trait-dispatch: a type-trait builtin is compared by spelling —" >&2
	echo "add it to type_trait_of's table and a case to eval_type_trait (src/parser.cpp):" >&2
	printf '%s\n' "$out" >&2
	exit 1
fi
echo "check-one-trait-dispatch: OK (one trait evaluator, dispatch by kind; control both ways)"
