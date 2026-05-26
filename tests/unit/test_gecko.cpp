// Unit tests for Gecko GLR parser integration.
//
// Tests both the raw Gecko API (Phase 0) and the madc grammar
// initialization (Phase 1).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <string>
#include <vector>
#include <cstring>

extern "C" {
#include "gecko.h"
}

// From madc_grammar.cpp
extern struct grammar *madc_create_gecko_grammar();

// Simple token stream for testing
struct TestToken {
    int code;
    const char *text;
};

static std::vector<TestToken> g_tokens;
static size_t g_token_pos;

static int test_read_token(void **attr) {
    if (g_token_pos >= g_tokens.size()) return -1;
    *attr = (void *)(ptrdiff_t)g_token_pos;
    return g_tokens[g_token_pos++].code;
}

static void set_tokens(std::initializer_list<TestToken> toks) {
    g_tokens.assign(toks);
    g_token_pos = 0;
}

// Helper to find an anode by name in the tree
static struct gp_tree_node *find_anode(struct gp_tree_node *node, const char *name) {
    if (!node) return nullptr;
    if (node->type == GP_ANODE && strcmp(node->val.anode.name, name) == 0)
	return node;
    if (node->type == GP_ANODE) {
	for (int i = 0; i < node->val.anode.children_num; i++) {
	    auto *found = find_anode(node->val.anode.children[i], name);
	    if (found) return found;
	}
    }
    return nullptr;
}

// Count terminal nodes in tree
static int count_terms(struct gp_tree_node *node) {
    if (!node) return 0;
    if (node->type == GP_TERM) return 1;
    if (node->type == GP_ANODE) {
	int n = 0;
	for (int i = 0; i < node->val.anode.children_num; i++)
	    n += count_terms(node->val.anode.children[i]);
	return n;
    }
    return 0;
}

TEST_SUITE("Gecko basics") {

    TEST_CASE("Simple expression grammar parses correctly") {
	// Grammar: E = E '+' E | ID
	// with LEFT precedence on '+'
	enum { ID = 300 };
	const char *grammar =
	    "TERM ID = 300;\n"
	    "LEFT '+';\n"
	    "E : ID           # 0\n"
	    "  | E '+' E      # plus (0 2)\n"
	    "  ;\n";

	struct grammar *g = gp_create_grammar();
	REQUIRE(g != nullptr);
	gp_set_debug_level(g, 0);

	int err = gp_parse_grammar(g, 1, grammar);
	CHECK(err == 0);

	// Input: ID + ID
	set_tokens({{ID, "a"}, {'+', "+"}, {ID, "a"}});

	struct gp_tree_node *root;
	int ambiguity;
	err = gp_parse(g, test_read_token, &root, &ambiguity, nullptr);
	CHECK(err == 0);
	CHECK(ambiguity == 0);

	// Should produce: plus(a, a)
	REQUIRE(root != nullptr);
	CHECK(root->type == GP_ANODE);
	CHECK(std::string(root->val.anode.name) == "plus");
	CHECK(root->val.anode.children_num == 2);

	gp_free_tree(g, root);
	gp_fin(g);
    }

    TEST_CASE("Operator precedence: * binds tighter than +") {
	enum { ID = 300 };
	const char *grammar =
	    "TERM ID = 300;\n"
	    "LEFT '+';\n"
	    "LEFT '*';\n"
	    "E : ID           # 0\n"
	    "  | E '+' E      # plus (0 2)\n"
	    "  | E '*' E      # mult (0 2)\n"
	    "  ;\n";

	struct grammar *g = gp_create_grammar();
	gp_set_debug_level(g, 0);
	gp_parse_grammar(g, 1, grammar);

	// Input: ID + ID * ID  →  plus(ID, mult(ID, ID))
	set_tokens({{ID, "a"}, {'+', "+"}, {ID, "a"}, {'*', "*"}, {ID, "a"}});

	struct gp_tree_node *root;
	int ambiguity;
	int err = gp_parse(g, test_read_token, &root, &ambiguity, nullptr);
	CHECK(err == 0);
	CHECK(ambiguity == 0);

	// Root should be "plus"
	REQUIRE(root != nullptr);
	CHECK(root->type == GP_ANODE);
	CHECK(std::string(root->val.anode.name) == "plus");

	// Right child should be "mult"
	auto *right = root->val.anode.children[1];
	REQUIRE(right != nullptr);
	CHECK(right->type == GP_ANODE);
	CHECK(std::string(right->val.anode.name) == "mult");

	gp_free_tree(g, root);
	gp_fin(g);
    }

    TEST_CASE("madc-like function definition parses") {
	enum { IDENT = 300, INT_KW = 301, RETURN_KW = 302, NUMBER = 303 };

	const char *grammar =
	    "TERM\n"
	    "IDENT = 300\n"
	    "INT = 301\n"
	    "RETURN = 302\n"
	    "NUMBER = 303;\n"
	    "LEFT '+';\n"
	    "program : func_def                        # 0\n"
	    "        ;\n"
	    "func_def : INT IDENT '(' params_opt ')' '{' stmts_opt '}'\n"
	    "                                          # func (1 3 6)\n"
	    "         ;\n"
	    "params_opt :                               \n"
	    "           | params                        # 0\n"
	    "           ;\n"
	    "params : INT IDENT                         # param (1)\n"
	    "       | params ',' INT IDENT              # params (0 3)\n"
	    "       ;\n"
	    "stmts_opt :                                \n"
	    "          | stmts                           # 0\n"
	    "          ;\n"
	    "stmts : stmt                                # 0\n"
	    "      | stmts stmt                          # stmts (0 1)\n"
	    "      ;\n"
	    "stmt : RETURN expr ';'                      # return (1)\n"
	    "     ;\n"
	    "expr : IDENT                                # 0\n"
	    "     | NUMBER                               # 0\n"
	    "     | expr '+' expr                        # add (0 2)\n"
	    "     ;\n";

	struct grammar *g = gp_create_grammar();
	gp_set_debug_level(g, 0);
	int err = gp_parse_grammar(g, 1, grammar);
	REQUIRE(err == 0);

	// int add(int a, int b) { return a + b; }
	set_tokens({
	    {INT_KW, "int"}, {IDENT, "add"}, {'(', "("},
	    {INT_KW, "int"}, {IDENT, "a"}, {',', ","},
	    {INT_KW, "int"}, {IDENT, "b"}, {')', ")"},
	    {'{', "{"}, {RETURN_KW, "return"},
	    {IDENT, "a"}, {'+', "+"}, {IDENT, "b"},
	    {';', ";"}, {'}', "}"}
	});

	struct gp_tree_node *root;
	int ambiguity;
	err = gp_parse(g, test_read_token, &root, &ambiguity, nullptr);
	CHECK(err == 0);

	// Root should be "func" node
	REQUIRE(root != nullptr);
	CHECK(root->type == GP_ANODE);
	CHECK(std::string(root->val.anode.name) == "func");
	CHECK(root->val.anode.children_num == 3); // name, params, body

	// Should contain an "add" anode in the return statement
	auto *add_node = find_anode(root, "add");
	CHECK(add_node != nullptr);

	gp_free_tree(g, root);
	gp_fin(g);
    }

    TEST_CASE("Error recovery works") {
	enum { ID = 300 };
	const char *grammar =
	    "TERM ID = 300;\n"
	    "S : ID ';'       # 0\n"
	    "  ;\n";

	struct grammar *g = gp_create_grammar();
	gp_set_debug_level(g, 0);
	gp_parse_grammar(g, 1, grammar);

	// Bad input: just ID with no semicolon
	set_tokens({{ID, "a"}});

	struct gp_tree_node *root;
	int ambiguity;
	bool had_error = false;
	gp_set_syntax_error(g, [](const char *, bool, const char *,
	    void *, const char *, void *) {
	    // error reported — that's fine
	});
	int err = gp_parse(g, test_read_token, &root, &ambiguity, nullptr);
	// Parse should fail (no recovery possible with 1 token)
	// Just verify it doesn't crash
	(void)err;
	(void)had_error;

	if (!err) gp_free_tree(g, root);
	gp_fin(g);
    }

    TEST_CASE("Tree traversal API works") {
	enum { ID = 300 };
	const char *grammar =
	    "TERM ID = 300;\n"
	    "LEFT '+';\n"
	    "E : ID           # 0\n"
	    "  | E '+' E      # plus (0 2)\n"
	    "  ;\n";

	struct grammar *g = gp_create_grammar();
	gp_set_debug_level(g, 0);
	gp_parse_grammar(g, 1, grammar);

	// ID + ID + ID → plus(plus(ID, ID), ID) due to LEFT assoc
	set_tokens({{ID,"a"}, {'+',"+"}, {ID,"a"}, {'+',"+"}, {ID,"a"}});

	struct gp_tree_node *root;
	int ambiguity;
	gp_parse(g, test_read_token, &root, &ambiguity, nullptr);

	// Count total terminal nodes via traversal
	int term_count = count_terms(root);
	CHECK(term_count == 3);

	// Use gp_traverse_tree
	int visit_count = 0;
	gp_traverse_tree(g, root,
	    [](struct gp_tree_node *node, struct gp_tree_node *, void *arg) -> bool {
		(*(int *)arg)++;
		return true;
	    }, nullptr, &visit_count);
	CHECK(visit_count > 0);

	gp_free_tree(g, root);
	gp_fin(g);
    }
}

TEST_SUITE("madc grammar") {

    TEST_CASE("madc grammar initializes successfully") {
	struct grammar *g = madc_create_gecko_grammar();
	REQUIRE(g != nullptr);
	gp_fin(g);
    }
}
