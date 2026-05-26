// Integration test: tokenize a madc source string with the real lexer,
// then feed the token stream to Gecko and verify it parses.
//
// This is the Phase 1 validation — proving the tokenizer adapter works
// with real madc code, not just hand-crafted token sequences.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <deque>
#include <queue>
#include <stack>
#include <stdint.h>
#include <asmjit/x86.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

extern "C" {
#include "gecko.h"
}

// From madc_tokenizer.cpp
extern struct gp_tree_node *madc_gecko_parse(std::deque<TokenBase *> *tokens,
					     int *out_ambiguity);
extern void madc_gecko_free_tree(struct gp_tree_node *root);

// From madc_grammar.cpp
extern int madc_token_to_gecko(TokenBase *tb);

// Tokenize a source string using madc's real lexer.
// The Program must stay alive while tokens are in use (they
// point into Program-owned storage for string data).
static Program *g_pgm = nullptr;

static std::deque<TokenBase *> *tokenize_source(const char *source)
{
    delete g_pgm;
    g_pgm = new Program();
    g_pgm->tokenize_buffer(std::string(source), "<test>");
    return &g_pgm->tokens;
}

// Count nodes in a Gecko tree
static int count_anodes(struct gp_tree_node *node)
{
    if (!node) return 0;
    if (node->type == GP_ANODE) {
	int n = 1;
	for (int i = 0; i < node->val.anode.children_num; i++)
	    n += count_anodes(node->val.anode.children[i]);
	return n;
    }
    return 0;
}

TEST_SUITE("Gecko parse integration") {

    TEST_CASE("Simple: int main() { return 0; }") {
	auto *tokens = tokenize_source("int main() { return 0; }");
	CHECK(tokens->size() > 0);

	// Verify token mapping works
	bool has_int = false, has_ident = false, has_return = false;
	for (auto *t : *tokens) {
	    int code = madc_token_to_gecko(t);
	    if (code == 314) has_int = true;     // GT_INT
	    if (code == 256) has_ident = true;   // GT_IDENT
	    if (code == 266) has_return = true;  // GT_RETURN
	}
	CHECK(has_int);
	CHECK(has_ident);
	CHECK(has_return);

	int ambiguity = 0;
	auto *root = madc_gecko_parse(tokens, &ambiguity);
	CHECK(root != nullptr);
	if (root) {
	    CHECK(count_anodes(root) > 0);
	    madc_gecko_free_tree(root);
	}
    }

    TEST_CASE("Function with arithmetic") {
	auto *tokens = tokenize_source(
	    "int add(int a, int b) { return a + b; }\n"
	    "int main() { return add(3, 4); }\n"
	);
	int ambiguity = 0;
	auto *root = madc_gecko_parse(tokens, &ambiguity);
	CHECK(root != nullptr);
	if (root) madc_gecko_free_tree(root);
    }

    TEST_CASE("Variable declarations and assignments") {
	auto *tokens = tokenize_source(
	    "int main() {\n"
	    "    int x = 42;\n"
	    "    int y = x + 1;\n"
	    "    double d = 3.14;\n"
	    "    x = y * 2;\n"
	    "    return x;\n"
	    "}\n"
	);
	int ambiguity = 0;
	auto *root = madc_gecko_parse(tokens, &ambiguity);
	CHECK(root != nullptr);
	if (root) madc_gecko_free_tree(root);
    }

    TEST_CASE("If/else and while") {
	auto *tokens = tokenize_source(
	    "int main() {\n"
	    "    int x = 10;\n"
	    "    if (x > 5) {\n"
	    "        x = 1;\n"
	    "    } else {\n"
	    "        x = 0;\n"
	    "    }\n"
	    "    while (x < 100) {\n"
	    "        x = x + 1;\n"
	    "    }\n"
	    "    return x;\n"
	    "}\n"
	);
	int ambiguity = 0;
	auto *root = madc_gecko_parse(tokens, &ambiguity);
	CHECK(root != nullptr);
	if (root) madc_gecko_free_tree(root);
    }

    TEST_CASE("For loop") {
	auto *tokens = tokenize_source(
	    "int main() {\n"
	    "    int sum = 0;\n"
	    "    for (int i = 0; i < 10; i++) {\n"
	    "        sum += i;\n"
	    "    }\n"
	    "    return sum;\n"
	    "}\n"
	);
	int ambiguity = 0;
	auto *root = madc_gecko_parse(tokens, &ambiguity);
	CHECK(root != nullptr);
	if (root) madc_gecko_free_tree(root);
    }

    TEST_CASE("Struct definition and access") {
	auto *tokens = tokenize_source(
	    "struct Point { int x; int y; };\n"
	    "int main() {\n"
	    "    struct Point p;\n"
	    "    p.x = 10;\n"
	    "    p.y = 20;\n"
	    "    return p.x + p.y;\n"
	    "}\n"
	);
	int ambiguity = 0;
	auto *root = madc_gecko_parse(tokens, &ambiguity);
	CHECK(root != nullptr);
	if (root) madc_gecko_free_tree(root);
    }

    TEST_CASE("Pointer operations") {
	auto *tokens = tokenize_source(
	    "int main() {\n"
	    "    int x = 42;\n"
	    "    int *p = &x;\n"
	    "    *p = 100;\n"
	    "    return x;\n"
	    "}\n"
	);
	int ambiguity = 0;
	auto *root = madc_gecko_parse(tokens, &ambiguity);
	CHECK(root != nullptr);
	if (root) madc_gecko_free_tree(root);
    }
}
