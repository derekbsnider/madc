/* test_cir_freeze.cpp — B2+B3: freeze/thaw a cir_node sub-DAG through the
 * madc::dis snapshot container (forest Phases 2+3).
 *
 * B2 pins: payload-class round-trips, share dedup (one record, one
 * materialized node), cycle termination, >800-deep chains (iterative
 * walkers), container placement round-trip, resolve-on-touch laziness, and
 * the end-to-end oracle: a thawed module tree is structurally identical to
 * the original AND compiles and runs to the same result through the
 * production cir_compile path.
 *
 * B3 pins: per-unit partitioning + connectors (a two-file program freezes
 * to two units, thaws identical, compiles + runs), on-demand unit loading
 * (touching one unit leaves the others cold), the cross-process closure
 * (a FRESH live string pool — the freezing pool's ids never consulted —
 * still thaws correct string payloads, accessor strings, and positions from
 * the container's own pool), the context-hash pin (mismatch rejects), and
 * the directory round-trip (unit names, libs, typeid->name closure).
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

// Build a forest container image in memory. `hash_override` (nonzero)
// re-stamps the context hash AFTER cir_forest_write set the real one —
// the pin-mismatch case. The image must outlive any forest opened over it.
static bool forest_image(const cir_frozen_forest &f, std::vector<uint8_t> &image,
			 uint64_t hash_override = 0)
{
	madc::dis::snapshot_writer w;
	if (!cir_forest_write(f, w))
		return false;
	if (hash_override)
		w.set_context_hash(hash_override);
	return w.build(image);
}

// Parse + translate a two-file program (main + an #include'd helper file on
// disk), so the module tree carries origins from two source files — the
// per-unit partition case. Caller owns the returned builder and the Program.
static node_t build_two_file_module(std::shared_ptr<Program> &prog,
				    CirBuilder *&builder, c2m_ctx_t c2m,
				    std::string &main_path)
{
	std::string inc_path = std::string("/tmp/madc_forest_inc_")
			     + std::to_string((long)getpid()) + ".h";
	main_path = std::string("/tmp/madc_forest_main_")
		  + std::to_string((long)getpid()) + ".mad";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "int helper(int v) { return v + 100; }\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { return helper(1); }\n";
	}
	prog = std::make_shared<Program>();
	TokenProgram *tp = prog->tokenize(main_path.c_str());
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());
	if (!tp || !prog->parse(tp))
		return NULL;
	builder = new CirBuilder(c2m);
	return builder->translate_module(prog.get());
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

TEST_CASE("B3: a two-file program partitions into units, thaws identical, and runs") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		std::shared_ptr<Program> prog;
		CirBuilder *builder = NULL;
		std::string main_path;
		node_t tree = build_two_file_module(prog, builder, c2m, main_path);
		REQUIRE(tree != nullptr);

		cir_frozen_forest f;
		REQUIRE(cir_freeze_forest(CIR_NODE(tree), main_path.c_str(), f));
		// Two source files -> (at least) two units, and the child
		// references crossing them are connectors.
		REQUIRE(f.units.size() >= 2);
		CHECK(f.root_unit == 0);	// unit 0 is the main unit
		size_t total_conn = 0;
		for (size_t u = 0; u < f.units.size(); ++u) {
			total_conn += f.units[u].connectors.size();
			// positions ride parallel to records in every unit
			CHECK(f.units[u].positions.size() == f.units[u].blob.records.size());
		}
		CHECK(total_conn > 0);

		std::vector<uint8_t> image;
		REQUIRE(forest_image(f, image));

		CirFrozenForest forest;
		REQUIRE(forest.open(image.data(), image.size(), c2m));
		CHECK(forest.unit_count() == (uint32_t)f.units.size());
		CHECK(forest.units_loaded() == 0);	// nothing loads at open
		// The directory names the units (main first).
		REQUIRE(forest.unit_name(0) != nullptr);
		CHECK(std::string(forest.unit_name(0)) == main_path);

		cir_node *thawed = forest.root();
		REQUIRE(thawed != nullptr);
		CHECK(forest.units_loaded() == forest.unit_count());
		CHECK(cir_trees_structurally_identical(tree, thawed->as_node()));

		// The thawed multi-unit tree compiles + runs (production path).
		int ok = cir_compile(mir_ctx, c2m, thawed->as_node(), "thawed_forest");
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
		CHECK(val.i == 101);

		delete builder;
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B3: units load on demand — touching one grove leaves the others cold") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		std::shared_ptr<Program> prog;
		CirBuilder *builder = NULL;
		std::string main_path;
		node_t tree = build_two_file_module(prog, builder, c2m, main_path);
		REQUIRE(tree != nullptr);

		cir_frozen_forest f;
		REQUIRE(cir_freeze_forest(CIR_NODE(tree), main_path.c_str(), f));
		REQUIRE(f.units.size() >= 2);

		// A leaf record in a NON-main unit: resolving it must load
		// only that unit (no children -> no connector crossings).
		uint32_t leaf_unit = 0, leaf_idx = 0;
		bool found = false;
		for (uint32_t u = 1; u < (uint32_t)f.units.size() && !found; ++u)
			for (uint32_t i = 0; i < (uint32_t)f.units[u].blob.records.size(); ++i)
				if (f.units[u].blob.records[i].nchildren == 0) {
					leaf_unit = u;
					leaf_idx = i;
					found = true;
					break;
				}
		REQUIRE(found);

		std::vector<uint8_t> image;
		REQUIRE(forest_image(f, image));
		CirFrozenForest forest;
		REQUIRE(forest.open(image.data(), image.size(), c2m));

		CHECK(forest.units_loaded() == 0);
		cir_node *leaf = forest.node_for(leaf_unit, leaf_idx);
		REQUIRE(leaf != nullptr);
		CHECK(forest.units_loaded() == 1);	// just that grove

		delete builder;
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B3: cross-process closure — a FRESH live pool thaws correct strings + positions") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		std::shared_ptr<Program> prog;
		CirBuilder *builder = NULL;
		std::string main_path;
		node_t tree = build_two_file_module(prog, builder, c2m, main_path);
		REQUIRE(tree != nullptr);
		// Decorate the root with accessor-visible strings so the
		// closure has something to prove beyond identifier payloads.
		CIR_NODE(tree)->set_typedef_name("FROZEN_ALIAS_T");

		cir_frozen_forest f;
		REQUIRE(cir_freeze_forest(CIR_NODE(tree), main_path.c_str(), f));
		std::vector<uint8_t> image;
		REQUIRE(forest_image(f, image));

		// Simulate the fresh process: bind a brand-new live pool. The
		// freezing pool's ids are never consulted again (the ORIGINAL
		// tree's accessors are dead from here — do not touch them).
		static madc::dis::intern_table fresh_pool;
		TokenBase::_active_strpool = &fresh_pool;

		CirFrozenForest forest;
		REQUIRE(forest.open(image.data(), image.size(), c2m));
		cir_node *thawed = forest.root();
		REQUIRE(thawed != nullptr);

		// Accessor strings re-interned into the FRESH pool by content.
		REQUIRE(thawed->typedef_name() != nullptr);
		CHECK(std::string(thawed->typedef_name()) == "FROZEN_ALIAS_T");

		// An identifier payload somewhere in the tree reads "helper"
		// (bytes came from the CONTAINER's pool, via c2mir_uniq_str).
		bool saw_helper = false;
		std::vector<node_t> work;
		std::set<node_t> seen;
		work.push_back(thawed->as_node());
		while (!work.empty()) {
			node_t n = work.back();
			work.pop_back();
			if (!seen.insert(n).second)
				continue;
			if (n->code == N_ID && n->u.s.s
			    && strncmp(n->u.s.s, "helper", 6) == 0
			    && n->u.s.len == 7)	// "helper" + NUL, as stored
				saw_helper = true;
			for (node_t op = c2mir_node_first_op(n); op;
			     op = c2mir_node_next_op(op))
				work.push_back(op);
		}
		CHECK(saw_helper);

		// Positions came from the side-car (the freezing process's
		// token arena is NOT consulted): freeze-side data proves the
		// side-car carries the include file's name for its unit.
		bool pos_names_inc = false;
		for (size_t u = 0; u < f.units.size() && !pos_names_inc; ++u)
			for (size_t i = 0; i < f.units[u].positions.size(); ++i)
				if (f.units[u].positions[i].fname_id
				    && f.units[u].positions[i].line > 0) {
					pos_names_inc = true;
					break;
				}
		CHECK(pos_names_inc);

		// And the thawed tree still compiles + runs with the fresh pool.
		int ok = cir_compile(mir_ctx, c2m, thawed->as_node(), "thawed_fresh");
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
		CHECK(val.i == 101);

		delete builder;
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B3: the context-hash pin rejects a mismatched container") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		CirBuilder b(c2m);
		freeze_test_bind_substrate();

		node_t root = b.node2(N_ADD, b.integer(40), b.integer(2));
		cir_frozen_forest f;
		REQUIRE(cir_freeze_forest(CIR_NODE(root), "<pin_test>", f));

		std::vector<uint8_t> good, bad;
		REQUIRE(forest_image(f, good));
		REQUIRE(forest_image(f, bad, madc_cir_context_hash() ^ 0xdeadbeefULL));

		CirFrozenForest okf;
		CHECK(okf.open(good.data(), good.size(), c2m));
		CirFrozenForest rejf;
		CHECK(!rejf.open(bad.data(), bad.size(), c2m));
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

TEST_CASE("B3: the directory round-trips libs and the typeid->name closure") {
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		CirBuilder b(c2m);
		freeze_test_bind_substrate();

		// A project-segment type stamped on a node makes the closure
		// non-empty (primitives are pinned and not serialized).
		static DataDef dd_custom("frozen_custom_t", 4, DataType::dtINT);
		node_t root = b.node1(N_EXPR_SIZEOF, b.integer(9));
		CIR_NODE(root)->set_datadef(&dd_custom);
		uint32_t tid = CIR_NODE(root)->datadef_id;
		REQUIRE(tid >= MADC_TYPEID_PROJECT_BASE);

		cir_frozen_forest f;
		REQUIRE(cir_freeze_forest(CIR_NODE(root), "<dir_test>", f));
		f.libs.push_back("libm.so.6");

		std::vector<uint8_t> image;
		REQUIRE(forest_image(f, image));
		CirFrozenForest forest;
		REQUIRE(forest.open(image.data(), image.size(), c2m));

		REQUIRE(forest.libs().size() == 1);
		CHECK(forest.libs()[0] == "libm.so.6");
		REQUIRE(forest.type_name_for(tid) != nullptr);
		CHECK(std::string(forest.type_name_for(tid)) == "frozen_custom_t");
		CHECK(forest.type_name_for(0x7fffffffu) == nullptr);
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);
}

// ---------------------------------------------------------------------------
// B4a: grove payload v2 (docs/plans/2026-07-04-forest-default-mode-design.md
// §2) — through the PRODUCTION pack path (madc_cir_freeze with
// pack_recording on, i.e. exactly what --freeze does), then read back with
// CirFrozenForest's v2 accessors.
// ---------------------------------------------------------------------------

TEST_CASE("B4a: grove payload v2 round-trips tokens, decl index, PP exports, edges, branch macros, canon order") {
	std::string inc_path = std::string("/tmp/madc_b4a_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_b4a_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_b4a_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "#ifndef B4A_T_H\n"
		       "#define B4A_T_H\n"
		       "#define B4A_VAL 1\n"
		       "#define B4A_FN(x) ((x)+1)\n"
		       "#undef B4A_VAL\n"
		       "typedef int b4a_alias_t;\n"
		       "struct b4a_rec { int f; };\n"
		       "int helper(int v);\n"
		       "#endif\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "#if B4A_MISSING > 0\n"
		      "typedef int never_t;\n"
		      "#endif\n"
		      "int helper(int v) { return v + 100; }\n"
		      "int main() { return helper(1); }\n";
	}

	std::shared_ptr<Program> prog = std::make_shared<Program>();
	prog->pack_recording = true;	// what --freeze sets before tokenize
	TokenProgram *tp = prog->tokenize(main_path.c_str());
	REQUIRE(tp != nullptr);
	REQUIRE(prog->parse(tp));
	REQUIRE(madc_cir_freeze(prog.get(), main_path.c_str(),
				snap_path.c_str(), /*append=*/false) == 0);
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));
	REQUIRE(forest.unit_count() >= 2);

	// Locate the two units by name.
	uint32_t u_main = 0xffffffffu, u_inc = 0xffffffffu;
	for (uint32_t u = 0; u < forest.unit_count(); ++u) {
		const char *nm = forest.unit_name(u);
		if (nm && main_path == nm) u_main = u;
		if (nm && inc_path == nm)  u_inc = u;
	}
	REQUIRE(u_main != 0xffffffffu);
	REQUIRE(u_inc != 0xffffffffu);

	// Canonical order = first-tokenization order: main, then the include.
	const std::vector<uint32_t> &canon = forest.canon_order();
	REQUIRE(canon.size() == 2);
	CHECK(canon[0] == u_main);
	CHECK(canon[1] == u_inc);

	// Branch macros: the include guard (#ifndef) and the #if consult.
	std::set<std::string> branch;
	for (size_t i = 0; i < forest.branch_macros().size(); ++i)
		if (const char *s = forest.pool_str(forest.branch_macros()[i]))
			branch.insert(s);
	CHECK(branch.count("B4A_T_H") == 1);
	CHECK(branch.count("B4A_MISSING") == 1);

	// Token slice: the include unit's stream deserializes to token_count
	// tokens through the .madh reader.
	std::vector<uint8_t> payload;
	uint32_t ntok = 0;
	REQUIRE(forest.unit_tokens(u_inc, payload, ntok));
	CHECK(forest.unit_anchor(u_inc) != CIR_FOREST_ANCHOR_NONE);
	REQUIRE(ntok >= 15);	// typedef/struct/fn decl tokens
	std::deque<TokenBase *> toks;
	REQUIRE(madc_pch::deserialize_tokens(payload.data(), payload.size(),
					     ntok, toks));
	CHECK(toks.size() == (size_t)ntok);
	for (size_t i = 0; i < toks.size(); ++i)
		delete toks[i];

	// Decl index: the include's exports, with in-range unit-local slices.
	std::vector<cir_forest_decl_entry> di;
	REQUIRE(forest.unit_decl_index(u_inc, di));
	REQUIRE(di.size() == forest.unit_anchor(u_inc));
	bool saw_alias = false, saw_rec = false, saw_helper = false;
	for (size_t i = 0; i < di.size(); ++i) {
		const char *nm = forest.pool_str(di[i].name_id);
		REQUIRE(nm != nullptr);
		CHECK(di[i].slice_begin < di[i].slice_end);
		CHECK(di[i].slice_end <= ntok);
		CHECK((di[i].aux & Program::PACK_DECL_SPANS_UNITS) == 0);
		if (!strcmp(nm, "b4a_alias_t") && di[i].kind == Program::pdkTypedef)
			saw_alias = true;
		if (!strcmp(nm, "b4a_rec") && di[i].kind == Program::pdkStruct)
			saw_rec = true;
		if (!strcmp(nm, "helper") && di[i].kind == Program::pdkFunction)
			saw_helper = true;
	}
	CHECK(saw_alias);
	CHECK(saw_rec);
	CHECK(saw_helper);

	// PP exports: define, define-fn, undef — in directive order.
	std::vector<uint32_t> ppe;
	REQUIRE(forest.unit_pp_events(u_inc, ppe));
	std::vector<std::pair<std::string, uint32_t> > events; // (name, tag)
	for (size_t k = 0; k + 5 <= ppe.size(); ) {
		const char *nm = forest.pool_str(ppe[k]);
		REQUIRE(nm != nullptr);
		events.push_back(std::make_pair(std::string(nm),
						ppe[k + 1] & 0xffu));
		k += 5 + ppe[k + 4];
	}
	REQUIRE(events.size() == 4);
	CHECK(events[0].first == "B4A_T_H");
	CHECK(events[0].second == (uint32_t)Program::PackMacroEvent::peDefine);
	CHECK(events[1].first == "B4A_VAL");
	CHECK(events[1].second == (uint32_t)Program::PackMacroEvent::peDefine);
	CHECK(events[2].first == "B4A_FN");
	CHECK(events[2].second == (uint32_t)Program::PackMacroEvent::peDefineFn);
	CHECK(events[3].first == "B4A_VAL");
	CHECK(events[3].second == (uint32_t)Program::PackMacroEvent::peUndef);

	// Edges: main includes the helper unit.
	std::vector<uint32_t> edges;
	REQUIRE(forest.unit_edges(u_main, edges));
	bool edge_to_inc = false;
	for (size_t i = 0; i < edges.size(); ++i)
		if (edges[i] == u_inc)
			edge_to_inc = true;
	CHECK(edge_to_inc);
}

// Phase 6 (design 2026-07-05): the forest is the serialized Tree-1 ROM; on bind
// the parser's symbol tables are RECONSTRUCTED from typed decl records — NEVER
// re-parsed. Slice 1 proves the smallest vertical of that: a file-scope typedef
// serializes as a decl record whose aliased *primitive* type-id survives freeze
// + reopen and resolves to the same DataDef (a pinned primitive id resolves in
// any process — the load-not-reparse property). Widens to struct/class/func/
// template (aggregate types get system-segment ids) in later slices.
TEST_CASE("Phase 6 slice 1: a file-scope typedef serializes as a decl record; "
	  "its primitive type-id round-trips (load, don't re-parse)") {
	std::string inc_path = std::string("/tmp/madc_p6_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_p6_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_p6_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "typedef int myint;\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { myint x = 5; return x; }\n";
	}

	std::shared_ptr<Program> prog = std::make_shared<Program>();
	prog->pack_recording = true;
	TokenProgram *tp = prog->tokenize(main_path.c_str());
	REQUIRE(tp != nullptr);
	REQUIRE(prog->parse(tp));

	// Capture the truth in THIS process: `myint` aliases some primitive DataDef.
	flat_datatype_map_iter mit = prog->datatype_map.find("myint");
	REQUIRE(mit != prog->datatype_map.end());
	DataDef *myint_dd = &(*mit)->definition;

	REQUIRE(madc_cir_freeze(prog.get(), main_path.c_str(),
				snap_path.c_str(), /*append=*/false) == 0);
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	// The serialized type table carries the typedef; materialize_types swizzles
	// its underlying back to the SAME primitive DataDef the live parse aliased
	// (a pinned primitive id resolves cross-process with no Program in hand).
	bool saw_myint = false;
	const std::vector<CirRestoredType> &types = forest.materialize_types();
	for (size_t i = 0; i < types.size(); ++i) {
		if (!types[i].name || strcmp(types[i].name, "myint"))
			continue;
		saw_myint = true;
		CHECK(types[i].kind == CIR_TYPEK_TYPEDEF);
		CHECK(types[i].underlying == myint_dd);
	}
	CHECK(saw_myint);
}

// Phase 6 slice 1b: RESTORE the loaded decl records into a FRESH Program's symbol
// tables (forest_restore_decls) and prove that Program now resolves the header
// typedef WITHOUT ever parsing the header — the "parser-resume" that makes bind a
// load, not a re-parse.
TEST_CASE("Phase 6 slice 1b: forest_restore_decls makes a fresh parser resolve a "
	  "header typedef with no header parse") {
	std::string inc_path = std::string("/tmp/madc_p6b_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_p6b_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_p6b_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	std::string triv_path = std::string("/tmp/madc_p6b_triv_")
			      + std::to_string((long)getpid()) + ".mad";
	{ std::ofstream inc(inc_path.c_str()); inc << "typedef int myint;\n"; }
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { myint x = 5; return x; }\n";
	}
	{ std::ofstream tv(triv_path.c_str()); tv << "int main() { return 0; }\n"; }

	// Freeze a container carrying the `myint` typedef decl record.
	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	// The aliased primitive the typedef record points at (materialize swizzles
	// the underlying id back to the DataDef*; idempotent — the restore below
	// reuses this same cached result).
	DataDef *expected = nullptr;
	const std::vector<CirRestoredType> &types = forest.materialize_types();
	for (size_t i = 0; i < types.size(); ++i)
		if (types[i].name && !strcmp(types[i].name, "myint"))
			expected = types[i].underlying;
	REQUIRE(expected != nullptr);

	// A FRESH Program, initialized by a trivial parse that never mentions myint.
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(triv_path.c_str());
	REQUIRE(tpB != nullptr);
	REQUIRE(progB->parse(tpB));
	std::remove(triv_path.c_str());
	CHECK(progB->datatype_map.find("myint") == progB->datatype_map.end());

	// RESTORE from the forest — no header parse — then it resolves.
	progB->forest_restore_decls(forest);
	flat_datatype_map_iter mit = progB->datatype_map.find("myint");
	REQUIRE(mit != progB->datatype_map.end());
	CHECK(&(*mit)->definition == expected);
}

// Phase 6 slice 3a: a file-scope plain struct serializes as a decl record + a
// member stream (a system-segment id for the struct itself), and
// forest_restore_decls RECONSTRUCTS a complete DataDefSTRUCT — members, offsets,
// size — into a fresh Program's struct_map with NO header parse.
TEST_CASE("Phase 6 slice 3a: forest_restore_decls reconstructs a header struct "
	  "(members + layout) with no header parse") {
	std::string inc_path = std::string("/tmp/madc_p6s_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_p6s_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_p6s_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	std::string triv_path = std::string("/tmp/madc_p6s_triv_")
			      + std::to_string((long)getpid()) + ".mad";
	{ std::ofstream inc(inc_path.c_str()); inc << "struct Point { int x; int y; };\n"; }
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { struct Point p; p.x = 0; return p.x; }\n";
	}
	{ std::ofstream tv(triv_path.c_str()); tv << "int main() { return 0; }\n"; }

	// Freeze a container carrying the `Point` struct record + member stream.
	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	// The serialized type table carries Point as a STRUCT record; materialize_types
	// swizzles it back to a complete DataDefSTRUCT (members + verbatim layout).
	bool found = false;
	const std::vector<CirRestoredType> &types = forest.materialize_types();
	for (size_t i = 0; i < types.size(); ++i) {
		if (!types[i].name || strcmp(types[i].name, "Point"))
			continue;
		found = true;
		CHECK(types[i].kind == CIR_TYPEK_STRUCT);
		DataDefSTRUCT *pd = dynamic_cast<DataDefSTRUCT *>(types[i].dd);
		REQUIRE(pd != nullptr);
		REQUIRE(pd->members.size() == 2);
		CHECK(pd->size == 8);
	}
	CHECK(found);

	// A FRESH Program, initialized by a trivial parse that never mentions Point.
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(triv_path.c_str());
	REQUIRE(tpB != nullptr);
	REQUIRE(progB->parse(tpB));
	std::remove(triv_path.c_str());
	CHECK(progB->struct_map.find("Point") == progB->struct_map.end());

	// RESTORE — no header parse — then Point is a complete struct with the same
	// layout a live parse would have produced (int x @0, int y @4, size 8).
	progB->forest_restore_decls(forest);
	datadef_map_iter sit = progB->struct_map.find("Point");
	REQUIRE(sit != progB->struct_map.end());
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(sit->second);
	REQUIRE(sdd != nullptr);
	CHECK(sdd->is_complete);
	REQUIRE(sdd->members.size() == 2);
	CHECK(sdd->members[0].first == "x");
	CHECK(sdd->members[1].first == "y");
	REQUIRE(sdd->member_offsets.size() == 2);
	CHECK(sdd->member_offsets[0] == 0);
	CHECK(sdd->member_offsets[1] == 4);
	CHECK(sdd->size == 8);
}

// Phase 6 v9: a struct whose members are non-pinned pointers (a scalar double*, a
// self-referential struct Node*, and a pointer to a sibling struct Point*) survives
// the freeze. Before v9 the member pass bailed on the first non-pinned pointer and
// dropped the whole aggregate. Now each pointer type serializes as a derived-type
// record (CIR_TYPEK_POINTER, ref0 = pointee typeid) and materialize_types' load
// fixpoint reconstructs it — the self-reference resolves because Node is allocated
// before its self-pointer, so the reconstructed pointer's base_type IS Node.
TEST_CASE("Phase 6 v9: a struct's pointer members reconstruct via derived-type records") {
	std::string inc_path = std::string("/tmp/madc_p6ptr_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_p6ptr_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_p6ptr_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{ std::ofstream inc(inc_path.c_str());
	  inc << "struct Point { int x; int y; };\n"
	         "struct Node { int v; double *dp; struct Node *next; struct Point *pp; };\n"; }
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { struct Node n; n.v = 0; n.dp = 0;"
		      " n.next = 0; n.pp = 0; return n.v; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	DataDefSTRUCT *node = NULL;
	const std::vector<CirRestoredType> &types = forest.materialize_types();
	for (size_t i = 0; i < types.size(); ++i)
		if (types[i].name && !strcmp(types[i].name, "Node"))
			node = dynamic_cast<DataDefSTRUCT *>(types[i].dd);
	REQUIRE(node != nullptr);
	REQUIRE(node->members.size() == 4);
	CHECK(node->members[0].first == "v");		// int (primitive)
	CHECK(node->members[1].second->is_pointer());	// double*
	CHECK(node->members[2].second->is_pointer());	// struct Node* (self-ref)
	CHECK(node->members[3].second->is_pointer());	// struct Point* (sibling)
	CHECK(node->size == 32);			// verbatim layout preserved

	// The self-referential pointer reconstructs to point at the SAME Node object.
	DataDefPTR *selfp = dynamic_cast<DataDefPTR *>(node->members[2].second);
	REQUIRE(selfp != nullptr);
	CHECK(selfp->base_type == node);
}

// Phase 6 v10: a struct defined inside a user namespace serializes its defining
// namespace (reverse-walked from namespace_datatype_map at freeze) so
// materialize_types reports rt.ns, and forest_restore_decls registers it into
// namespace_map + namespace_datatype_map — a bound `N::P` then resolves (before v10
// only the flat maps were restored, so it failed "Unknown namespace 'N'").
TEST_CASE("Phase 6 v10: a namespaced struct restores into the namespace maps") {
	std::string inc_path = std::string("/tmp/madc_p6ns_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_p6ns_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_p6ns_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	std::string triv_path = std::string("/tmp/madc_p6ns_triv_")
			      + std::to_string((long)getpid()) + ".mad";
	{ std::ofstream inc(inc_path.c_str());
	  inc << "namespace N { struct P { int x; int y; }; }\n"; }
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { N::P p; p.x = 0; return p.x; }\n";
	}
	{ std::ofstream tv(triv_path.c_str()); tv << "int main() { return 0; }\n"; }

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	// materialize_types reports P's defining namespace as "N".
	bool found = false;
	const std::vector<CirRestoredType> &types = forest.materialize_types();
	for (size_t i = 0; i < types.size(); ++i) {
		if (!types[i].name || strcmp(types[i].name, "P"))
			continue;
		found = true;
		REQUIRE(types[i].ns != nullptr);
		CHECK(std::string(types[i].ns) == "N");
	}
	CHECK(found);

	// A FRESH Program that never mentioned N restores it into BOTH namespace maps.
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(triv_path.c_str());
	REQUIRE(tpB != nullptr);
	REQUIRE(progB->parse(tpB));
	std::remove(triv_path.c_str());
	CHECK(progB->namespace_map.find("N") == progB->namespace_map.end());

	progB->forest_restore_decls(forest);
	REQUIRE(progB->namespace_map.find("N") != progB->namespace_map.end());
	namespace_datatype_map_t::iterator ni = progB->namespace_datatype_map.find("N");
	REQUIRE(ni != progB->namespace_datatype_map.end());
	CHECK(ni->find("P") != ni->end());	// N::P resolvable (ni-> is the inner map)
	CHECK(progB->struct_map.find("P") != progB->struct_map.end());	// bare tag too
}

// Phase 6: named AND unnamed-gap bitfield structs reconstruct with the right bit
// layout. Because member offsets serialize VERBATIM (loaded as stored, never
// regenerated by a members-only rebuild), an unnamed-bitfield gap is preserved —
// the following member keeps its true bit-offset — so both Flags and the
// unnamed-gap Gap bind correctly (the old members-only rebuild had to refuse Gap).
TEST_CASE("Phase 6: named + unnamed-gap bitfield structs reconstruct verbatim") {
	std::string inc_path = std::string("/tmp/madc_p6bf_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_p6bf_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_p6bf_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	std::string triv_path = std::string("/tmp/madc_p6bf_triv_")
			      + std::to_string((long)getpid()) + ".mad";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "struct Flags { unsigned a : 3; unsigned b : 5; int c; };\n"
		       "struct Gap { unsigned a : 3; unsigned : 2; unsigned b : 5; };\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { struct Flags f; struct Gap g; f.a = 0; g.a = 0;"
		      " return f.a + g.a; }\n";
	}
	{ std::ofstream tv(triv_path.c_str()); tv << "int main() { return 0; }\n"; }

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	// Both structs serialize now: verbatim member offsets carry the unnamed-gap
	// struct's real bit layout (b lands at bit 5, past the 3-bit a + 2-bit gap).
	bool saw_flags = false, saw_gap = false;
	const std::vector<CirRestoredType> &types = forest.materialize_types();
	for (size_t i = 0; i < types.size(); ++i) {
		if (types[i].kind != CIR_TYPEK_STRUCT) continue;
		if (types[i].name && !strcmp(types[i].name, "Flags")) saw_flags = true;
		if (types[i].name && !strcmp(types[i].name, "Gap"))   saw_gap = true;
	}
	CHECK(saw_flags);
	CHECK(saw_gap);

	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(triv_path.c_str());
	REQUIRE(tpB != nullptr);
	REQUIRE(progB->parse(tpB));
	std::remove(triv_path.c_str());

	progB->forest_restore_decls(forest);
	// Flags restored with the right bit layout: a@bit0 w3, b@bit3 w5 (no gap).
	datadef_map_iter fit = progB->struct_map.find("Flags");
	REQUIRE(fit != progB->struct_map.end());
	DataDefSTRUCT *fl = dynamic_cast<DataDefSTRUCT *>(fit->second);
	REQUIRE(fl != nullptr);
	REQUIRE(fl->members.size() == 3);
	REQUIRE(fl->member_bitfields.size() == 3);
	CHECK(fl->member_bitfields[0].is_bitfield);
	CHECK(fl->member_bitfields[0].bit_offset == 0);
	CHECK(fl->member_bitfields[0].bit_width == 3);
	CHECK(fl->member_bitfields[1].is_bitfield);
	CHECK(fl->member_bitfields[1].bit_offset == 3);
	CHECK(fl->member_bitfields[1].bit_width == 5);
	CHECK(fl->members[2].first == "c");
	CHECK(fl->member_offsets[2] == 4);
	// Gap binds too, with its true bit layout: a@bit0 w3, then the unnamed :2
	// gap, so b@bit5 w5 (verbatim offsets preserve the gap a rebuild would lose).
	datadef_map_iter git = progB->struct_map.find("Gap");
	REQUIRE(git != progB->struct_map.end());
	DataDefSTRUCT *gp = dynamic_cast<DataDefSTRUCT *>(git->second);
	REQUIRE(gp != nullptr);
	REQUIRE(gp->members.size() == 2);
	REQUIRE(gp->member_bitfields.size() == 2);
	CHECK(gp->member_bitfields[0].bit_offset == 0);
	CHECK(gp->member_bitfields[0].bit_width == 3);
	CHECK(gp->member_bitfields[1].bit_offset == 5);
	CHECK(gp->member_bitfields[1].bit_width == 5);
}

// Phase 6 slice 3c: a non-polymorphic class serializes as a CLASS record —
// members verbatim (inheritance-flattened) + bases (subobject offsets) + flags +
// size/align — and materialize_types + forest_restore_decls reconstruct a
// complete DataDefCLASS with NO header parse: each base id swizzles back to the
// loaded base object, and layout loads VERBATIM (no compute_layout re-run).
// Multiple inheritance exercises a nonzero base subobject offset (B at +4).
TEST_CASE("Phase 6 slice 3c: forest_restore_decls reconstructs a header class "
	  "(members + bases + layout) with no header parse") {
	std::string inc_path = std::string("/tmp/madc_p6c_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_p6c_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_p6c_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	std::string triv_path = std::string("/tmp/madc_p6c_triv_")
			      + std::to_string((long)getpid()) + ".mad";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "class A { public: int a; };\n"
		       "class B { public: int b; };\n"
		       "class C : public A, public B { public: int c; };\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { C x; x.a = 0; return x.a; }\n";
	}
	{ std::ofstream tv(triv_path.c_str()); tv << "int main() { return 0; }\n"; }

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	// C serializes as a CLASS record; materialize_types swizzles it to a complete
	// DataDefCLASS whose bases point at the loaded A / B objects, at verbatim
	// offsets (A@0, B@4), with the inheritance-flattened member set (a, b, c).
	bool found = false;
	const std::vector<CirRestoredType> &types = forest.materialize_types();
	for (size_t i = 0; i < types.size(); ++i) {
		if (!types[i].name || strcmp(types[i].name, "C"))
			continue;
		found = true;
		CHECK(types[i].kind == CIR_TYPEK_CLASS);
		DataDefCLASS *cd = dynamic_cast<DataDefCLASS *>(types[i].dd);
		REQUIRE(cd != nullptr);
		REQUIRE(cd->members.size() == 3);
		REQUIRE(cd->bases.size() == 2);
		CHECK(cd->bases[0].offset == 0);
		CHECK(cd->bases[1].offset == 4);
		REQUIRE(cd->bases[0].base != nullptr);
		REQUIRE(cd->bases[1].base != nullptr);
		CHECK(cd->bases[0].base->name == "A");
		CHECK(cd->bases[1].base->name == "B");
		CHECK(cd->size == 12);
	}
	CHECK(found);

	// A FRESH Program that never parsed the header.
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(triv_path.c_str());
	REQUIRE(tpB != nullptr);
	REQUIRE(progB->parse(tpB));
	std::remove(triv_path.c_str());
	CHECK(progB->struct_map.find("C") == progB->struct_map.end());

	// RESTORE — no header parse — then C is a complete class: registered both as a
	// type name (datatype_map) and a tag (struct_map), 3 members, 2 bases,
	// base_class == the first base, and the same verbatim layout a live parse gives.
	progB->forest_restore_decls(forest);
	datadef_map_iter cit = progB->struct_map.find("C");
	REQUIRE(cit != progB->struct_map.end());
	DataDefCLASS *cd = dynamic_cast<DataDefCLASS *>(cit->second);
	REQUIRE(cd != nullptr);
	CHECK(cd->is_complete);
	REQUIRE(cd->members.size() == 3);
	REQUIRE(cd->bases.size() == 2);
	CHECK(cd->bases[1].offset == 4);
	REQUIRE(cd->base_class != nullptr);
	CHECK(cd->base_class->name == "A");
	CHECK(cd->size == 12);
	CHECK(progB->datatype_map.find("C") != progB->datatype_map.end());
}

// Phase 6 slice 3d + v8: a non-virtual class method serializes as a method record
// (name / display / return / explicit-params / emit_symbol / flags) and
// materialize_types rebuilds a FuncDef + Variable into the class's method_map —
// the hidden __this (param 0 of a non-static method) rebuilt as a pointer to the
// class. This makes a member call RESOLVE. v8: an INLINE method (body only in the
// header, no .so) also records its Tree-1 body location (has_forest_body, NOT
// declaration_only) so bind copies the saved body into the consumer's module on
// use (the run is exercised cross-process by scripts/forest_bind_gate.sh).
TEST_CASE("Phase 6 slice 3d: a class's non-virtual methods reconstruct into method_map") {
	std::string inc_path = std::string("/tmp/madc_p6m_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_p6m_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_p6m_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "class Counter { public: int n;\n"
		       "    int get() { return n; }\n"
		       "    void add(int x) { n += x; } };\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { Counter c; c.n = 0; return c.get(); }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(inc_path.c_str());
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	DataDefCLASS *cd = NULL;
	const std::vector<CirRestoredType> &types = forest.materialize_types();
	for (size_t i = 0; i < types.size(); ++i)
		if (types[i].name && !strcmp(types[i].name, "Counter"))
			cd = dynamic_cast<DataDefCLASS *>(types[i].dd);
	REQUIRE(cd != nullptr);

	// get(): returns int, one param (the hidden __this — a pointer to Counter).
	std::map<std::string, Variable *>::iterator gi = cd->method_map.find("get");
	REQUIRE(gi != cd->method_map.end());
	FuncDef *gf = dynamic_cast<FuncDef *>(gi->second->type);
	REQUIRE(gf != nullptr);
	CHECK(gf->returns.rawtype() == DataType::dtINT);
	REQUIRE(gf->parameters.size() == 1);
	REQUIRE(gf->parameters[0] != nullptr);
	CHECK(gf->parameters[0]->is_pointer());
	CHECK(gi->second->name == "Counter__get");	// mangled call symbol

	// add(int): returns void, __this + one explicit int param.
	std::map<std::string, Variable *>::iterator ai = cd->method_map.find("add");
	REQUIRE(ai != cd->method_map.end());
	FuncDef *af = dynamic_cast<FuncDef *>(ai->second->type);
	REQUIRE(af != nullptr);
	REQUIRE(af->parameters.size() == 2);
	CHECK(af->parameters[0]->is_pointer());		// __this
	CHECK(af->parameters[1]->rawtype() == DataType::dtINT);

	// v8: both are INLINE methods — the loaded FuncDef carries its Tree-1 body
	// location and is NOT declaration-only, so the consumer emits the body itself
	// (a LIBRARY method would be declaration_only with an emit_symbol instead).
	CHECK(gf->has_forest_body);
	CHECK(!gf->declaration_only);
	CHECK(af->has_forest_body);
	CHECK(!af->declaration_only);
}
