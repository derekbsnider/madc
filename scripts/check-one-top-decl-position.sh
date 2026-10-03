#!/bin/bash
# DRIFT-PREVENTION GATE -- "where is this file-scope declaration" has ONE
# owner, Program::top_decl_position() (src/parser.cpp).
#
# A TopDecl carries three places: its origin token, the file/line recorded at
# the record site, and (dkGlobalVar) the parser's own position when it
# recorded it. They disagree when the origin token is another file's -- a
# qualified typedef name's token is its header's (`std::string s`, BUGS.md
# B94) -- and a reader that picked `origin ? origin->file : file` itself
# read the header: `?` showed stringfwd.h, the REPL kept a unit's static,
# the emitter called an unreferenced `std::string s = f();` system-origin
# and dropped its initializer. Every reader asks the owner.
#
# Writes (the record sites) are not reads. A TokenDecl's own file (`td->file`
# on a declaration statement) is a token's place, not a TopDecl's.
#
# Two-sided: the negative control proves the pattern still matches a reader
# and still passes a record-site write.
set -u
cd "$(dirname "$0")/.."

PAT='\b(td|gtd)(\.|->)origin->(file|line|column)\b|top_decls\[[^]]*\]\.origin->(file|line|column)\b|\b(td|gtd)\.(file|line|parse_(file|line|column))\b'
WRITE='\b(td|gtd)\.(file|line|parse_(file|line|column)) *= *[^=]'

ctl=$(printf '\tconst char *f = td.origin ? td.origin->file : td.file;\n\tf["line"] = value((int64_t)td.origin->line);\n' | grep -cE "$PAT")
wr=$(printf '\ttd.parse_file = TokenBase::_parse_file;\n' | grep -E "$PAT" | grep -cvE "$WRITE")
if [ "$ctl" -ne 2 ] || [ "$wr" -ne 0 ]; then
	echo "check-one-top-decl-position: NEGATIVE CONTROL FAILED -- the pattern no longer"
	echo "  matches a TopDecl position reader (matched $ctl of 2) or flags a write ($wr)"
	exit 1
fi

# The owner's own body is the one place the fields are read.
range=$(awk '/^const char \*Program::top_decl_position\(/{s=NR} s&&/^}/{print s" "NR; exit}' src/parser.cpp)
if [ -z "$range" ]; then
	echo "check-one-top-decl-position: the owner Program::top_decl_position is missing from src/parser.cpp"
	exit 1
fi
lo=${range% *}
hi=${range#* }

bypass=$(git grep -nE "$PAT" -- src include tests/unit \
	| grep -vE "$WRITE" \
	| grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' \
	| awk -F: -v lo="$lo" -v hi="$hi" '!($1 == "src/parser.cpp" && $2 >= lo && $2 <= hi)')
n=$(printf '%s' "$bypass" | grep -c . || true)
echo "TopDecl position reads outside Program::top_decl_position(): $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bypass"
	echo "  -> ask Program::top_decl_position(td, line[, &column]): an origin token may be another file's."
	exit 1
fi
echo "GREEN -- a file-scope declaration's place has one reader."
