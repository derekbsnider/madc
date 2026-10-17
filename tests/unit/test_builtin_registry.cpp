// The dialect is builtin (docs/plans/2026-10-17-dialect-builtin-registration.md,
// R1): the generated registration registers exactly what a live parse of the
// dialect files registers, field by field (builtin_function_line) — the
// fidelity gate the generator answers to.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_builtin_registry.h"

// An empty program (the pack bind off, as the dialect's text parse runs).
static std::shared_ptr<Program> parse_empty(const char *tag)
{
	std::shared_ptr<Program> prog = std::make_shared<Program>();
	prog->registration_policy.enable_forest_bind = false;
	prog->forest_bind_path.clear();
	TokenProgram *tp = prog->tokenize_buffer(madc::dis::istring("int main() { return 0; }\n"),
						 madc::dis::istring(tag));
	REQUIRE(tp != nullptr);
	REQUIRE(prog->parse(tp));
	return prog;
}

TEST_CASE("R1: the generated registration registers what a live parse of the dialect files does") {
	DialectTextParse dp;
	std::string err;
	REQUIRE(parse_dialect_as_text(dp, err));
	REQUIRE(dp.function_keys.size() > 700u);
	std::vector<BuiltinGroup> groups;
	plan_builtin_registry(dp, groups);

	// The generated side: every builder of the table, into a program that
	// parsed nothing of the dialect.
	REQUIRE(madc::builtins::dialect_table_count > 0u);
	std::shared_ptr<Program> gen = parse_empty("<gen>");
	for (size_t i = 0; i < madc::builtins::dialect_table_count; ++i)
		madc::builtins::dialect_table[i].build(*gen);

	size_t compared = 0, differ = 0, skipped = 0;
	std::map<std::string, size_t> skip_reasons;
	for (size_t g = 0; g < groups.size(); ++g) {
		if (!groups[g].skip.empty()) {
			skipped += groups[g].keys.size();
			++skip_reasons[groups[g].skip];
			continue;
		}
		for (size_t k = 0; k < groups[g].keys.size(); ++k) {
			madc::dis::istring want = builtin_function_line(*dp.live, groups[g].keys[k]);
			madc::dis::istring got = builtin_function_line(*gen, groups[g].keys[k]);
			++compared;
			if (want != got) {
				if (differ < 20)
					MESSAGE("live: " << want.c_str() << "\ngen:  " << got.c_str());
				++differ;
			}
		}
	}
	for (std::map<std::string, size_t>::const_iterator it = skip_reasons.begin();
	     it != skip_reasons.end(); ++it)
		MESSAGE("not yet built: " << it->second << " name(s) — " << it->first);
	MESSAGE("compared " << compared << " declaration(s); " << skipped << " not yet built");
	CHECK(compared > 0u);
	CHECK(differ == 0u);
}
