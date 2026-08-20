// Unit tests for the std::format engine (src/rt/rt_format.c) — the runtime
// half of the std::print / std::println / std::format intrinsic arc
// (docs/plans/2026-08-19-std-print-format-intrinsics.md).
//
// THE ORACLE IS THE FIXTURE. Every row of rt_format_oracle.inc is one
// (kind, value, spec) cell captured from the REAL libstdc++ std::vformat by
// scripts/gen_format_oracle.cpp (g++ -std=c++23, GCC 13.3) — never retyped
// from memory. The engine must reproduce each cell byte-for-byte into the
// dump runtime's capture sink. 1400+ cells cover: integer presentations
// (d/b/B/o/x/X/c) with sign/alt/zero/width/fill/align, the float family
// (shortest-round-trip default, f/e/g/a and uppercase twins, inf/nan
// padding), byte-string width/precision, char/bool routing, and pointers.
//
// The grammar (field iterator + spec parser) gets its own direct cases,
// including the error paths the compile-time checker turns into diagnostics.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

// Engine contracts are internal, not public API — relative includes, exactly
// like test_dump_value.cpp does for the dump runtime.
#include "../../src/rt/rt_dump.h"
#include "../../src/rt/rt_format.h"

enum {
	FMTORC_I64, FMTORC_U64, FMTORC_F64, FMTORC_STR,
	FMTORC_CHAR, FMTORC_BOOL, FMTORC_PTR
};

struct fmt_oracle_row {
	int kind;
	const char *spec;
	unsigned long long bits;
	const char *str;
	const char *expect;
	size_t expect_n;	// expect can hold an embedded NUL ("{:5c}" of 0)
};

static const fmt_oracle_row fmt_oracle_rows[] = {
#include "rt_format_oracle.inc"
};

TEST_CASE("rt_format reproduces every libstdc++ oracle cell") {
	const size_t nrows = sizeof(fmt_oracle_rows) / sizeof(*fmt_oracle_rows);
	for ( size_t i = 0; i < nrows; ++i )
	{
		const fmt_oracle_row &r = fmt_oracle_rows[i];
		void *sink = __madc_dump_sink_open();
		REQUIRE(sink != nullptr);
		long long sn = (long long)strlen(r.spec);
		switch ( r.kind )
		{
		case FMTORC_I64:
			__madc_fmt_i64(sink, r.spec, sn, (long long)r.bits);
			break;
		case FMTORC_U64:
			__madc_fmt_u64(sink, r.spec, sn, r.bits);
			break;
		case FMTORC_F64: {
			double v;
			std::memcpy(&v, &r.bits, sizeof v);
			__madc_fmt_f64(sink, r.spec, sn, v);
			break;
		}
		case FMTORC_STR:
			__madc_fmt_str_n(sink, r.spec, sn, r.str,
					 (long long)strlen(r.str));
			break;
		case FMTORC_CHAR:
			__madc_fmt_char(sink, r.spec, sn, (int)r.bits);
			break;
		case FMTORC_BOOL:
			__madc_fmt_bool(sink, r.spec, sn, (int)r.bits);
			break;
		case FMTORC_PTR:
			__madc_fmt_ptr(sink, r.spec, sn,
				       (const void *)(uintptr_t)r.bits);
			break;
		}
		CHECK(!__madc_dump_sink_failed(sink));
		std::string got(__madc_dump_sink_text(sink),
				__madc_dump_sink_length(sink));
		INFO("row " << i << " kind " << r.kind
		     << " spec \"" << r.spec << "\"");
		CHECK(got == std::string(r.expect, r.expect_n));
		__madc_dump_sink_close(sink);
	}
}

// ---------------------------------------------------------------------------
// the format-string iterator
// ---------------------------------------------------------------------------

struct walked_item {
	int kind;
	std::string text;	// TEXT: the run
	int arg_id;		// FIELD
	std::string spec;	// FIELD
};

static bool walk(const char *fmt, std::vector<walked_item> &out,
		 const char **err)
{
	long long n = (long long)strlen(fmt), pos = 0;
	*err = nullptr;
	for ( ;; )
	{
		madc_fmt_item it;
		long long next = __madc_fmt_next(fmt, n, pos, &it, err);
		if ( next == -1 )
			return true;
		if ( next == -2 )
			return false;
		walked_item w;
		w.kind = it.kind;
		w.arg_id = it.arg_id;
		if ( it.kind == MADC_FMT_TEXT )
			w.text.assign(it.text, (size_t)it.text_n);
		else
			w.spec.assign(it.spec ? it.spec : "",
				      (size_t)it.spec_n);
		out.push_back(w);
		pos = next;
	}
}

TEST_CASE("format-string iterator: runs, fields, escapes") {
	std::vector<walked_item> items;
	const char *err = nullptr;
	REQUIRE(walk("a{}b{{x}}{0:>3} {1:*^8.2f}!", items, &err));
	REQUIRE(items.size() == 10);
	CHECK(items[0].text == "a");
	CHECK(items[1].kind == MADC_FMT_FIELD);
	CHECK(items[1].arg_id == -1);
	CHECK(items[1].spec == "");
	CHECK(items[2].text == "b");
	CHECK(items[3].text == "{");	// "{{" collapsed
	CHECK(items[4].text == "x");
	CHECK(items[5].text == "}");	// "}}" collapsed
	CHECK(items[6].arg_id == 0);
	CHECK(items[6].spec == ">3");
	CHECK(items[7].text == " ");
	CHECK(items[8].arg_id == 1);
	CHECK(items[8].spec == "*^8.2f");
	CHECK(items[9].text == "!");
}

TEST_CASE("format-string iterator: malformed strings are loud") {
	std::vector<walked_item> items;
	const char *err = nullptr;
	CHECK(!walk("a } b", items, &err));
	CHECK(err != nullptr);
	items.clear();
	CHECK(!walk("tail {", items, &err));
	CHECK(err != nullptr);
	items.clear();
	CHECK(!walk("{:d", items, &err));
	CHECK(err != nullptr);
}

// ---------------------------------------------------------------------------
// the spec parser's error paths (the compile-time diagnostics)
// ---------------------------------------------------------------------------

static const char *parse(const char *spec, madc_fmt_spec &s)
{
	return __madc_fmt_parse_spec(spec, (long long)strlen(spec), &s);
}

TEST_CASE("format-spec parser: shapes and diagnostics") {
	madc_fmt_spec s;
	CHECK(parse("", s) == nullptr);
	CHECK(parse("*^8.2f", s) == nullptr);
	CHECK(s.fill == '*');
	CHECK(s.align == '^');
	CHECK(s.width == 8);
	CHECK(s.precision == 2);
	CHECK(s.type == 'f');
	CHECK(parse("+#010X", s) == nullptr);
	CHECK(s.sign == '+');
	CHECK(s.alt == 1);
	CHECK(s.zero == 1);
	CHECK(s.width == 10);
	CHECK(s.type == 'X');
	CHECK(parse("-8d", s) == nullptr);	// '-' is the default sign
	CHECK(s.sign == 0);
	CHECK(s.width == 8);
	// diagnostics — the exact strings the compiler shows
	CHECK(parse("{<5", s) != nullptr);	// brace fill
	CHECK(parse("{}d", s) != nullptr);	// width-from-argument
	CHECK(parse(".", s) != nullptr);	// missing precision
	CHECK(parse("Ld", s) != nullptr);	// locale form
	CHECK(parse("5dx", s) != nullptr);	// trailing bytes
	CHECK(parse("999999999", s) != nullptr);	// width cap
}
