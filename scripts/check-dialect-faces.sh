#!/bin/bash
# DRIFT-PREVENTION GATE -- what madc builds for itself is exposed in the
# dialect (.claude/rules/dialect-faces.md): a host program (madcide) does
# what the CLI does by calling madc's code through its face, never a copy.
#
# The registry is scripts/dialect_faces.tsv:
#   struct rows -- every member of an engine struct a host reads is a key its
#     face function writes, or is omitted with a reason. A member added to the
#     struct and not to the face is RED (ex::outcome gained `code`, the CLI
#     painted :%p from it, outcome_value never carried it, and madcide's REPL
#     showed :%p uncoloured).
#   public rows -- every namespace-scope function of a registered header has
#     a dialect face (a declaration in a dialect header) or an exempt reason.
#     A new public without a row is RED (madc_theme.h's painter had no face,
#     and madcide grew its own).
#
# Two-sided: the negative controls prove each reader still finds members,
# face keys and publics in a known text.
set -u
cd "$(dirname "$0")/.."
REG=${DIALECT_FACES_REG:-scripts/dialect_faces.tsv}	# another registry: the gate's own proof

# The data members of struct $1 in the header text on stdin, one per line.
struct_fields()
{
	awk -v n="$1" '
	!on && ($0 ~ "^[ \t]*struct[ \t]+" n "[ \t]*$" || $0 ~ "^[ \t]*struct[ \t]+" n "[ \t]*[{]") { on = 1; depth = 0 }
	on {
		line = $0
		sub(/\/\/.*/, "", line)
		o = gsub(/[{]/, "{", line)
		c = gsub(/[}]/, "}", line)
		if ( depth == 1 && line ~ /;[ \t]*$/ && line !~ /[(]/ && line !~ /^[ \t]*[}]/ \
		     && line !~ /^[ \t]*(typedef|using|friend|static|enum|struct|class|public|private|protected)[ \t:]/ )
		{
			m = line
			sub(/;[ \t]*$/, "", m)
			sub(/=.*/, "", m)
			sub(/\[.*\]/, "", m)
			sub(/[ \t]+$/, "", m)
			k = split(m, w, /[ \t*&]+/)
			print w[k]
		}
		depth += o - c
		if ( o )
			started = 1
		if ( started && depth == 0 )
			exit
	}'
}

# The keys the face function $1 writes (`["key"]`) in the source on stdin.
face_keys()
{
	awk -v f="$1" '
	!on && $0 ~ "^[A-Za-z_].*[ \t*&]" f "[(]" && $0 !~ /;[ \t]*$/ { on = 1 }
	on { print }
	on && /^}/ { exit }' | grep -oE '\["[A-Za-z_][A-Za-z0-9_]*"\]' | tr -d '[]"' | sort -u
}

# The namespace-scope functions the header text on stdin declares.
header_publics()
{
	grep -E '^[A-Za-z_][^(]*[ 	*&][A-Za-z_][A-Za-z0-9_]*\(' \
		| grep -vE '^(struct|class|namespace|enum|typedef|using|template)\b' \
		| sed -E 's/^[^(]*[ 	*&]([A-Za-z_][A-Za-z0-9_]*)\(.*/\1/' | sort -u
}

ctl_f=$(printf 'struct s\n{\n\tint a;\n\tstd::vector<x> b;\t// note\n\ts() : a(0) {}\n};\nint after;\n' | struct_fields s | tr '\n' ' ')
ctl_k=$(printf 'value f(const s &x)\n{\n\tm["a"] = 1;\n\tm["b"] = g(x["c"]);\n}\nvalue h()\n{\n\tm["z"] = 0;\n}\n' | face_keys f | tr '\n' ' ')
ctl_p=$(printf 'const theme &tf();\nbool tr(const std::string &t,\n\tint x);\nstruct q { int z(); };\n' | header_publics | tr '\n' ' ')
if [ "$ctl_f" != "a b " ] || [ "$ctl_k" != "a b c " ] || [ "$ctl_p" != "tf tr " ]; then
	echo "check-dialect-faces: NEGATIVE CONTROL FAILED -- members [$ctl_f] (want a b),"
	echo "  face keys [$ctl_k] (want a b c), publics [$ctl_p] (want tf tr)"
	exit 1
fi

red=0
bad() { echo "RED  $*"; red=$((red + 1)); }

# struct rows
while IFS=$'\t' read -r kind header name file fn members; do
	[ "$kind" = struct ] || continue
	fields=$(struct_fields "$name" < "$header")
	[ -n "$fields" ] || { bad "$header: no struct $name"; continue; }
	keys=$(face_keys "$fn" < "$file")
	[ -n "$keys" ] || { bad "$file: no face function $fn"; continue; }
	accounted=""
	for m in $members; do
		case "$m" in
		-*)	f=${m#-}; f=${f%%:*}
			[ "${m#*:}" != "$m" ] && [ -n "${m#*:}" ] || bad "$name.$f: an omitted member needs its reason (-$f:why)" ;;
		*=*)	f=${m%%=*}; k=${m#*=} ;;
		*)	f=$m; k=$m ;;
		esac
		accounted="$accounted $f"
		printf '%s\n' "$fields" | grep -qx "$f" || bad "$name.$f: the registry names a member $header no longer has"
		case "$m" in -*) continue ;; esac
		printf '%s\n' "$keys" | grep -qx "$k" || bad "$name.$f: $fn ($file) writes no \"$k\" -- the dialect never sees it"
	done
	for f in $fields; do
		case " $accounted " in *" $f "*) ;; *)
			bad "$name.$f: a member with no dialect face -- write it in $fn ($file), or omit it with a reason in $REG" ;;
		esac
	done
done < <(grep -v '^#' "$REG")

# public rows
for header in $(awk -F'\t' '$1 == "public" { print $2 }' "$REG" | sort -u); do
	for p in $(header_publics < "$header"); do
		awk -F'\t' -v h="$header" -v p="$p" '$1 == "public" && $2 == h && $3 == p { f = 1 } END { exit !f }' "$REG" \
			|| bad "$header: $p has no dialect face -- declare one in a dialect header, or an exempt row in $REG"
	done
done
while IFS=$'\t' read -r kind header name dheader face; do
	[ "$kind" = public ] || continue
	header_publics < "$header" | grep -qx "$name" || bad "$header: the registry names $name, which it no longer declares"
	case "$face" in
	exempt:*) [ -n "${face#exempt:}" ] || bad "$name: an exempt row needs its reason" ;;
	*) grep -qE "\b$face\(" "$dheader" || bad "$name: no $face( declared in $dheader -- a host cannot reach it" ;;
	esac
done < <(grep -v '^#' "$REG")

n_s=$(grep -c '^struct	' "$REG")
n_p=$(grep -c '^public	' "$REG")
echo "dialect faces: $n_s struct rows, $n_p public rows, $red red"
[ "$red" -eq 0 ] || { echo "  -> the dialect reaches madc's own code; see .claude/rules/dialect-faces.md"; exit 1; }
echo "GREEN -- every registered capability has its dialect face."
