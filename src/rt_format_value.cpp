/* rt_format_value.cpp — the C++ half of the std::format engine.
 *
 * Three entries the strict-C11 lane (src/rt/rt_format.c) cannot hold, on the
 * exact split rt_dump_value.cpp documents: they touch std::string and
 * madc::value, so a program reaching them already needs the C++ script
 * runtime, while ordinary C arguments keep formatting from the pure-C11
 * ledger lane.
 *
 *   __madc_fmt_stdstring  a std::string argument's bytes
 *   __madc_fmt_value      a madc::value argument — the ONE runtime dispatch
 *                         in the whole arc, because a value's kind is a
 *                         property of the value, not of the type
 *   __madc_fmt_take       std::format's result hand-off into the caller's
 *                         std::string temporary (and the sink close) —
 *                         the strict-C++ shape, kept for hosts
 *   __madc_fmt_take_cstr  dialect std::format's hand-off into the shared
 *                         text ring (the c_str() contract; the fragment
 *                         declares `const char *format(...)`)
 *
 * The value dispatch follows the `cout << value` contract (owner ruling
 * 2026-08-19): each kind formats EXACTLY as the contained type would — the
 * spec is passed through to the contained kind's primitive, so
 * std::println("{:x}", v) of an integer-kind value hex-formats it. null
 * formats as nothing; array/object/bytes/instance are not formattable
 * (C++ gives std::vector no formatter either; the kind is only known at run
 * time, so this is the loud marker, never silence).
 */

#include <string>

#include "libmadc/value.h"
#include "ns_common.h"
#include "rt/rt_dump.h"
#include "rt/rt_format.h"

extern "C" void __madc_fmt_stdstring(void *sink, const char *spec,
				     long long spec_n, const void *sp)
{
	const std::string *s = static_cast<const std::string *>(sp);
	__madc_fmt_str_n(sink, spec, spec_n, s->data(), (long long)s->size());
}

extern "C" void __madc_fmt_take(void *strp, void *sink)
{
	std::string *s = static_cast<std::string *>(strp);
	s->assign(__madc_dump_sink_text(sink), __madc_dump_sink_length(sink));
	__madc_dump_sink_close(sink);
}

extern "C" const char *__madc_fmt_take_cstr(void *sink)
{
	std::string &slot = ns_common::ring_slot();
	slot.assign(__madc_dump_sink_text(sink), __madc_dump_sink_length(sink));
	__madc_dump_sink_close(sink);
	return slot.c_str();
}

extern "C" void __madc_fmt_value(void *sink, const char *spec,
				 long long spec_n, const void *vp)
{
	const madc::value *v = static_cast<const madc::value *>(vp);
	switch ( v->type() )
	{
	case madc::value::kind::null:
		return;		// null streams nothing (the cout contract)
	case madc::value::kind::boolean:
		__madc_fmt_bool(sink, spec, spec_n, v->as_boolean() ? 1 : 0);
		return;
	case madc::value::kind::integer:
		__madc_fmt_i64(sink, spec, spec_n, v->as_integer());
		return;
	case madc::value::kind::real:
		__madc_fmt_f64(sink, spec, spec_n, v->as_real());
		return;
	case madc::value::kind::string:
		__madc_fmt_str_n(sink, spec, spec_n,
				 (const char *)v->data(),
				 (long long)v->size());
		return;
	default:
		break;
	}
	{
		static const char what[] =
			"[madc format failed: value kind not formattable]";
		__madc_fmt_text(sink, what, (long long)(sizeof what - 1));
	}
}
