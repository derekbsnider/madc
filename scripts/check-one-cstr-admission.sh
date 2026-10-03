#!/bin/bash
# DRIFT-PREVENTION GATE -- "does this object argument coerce to text through
# c_str" has ONE admission, CirBuilder::is_class_object_expr(): a class /
# carrier object expression of ANY value category (is_class_object_value's
# lvalue shapes, or a class prvalue -- object_returning_call_class).
#
# The lvalue-only is_class_object_value() guarded eight of the nine
# object_cstr_arg coercion sites (char* parameters, varargs, the printstr /
# dlcall builtins); only the throw site composed the prvalue half by hand. A
# `value("t")` temporary or a by-value `var` call into a const char*
# parameter took the raw pass and the callee read the temporary's storage
# words (tests/testcarriertempcstr.mad).
#
# Two rules over src/:
#   1. no is_class_object_value(...) guard paired with is_char_pointer(...)
#      on its line, or whose statement (the same line or the next code line)
#      is an object_cstr_arg(...) coercion;
#   2. no hand-composed `is_class_object_value(x) || object_returning_call_class(x)`
#      (one line, or split across two) outside the owner, which is marked
#      `// allowed-exception: the owner`.
# Two-sided: the negative control proves both rules still bite.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '
	function code(s) { return s !~ /^[[:space:]]*\/\// }
	{
		if (!code($0)) next
		if (pending != "") {
			if ($0 ~ /object_cstr_arg\(/)
				print pending " -> " FILENAME ":" FNR ": guard admits lvalues only"
			if ($0 ~ /^[[:space:]]*\|\|[[:space:]]*object_returning_call_class\(/ && $0 !~ /allowed-exception/)
				print pending " -> " FILENAME ":" FNR ": hand-composed prvalue admission"
			pending = ""
		}
		if ($0 ~ /is_class_object_value\(/ && $0 !~ /allowed-exception/ && $0 !~ /bool CirBuilder::is_class_object_value|static bool is_class_object_value/) {
			if ($0 ~ /object_cstr_arg\(/ || $0 ~ /is_char_pointer\(/)
				print FILENAME ":" FNR ": guard admits lvalues only"
			else if ($0 ~ /is_class_object_value\([^)]*\)[[:space:]]*\|\|[[:space:]]*object_returning_call_class\(/)
				print FILENAME ":" FNR ": hand-composed prvalue admission"
			else
				pending = FILENAME ":" FNR
		}
	}' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
		else if (is_char_pointer(pt) && is_class_object_value(arg))
			append(args, object_cstr_arg(arg));
	bool t = is_class_object_value(e)
		|| object_returning_call_class(e);
	bool u = is_class_object_value(e) || object_returning_call_class(e);
	if (is_class_object_value(p))
		// a comment line is skipped
		append(a, translate_expr(p));
	if (is_class_object_expr(p))
		append(a, object_cstr_arg(p));
	return is_class_object_value(a) || object_returning_call_class(a) != NULL; // allowed-exception: the owner
	} else if (is_char_pointer(pt) && is_class_object_value(top->right)) {
		eparams.push_back(shape);
CTL
c=$(scan "$ctl" | grep -c .)
if [ "$c" -ne 4 ]; then
	echo "check-one-cstr-admission: NEGATIVE CONTROL FAILED -- matched $c of 4"
	scan "$ctl"
	exit 1
fi

files=$(git ls-files 'src/*.cpp')
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "c_str coercion admissions outside is_class_object_expr: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits"
	echo "  -> admit through CirBuilder::is_class_object_expr(): it reads both value"
	echo "     categories, and object_cstr_arg's object_arg_addr materializes a prvalue."
	exit 1
fi
echo "GREEN -- the c_str coercion has one admission."
