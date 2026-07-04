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
