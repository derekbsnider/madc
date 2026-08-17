/* cir_dump.cpp — the GENERATED side of php::print_r / php::var_dump.
 *
 * These are declared in <ns_php> and defined NOWHERE: the compiler is their
 * implementation. A call site knows the concrete argument type, so the dumper
 * for that type is generated here (approach A) and the runtime carries only
 * OUTPUT PRIMITIVES (src/rt/rt_dump.c). No runtime type-descriptor table
 * exists, so a program that never dumps pays nothing.
 *
 * PHP is the oracle: php::print_r must render a madc value the way a PHP
 * developer expects PHP to render it. The contract, with php-cli 8.3.6 output
 * captured verbatim, is docs/plans/2026-08-17-php-print-r-var-dump-plan.md.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "token_arena.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_dl.h"
#include "cir_builder.h"

extern "C" {
#include "c2mir/c2mir_api.h"
}

extern thread_local bool madc_verbose;

// ---------------------------------------------------------------------------
// Which compiler-implemented dump is this call?
// ---------------------------------------------------------------------------
// The ONE place a dump intrinsic's spelling is compared. The parser stamped
// FuncDef::inline_builtin_kind at declaration registration (the same carrier
// std::addressof / std::forward / __destroy use, and one the forest already
// serializes), so this reads a tag rather than a user-facing name, and it is
// converted to an enum here — at the boundary — per enum-over-strings.
enum DumpFlavor { dfNone, dfPrintR };

static DumpFlavor dump_flavor(FuncDef *fd)
{
	if (!fd)
		return dfNone;
	if (fd->inline_builtin_kind == "php_print_r")
		return dfPrintR;
	return dfNone;
}

// ---------------------------------------------------------------------------
// print_r of one value
// ---------------------------------------------------------------------------
// PHP's print_r on a scalar emits the value ALONE — no type, no newline, no
// trailing space. print_r(42) is exactly "42", print_r(true) is "1", and
// print_r(false) and print_r(null) are both the empty string. The nested
// indentation of an array/object dump belongs to the aggregate renderer, which
// is why nothing here takes a depth.
node_t CirBuilder::dump_print_r_value(TokenBase *arg, TokenBase *origin)
{
	DataDef *dd = arg ? arg->datadef() : NULL;
	if (!dd)
		return error_node("php::print_r: argument has no resolved type",
				  origin);

	// A character POINTER or array is text (PHP's string), not a number —
	// test it before the integer arms, where is_integer() is true for every
	// pointer.
	if (dd->is_cstr()) {
		need_output_extern("__madc_dump_pr_cstr", false,
				   { { {N_CHAR}, true } });
		node_t a = list();
		append(a, translate_expr(arg));
		return node2(N_CALL, id("__madc_dump_pr_cstr", origin), a, origin);
	}

	// Everything that is NOT a scalar, and everything whose scalar-looking
	// predicates would lie, is refused by NAME of the type — never guessed
	// at. is_integer() is true for a pointer (DataDefPTR), for a
	// pointer-to-member, and for a function pointer, and a SIMD vector's
	// is_integer() follows its element: any of those falling into the integer
	// arm below would print an address or a lane as a decimal. An enum's
	// print_r shows the ENUMERATOR name, which needs the enumerator table, so
	// it waits for its own slice rather than shipping as a bare number that a
	// later slice would then change.
	if (dd->is_pointer() || dd->is_reference() || dd->is_member_pointer()
	    || dd->is_struct() || dd->is_object() || dd->is_function()
	    || dd->is_simd() || dd->is_complex()
	    || dynamic_cast<DataDefENUM *>(dd) != NULL
	    || dynamic_cast<DataDefCArray *>(dd) != NULL)
		return error_node((std::string("php::print_r: no dumper for type '")
				   + dd->name + "' yet — scalars and C strings"
				   " only so far").c_str(), origin);

	if (dd->rawtype() == DataType::dtBOOL) {
		need_output_extern("__madc_dump_pr_bool", false,
				   { { {N_INT}, false } });
		node_t a = list();
		append(a, translate_expr(arg));
		return node2(N_CALL, id("__madc_dump_pr_bool", origin), a, origin);
	}

	// A char is one character of text. dtCHAR IS dtINT8 (and uint8_t is
	// dtBYTE) — the same aliasing C++ itself has, where `cout << (int8_t)65`
	// prints 'A'. Rendering the byte matches that and matches what a PHP
	// developer sees from chr(65).
	if (dd->rawtype() == DataType::dtINT8 || dd->rawtype() == DataType::dtUINT8) {
		need_output_extern("__madc_dump_pr_char", false,
				   { { {N_INT}, false } });
		node_t a = list();
		append(a, translate_expr(arg));
		return node2(N_CALL, id("__madc_dump_pr_char", origin), a, origin);
	}

	if (dd->is_real()) {
		// long double narrows to double: PHP has no wider type, and the
		// 14-significant-digit format cannot show the difference.
		need_output_extern("__madc_dump_pr_f64", false,
				   { { {N_DOUBLE}, false } });
		node_t a = list();
		append(a, translate_expr(arg));
		return node2(N_CALL, id("__madc_dump_pr_f64", origin), a, origin);
	}

	if (dd->is_integer()) {
		need_output_extern("__madc_dump_pr_i64", false,
				   { { {N_LONG, N_LONG}, false },
				     { {N_INT}, false } });
		node_t a = list();
		append(a, translate_expr(arg));
		append(a, integer(dd->is_unsigned() ? 1 : 0, origin));
		return node2(N_CALL, id("__madc_dump_pr_i64", origin), a, origin);
	}

	return error_node((std::string("php::print_r: no dumper for type '")
			   + dd->name + "' yet").c_str(), origin);
}

// ---------------------------------------------------------------------------
// The call intercept
// ---------------------------------------------------------------------------
// Returns NULL when the callee is not a dump intrinsic, so translate_expr's
// ordinary call path continues untouched.
node_t CirBuilder::lower_dump_call(TokenCallFunc *tcf, FuncDef *fd,
				   TokenBase *origin)
{
	if (!tcf)
		return NULL;
	DumpFlavor flavor = dump_flavor(fd);
	if (flavor == dfNone)
		// The RESOLVED callee may be absent: a declared-only template's
		// placeholder FuncDef carries no parameters, so overload ranking
		// can filter it out of a 1-argument call. The intrinsic tag lives
		// on the DECLARATION the call token is bound to, which is that
		// placeholder either way.
		flavor = dump_flavor(dynamic_cast<FuncDef *>(tcf->var.type));
	switch (flavor) {
	case dfPrintR:
		// PHP's print_r($v) — the second ($return) argument is a separate
		// slice (it returns the text as a madc::value).
		if (tcf->parameters.size() != 1)
			return error_node("php::print_r takes one argument",
					  origin);
		return dump_print_r_value(tcf->parameters[0], origin);
	case dfNone:
		break;
	}
	return NULL;
}
