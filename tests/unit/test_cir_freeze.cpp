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
	// Libs ride the directory: a hand-staged (type-less) container is enough.
	MIR_context_t mir_ctx = MIR_init();
	c2mir_init(mir_ctx);
	c2m_ctx_t c2m = cir_init(mir_ctx);
	REQUIRE(c2m != nullptr);
	{
		CirBuilder b(c2m);
		freeze_test_bind_substrate();

		node_t root = b.node1(N_EXPR_SIZEOF, b.integer(9));
		cir_frozen_forest f;
		REQUIRE(cir_freeze_forest(CIR_NODE(root), "<dir_test>", f));
		f.libs.push_back("libm.so.6");

		std::vector<uint8_t> image;
		REQUIRE(forest_image(f, image));
		CirFrozenForest forest;
		REQUIRE(forest.open(image.data(), image.size(), c2m));

		REQUIRE(forest.libs().size() == 1);
		CHECK(forest.libs()[0] == "libm.so.6");
	}
	cir_finish(c2m);
	c2mir_finish(mir_ctx);
	MIR_finish(mir_ctx);

	// The typeid->name closure derives from the ARENA records (v18), so it
	// needs a production freeze: parse a project type with the arena on, note
	// its live project id (== its arena slot id), freeze, open, resolve.
	std::string main_path = std::string("/tmp/madc_b3dir_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_b3dir_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream mn(main_path.c_str());
		mn << "struct frozen_custom_t { int x; };\n"
		      "int main() { struct frozen_custom_t v; v.x = 1; return v.x; }\n";
	}
	std::shared_ptr<Program> prog = std::make_shared<Program>();
	prog->pack_recording = true;		// as --freeze sets (madc.cpp)
	prog->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
	TokenProgram *tp = prog->tokenize(main_path.c_str());
	REQUIRE(tp != nullptr);
	REQUIRE(prog->parse(tp));
	datadef_map_citer smi = prog->struct_map.find("frozen_custom_t");
	REQUIRE(smi != prog->struct_map.end());
	uint32_t tid = madc_type_id_for(smi->second);
	REQUIRE(tid >= MADC_TYPEID_PROJECT_BASE);
	REQUIRE(madc_cir_freeze(prog.get(), main_path.c_str(),
				snap_path.c_str(), /*append=*/false) == 0);
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));
	REQUIRE(forest.type_name_for(tid) != nullptr);
	CHECK(std::string(forest.type_name_for(tid)) == "frozen_custom_t");
	CHECK(forest.type_name_for(0x7fffffffu) == nullptr);
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
	prog->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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
	prog->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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

	// The serialized type table carries the typedef; materialize_from_arena swizzles
	// its underlying back to the SAME primitive DataDef the live parse aliased
	// (a pinned primitive id resolves cross-process with no Program in hand).
	bool saw_myint = false;
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
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
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
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
	// (Production order: restore stages the flat datatype registrations —
	// they must not promote token shapes mid-tokenize — and the tokenize
	// tail's flush applies them; the harness calls both explicitly.)
	progB->forest_restore_decls(forest);
	progB->flush_forest_pending_globals();
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
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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

	// The serialized type table carries Point as a STRUCT record; materialize_from_arena
	// swizzles it back to a complete DataDefSTRUCT (members + verbatim layout).
	bool found = false;
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
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
	datadef_map_citer sit = progB->struct_map.find("Point");
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
// record (CIR_TYPEK_POINTER, ref0 = pointee typeid) and materialize_from_arena' load
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
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
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
// materialize_from_arena reports rt.ns, and forest_restore_decls registers it into
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
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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

	// materialize_from_arena reports P's defining namespace as "N".
	bool found = false;
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
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
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
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
	datadef_map_citer fit = progB->struct_map.find("Flags");
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
	datadef_map_citer git = progB->struct_map.find("Gap");
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
// size/align — and materialize_from_arena + forest_restore_decls reconstruct a
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
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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

	// C serializes as a CLASS record; materialize_from_arena swizzles it to a complete
	// DataDefCLASS whose bases point at the loaded A / B objects, at verbatim
	// offsets (A@0, B@4), with the inheritance-flattened member set (a, b, c).
	bool found = false;
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
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
	progB->flush_forest_pending_globals();	// applies staged datatype_map writes
	datadef_map_citer cit = progB->struct_map.find("C");
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
// materialize_from_arena rebuilds a FuncDef + Variable into the class's method_map —
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
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
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
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
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

// Phase 6 A2: a member whose type is a SCALAR-primitive typedef alias given a
// distinct PROJECT id at instantiation (std::string's size_type == unsigned long,
// materialized fresh by tsubst rather than aliased to the pinned primitive) used to
// make the freeze's derived-type check fail, dropping the WHOLE aggregate — so the
// real std::string product basic_string<char,...> never materialized. A scalar emits
// SOLELY from its rawtype (append_type_specs; verified: live emits `unsigned long`,
// no `size_type` typedef), so it is byte-identical to the pinned primitive of that
// rawtype; the freeze now serializes such a member as the pinned id
// (forest_serialize_type_id) and load resolves it via madc_type_from_id with NO
// record. This gates the fix on the REAL corpus target — a minimal .h can't
// reproduce the distinct alias (madc resolves a directly-parsed typedef to the
// primitive; only tsubst produces the fresh copy).
TEST_CASE("Phase 6 A2: std::string's scalar-alias size_type members no longer bail the aggregate") {
	std::string main_path = std::string("/tmp/madc_p6a2_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string snap_path = std::string("/tmp/madc_p6a2_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include <string>\n"
		      "int main() { std::string s; return (int)s.size(); }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	// The real std::string product (char + std::allocator<char>, NOT the pmr or
	// wide-char variants) now materializes — before A2 its size_type members made
	// the aggregate bail, so it was never in the type records at all.
	DataDefSTRUCT *str = NULL;
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
	for (size_t i = 0; i < types.size(); ++i)
		if (types[i].name && !strcmp(types[i].name,
			"basic_string_char_std__char_traits_char__std__allocator_char_"))
			str = dynamic_cast<DataDefSTRUCT *>(types[i].dd);
	REQUIRE(str != nullptr);			// aggregate recorded + materialized

	// Every member resolved (a dropped scalar-alias would leave a NULL type) and
	// the size_type members resolve to an 8-byte integer (the pinned unsigned long).
	REQUIRE(str->members.size() >= 2);
	bool saw_scalar_alias = false;
	for (size_t m = 0; m < str->members.size(); ++m) {
		REQUIRE(str->members[m].second != nullptr);
		if (str->members[m].first == "_M_string_length") {
			DataDef *mt = str->members[m].second;
			CHECK(mt->is_integer());
			CHECK(mt->size == 8);
			saw_scalar_alias = true;
		}
	}
	CHECK(saw_scalar_alias);
}

// Phase 6 A1: std::string is a NAMESPACED alias (namespace_datatype_map["std"]
// ["string"]) for the basic_string<char,...> product. The v10 namespace walk
// STAMPED a product's own record with its defining namespace but SKIPPED an alias
// whose key != the record name (std::string's key "string" != the product's
// mangled record name). A1 turns that skip into an EMIT: a namespaced typedef
// record (name="string", ns="std", ref0=product id). materialize_from_arena then
// produces a typedef CirRestoredType with ns="std" and underlying = the recorded
// product, so forest_restore_decls can register namespace_datatype_map["std"]
// ["string"] and a bound `std::string` resolves to it. (Live emits NO
// `typedef ... string;` — the product name is used directly — so this restores
// the parse-time NAME resolution, not a C typedef; the full bind needs the
// method-linking slice, so A1 is gated here at the materialize level, like A2.)
TEST_CASE("Phase 6 A1: std::string materializes as a namespaced typedef -> the basic_string<char> product") {
	std::string main_path = std::string("/tmp/madc_p6a1_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string snap_path = std::string("/tmp/madc_p6a1_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include <string>\n"
		      "int main() { std::string s; return (int)s.size(); }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();

	// The `std::string` typedef materializes: kind TYPEDEF, ns "std", and its
	// underlying is the recorded basic_string<char,...> product (a real aggregate).
	const CirRestoredType *str_alias = NULL;
	for (size_t i = 0; i < types.size(); ++i)
		if (types[i].kind == CIR_TYPEK_TYPEDEF && types[i].name
		    && !strcmp(types[i].name, "string")
		    && types[i].ns && !strcmp(types[i].ns, "std"))
			str_alias = &types[i];
	REQUIRE(str_alias != nullptr);
	REQUIRE(str_alias->underlying != nullptr);
	// The underlying is the char basic_string product (matches the A2 aggregate).
	CHECK(str_alias->underlying->name
	      == "basic_string_char_std__char_traits_char__std__allocator_char_");
}

// Phase 6 v12: the freeze used to SKIP a class's ctors (disp==name / empty-disp),
// dtor (disp[0]=='~'), and operators, so a restored std::string had an EMPTY ctors
// set — binding `std::string s;` failed "no matching constructor". v12 classifies
// each method structurally (ctor = member of cdd->ctors; dtor = the class_own_dtor
// var, a "~" method_map key in methods; operator = display begins "operator") and
// serializes the LIBRARY ones (concrete emit_symbol) with CIR_METHF_CTOR/DTOR. This
// asserts the RESTORED basic_string<char>'s state matches parse: a default ctor in
// cdd->ctors, a dtor discoverable exactly as class_own_dtor does (both with real
// Itanium emit_symbols), and operator= in method_map. (The end-to-end bind — a
// std::string consumer constructs/assigns/sizes/destroys, output == live == g++ —
// is the forest_bind_gate `strbind` case; here we gate the restored type state.)
TEST_CASE("Phase 6 v12: std::string's ctors + dtor + operator= reconstruct into the class's slots") {
	std::string main_path = std::string("/tmp/madc_p6v12_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string snap_path = std::string("/tmp/madc_p6v12_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include <string>\n"
		      "int main() { std::string s; s = \"hi\"; return (int)s.size(); }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	DataDefCLASS *str = NULL;
	const std::vector<CirRestoredType> &types = forest.materialize_from_arena();
	for (size_t i = 0; i < types.size(); ++i)
		if (types[i].name && !strcmp(types[i].name,
			"basic_string_char_std__char_traits_char__std__allocator_char_"))
			str = dynamic_cast<DataDefCLASS *>(types[i].dd);
	REQUIRE(str != nullptr);

	// A default ctor (only the hidden __this required) is in cdd->ctors, and every
	// ctor is a LIBRARY method bound to a real Itanium symbol (no madc body).
	bool saw_default_ctor = false;
	for (size_t i = 0; i < str->ctors.size(); ++i) {
		FuncDef *cf = str->ctors[i] ? dynamic_cast<FuncDef *>(str->ctors[i]->type) : NULL;
		if (!cf) continue;
		if (cf->required_param_count() <= 1)	// only __this required
			saw_default_ctor = true;
	}
	REQUIRE(!str->ctors.empty());
	CHECK(saw_default_ctor);

	// The dtor is discoverable exactly as CirBuilder::class_own_dtor does — a "~"
	// method_map key whose Variable is also in methods — and carries its D1 symbol.
	Variable *dtor = NULL;
	for (std::map<std::string, Variable *>::const_iterator kv = str->method_map.begin();
	     kv != str->method_map.end(); ++kv) {
		if (kv->first.empty() || kv->first[0] != '~' || !kv->second)
			continue;
		bool in_methods = false;
		for (size_t mi = 0; mi < str->methods.size() && !in_methods; ++mi)
			if (str->methods[mi] == kv->second) in_methods = true;
		if (in_methods) { dtor = kv->second; break; }
	}
	REQUIRE(dtor != nullptr);
	FuncDef *dfd = dynamic_cast<FuncDef *>(dtor->type);
	REQUIRE(dfd != nullptr);
	CHECK(!dfd->emit_symbol.empty());

	// operator= is restored into method_map (assignment resolution reads it).
	CHECK(str->method_map.find("operator=") != str->method_map.end());
}

// Phase 6 v13: a header's file-scope global VARIABLE definitions. The forest
// serializes TYPES; a header's inline globals (in_place, piecewise_construct,
// allocator_arg, ignore, …) are a separate category, so binding <string> used to
// omit them and the __madc_global_init that runs their ctors. v13 serializes each
// CLASS-typed file-scope global (cir_forest_global_record) and materialize_from_arena
// swizzles its type back to a DataDef*, so restored_globals() lists them; the bind
// layer rebuilds each into tkProgram->variables + a dkGlobalVar TopDecl and the
// existing passes emit the global + __madc_global_init. This gates the SAVE/LOAD
// at the forest level (the end-to-end emit — a bound <string> consumer emits
// __madc_global_init — is the forest_bind_gate strbind case).
TEST_CASE("Phase 6 v13: std::string's header file-scope globals restore with their types") {
	std::string main_path = std::string("/tmp/madc_p6v13_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string snap_path = std::string("/tmp/madc_p6v13_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include <string>\n"
		      "int main() { std::string s; return (int)s.size(); }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	forest.materialize_from_arena();		// builds the restored-globals view
	const std::vector<CirRestoredGlobal> &globals = forest.restored_globals();
	REQUIRE(!globals.empty());

	// The tag globals restore with their (class) types swizzled back to real
	// DataDef*s — e.g. std::piecewise_construct : piecewise_construct_t.
	bool saw_piecewise = false;
	for (size_t i = 0; i < globals.size(); ++i) {
		REQUIRE(globals[i].name != nullptr);
		REQUIRE(globals[i].type != nullptr);	// type resolved (else it'd be dropped)
		if (!strcmp(globals[i].name, "piecewise_construct")) {
			saw_piecewise = true;
			CHECK(globals[i].type->name == "piecewise_construct_t");
		}
	}
	CHECK(saw_piecewise);
}

// Phase 6 v14: a header's file-scope SCALAR-const globals. Unlike a class global
// (v13, default-ctor synthesized on load), a scalar global's init is a
// compile-time constant with no ctor — live emits it as a data item (`u64 64`),
// so its integer VALUE is serialized (CIR_GLOBALF_SCALAR_INIT + init_value) and
// load rebuilds a `T name = value;` decl. <new>'s hardware_*_interference_size = 64
// are exactly this. This gates the SAVE/LOAD at the forest level; the end-to-end
// emit (a bound <string> consumer emits `hardware_*: u64 64`, byte-identically to
// live) is the forest_bind_gate strbind case.
TEST_CASE("Phase 6 v14: std::string's scalar-const file-scope globals restore with their init value") {
	std::string main_path = std::string("/tmp/madc_p6v14_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string snap_path = std::string("/tmp/madc_p6v14_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include <string>\n"
		      "int main() { std::string s; return (int)s.size(); }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	forest.materialize_from_arena();
	const std::vector<CirRestoredGlobal> &globals = forest.restored_globals();

	// A scalar-const global carries CIR_GLOBALF_SCALAR_INIT + its integer value.
	// hardware_destructive_interference_size = 64 (a 64-bit scalar in <new>).
	bool saw_hw = false;
	for (size_t i = 0; i < globals.size(); ++i) {
		if (!(globals[i].gflags & CIR_GLOBALF_SCALAR_INIT))
			continue;
		REQUIRE(globals[i].name != nullptr);
		REQUIRE(globals[i].type != nullptr);	// a pinned scalar, resolved
		if (!strcmp(globals[i].name, "hardware_destructive_interference_size")) {
			saw_hw = true;
			CHECK(globals[i].init_value == 64);
		}
	}
	CHECK(saw_hw);
}

// Phase 6 v16: a header's file-scope CLASS-typed globals carry their INITIALIZER
// FORM so a bound consumer's __madc_global_init emits byte-identically to a live
// parse. v13 stored no form (flush set decl=NULL -> a direct-on-global default
// ctor); live runs the class-instance init path: `T x = T()` builds a stack temp
// then copies it in (COPY_TEMP), `T x{}` is a trivially-copyable self-copy
// (VALUE_INIT, needs no ctor -> in_place binds even with an empty restored ctor
// set). v16 also serializes DataDefCLASS::nvsize (left behind before v16 -> a
// restored empty class defaulted nvsize=0, so its temp alloca + struct-copy were
// 0-sized). <bits/...>'s in_place = `in_place_t in_place{}` (VALUE_INIT) and
// piecewise_construct = `piecewise_construct_t()` (COPY_TEMP) are exactly these.
TEST_CASE("Phase 6 v16: class-typed file-scope globals restore their init form + nvsize") {
	std::string main_path = std::string("/tmp/madc_p6v16_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string snap_path = std::string("/tmp/madc_p6v16_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include <string>\n"
		      "int main() { std::string s; return (int)s.size(); }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;	// B3 (v18): the arena IS the type-graph dump
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));

	forest.materialize_from_arena();
	const std::vector<CirRestoredGlobal> &globals = forest.restored_globals();

	// in_place is a value-init (`in_place_t in_place{}`); piecewise_construct is a
	// copy-from-default-temporary (`= piecewise_construct_t()`). Both are empty tag
	// classes, so their restored class must carry a nonzero nvsize (verbatim).
	bool saw_in_place = false, saw_piecewise = false;
	for (size_t i = 0; i < globals.size(); ++i) {
		if (globals[i].name == nullptr || globals[i].type == nullptr)
			continue;
		DataDefCLASS *c = dynamic_cast<DataDefCLASS *>(globals[i].type);
		if (!strcmp(globals[i].name, "in_place")) {
			saw_in_place = true;
			CHECK((globals[i].gflags & CIR_GLOBALF_CLASS_VALUE_INIT) != 0);
			REQUIRE(c != nullptr);
			CHECK(c->nvsize > 0);		// v16: nvsize serialized (was 0)
		} else if (!strcmp(globals[i].name, "piecewise_construct")) {
			saw_piecewise = true;
			CHECK((globals[i].gflags & CIR_GLOBALF_CLASS_COPY_TEMP) != 0);
			REQUIRE(c != nullptr);
			CHECK(c->nvsize > 0);
		}
	}
	CHECK(saw_in_place);
	CHECK(saw_piecewise);
}

// B3 flip Chunk 1: the dumped DefArena reaches freeze-time fidelity — DK_TYPEDEF
// records (flat + namespaced-alias), ns_id stamping, and INLINE method body
// locations — and the container's arena segments bind back readably (the FIRST
// reader over the Chunk-A dump; until now the dumped bytes were unverified).
// Freeze exactly as --freeze does (pack_recording + forest_arena_enabled), open,
// and read the FrozenDefArena directly.
TEST_CASE("B3 flip chunk 1: the dumped arena carries typedefs, namespace ids, and inline body locations") {
	std::string inc_path = std::string("/tmp/madc_b3c1_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_b3c1_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string snap_path = std::string("/tmp/madc_b3c1_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "typedef unsigned long myword_t;\n"
		       "namespace N { struct P { int x; int y; }; }\n"
		       "class Counter { public: int c; int get() { return c; } };\n"
		       // A plain struct USED AS A BASE is promoted to a fresh
		       // DataDefCLASS sharing the struct's type_id — the id table
		       // repoints to the promoted object, so the arena records it
		       // as a CLASS (the iterator-tag shape the oracle caught).
		       "struct PBase { int pb; };\n"
		       "class PDeriv : public PBase { public: int pd; };\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { Counter k; k.c = 3; N::P p; p.x = 1;\n"
		      "             PDeriv d; d.pd = 4;\n"
		      "             myword_t w = 2; return k.get() + p.x + d.pd + (int)w; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;		// as --freeze sets (madc.cpp)
		progA->forest_arena_enabled = true;	// B3: populate forest_arena during parse
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

	const madc::dis::FrozenDefArena &a = forest.arena();
	REQUIRE(a.def_slots() > 0);		// the arena segments bound (non-empty view)

	// One scan; collect the three fidelity surfaces.
	bool saw_typedef = false;		// DK_TYPEDEF "myword_t" -> a pinned scalar
	bool saw_ns = false;			// P's own record stamped ns_id == "N"
	uint32_t counter_id = 0;
	madc::dis::defrec counter_rec;
	memset(&counter_rec, 0, sizeof(counter_rec));
	for (uint32_t s = 0; s < a.def_slots(); ++s) {
		uint32_t tid = madc::dis::arena_id_of(s);
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r) || r.kind == madc::dis::DK_NONE)
			continue;
		const char *nm = r.name_id ? a.c_str(r.name_id) : "";
		if (r.kind == madc::dis::DK_TYPEDEF && !strcmp(nm, "myword_t")) {
			saw_typedef = true;
			CHECK(madc::dis::arena_id_is_pinned(r.ref0));	// unsigned long -> pinned slot
			CHECK(r.ns_id == 0);				// flat (global) typedef
		}
		if (r.kind == madc::dis::DK_STRUCT && !strcmp(nm, "P")) {
			saw_ns = true;
			REQUIRE(r.ns_id != 0);
			CHECK(std::string(a.c_str(r.ns_id)) == "N");
		}
		if (r.kind == madc::dis::DK_CLASS && !strcmp(nm, "Counter")) {
			counter_id = tid;
			counter_rec = r;
		}
	}
	CHECK(saw_typedef);
	CHECK(saw_ns);

	// Counter::get is INLINE (its func-def is in the frozen AST) -> its DK_FUNC
	// record carries DF_HAS_FOREST_BODY + a body location inside the grove.
	REQUIRE(counter_id != 0);
	bool saw_inline_get = false;
	for (uint32_t i = 0; i < counter_rec.methods_count; ++i) {
		madc::dis::methodrec md;
		REQUIRE(a.get_payload(counter_rec.methods_begin, i, md));
		if (!md.disp_key_id || strcmp(a.c_str(md.disp_key_id), "get"))
			continue;
		REQUIRE(madc::dis::arena_id_is_project(md.func_id));
		madc::dis::defrec fr;
		REQUIRE(a.get_def_at(md.func_id, fr));
		CHECK(fr.kind == madc::dis::DK_FUNC);
		bool has_body = (fr.flags & madc::dis::DF_HAS_FOREST_BODY) != 0;
		CHECK(has_body);
		CHECK(fr.body_unit < forest.unit_count());
		saw_inline_get = true;
	}
	CHECK(saw_inline_get);

	// v18: the arena reconstruct IS the load path — it must surface the
	// typedef, the namespaced struct, the class with its inline method, and
	// the promoted struct-as-base (PBase -> class) to forest_restore_decls.
	const std::vector<CirRestoredType> &av = forest.materialize_from_arena();
	bool r_typedef = false, r_ns = false, r_counter = false, r_pderiv = false;
	for (size_t i = 0; i < av.size(); ++i) {
		if (!av[i].name)
			continue;
		std::string nm(av[i].name);
		if (av[i].kind == CIR_TYPEK_TYPEDEF && nm == "myword_t"
		    && av[i].underlying)
			r_typedef = true;
		if (av[i].kind == CIR_TYPEK_STRUCT && nm == "P"
		    && av[i].ns && std::string(av[i].ns) == "N")
			r_ns = true;
		if (av[i].kind == CIR_TYPEK_CLASS && nm == "Counter") {
			r_counter = true;
			DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(av[i].dd);
			REQUIRE(cdd != nullptr);
			std::map<std::string, Variable *>::iterator gi =
				cdd->method_map.find("get");
			REQUIRE(gi != cdd->method_map.end());
			FuncDef *gfd = dynamic_cast<FuncDef *>(gi->second->type);
			REQUIRE(gfd != nullptr);
			CHECK(gfd->has_forest_body);	// inline body reconnected
			CHECK(!gfd->declaration_only);
		}
		if (av[i].kind == CIR_TYPEK_CLASS && nm == "PDeriv") {
			r_pderiv = true;
			DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(av[i].dd);
			REQUIRE(cdd != nullptr);
			REQUIRE(cdd->bases.size() == 1);
			REQUIRE(cdd->bases[0].base != nullptr);
			CHECK(cdd->bases[0].base->name == "PBase");	// promoted base restored
		}
	}
	CHECK(r_typedef);
	CHECK(r_ns);
	CHECK(r_counter);
	CHECK(r_pderiv);
}

// ---------------------------------------------------------------------------
// RC2: file-scope FREE-FUNCTION declarations freeze into the arena (DK_FUNC +
// DF_IS_FREE_FUNC, name = the funcdef_map key) and restore as declaration-only
// FuncDefs — so a bound call resolves the real signature (typed extern proto)
// instead of the dlsym implicit-variadic fallback. Slice 1 = prototypes only:
// a BODIED function (an inline helper, or the producer's own main) must NOT
// restore — the inline free-function body model is the follow-on.
// ---------------------------------------------------------------------------

TEST_CASE("RC2: free-function prototypes freeze into the arena and restore") {
	std::string inc_path = std::string("/tmp/madc_rc2_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_rc2_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string snap_path = std::string("/tmp/madc_rc2_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "int rc2_vprobe(const char *fmt, ...);\n"
		       "unsigned long rc2_len(const char *s);\n"
		       "static inline int rc2_inline(int v) { return v + 1; }\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { return rc2_inline(0); }\n";
	}

	std::shared_ptr<Program> prog = std::make_shared<Program>();
	prog->pack_recording = true;		// as --freeze sets (madc.cpp)
	prog->forest_arena_enabled = true;	// B3 (v18+): the arena IS the type-graph dump
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
	forest.materialize_from_arena();

	const std::vector<CirRestoredFunc> &fns = forest.restored_funcs();
	const CirRestoredFunc *vprobe = NULL, *len = NULL;
	bool saw_inline = false, saw_main = false;
	for (size_t i = 0; i < fns.size(); ++i) {
		if (!fns[i].name)
			continue;
		std::string nm(fns[i].name);
		if (nm == "rc2_vprobe") vprobe = &fns[i];
		if (nm == "rc2_len")    len = &fns[i];
		if (nm == "rc2_inline") saw_inline = true;
		if (nm == "main")       saw_main = true;
	}
	// The two prototypes restore with their REAL signatures.
	REQUIRE(vprobe != NULL);
	REQUIRE(vprobe->fd != NULL);
	CHECK(vprobe->fd->declaration_only);
	CHECK(vprobe->fd->is_varargs);
	// The live parse stores [const char*, hidden ddINT64 __va_args slot] for a
	// `(const char *, ...)` prototype — the restore reproduces that verbatim.
	REQUIRE(vprobe->fd->parameters.size() == 2);
	{
		bool p0_ptr = vprobe->fd->parameters[0]
			   && vprobe->fd->parameters[0]->is_pointer();
		CHECK(p0_ptr);
		bool p1_va = vprobe->fd->parameters[1]
			  && vprobe->fd->parameters[1]->size == 8;
		CHECK(p1_va);
		// int return — 4-byte integer, NOT the fallback's 64-bit long.
		CHECK(vprobe->fd->return_value_type().size == 4);
	}
	REQUIRE(len != NULL);
	REQUIRE(len->fd != NULL);
	CHECK(len->fd->declaration_only);
	CHECK(!len->fd->is_varargs);
	REQUIRE(len->fd->parameters.size() == 1);
	CHECK(len->fd->return_value_type().size == 8);
	// v25 (root-vs-include, the v24 discriminator): a BODIED function
	// restores whenever its definition came from an INCLUDED file — system,
	// user, or embedded header alike (the <ns_*> wrappers, testinclude's
	// helper). This header's inline helper therefore RESTORES; the
	// producer's own main() (the TU root) must NEVER restore into a consumer.
	CHECK(saw_inline);
	CHECK(!saw_main);
}

// ---------------------------------------------------------------------------
// #23: a restored class's METHODS register into the program exactly as
// parseFunction's prototype tail leaves them — funcdef_map[method-id] + a
// program-scope Variable whose data is a Method with owner_class set. That is
// what lets Pass 0.75 emit the ctor/dtor typed extern protos at the same
// sorted funcdef_map positions as a live parse (the dtor's scope-exit cleanup
// reference has no other declaration source), closing the last import/export
// and proto divergences of the whole-TU byte-identity gate.
// ---------------------------------------------------------------------------

TEST_CASE("#23: restored class methods register as funcdef_map[method-id] + program Variable") {
	std::string inc_path = std::string("/tmp/madc_b23_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_b23_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string cons_path = std::string("/tmp/madc_b23_cons_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_b23_snap_")
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
		std::ofstream cn(cons_path.c_str());
		cn << "int main() { return 0; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;
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

	// A FRESH Program initialized by a trivial tokenize (strpool + tkProgram),
	// then restore + flush. (In the real flow the restore runs DURING the
	// consumer's tokenize — at #include time, before the end-of-tokenize
	// flush; here the flush is invoked explicitly after staging.)
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(cons_path.c_str());
	REQUIRE(tpB != nullptr);
	std::remove(cons_path.c_str());
	progB->forest_restore_decls(forest);
	progB->flush_forest_pending_globals();

	// funcdef_map holds each method under its live method-id key.
	REQUIRE(progB->funcdef_map.count("Counter__get") == 1);
	REQUIRE(progB->funcdef_map.count("Counter__add") == 1);
	FuncDef *gf = progB->funcdef_map["Counter__get"];
	REQUIRE(gf != nullptr);
	REQUIRE(gf->parameters.size() == 1);		// the hidden __this
	CHECK(gf->parameters[0]->is_pointer());

	// The program-scope Variable + Method(owner_class) parseFunction leaves.
	Variable *gv = progB->findVariable("Counter__get");
	REQUIRE(gv != nullptr);
	REQUIRE(gv->data != nullptr);
	Method *gm = (Method *)gv->data;
	DataDefCLASS *owner = gm->owner_class;
	REQUIRE(owner != nullptr);
	CHECK(owner->name == "Counter");
	CHECK(progB->struct_map.count("Counter") == 1);
	CHECK(progB->struct_map.find("Counter")->second == owner);

	// Live keeps ONE Variable shared by tkProgram scope and the class's
	// methods/method_map (TokenCLASS::parse re-finds parseFunction's
	// registration) — the flush must reuse the class's Variable, not mint a
	// duplicate. And EVERY restored method Variable carries its
	// Method(owner_class) — findMethodOverload derives the hidden-__this
	// skip from it (data==NULL broke overload arity ranking: the
	// append(initializer_list<char>) mispick).
	bool shared = false;
	for ( size_t mi = 0; mi < owner->methods.size(); ++mi ) {
		Variable *mv = owner->methods[mi];
		if ( !mv ) continue;
		REQUIRE(mv->data != nullptr);
		CHECK(((Method *)mv->data)->owner_class == owner);
		if ( mv == gv ) shared = true;
	}
	CHECK(shared);
}

// ---------------------------------------------------------------------------
// v20 (widening slice 2): the parser's TEMPLATE-NAME state — the pattern maps,
// each definition's captured TOKEN runs (.madh record form) + params/flags/ns
// — serializes at freeze and restores into a fresh Program's maps, so a bound
// consumer resolves the template name ("use of undeclared identifier 'vector'"
// was the corpus blocker: the instantiation PRODUCT was in the arena, the NAME
// was not) and the UNCHANGED live instantiation machinery runs.
// ---------------------------------------------------------------------------

TEST_CASE("v20: template-NAME state (pattern maps + token bodies) freezes and restores") {
	uint64_t producer_primary_fingerprint = 0;
	uint64_t producer_partial_fingerprint = 0;
	uint64_t producer_nested_fingerprint = 0;
	std::string inc_path = std::string("/tmp/madc_v20t_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_v20t_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string cons_path = std::string("/tmp/madc_v20t_cons_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_v20t_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "template<typename T, typename U = T> struct W2Box {\n"
		       "    T v; U u;\n"
		       "    T get() { return v; }\n"
		       "};\n"
		       "template<typename T> struct W2Box<T*, T*> { T *p; };\n"
		       "struct W2Owner {\n"
		       "    template<typename T> struct Inner { T value; };\n"
		       "};\n"
		       "struct W2AnonBefore { union { int before; }; };\n"
		       "template<typename T> struct W2AnonPattern {\n"
		       "    union { T value; int other; };\n"
		       "};\n"
		       "struct W2AnonAfter { union { int after; }; };\n"
		       "template<typename T> struct W2UnusedAlias {\n"
		       "    using W2CaptureOnlyAlias = T;\n"
		       "};\n"
		       "template<typename L, typename R>\n"
		       "struct W2DependentChain : W2DependentChain<L*, R> {};\n"
		       "template<typename L, typename R>\n"
		       "struct W2DependentBase : W2DependentChain<L, R> {};\n"
		       "template<typename T> using W2Same = W2Box<T, T>;\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { W2Box<int> b; b.v = 3; b.u = 4; return b.get(); }\n";
	}
	{
		std::ofstream cn(cons_path.c_str());
		cn << "int main() { return 0; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		Program::TemplateDef *live_primary = progA->find_template("W2Box");
		REQUIRE(live_primary != nullptr);
		REQUIRE(live_primary->class_pattern_id != 0);
		const Program::ClassPattern *live_primary_pattern =
			progA->class_pattern_arena.get(live_primary->class_pattern_id);
		REQUIRE(live_primary_pattern != nullptr);
		CHECK(live_primary_pattern->capture_reason ==
		      Program::ClassParseReason::None);
		producer_primary_fingerprint =
			live_primary_pattern->semantic_fingerprint;
		std::vector<Program::TemplateDef> *live_partials =
			progA->partial_spec_map.find("W2Box");
		REQUIRE(live_partials != nullptr);
		REQUIRE(live_partials->size() == 1);
		INFO("partial capture reason="
		     << (uint32_t)(*live_partials)[0].class_pattern_reason);
		REQUIRE((*live_partials)[0].class_pattern_id != 0);
		CHECK((*live_partials)[0].class_pattern_id !=
		      live_primary->class_pattern_id);
		const Program::ClassPattern *live_partial_pattern =
			progA->class_pattern_arena.get(
				(*live_partials)[0].class_pattern_id);
		REQUIRE(live_partial_pattern != nullptr);
		CHECK(live_partial_pattern->capture_reason ==
		      Program::ClassParseReason::None);
		producer_partial_fingerprint =
			live_partial_pattern->semantic_fingerprint;
		datadef_map_citer owner_it = progA->struct_map.find("W2Owner");
		REQUIRE(owner_it != progA->struct_map.end());
		DataDefCLASS *live_owner = dynamic_cast<DataDefCLASS *>(owner_it->second);
		REQUIRE(live_owner != nullptr);
		for (std::map<std::string, DataDef *>::const_iterator it =
		     live_owner->type_aliases.begin();
		     it != live_owner->type_aliases.end(); ++it)
			CHECK(it->first.find("__madc_class_pattern_") == std::string::npos);
		std::vector<Program::TemplateDef> *nested_defs =
			progA->template_map.find("Inner");
		REQUIRE(nested_defs != nullptr);
		Program::TemplateDef *live_nested = nullptr;
		for (size_t i = 0; i < nested_defs->size(); ++i)
			if ((*nested_defs)[i].owner_class == live_owner)
				live_nested = &(*nested_defs)[i];
		REQUIRE(live_nested != nullptr);
		REQUIRE(live_nested->class_pattern_id != 0);
		const Program::ClassPattern *live_nested_pattern =
			progA->class_pattern_arena.get(live_nested->class_pattern_id);
		REQUIRE(live_nested_pattern != nullptr);
		producer_nested_fingerprint =
			live_nested_pattern->semantic_fingerprint;
		datadef_map_citer anon_before_it =
			progA->struct_map.find("W2AnonBefore");
		datadef_map_citer anon_after_it =
			progA->struct_map.find("W2AnonAfter");
		REQUIRE(anon_before_it != progA->struct_map.end());
		REQUIRE(anon_after_it != progA->struct_map.end());
		DataDefSTRUCT *anon_before =
			dynamic_cast<DataDefSTRUCT *>(anon_before_it->second);
		DataDefSTRUCT *anon_after =
			dynamic_cast<DataDefSTRUCT *>(anon_after_it->second);
		REQUIRE(anon_before != nullptr);
		REQUIRE(anon_after != nullptr);
		REQUIRE(anon_before->anonymous_aggregates.size() == 1);
		REQUIRE(anon_after->anonymous_aggregates.size() == 1);
		const std::string &before_name =
			anon_before->anonymous_aggregates[0].aggregate->name;
		const std::string &after_name =
			anon_after->anonymous_aggregates[0].aggregate->name;
		REQUIRE(before_name.find("__anon_") == 0);
		REQUIRE(after_name.find("__anon_") == 0);
		CHECK(std::stoull(after_name.substr(7)) ==
		      std::stoull(before_name.substr(7)) + 1);
		CHECK(progA->user_typedef_names.count("W2CaptureOnlyAlias") == 0);
		Program::TemplateDef *dependent_base =
			progA->find_template("W2DependentBase");
		REQUIRE(dependent_base != nullptr);
		REQUIRE(dependent_base->class_pattern_id != 0);
		const Program::ClassPattern *dependent_base_pattern =
			progA->class_pattern_arena.get(
				dependent_base->class_pattern_id);
		REQUIRE(dependent_base_pattern != nullptr);
		CHECK(dependent_base_pattern->capture_reason ==
		      Program::ClassParseReason::None);
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));
	forest.materialize_from_arena();

	// The metadata view: the primary (with its per-param default run), the
	// partial spec (with per-slot pattern runs), and the alias template.
	const std::vector<CirRestoredTemplate> &ts = forest.restored_templates();
	const CirRestoredTemplate *primary = NULL, *partial = NULL, *alias = NULL;
	for (size_t i = 0; i < ts.size(); ++i) {
		if (!ts[i].key) continue;
		std::string k(ts[i].key);
		if (k == "W2Box" && ts[i].kind == CIR_TMPLK_CLASS)   primary = &ts[i];
		if (k == "W2Box" && ts[i].kind == CIR_TMPLK_PARTIAL) partial = &ts[i];
		if (k == "W2Same" && ts[i].kind == CIR_TMPLK_ALIAS)  alias = &ts[i];
	}
	REQUIRE(primary != NULL);
	REQUIRE(primary->params.size() == 2);
	CHECK(std::string(primary->params[0].first) == "T");
	CHECK((primary->params[0].second & CIR_TMPLP_IS_TYPE) != 0);
	CHECK(primary->body.count > 0);			// the captured class body
	CHECK(primary->pattern != NULL);
	CHECK(primary->pattern_words > 0);
	CHECK(primary->pattern_reason ==
	      (uint32_t)Program::ClassParseReason::None);
	REQUIRE(primary->defaults.size() == 2);
	CHECK(primary->defaults[0].count == 0);		// T has no default
	CHECK(primary->defaults[1].count > 0);		// U = T
	{
		bool has_file = primary->body.file != NULL
			     && std::string(primary->body.file) == inc_path;
		CHECK(has_file);			// token provenance (origin file)
	}
	REQUIRE(partial != NULL);
	CHECK((partial->flags & CIR_TMPLF_IS_PARTIAL_SPEC) != 0);
	CHECK(partial->spec.size() == 2);		// one pattern run per arg slot
	CHECK(partial->body.count > 0);
	CHECK(partial->pattern != NULL);
	CHECK(partial->pattern_words > 0);
	REQUIRE(alias != NULL);
	CHECK(alias->body.count > 0);			// the alias TARGET tokens

	// RESTORE into a fresh Program: the maps hold parse-equivalent state and
	// the live selection machinery (find_template / template_with_body /
	// find_template_alias) reads it unchanged.
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(cons_path.c_str());
	REQUIRE(tpB != nullptr);
	std::remove(cons_path.c_str());
	progB->forest_restore_decls(forest);

	Program::TemplateDef *td = progB->find_template("W2Box");
	REQUIRE(td != nullptr);
	REQUIRE(td->typeparams.size() == 2);
	CHECK(td->typeparams[0] == "T");
	CHECK(td->typeparams[1] == "U");
	CHECK(td->typeparam_is_type[0]);
	CHECK(!td->body.empty());
	REQUIRE(td->class_pattern_id != 0);
	const Program::ClassPattern *restored_primary_pattern =
		progB->class_pattern_arena.get(td->class_pattern_id);
	REQUIRE(restored_primary_pattern != nullptr);
	CHECK(restored_primary_pattern->semantic_fingerprint ==
	      producer_primary_fingerprint);
	CHECK(progB->class_pattern_fingerprint(*restored_primary_pattern) ==
	      producer_primary_fingerprint);
	REQUIRE(td->typeparam_defaults.size() == 2);
	CHECK(td->typeparam_defaults[0].empty());
	CHECK(!td->typeparam_defaults[1].empty());
	CHECK(progB->template_with_body("W2Box") != nullptr);
	std::vector<Program::TemplateDef> *restored_nested_defs =
		progB->template_map.find("Inner");
	REQUIRE(restored_nested_defs != nullptr);
	Program::TemplateDef *restored_nested = nullptr;
	for (size_t i = 0; i < restored_nested_defs->size(); ++i)
		if ((*restored_nested_defs)[i].owner_class
		    && (*restored_nested_defs)[i].owner_class->name == "W2Owner")
			restored_nested = &(*restored_nested_defs)[i];
	REQUIRE(restored_nested != nullptr);
	REQUIRE(restored_nested->class_pattern_id != 0);
	const Program::ClassPattern *restored_nested_pattern =
		progB->class_pattern_arena.get(restored_nested->class_pattern_id);
	REQUIRE(restored_nested_pattern != nullptr);
	CHECK(restored_nested_pattern->semantic_fingerprint ==
	      producer_nested_fingerprint);
	{
		// Restored body tokens carry the header's file (provenance drives
		// from_system_header classification + error attribution at
		// instantiation) and non-zero line numbers (.madh keeps line/col).
		TokenBase *bt0 = NULL;
		for (TokenBase *t : td->body)
			if (t) { bt0 = t; break; }
		REQUIRE(bt0 != NULL);
		bool file_ok = bt0->file && std::string(bt0->file) == inc_path;
		CHECK(file_ok);
		CHECK(bt0->line > 0);
	}
	CHECK(progB->partial_spec_map.count("W2Box") == 1);
	{
		std::vector<Program::TemplateDef> *psv =
			progB->partial_spec_map.find("W2Box");
		REQUIRE(psv != nullptr);
		REQUIRE(psv->size() == 1);
		CHECK((*psv)[0].is_partial_specialization);
		CHECK((*psv)[0].spec_pattern.size() == 2);
		REQUIRE((*psv)[0].class_pattern_id != 0);
		CHECK((*psv)[0].class_pattern_id != td->class_pattern_id);
		const Program::ClassPattern *restored_partial_pattern =
			progB->class_pattern_arena.get((*psv)[0].class_pattern_id);
		REQUIRE(restored_partial_pattern != nullptr);
		CHECK(restored_partial_pattern->semantic_fingerprint ==
		      producer_partial_fingerprint);
		CHECK(progB->class_pattern_fingerprint(*restored_partial_pattern) ==
		      producer_partial_fingerprint);
	}
	CHECK(progB->template_alias_map.count("W2Same") == 1);
	std::remove(inc_path.c_str());
}

// ---------------------------------------------------------------------------
// v21 (widening slice 3, part 1): the skipped-namespace-fn-template PLACEHOLDER
// surface. A live parse of `namespace std { template<...> void _Destroy(...) }`
// leaves a declaration-only placeholder FuncDef (__ns_std__Destroy) in
// funcdef_map, a namespace_map[ns][display] binding — the resolution chokepoint
// a qualified `std::_Destroy(...)` call reads — and a placeholder seed in
// namespace_fn_overload_sets (so instantiations always mint fresh __oN
// symbols). v20 restored the pattern TOKENS but not this surface, so a NEW
// specialization in a consumer failed with "use of undeclared identifier".
// ---------------------------------------------------------------------------

TEST_CASE("v21: skipped-ns-fn-template placeholder restores with its namespace binding + overload seed") {
	std::string inc_path = std::string("/tmp/madc_v21p_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_v21p_main_")
			      + std::to_string((long)getpid()) + ".cpp";
	std::string cons_path = std::string("/tmp/madc_v21p_cons_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_v21p_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "namespace w3 {\n"
		       "template<typename T> T w3pick(T a, T b) { return a < b ? a : b; }\n"
		       "}\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { return 0; }\n";
	}
	{
		std::ofstream cn(cons_path.c_str());
		cn << "int main() { return 0; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		// The live registration surface this case must round-trip.
		REQUIRE(progA->funcdef_map.count("__ns_w3_w3pick") == 1);
		REQUIRE(madc_cir_freeze(progA.get(), main_path.c_str(),
					snap_path.c_str(), /*append=*/false) == 0);
	}
	std::remove(main_path.c_str());

	const void *image = NULL;
	size_t image_len = 0;
	REQUIRE(cir_forest_map_image(snap_path.c_str(), image, image_len));
	std::remove(snap_path.c_str());
	CirFrozenForest forest;
	REQUIRE(forest.open(image, image_len, /*c2m=*/NULL));
	forest.materialize_from_arena();

	// The placeholder restores verbatim: declaration-only, with the source
	// identity (display + namespace) the live registration set.
	const std::vector<CirRestoredFunc> &fns = forest.restored_funcs();
	const CirRestoredFunc *ph = NULL;
	for (size_t i = 0; i < fns.size(); ++i)
		if (fns[i].name && std::string(fns[i].name) == "__ns_w3_w3pick")
			ph = &fns[i];
	REQUIRE(ph != NULL);
	REQUIRE(ph->fd != NULL);
	CHECK(ph->fd->declaration_only);
	CHECK(ph->fd->function_display_name == "w3pick");
	CHECK(ph->fd->namespace_name == "w3");

	// A fresh Program: restore + flush reproduce the registration-site state.
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(cons_path.c_str());
	REQUIRE(tpB != nullptr);
	std::remove(cons_path.c_str());
	progB->forest_restore_decls(forest);
	progB->flush_forest_pending_globals();

	REQUIRE(progB->funcdef_map.count("__ns_w3_w3pick") == 1);
	Variable *pv = progB->findVariable("__ns_w3_w3pick");
	REQUIRE(pv != nullptr);
	// The namespace binding — what a qualified `w3::w3pick(...)` call reads.
	variable_map_t &nsmap = progB->namespace_map["w3"];
	REQUIRE(nsmap.find("w3pick") != nsmap.end());
	CHECK(nsmap["w3pick"] == pv);
	// The overload-set placeholder seed (the retained body-bearing pattern is
	// in the restored fn_template_map, so the live seed condition holds).
	CHECK(progB->fn_template_map.count("w3::w3pick") == 1);
	REQUIRE(progB->namespace_fn_overload_sets.count("w3::w3pick") == 1);
	{
		std::vector<Program::NamespaceFnOverload> &ovset =
			progB->namespace_fn_overload_sets["w3::w3pick"];
		REQUIRE(ovset.size() == 1);
		CHECK(ovset[0].param_spelling == "\x01fn-template-placeholder");
		CHECK(ovset[0].var == pv);
	}
	std::remove(inc_path.c_str());
}

// ---------------------------------------------------------------------------
// v23: DEFAULT ARGUMENTS — a parameter's default expression is a PARSED TREE
// on the live FuncDef (param_defaults[i], built by parseExpression at
// parseFunction's `= expr` branch), which both the arity gate
// (required_param_count) and the call-site default fill read. The freeze
// captures the default's RAW SOURCE TOKENS (param_default_tokens -> the
// paramrec def_tok_* run in the arena tokbytes block) and the pending-funcs
// flush re-runs parseExpression over them inside the owner's class scope —
// so a bound `string greet = "hello"` selects basic_string(const char*,
// const _Alloc& = _Alloc()) exactly as live does.
// ---------------------------------------------------------------------------

TEST_CASE("v23: method param DEFAULT ARGUMENTS freeze as token runs and restore as parsed trees") {
	std::string inc_path = std::string("/tmp/madc_v23d_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_v23d_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string cons_path = std::string("/tmp/madc_v23d_cons_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_v23d_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		// y = literal default; z = a CLASS-STATIC const default (K) —
		// the flush must resolve it through the reproduced owner scope
		// (the live parse ran inside parseFunction's param compound,
		// whose method->owner_class feeds the identifier arm).
		inc << "class Adder { public: static const int K = 5; int n;\n"
		       "    int add(int x, int y = 7, int z = K)"
		       " { return x + y + z + n; } };\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { Adder a; a.n = 0; return a.add(1); }\n";
	}
	{
		std::ofstream cn(cons_path.c_str());
		cn << "int main() { return 0; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		// The LIVE FuncDef captured the raw token runs (capture is gated
		// on forest_arena_enabled).
		REQUIRE(progA->funcdef_map.count("Adder__add") == 1);
		FuncDef *lf = progA->funcdef_map["Adder__add"];
		REQUIRE(lf->param_default_tokens.size() >= 4);
		CHECK(lf->param_default_tokens[2].size() == 1);	// `7`
		CHECK(lf->param_default_tokens[3].size() == 1);	// `K`
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

	std::shared_ptr<Program> progB = std::make_shared<Program>();
	progB->bind_forest = &forest;	// the flush reads restored_param_defaults
	TokenProgram *tpB = progB->tokenize(cons_path.c_str());
	REQUIRE(tpB != nullptr);
	std::remove(cons_path.c_str());
	progB->forest_restore_decls(forest);
	progB->flush_forest_pending_globals();
	progB->bind_forest = NULL;	// forest is stack-local; detach before dtors

	REQUIRE(progB->funcdef_map.count("Adder__add") == 1);
	FuncDef *rf = progB->funcdef_map["Adder__add"];
	REQUIRE(rf != nullptr);
	REQUIRE(rf->parameters.size() == 4);		// __this, x, y, z
	// param_defaults rebuilt index-aligned: hidden __this + x are NULL,
	// y and z carry parsed expression trees.
	REQUIRE(rf->param_defaults.size() == 4);
	CHECK(rf->param_defaults[0] == nullptr);
	CHECK(rf->param_defaults[1] == nullptr);
	REQUIRE(rf->param_defaults[2] != nullptr);
	REQUIRE(rf->param_defaults[3] != nullptr);
	CHECK(rf->param_defaults[2]->ival() == 7);
	CHECK(rf->param_defaults[3]->ival() == 5);	// K resolved in owner scope
	// The arity gate — a 1-arg call (`a.add(1)`) is legal, as live.
	CHECK(rf->required_param_count() == 2);		// __this + x
}

// v25: an ARRAY-typed typedef (the va_list shape — `typedef struct tag {...}
// name[1];`) freezes as a DK_CARRAY derived record (ref0 = element, folded
// count) and restores VERBATIM: the typedef resolves and its underlying is a
// DataDefCArray over the restored struct. Before v25 the array type had no
// arena record, so the typedef "cleanly lacked" and a bound <stdarg.h> had no
// va_list (soak family a).
TEST_CASE("v25: an array-typed typedef (va_list shape) freezes as DK_CARRAY and restores") {
	std::string inc_path = std::string("/tmp/madc_v25a_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_v25a_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string cons_path = std::string("/tmp/madc_v25a_cons_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_v25a_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		// myva = the va_list shape; grid = a multi-dim fold (2*3 -> one
		// record with count 6) over a PINNED element (int).
		inc << "typedef struct __va_tag { unsigned int gp; unsigned int fp;\n"
		       "    void *oa; void *rsa; } myva[1];\n"
		       "typedef int grid[2][3];\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { myva ap; grid g; return 0; }\n";
	}
	{
		std::ofstream cn(cons_path.c_str());
		cn << "int main() { return 0; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		// The LIVE typedef's underlying is the array type.
		flat_datatype_map_iter li = progA->datatype_map.find("myva");
		REQUIRE(li != progA->datatype_map.end());
		REQUIRE(dynamic_cast<DataDefCArray *>(&(*li)->definition) != nullptr);
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

	// A FRESH Program that never parses the header.
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(cons_path.c_str());
	REQUIRE(tpB != nullptr);
	std::remove(cons_path.c_str());
	CHECK(progB->datatype_map.find("myva") == progB->datatype_map.end());
	progB->forest_restore_decls(forest);
	progB->flush_forest_pending_globals();	// applies staged datatype_map writes

	flat_datatype_map_iter mit = progB->datatype_map.find("myva");
	REQUIRE(mit != progB->datatype_map.end());
	DataDefCArray *va = dynamic_cast<DataDefCArray *>(&(*mit)->definition);
	REQUIRE(va != nullptr);
	CHECK(va->count == 1);
	CHECK(va->count_expr == nullptr);
	CHECK(va->size == 24);				// the 24-byte tag struct
	DataDefSTRUCT *tag = dynamic_cast<DataDefSTRUCT *>(va->element_type);
	REQUIRE(tag != nullptr);
	CHECK(tag->members.size() == 4);

	flat_datatype_map_iter git = progB->datatype_map.find("grid");
	REQUIRE(git != progB->datatype_map.end());
	DataDefCArray *gr = dynamic_cast<DataDefCArray *>(&(*git)->definition);
	REQUIRE(gr != nullptr);
	CHECK(gr->count == 6);				// dims folded (2*3), as live
	REQUIRE(gr->element_type != nullptr);
	// The element swizzles back as a pinned 4-byte integer (a plain `int`
	// serializes via its pinned rawtype slot — assert shape, not slot).
	CHECK(gr->element_type->size == 4);
	CHECK(gr->element_type->is_integer());
	CHECK(gr->size == 24);				// 6 * sizeof(int)
	CHECK(progB->user_typedef_names.count("myva") == 1);
}

// v25: a ctor-syntax file-scope global `T name(args);` (the <compare> ordering
// constants' out-of-class static member definition shape) serializes its args
// list as a RAW TOKEN run (CIR_GLOBALF_CTOR_ARG_TOKENS) and the flush re-runs
// the live args-list parse over it — the rebuilt TokenDecl::ctor_args carries
// the real argument, so global_ctor_call selects the real ctor overload
// instead of a no-matching default construction.
TEST_CASE("v25: a ctor-syntax header global restores its ctor ARGUMENTS as parsed trees") {
	std::string inc_path = std::string("/tmp/madc_v25b_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_v25b_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string cons_path = std::string("/tmp/madc_v25b_cons_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_v25b_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		// The <compare> shape reduced: a class whose ONLY ctor takes an
		// argument, plus a file-scope constant constructed with one.
		inc << "class Tagv { public: int v; Tagv(int x) { v = x; } };\n"
		       "Tagv t_less(-1);\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { return t_less.v; }\n";
	}
	{
		std::ofstream cn(cons_path.c_str());
		cn << "int main() { return 0; }\n";
	}

	TokenID live_arg_id = TokenID::tkBase;	// live's parsed ctor-arg node kind
	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		// The LIVE decl carries one parsed ctor arg + the captured raw run.
		bool live_found = false;
		for (const Program::TopDecl &td : progA->top_decls)
			if (td.kind == Program::DeclKind::dkGlobalVar
			    && td.name == "t_less" && td.decl) {
				live_found = true;
				REQUIRE(td.decl->ctor_args.size() == 1);
				REQUIRE(td.decl->ctor_args[0] != nullptr);
				live_arg_id = td.decl->ctor_args[0]->id();
				CHECK(td.decl->ctor_arg_src.size() == 2);	// `-` `1`
			}
		REQUIRE(live_found);
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

	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(cons_path.c_str());
	REQUIRE(tpB != nullptr);
	std::remove(cons_path.c_str());
	progB->forest_restore_decls(forest);
	progB->flush_forest_pending_globals();

	// The restored global carries the args-token run (restored_globals fills
	// lazily inside forest_restore_decls' materialization).
	bool found = false;
	for (const CirRestoredGlobal &rg : forest.restored_globals())
		if (rg.name && !strcmp(rg.name, "t_less")) {
			found = true;
			CHECK((rg.gflags & CIR_GLOBALF_CTOR_ARG_TOKENS) != 0);
			CHECK(rg.ctor_bytes != nullptr);
			CHECK(rg.ctor_count >= 1);	// `-1` (one or two tokens)
		}
	REQUIRE(found);

	// The flush rebuilt the dkGlobalVar TopDecl with the PARSED ctor args —
	// the state global_ctor_call reads. Loaded == parsed: the rebuilt arg is
	// the SAME node kind live's parse produced (the -1 emits correctly end to
	// end — the bind-vs-live output equality rides the reducer/soak gates).
	bool decl_found = false;
	for (const Program::TopDecl &td : progB->top_decls)
		if (td.kind == Program::DeclKind::dkGlobalVar && td.name == "t_less") {
			decl_found = true;
			REQUIRE(td.decl != nullptr);
			REQUIRE(td.decl->ctor_args.size() == 1);
			REQUIRE(td.decl->ctor_args[0] != nullptr);
			CHECK(td.decl->ctor_args[0]->id() == live_arg_id);
		}
	REQUIRE(decl_found);
}

// A struct/class-KEYWORD typedef (`typedef struct {...} X;` — the tagless
// glibc fd_set/div_t shape, or `typedef struct tag X;`) ALSO registers the
// alias as a TAG in the producer (struct_map.set(X, the aggregate,
// TokenSTRUCT::parse ~24055/~25002) so `struct X v);` resolves. The plain
// TokenTYPEDEF path never writes struct_map. The save stamps
// DF_TYPEDEF_TAG_ALIAS from the producer's own map state (same key -> same
// definition); the restore reproduces the struct_map write — and must NOT
// invent one for a plain alias.
TEST_CASE("tagless-typedef struct: the alias restores as a struct_map tag too") {
	std::string inc_path = std::string("/tmp/madc_taga_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_taga_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string cons_path = std::string("/tmp/madc_taga_cons_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_taga_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		// fdset_like = the tagless shape (struct-keyword typedef -> tag
		// alias); fl2 = a PLAIN typedef of it (never a tag in live);
		// jb = the no-body ARRAY form (`typedef struct tag X[N];`, the
		// jmp_buf shape) whose tag key maps the ELEMENT struct.
		inc << "typedef struct { long bits[2]; } fdset_like;\n"
		       "typedef fdset_like fl2;\n"
		       "struct jbtag { int depth; };\n"
		       "typedef struct jbtag jb[1];\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { struct fdset_like v; fl2 w; jb b; return 0; }\n";
	}
	{
		std::ofstream cn(cons_path.c_str());
		cn << "int main() { return 0; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;
		TokenProgram *tpA = progA->tokenize(main_path.c_str());
		REQUIRE(tpA != nullptr);
		REQUIRE(progA->parse(tpA));
		// The LIVE premise: the struct-keyword alias is a tag, the plain
		// alias is not.
		flat_datatype_map_iter li = progA->datatype_map.find("fdset_like");
		REQUIRE(li != progA->datatype_map.end());
		datadef_map_citer si = progA->struct_map.find("fdset_like");
		REQUIRE(si != progA->struct_map.end());
		CHECK(si->second == &(*li)->definition);
		CHECK(progA->struct_map.find("fl2") == progA->struct_map.end());
		// The array form's live premise: the alias key maps the ELEMENT
		// struct while the datatype is the CARRAY.
		flat_datatype_map_iter ji = progA->datatype_map.find("jb");
		REQUIRE(ji != progA->datatype_map.end());
		REQUIRE(dynamic_cast<DataDefCArray *>(&(*ji)->definition) != nullptr);
		datadef_map_citer jsi = progA->struct_map.find("jb");
		REQUIRE(jsi != progA->struct_map.end());
		CHECK(jsi->second == dynamic_cast<DataDefCArray *>(
			&(*ji)->definition)->element_type);
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

	// A FRESH Program that never parses the header.
	std::shared_ptr<Program> progB = std::make_shared<Program>();
	TokenProgram *tpB = progB->tokenize(cons_path.c_str());
	REQUIRE(tpB != nullptr);
	std::remove(cons_path.c_str());
	CHECK(progB->struct_map.find("fdset_like") == progB->struct_map.end());
	progB->forest_restore_decls(forest);
	progB->flush_forest_pending_globals();	// applies staged datatype_map writes

	// Loaded == parsed: datatype_map AND struct_map hold the alias, both
	// resolving to the SAME restored aggregate (live's 25002 write).
	flat_datatype_map_iter mit = progB->datatype_map.find("fdset_like");
	REQUIRE(mit != progB->datatype_map.end());
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(&(*mit)->definition);
	REQUIRE(sdd != nullptr);
	CHECK(sdd->members.size() == 1);
	datadef_map_citer sit = progB->struct_map.find("fdset_like");
	REQUIRE(sit != progB->struct_map.end());
	CHECK(sit->second == sdd);
	// The PLAIN alias restores as a datatype only — no invented tag.
	flat_datatype_map_iter pit = progB->datatype_map.find("fl2");
	REQUIRE(pit != progB->datatype_map.end());
	CHECK(&(*pit)->definition == sdd);
	CHECK(progB->struct_map.find("fl2") == progB->struct_map.end());
	// The no-body ARRAY form: datatype_map[jb] = the CARRAY, struct_map[jb]
	// = the ELEMENT struct (live's ~24055 write through the array walk).
	flat_datatype_map_iter jit = progB->datatype_map.find("jb");
	REQUIRE(jit != progB->datatype_map.end());
	DataDefCArray *jarr = dynamic_cast<DataDefCArray *>(&(*jit)->definition);
	REQUIRE(jarr != nullptr);
	datadef_map_citer jsi = progB->struct_map.find("jb");
	REQUIRE(jsi != progB->struct_map.end());
	CHECK(jsi->second == jarr->element_type);
	CHECK(dynamic_cast<DataDefSTRUCT *>(jsi->second) != nullptr);
}

// A header's file-scope NULL-initialized pointer global (`char *last = NULL;`
// — NULL is `((void *)0)`, a TokenCast over the literal, which the v14
// bare-tkInt check dropped) and an UNINITIALIZED tentative definition
// (`int REQ;` — no initializer at all) both restore: the cast unwraps to its
// literal (SCALAR_INIT, value 0), the tentative definition records the
// SCALAR_UNINIT form (flush leaves the rebuilt decl's initialize NULL ->
// live's bss item). smaug_requests_mud.mah's last_log/last_who_arg + REQ.
TEST_CASE("NULL-init pointer + uninitialized file-scope globals restore") {
	std::string inc_path = std::string("/tmp/madc_gnull_inc_")
			     + std::to_string((long)getpid()) + ".h";
	std::string main_path = std::string("/tmp/madc_gnull_main_")
			      + std::to_string((long)getpid()) + ".mad";
	std::string snap_path = std::string("/tmp/madc_gnull_snap_")
			      + std::to_string((long)getpid()) + ".msnap";
	{
		std::ofstream inc(inc_path.c_str());
		inc << "#include <stddef.h>\n"
		       "char *g_last = NULL;\n"
		       "int g_req;\n"
		       "int g_calls = 7;\n";
	}
	{
		std::ofstream mn(main_path.c_str());
		mn << "#include \"" << inc_path << "\"\n"
		      "int main() { return 0; }\n";
	}

	{
		std::shared_ptr<Program> progA = std::make_shared<Program>();
		progA->pack_recording = true;
		progA->forest_arena_enabled = true;
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

	forest.materialize_from_arena();
	const std::vector<CirRestoredGlobal> &globals = forest.restored_globals();

	bool saw_last = false, saw_req = false, saw_calls = false;
	for (size_t i = 0; i < globals.size(); ++i) {
		if (!globals[i].name)
			continue;
		if (!strcmp(globals[i].name, "g_last")) {
			saw_last = true;
			CHECK((globals[i].gflags & CIR_GLOBALF_SCALAR_INIT) != 0);
			CHECK(globals[i].init_value == 0);
			REQUIRE(globals[i].type != nullptr);
			CHECK(globals[i].type->is_pointer());
		} else if (!strcmp(globals[i].name, "g_req")) {
			saw_req = true;
			CHECK((globals[i].gflags & CIR_GLOBALF_SCALAR_UNINIT) != 0);
			CHECK((globals[i].gflags & CIR_GLOBALF_SCALAR_INIT) == 0);
		} else if (!strcmp(globals[i].name, "g_calls")) {
			saw_calls = true;
			CHECK((globals[i].gflags & CIR_GLOBALF_SCALAR_INIT) != 0);
			CHECK(globals[i].init_value == 7);
		}
	}
	CHECK(saw_last);
	CHECK(saw_req);
	CHECK(saw_calls);
}
