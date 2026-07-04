/* test_cir_freeze.cpp — B2: freeze/thaw a cir_node sub-DAG through the
 * madc::dis snapshot container (forest Phase 2).
 *
 * Pins: payload-class round-trips, share dedup (one record, one materialized
 * node), cycle termination, >800-deep chains (iterative walkers), container
 * placement round-trip, resolve-on-touch laziness, and the end-to-end oracle:
 * a thawed module tree is structurally identical to the original AND compiles
 * and runs to the same result through the production cir_compile path.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <dlfcn.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <fstream>

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir/c2mir.h"
#include "c2mir/c2mir_api.h"
}

#include "../src/madc_cir.h"
#include "../src/cir_builder.h"
#include "../src/cir_freeze.h"

// Import resolver for MIR_link (mirrors test_cir.cpp).
static void *freeze_test_import_resolver(const char *name) {
	return dlsym(RTLD_DEFAULT, name);
}

// B2 tests that build synthetic trees (no Program/parse) still intern string
// payloads at freeze — bind the file-local substrate (same discipline as
// test_cir.cpp's cir_test_bind_substrate).
static void freeze_test_bind_substrate()
{
	static madc::dis::intern_table strpool;
	static madc::dis::id_table<DataDef> types(MADC_TYPEID_PROJECT_BASE);
	madc_stamp_primitive_type_ids();
	TokenBase::_active_strpool = &strpool;
	madc_active_project_types = &types;
}

// Round-trip a blob through an in-memory container image.
static bool container_roundtrip(const cir_frozen_blob &in, cir_frozen_blob &out)
{
	madc::dis::snapshot_writer w;
	if (!cir_freeze_write(in, w, 100))
		return false;
	std::vector<uint8_t> image;
	if (!w.build(image))
		return false;
	madc::dis::snapshot_reader r;
	if (!r.open(image.data(), image.size()))
		return false;
	return cir_freeze_read(r, 100, out);
}

TEST_CASE("B2: every payload class round-trips through freeze/container/thaw") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		CirBuilder b(c2m);
		freeze_test_bind_substrate();

		node_t items = b.list();
		b.append(items, b.integer(-42));               // N_I/N_L family
		b.append(items, b.integer_typed(0xffffffffffULL, (DataDef *)&ddUINT64));
		b.append(items, b.real(3.25));                 // N_D
		b.append(items, b.real_float(1.5f));           // N_F
		b.append(items, b.ch('Q'));                    // N_CH
		b.append(items, b.str("hello\0world", 12));    // embedded NUL, exact len
		b.append(items, b.id("some_identifier"));      // N_ID
		b.append(items, b.ignore());                   // N_IGNORE leaf
		node_t root = b.node1(N_EXPR_SIZEOF, items);
		CIR_NODE(root)->set_datadef((DataDef *)&ddINT64);
		CIR_NODE(root)->set_error_msg("not really an error");
		CIR_NODE(root)->set_typedef_name("ALIAS_T");

		cir_frozen_blob blob;
		REQUIRE(cir_freeze_subtree(CIR_NODE(root), blob));
		CHECK(blob.records.size() == 10);   // root + list + 8 leaves

		cir_frozen_blob loaded;
		REQUIRE(container_roundtrip(blob, loaded));
		CHECK(loaded.records.size() == blob.records.size());
		CHECK(loaded.children == blob.children);
		CHECK(memcmp(loaded.records.data(), blob.records.data(),
			     blob.records.size() * sizeof(cir_frozen_record)) == 0);

		CirFrozenSegment seg(std::move(loaded), c2m);
		CHECK(seg.record_count() == 10);
		CHECK(seg.materialized_count() == 0);          // cold until touched
		cir_node *thawed = seg.node_at(0);
		REQUIRE(thawed != nullptr);
		CHECK(seg.materialized_count() == 10);
		CHECK(thawed->self.seg == seg.seg());
		CHECK(thawed->self.idx == 0);
		CHECK(cir_trees_structurally_identical(root, thawed->as_node()));
		// Same record → same node (memoized identity).
		CHECK(seg.node_at(0) == thawed);
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B2: a shared subtree freezes to ONE record and thaws to ONE node") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		CirBuilder b(c2m);
		freeze_test_bind_substrate();

		// The c2mir sharing pattern: one spec under two arity-1 N_SHARE
		// parents (the second append owns the intrusive links; each
		// parent still reads its single child).
		node_t spec = b.list();
		b.append(spec, b.id("shared_spec"));
		node_t share1 = b.node1(N_SHARE, spec);
		node_t share2 = b.node1(N_SHARE, spec);
		node_t root = b.node2(N_LIST, share1, share2);

		cir_frozen_blob blob;
		REQUIRE(cir_freeze_subtree(CIR_NODE(root), blob));
		// root + share1 + share2 + spec + id — the spec is ONE record.
		CHECK(blob.records.size() == 5);

		CirFrozenSegment seg(std::move(blob), c2m);
		cir_node *thawed = seg.node_at(0);
		REQUIRE(thawed != nullptr);
		node_t t1 = c2mir_node_first_op(thawed->as_node());
		node_t t2 = c2mir_node_next_op(t1);
		REQUIRE(t1 != nullptr);
		REQUIRE(t2 != nullptr);
		// Both thawed shares resolve to the SAME materialized child.
		CHECK(c2mir_node_first_op(t1) == c2mir_node_first_op(t2));
		CHECK(cir_trees_structurally_identical(root, thawed->as_node()));
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B2: a cyclic sub-DAG freezes and thaws without hanging") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		CirBuilder b(c2m);
		freeze_test_bind_substrate();

		// The __max_size_type defense: A -> B -> A.
		node_t a = b.list();
		node_t bb = b.list();
		b.append(a, bb);
		b.append(bb, a);

		cir_frozen_blob blob;
		REQUIRE(cir_freeze_subtree(CIR_NODE(a), blob));
		CHECK(blob.records.size() == 2);

		CirFrozenSegment seg(std::move(blob), c2m);
		cir_node *thawed = seg.node_at(0);
		REQUIRE(thawed != nullptr);
		// The thawed cycle closes onto the memoized nodes.
		node_t tb2 = c2mir_node_first_op(thawed->as_node());
		REQUIRE(tb2 != nullptr);
		CHECK(c2mir_node_first_op(tb2) == thawed->as_node());
		CHECK(cir_trees_structurally_identical(a, thawed->as_node()));
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B2: >800-deep chains survive the iterative walkers") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		CirBuilder b(c2m);
		freeze_test_bind_substrate();

		node_t n = b.integer(7);
		for (int i = 0; i < 10000; ++i)
			n = b.node1(N_ADDR, n);

		cir_frozen_blob blob;
		REQUIRE(cir_freeze_subtree(CIR_NODE(n), blob));
		CHECK(blob.records.size() == 10001);

		CirFrozenSegment seg(std::move(blob), c2m);
		// Resolve-on-touch: touching the LEAF record materializes just it.
		cir_node *leaf = seg.node_at(10000);
		REQUIRE(leaf != nullptr);
		CHECK(seg.materialized_count() == 1);
		cir_node *thawed = seg.node_at(0);
		REQUIRE(thawed != nullptr);
		CHECK(seg.materialized_count() == 10001);
		CHECK(cir_trees_structurally_identical(n, thawed->as_node()));
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B2: file-placement container round-trips a frozen tree") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		CirBuilder b(c2m);
		freeze_test_bind_substrate();

		node_t root = b.node2(N_ADD, b.integer(40), b.integer(2));
		cir_frozen_blob blob;
		REQUIRE(cir_freeze_subtree(CIR_NODE(root), blob));

		madc::dis::snapshot_writer w;
		REQUIRE(cir_freeze_write(blob, w, 7));
		std::string path = std::string("/tmp/madc_cirfreeze_")
				 + std::to_string((long)getpid()) + ".snap";
		std::remove(path.c_str());
		REQUIRE(w.write_file(path.c_str()));

		std::ifstream is(path.c_str(), std::ios::binary);
		REQUIRE(is.good());
		std::vector<uint8_t> image((std::istreambuf_iterator<char>(is)),
					   std::istreambuf_iterator<char>());
		is.close();
		std::remove(path.c_str());

		madc::dis::snapshot_reader r;
		REQUIRE(r.open(image.data(), image.size()));
		cir_frozen_blob loaded;
		REQUIRE(cir_freeze_read(r, 7, loaded));

		CirFrozenSegment seg(std::move(loaded), c2m);
		cir_node *thawed = seg.node_at(0);
		REQUIRE(thawed != nullptr);
		CHECK(cir_trees_structurally_identical(root, thawed->as_node()));
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B2: a thawed module tree is identical AND compiles + runs (the Phase-2 oracle)") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		auto prog = std::make_shared<Program>();
		TokenProgram *tp = prog->tokenize_buffer(
			"int mul(int a, int b) { return a * b; }\n"
			"int main() { int x = 6; int y = 7; return mul(x, y); }\n",
			"<freeze_test>");
		REQUIRE(tp != nullptr);
		REQUIRE(prog->parse(tp));

		CirBuilder builder(c2m);
		node_t tree = builder.translate_module(prog.get());
		REQUIRE(tree != nullptr);

		// Freeze the whole module tree, round-trip the container image,
		// thaw, and compare BEFORE anything compiles (c2mir's checker
		// writes attr scratch into whatever tree it compiles).
		cir_frozen_blob blob;
		REQUIRE(cir_freeze_subtree(CIR_NODE(tree), blob));
		cir_frozen_blob loaded;
		REQUIRE(container_roundtrip(blob, loaded));

		CirFrozenSegment seg(std::move(loaded), c2m);
		cir_node *thawed = seg.node_at(0);
		REQUIRE(thawed != nullptr);
		CHECK(cir_trees_structurally_identical(tree, thawed->as_node()));

		// Compile and run the THAWED tree through the production path.
		int ok = cir_compile(mir_ctx, c2m, thawed->as_node(), "thawed_mod");
		REQUIRE(ok == 1);
		MIR_module_t mod = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(mir_ctx));
		MIR_load_module(mir_ctx, mod);
		MIR_link(mir_ctx, MIR_set_interp_interface, freeze_test_import_resolver);
		MIR_item_t func_item = NULL;
		for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
		     item != NULL; item = DLIST_NEXT(MIR_item_t, item))
			if (item->item_type == MIR_func_item
			    && strcmp(item->u.func->name, "main") == 0)
				func_item = item;
		REQUIRE(func_item != nullptr);
		MIR_val_t val;
		MIR_interp(mir_ctx, func_item, &val, 0);
		CHECK(val.i == 42);
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}
