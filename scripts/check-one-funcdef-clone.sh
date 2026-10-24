#!/bin/bash
# DRIFT-PREVENTION GATE -- a FuncDef rebuilt around a new return type has ONE
# owner: clone_funcdef_with_return (src/parser.cpp). FuncDef::returns is a
# reference, so a return-type change rebuilds the object, and the rebuild
# keeps every other field of the declaration.
#
# parseFunction's two refreshes (an unprototyped C declaration met by its
# definition; a declaration whose return type the definition changes) each
# hand-copied a subset of the fields, ~25 lines apiece, and the three copies
# drifted: both refreshes dropped no_strict_aliasing; the return-type refresh
# also dropped default arguments, const_params and namespace_name; the clone
# dropped c_linkage and declared_unprototyped.
#
# Rule over src/: no run of MIN (8) or more consecutive member-wise field
# copies (`a->f = b->f;`, the same field on both sides; blank and comment
# lines and a copy wrapped after its `=` keep the run) outside
# clone_funcdef_with_return's body. A run that long is a clone: a FuncDef's
# goes through the owner, another type gets its own copy owner. Shorter runs
# are a narrower concept (member_function_type's five signature fields, a
# token's location). Comments are stripped first. Two-sided: the negative
# control proves a run of MIN still bites, while MIN - 1 and the owner's body
# pass.
set -u
cd "$(dirname "$0")/.."

MIN=8
OWNER='clone_funcdef_with_return'

# Print file:line for every run of >= MIN field copies outside the owner.
copy_runs() {
	MIN="$MIN" OWNER="$OWNER" awk '
		BEGIN { min = ENVIRON["MIN"] + 0; owner = ENVIRON["OWNER"] }
		function flush() {
			if ( n >= min && !in_owner )
				print FILENAME ":" start ": " n " field copies " src " -> " dst
			n = 0
		}
		FNR == 1 { flush(); inblk = 0; in_owner = 0; held = "" }
		{
			line = $0; out = ""
			while ( length(line) ) {
				if ( inblk ) {
					e = index(line, "*/")
					if ( !e ) { line = ""; break }
					line = substr(line, e + 2); inblk = 0
					continue
				}
				s = index(line, "/*"); c = index(line, "//")
				if ( c && (!s || c < s) ) { out = out substr(line, 1, c - 1); line = ""; break }
				if ( s ) { out = out substr(line, 1, s - 1); line = substr(line, s + 2); inblk = 1; continue }
				out = out line; line = ""
			}
			if ( out ~ ("^static FuncDef \\*" owner "\\(") && out !~ /;[ \t]*$/ ) in_owner = 1
			else if ( in_owner && out ~ /^}/ ) { flush(); in_owner = 0 }
			t = out; gsub(/[ \t]/, "", t)
			# a copy wrapped after its `=` joins the next line
			if ( held != "" ) { t = held t; held = "" }
			if ( t ~ /^[A-Za-z_][A-Za-z_0-9]*->[A-Za-z_][A-Za-z_0-9]*=$/ ) { held = t; next }
			if ( t ~ /^[A-Za-z_][A-Za-z_0-9]*->[A-Za-z_][A-Za-z_0-9]*=[A-Za-z_][A-Za-z_0-9]*->[A-Za-z_][A-Za-z_0-9]*;$/ ) {
				sub(/;$/, "", t)
				split(t, side, "=")
				split(side[1], l, "->"); split(side[2], r, "->")
				if ( l[2] == r[2] && l[1] != r[1] ) {
					if ( n > 0 && l[1] == dst && r[1] == src ) { n++; next }
					flush(); n = 1; start = FNR; dst = l[1]; src = r[1]
					next
				}
			}
			# a blank or comment-only line keeps the run
			if ( n > 0 && t == "" ) next
			flush()
		}
		END { flush() }' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
static FuncDef *clone_funcdef_with_return(FuncDef *src, DataDef &new_ret)
{
	f->a = src->a;
	f->b = src->b;
	f->c = src->c;
	f->d = src->d;
	f->e = src->e;
	f->f = src->f;
	f->g = src->g;
	f->h = src->h;
	return f;
}
void refresh()
{
	fresh->a = func->a;
	fresh->b = func->b;
	// a comment line inside the run
	fresh->c = func->c;

	fresh->d =
	    func->d;
	fresh->e = func->e;
	fresh->f = func->f;
	fresh->g = func->g;
	fresh->h = func->h;
	other->x = func->y;
	seven->a = func->a;
	seven->b = func->b;
	seven->c = func->c;
	seven->d = func->d;
	seven->e = func->e;
	seven->f = func->f;
	seven->g = func->g;
}
CTL
k=$(copy_runs "$ctl" | grep -c .)
if [ "$k" -ne 1 ]; then
	echo "check-one-funcdef-clone: NEGATIVE CONTROL FAILED"
	echo "  reported $k runs, expected 1 (the eight-copy run outside the owner)"
	copy_runs "$ctl"
	exit 1
fi

files=$(git ls-files --cached --others --exclude-standard 'src/*.cpp' 'src/**/*.cpp' | sort -u)
bad=$(copy_runs $files)
n=$(printf '%s' "$bad" | grep -c . || true)
echo "member-wise field-copy runs outside $OWNER: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> rebuild a FuncDef through clone_funcdef_with_return (src/parser.cpp)."
	exit 1
fi
echo "GREEN -- a FuncDef rebuild has one owner, clone_funcdef_with_return."
