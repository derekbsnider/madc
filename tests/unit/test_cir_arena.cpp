/* test_cir_arena.cpp — B3 step-2: prove a DataDef's state survives the arena
 * round-trip (encode -> copy-the-blocks [== dump/load, since everything is
 * type-id/offset relative with zero live pointers] -> decode) losslessly.
 *
 * This is the empirical validation of the record SCHEMA before any of the ~548
 * live mutation sites are touched. If these shapes cannot round-trip byte-stably
 * here, B3's storage model is wrong and we learn it now.
 *
 * KEYED BY TYPE-ID (the spine). Cross-references are the referent's SERIALIZED
 * type_id (madc_cir.cpp's forest_serialize_type_id policy: a pinned primitive is
 * its own slot with NO record; a project type takes its project id, whose record
 * lives at arena slot (id - PROJECT_BASE)). The encoder below models that policy
 * through the PUBLIC type-id chokepoints (madc_type_id_for / madc_type_from_id);
 * the unit test has no scalar-primitive aliases, so the pinned check is simply
 * "dd->type_id in the primitive band" (the real save side adds alias
 * normalization via forest_pinned_primitive_id). A local id_table<DataDef> is
 * bound as the active project table, exactly as parser.cpp anticipates for tests.
 *
 * Covered:
 *   - a PINNED primitive is referenced by id and NEVER recorded (DK_PTR -> int)
 *   - DK_STRUCT (payload (begin,count) slice + per-member type-ids + offsets)
 *   - NESTED struct (proves the resolve-first payload discipline: a member whose
 *     type is itself an aggregate must NOT interleave its member run with ours)
 *   - DK_FUNC (FuncDef return + parameters run, both by pinned id)
 *   - DK_CLASS with the three hard-tier FLATTENINGS: bases, methods, the
 *     pointer-keyed vbase_offset map -> a sorted (class_id,offset) run, and the
 *     nested vtable_groups -> a two-level (vgrouprec + slot-id) slice
 *   - a REAL byte round-trip through the snapshot container (not a copy)
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <utility>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <fstream>

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

#include "cir_arena.h"
#include "madcdis/snapshot.h"

using namespace madc::dis;

// Bind a fresh project id_table as the active table + (re)stamp the pinned primitive
// ids. Each case gets its own project segment; the shared primitive globals keep their
// pinned ids (idempotent re-stamp). Returns the int primitive global (pinned id
// MADC_TYPEID_INT) — the real object a member/param of type `int` points at in
// production (NOT a fresh DataDefINT, whose type_id would be 0 -> a spurious project id).
static DataDef *arena_test_setup(id_table<DataDef> &proj)
{
	madc_stamp_primitive_type_ids();
	madc_active_project_types = &proj;
	DataDef *ii = madc_primitive_for_slot(MADC_TYPEID_INT);
	REQUIRE(ii != NULL);
	CHECK(ii->type_id == (uint32_t)MADC_TYPEID_INT);	// pinned, in the primitive band
	return ii;
}

// ---- encode: a live DataDef* -> its record in the arena; RETURN its SERIALIZED type_id.
// A pinned primitive returns its pinned slot with NO record. A project type is assigned its
// project id (memoized into dd->type_id) and its record written at slot (id - PROJECT_BASE).
// `done` guards re-encode and self-reference (marked BEFORE recursion). PAYLOAD DISCIPLINE:
// every run resolves ALL child type-ids FIRST (a recursive encode may append payload) and
// only THEN appends its own contiguous run — otherwise a nested aggregate's run interleaves.
static uint32_t arena_ensure(DefArena &a, DataDef *dd, std::set<DataDef *> &done)
{
	if ( !dd ) return (uint32_t)MADC_TYPEID_INVALID;

	// The save-side cross-ref policy (forest_serialize_type_id): a pinned primitive
	// serializes as its pinned slot with NO record; everything else takes its project id.
	if ( dd->type_id && dd->type_id < (uint32_t)MADC_TYPEID_PRIMITIVE_END )
		return dd->type_id;			// pinned primitive: referenced by id, never recorded

	uint32_t tid = madc_type_id_for(dd);		// project id (memoized into dd->type_id)
	if ( done.count(dd) ) return tid;		// already encoded, or being encoded (self-ref)
	done.insert(dd);

	defrec r;
	std::memset(&r, 0, sizeof(r));
	r.name_id  = a.strings.intern(dd->name.c_str());
	r.canon_id = dd->canonical_cpp_spelling().empty()
		     ? 0u : a.strings.intern(dd->canonical_cpp_spelling().c_str());
	r.size     = (uint32_t)dd->size;
	r.datatype = (uint32_t)dd->rawtype();

	if ( DataDefSTRUCT *s = dynamic_cast<DataDefSTRUCT *>(dd) )
	{
		// --- members (resolve types first, then append the contiguous run) ---
		std::vector<uint32_t> mt(s->members.size());
		for ( size_t i = 0; i < s->members.size(); ++i )
			mt[i] = arena_ensure(a, s->members[i].second, done);
		uint32_t mbegin = (uint32_t)a.payload.size();
		for ( size_t i = 0; i < s->members.size(); ++i )
		{
			memberrec m;
			std::memset(&m, 0, sizeof(m));
			m.name_id    = a.strings.intern(s->members[i].first.c_str());
			m.type_id    = mt[i];
			m.typedef_id = s->members[i].typedef_name.empty()
				? 0u : a.strings.intern(s->members[i].typedef_name.c_str());
			m.offset = (uint32_t)(i < s->member_offsets.size() ? s->member_offsets[i] : 0);
			m.count  = (uint32_t)(i < s->member_counts.size() ? s->member_counts[i] : 1);
			m.flags  = (i < s->member_array_flags.size() && s->member_array_flags[i]) ? 1u : 0u;
			a.add_payload(m);
		}
		r.members_begin = mbegin;
		r.members_count = (uint32_t)s->members.size();
		r.pack               = (uint32_t)s->pack;
		r.max_align          = (uint32_t)s->max_align;
		r.tag_explicit_align = (uint32_t)s->tag_explicit_align;
		if ( s->union_layout ) r.flags |= DF_UNION_LAYOUT;
		if ( s->is_complete )  r.flags |= DF_IS_COMPLETE;

		if ( DataDefCLASS *c = dynamic_cast<DataDefCLASS *>(dd) )
		{
			r.kind = DK_CLASS;
			if ( c->has_vtable )         r.flags |= DF_HAS_VTABLE;
			if ( c->has_vptr_slot )      r.flags |= DF_HAS_VPTR_SLOT;
			if ( c->from_system_header ) r.flags |= DF_FROM_SYSTEM_HDR;
			if ( c->has_user_ctor )      r.flags |= DF_HAS_USER_CTOR;
			if ( c->has_user_dtor )      r.flags |= DF_HAS_USER_DTOR;
			r.nvsize        = (uint32_t)c->nvsize;
			r.class_align   = (uint32_t)c->class_align;
			r.own_block_off = (uint32_t)c->own_block_off;

			// --- bases (resolve base type-ids first) ---
			std::vector<uint32_t> bid(c->bases.size());
			for ( size_t i = 0; i < c->bases.size(); ++i )
				bid[i] = arena_ensure(a, c->bases[i].base, done);
			uint32_t bbegin = (uint32_t)a.payload.size();
			for ( size_t i = 0; i < c->bases.size(); ++i )
			{
				baserec b;
				b.base_id = bid[i];
				b.offset  = (uint32_t)c->bases[i].offset;
				b.flags   = (c->bases[i].is_virtual ? BSF_VIRTUAL : 0u)
					  | (c->bases[i].is_primary ? BSF_PRIMARY : 0u)
					  | (c->bases[i].access << BSF_ACCESS_SHIFT);
				a.add_payload(b);
			}
			r.bases_begin = bbegin;
			r.bases_count = (uint32_t)c->bases.size();

			// --- methods (resolve each method's FuncDef type-id first) ---
			std::vector<uint32_t> fid(c->methods.size());
			for ( size_t i = 0; i < c->methods.size(); ++i )
				fid[i] = arena_ensure(a, c->methods[i]->type, done);
			uint32_t mmbegin = (uint32_t)a.payload.size();
			for ( size_t i = 0; i < c->methods.size(); ++i )
			{
				methodrec md;
				md.name_id = a.strings.intern(c->methods[i]->name.c_str());
				md.func_id = fid[i];
				md.flags   = 0;
				a.add_payload(md);
			}
			r.methods_begin = mmbegin;
			r.methods_count = (uint32_t)c->methods.size();

			// --- vbase_offset: flatten pointer-keyed map -> sorted (class_id,offset) run ---
			std::vector<std::pair<uint32_t, uint32_t> > vb;
			for ( std::map<DataDefCLASS *, size_t>::const_iterator vi = c->vbase_offset.begin();
			      vi != c->vbase_offset.end(); ++vi )
				vb.push_back(std::make_pair(arena_ensure(a, vi->first, done),
							    (uint32_t)vi->second));
			std::sort(vb.begin(), vb.end());
			uint32_t vbbegin = (uint32_t)a.payload.size();
			for ( size_t i = 0; i < vb.size(); ++i )
			{
				vbaserec vr; vr.class_id = vb[i].first; vr.offset = vb[i].second;
				a.add_payload(vr);
			}
			r.vbase_begin = vbbegin;
			r.vbase_count = (uint32_t)vb.size();

			// --- vtable_groups: nested vector -> two-level slice. Resolve owners
			//     first; append each group's slot-id run; THEN append the vgrouprec
			//     run contiguously (slot runs carry no recursion). ---
			std::vector<uint32_t> owners(c->vtable_groups.size());
			for ( size_t g = 0; g < c->vtable_groups.size(); ++g )
				owners[g] = arena_ensure(a, c->vtable_groups[g].owner, done);
			std::vector<vgrouprec> vgs(c->vtable_groups.size());
			for ( size_t g = 0; g < c->vtable_groups.size(); ++g )
			{
				const DataDefCLASS::VtableGroup &vg = c->vtable_groups[g];
				uint32_t sbegin = (uint32_t)a.payload.size();
				for ( size_t k = 0; k < vg.slots.size(); ++k )
					a.add_word(a.strings.intern(vg.slots[k].c_str()));
				vgs[g].owner_id    = owners[g];
				vgs[g].this_offset = (uint32_t)vg.this_offset;
				vgs[g].slots_begin = sbegin;
				vgs[g].slots_count = (uint32_t)vg.slots.size();
				vgs[g].addr_point  = (uint32_t)vg.addr_point;
			}
			uint32_t vgbegin = (uint32_t)a.payload.size();
			for ( size_t g = 0; g < vgs.size(); ++g )
				a.add_payload(vgs[g]);
			r.vgroup_begin = vgbegin;
			r.vgroup_count = (uint32_t)vgs.size();
		}
		else
		{
			r.kind = s->union_layout ? DK_UNION : DK_STRUCT;
		}
	}
	else if ( FuncDef *fd = dynamic_cast<FuncDef *>(dd) )
	{
		r.kind = DK_FUNC;
		r.ref0 = arena_ensure(a, &fd->returns, done);	// return type-id
		std::vector<uint32_t> pt(fd->parameters.size());
		for ( size_t i = 0; i < fd->parameters.size(); ++i )
			pt[i] = arena_ensure(a, fd->parameters[i], done);
		uint32_t pbegin = (uint32_t)a.payload.size();
		for ( size_t i = 0; i < fd->parameters.size(); ++i )
		{
			paramrec p;
			p.type_id         = pt[i];
			p.flags           = (i < fd->const_params.size() && fd->const_params[i]) ? 1u : 0u;
			p.cpp_spelling_id = (i < fd->param_cpp_spellings.size() && !fd->param_cpp_spellings[i].empty())
				? a.strings.intern(fd->param_cpp_spellings[i].c_str()) : 0u;
			a.add_payload(p);
		}
		r.params_begin = pbegin;
		r.params_count = (uint32_t)fd->parameters.size();
		if ( fd->is_varargs )       r.flags |= DF_IS_VARARGS;
		if ( fd->is_void_params )   r.flags |= DF_IS_VOID_PARAMS;
		if ( fd->declaration_only ) r.flags |= DF_DECLARATION_ONLY;
	}
	else if ( DataDefCONST *k = dynamic_cast<DataDefCONST *>(dd) )
	{
		r.kind = DK_CONST;
		r.ref0 = arena_ensure(a, k->base_type, done);
	}
	else if ( DataDefREF *rf = dynamic_cast<DataDefREF *>(dd) )		// REF is-a PTR: check first
	{
		r.kind = DK_REF;
		r.ref0 = arena_ensure(a, rf->base_type, done);
	}
	else if ( DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd) )
	{
		r.kind = DK_PTR;
		r.ref0 = arena_ensure(a, p->base_type, done);
	}
	else if ( dynamic_cast<DataDefENUM *>(dd) )
	{
		r.kind = DK_ENUM;
	}
	else
	{
		r.kind = (dd->rawtype() == DataType::dtVOID) ? DK_VOID : DK_PRIM;
	}

	a.set_def_at(tid, r);	// write the record at its project-id slot
	return tid;
}

// Simulate dump/load: the three blocks are id/offset-relative with no live pointers,
// so a plain value copy is byte-for-byte what a dump-then-load produces.
static DefArena roundtrip(const DefArena &a)
{
	DefArena b;
	b.strings = a.strings;
	b.defs    = a.defs;
	b.payload = a.payload;
	return b;
}

// Resolve a stored cross-ref type-id to a name (the load-side dispatch): a PINNED id
// resolves to the process primitive global (via madc_type_from_id, no record); a PROJECT
// id reads its arena record.
static std::string refname(const DefArena &a, uint32_t type_id)
{
	if ( arena_id_is_pinned(type_id) )
	{
		DataDef *p = madc_type_from_id(type_id);
		return p ? p->name : "<pin?>";
	}
	defrec r;
	if ( !a.get_def_at(type_id, r) ) return "<oob>";
	return std::string(a.strings.c_str(r.name_id));
}

// No primitive is ever recorded (the correction): assert no live slot carries a DK_PRIM /
// DK_VOID record.
static void assert_no_primitive_records(const DefArena &a)
{
	for ( uint32_t slot = 0; slot < a.def_slots(); ++slot )
	{
		defrec r;
		REQUIRE(a.get_def_at(arena_id_of(slot), r));
		CHECK(r.kind != DK_PRIM);
		CHECK(r.kind != DK_VOID);
	}
}

TEST_CASE("B3 arena: a PINNED primitive is referenced by id, never recorded")
{
	id_table<DataDef> proj(MADC_TYPEID_PROJECT_BASE);
	DataDef *ii = arena_test_setup(proj);

	DataDefPTR pp(*ii);				// int*
	DefArena a;
	std::set<DataDef *> done;
	uint32_t tid = arena_ensure(a, &pp, done);

	CHECK(arena_id_is_project(tid));		// the pointer itself IS a project type
	DefArena b = roundtrip(a);
	defrec pr;
	REQUIRE(b.get_def_at(tid, pr));
	CHECK(pr.kind == DK_PTR);
	CHECK(pr.ref0 == (uint32_t)MADC_TYPEID_INT);	// pointee stored as its PINNED id
	CHECK(refname(b, pr.ref0) == "int");		// resolves via the pinned dispatch
	CHECK(b.has_def((uint32_t)MADC_TYPEID_INT) == false);	// int has NO record
	assert_no_primitive_records(b);
}

TEST_CASE("B3 arena: a plain struct round-trips (payload slice + member type-ids + offsets)")
{
	id_table<DataDef> proj(MADC_TYPEID_PROJECT_BASE);
	DataDef *ii = arena_test_setup(proj);

	DataDefSTRUCT pt("Point", 0);
	pt.addMember("x", *ii, 1);
	pt.addMember("y", *ii, 1);
	pt.finalize();
	REQUIRE(pt.members.size() == 2);

	DefArena a;
	std::set<DataDef *> done;
	uint32_t tid = arena_ensure(a, &pt, done);

	DefArena b = roundtrip(a);
	defrec sr;
	REQUIRE(b.get_def_at(tid, sr));
	CHECK(sr.kind == DK_STRUCT);
	CHECK(std::string(b.strings.c_str(sr.name_id)) == "Point");
	CHECK(sr.size == pt.size);
	CHECK(sr.max_align == pt.max_align);
	REQUIRE(sr.members_count == 2);

	memberrec m0, m1;
	REQUIRE(b.get_payload(sr.members_begin, 0, m0));
	REQUIRE(b.get_payload(sr.members_begin, 1, m1));
	CHECK(std::string(b.strings.c_str(m0.name_id)) == "x");
	CHECK(m0.offset == 0);
	CHECK(std::string(b.strings.c_str(m1.name_id)) == "y");
	CHECK(m1.offset == 4);
	CHECK(m0.type_id == m1.type_id);		// shared int, by pinned id
	CHECK(m0.type_id == (uint32_t)MADC_TYPEID_INT);
	CHECK(refname(b, m0.type_id) == "int");
	assert_no_primitive_records(b);
}

TEST_CASE("B3 arena: NESTED struct round-trips (resolve-first payload discipline)")
{
	id_table<DataDef> proj(MADC_TYPEID_PROJECT_BASE);
	DataDef *ii = arena_test_setup(proj);

	DataDefSTRUCT pt("Point", 0);
	pt.addMember("x", *ii, 1);
	pt.addMember("y", *ii, 1);
	pt.finalize();
	DataDefSTRUCT ln("Line", 0);
	ln.addMember("a", pt, 1);			// a member whose TYPE is itself an aggregate
	ln.addMember("b", pt, 1);
	ln.finalize();

	DefArena a;
	std::set<DataDef *> done;
	uint32_t lidx = arena_ensure(a, &ln, done);

	DefArena b = roundtrip(a);
	defrec lr;
	REQUIRE(b.get_def_at(lidx, lr));
	CHECK(lr.kind == DK_STRUCT);
	REQUIRE(lr.members_count == 2);			// NOT corrupted by Point's member run interleaving

	memberrec a0, a1;
	REQUIRE(b.get_payload(lr.members_begin, 0, a0));
	REQUIRE(b.get_payload(lr.members_begin, 1, a1));
	CHECK(std::string(b.strings.c_str(a0.name_id)) == "a");
	CHECK(a0.offset == 0);
	CHECK(std::string(b.strings.c_str(a1.name_id)) == "b");
	CHECK(a1.offset == 8);
	CHECK(a0.type_id == a1.type_id);		// both are Point (same project id)
	CHECK(arena_id_is_project(a0.type_id));

	// The nested Point def is intact with its OWN 2 members.
	defrec pr;
	REQUIRE(b.get_def_at(a0.type_id, pr));
	CHECK(std::string(b.strings.c_str(pr.name_id)) == "Point");
	REQUIRE(pr.members_count == 2);
	memberrec px, py;
	REQUIRE(b.get_payload(pr.members_begin, 0, px));
	REQUIRE(b.get_payload(pr.members_begin, 1, py));
	CHECK(std::string(b.strings.c_str(px.name_id)) == "x");
	CHECK(std::string(b.strings.c_str(py.name_id)) == "y");
	assert_no_primitive_records(b);
}

TEST_CASE("B3 arena: a FuncDef round-trips its return + parameter run (by pinned id)")
{
	id_table<DataDef> proj(MADC_TYPEID_PROJECT_BASE);
	DataDef *ii = arena_test_setup(proj);

	FuncDef *add = new FuncDef(*ii);		// returns int
	add->name = "add";
	add->parameters.push_back(ii);
	add->parameters.push_back(ii);

	DefArena a;
	std::set<DataDef *> done;
	uint32_t tid = arena_ensure(a, add, done);

	DefArena b = roundtrip(a);
	defrec fr;
	REQUIRE(b.get_def_at(tid, fr));
	CHECK(fr.kind == DK_FUNC);
	CHECK(std::string(b.strings.c_str(fr.name_id)) == "add");
	CHECK(fr.ref0 == (uint32_t)MADC_TYPEID_INT);	// return type by pinned id
	CHECK(refname(b, fr.ref0) == "int");
	REQUIRE(fr.params_count == 2);
	paramrec p0, p1;
	REQUIRE(b.get_payload(fr.params_begin, 0, p0));
	REQUIRE(b.get_payload(fr.params_begin, 1, p1));
	CHECK(p0.type_id == (uint32_t)MADC_TYPEID_INT);
	CHECK(p1.type_id == (uint32_t)MADC_TYPEID_INT);
	assert_no_primitive_records(b);
	delete add;
}

TEST_CASE("B3 arena: DK_CLASS round-trips all three hard-tier flattenings")
{
	id_table<DataDef> proj(MADC_TYPEID_PROJECT_BASE);
	DataDef *ii = arena_test_setup(proj);

	DataDefCLASS base("Base", 0, DataType::dtRESERVED);
	base.finalize();
	DataDefCLASS der("Derived", 0, DataType::dtRESERVED);
	der.finalize();

	// base (virtual, at offset 8)
	BaseSpec bs; bs.base = &base; bs.offset = 8; bs.is_virtual = true; bs.access = 0; bs.is_primary = false;
	der.bases.push_back(bs);
	// pointer-keyed vbase_offset map
	der.vbase_offset[&base] = 16;
	// a method: Variable("foo") wrapping a FuncDef returning int, one int param
	FuncDef *fd = new FuncDef(*ii);
	fd->name = "foo";
	fd->parameters.push_back(ii);
	Variable *mv = new Variable("foo", *fd);
	der.methods.push_back(mv);
	// nested vtable_groups
	DataDefCLASS::VtableGroup vg;
	vg.owner = &der; vg.this_offset = 0; vg.slots.push_back("foo"); vg.slots.push_back("bar"); vg.addr_point = 2;
	der.vtable_groups.push_back(vg);
	der.has_vtable = true;
	der.nvsize = 8; der.class_align = 8;

	DefArena a;
	std::set<DataDef *> done;
	uint32_t tid = arena_ensure(a, &der, done);

	DefArena b = roundtrip(a);
	defrec cr;
	REQUIRE(b.get_def_at(tid, cr));
	CHECK(cr.kind == DK_CLASS);
	CHECK(std::string(b.strings.c_str(cr.name_id)) == "Derived");
	CHECK((cr.flags & DF_HAS_VTABLE) != 0);
	CHECK(cr.nvsize == 8);
	CHECK(cr.class_align == 8);

	// bases
	REQUIRE(cr.bases_count == 1);
	baserec br;
	REQUIRE(b.get_payload(cr.bases_begin, 0, br));
	CHECK(refname(b, br.base_id) == "Base");
	CHECK(br.offset == 8);
	CHECK((br.flags & BSF_VIRTUAL) != 0);
	CHECK((br.flags & BSF_PRIMARY) == 0);

	// methods -> FuncDef by type-id
	REQUIRE(cr.methods_count == 1);
	methodrec md;
	REQUIRE(b.get_payload(cr.methods_begin, 0, md));
	CHECK(std::string(b.strings.c_str(md.name_id)) == "foo");
	defrec mfr;
	REQUIRE(b.get_def_at(md.func_id, mfr));
	CHECK(mfr.kind == DK_FUNC);
	CHECK(refname(b, mfr.ref0) == "int");
	REQUIRE(mfr.params_count == 1);

	// vbase_offset flattening
	REQUIRE(cr.vbase_count == 1);
	vbaserec vbr;
	REQUIRE(b.get_payload(cr.vbase_begin, 0, vbr));
	CHECK(refname(b, vbr.class_id) == "Base");
	CHECK(vbr.offset == 16);

	// vtable_groups flattening (two-level slice)
	REQUIRE(cr.vgroup_count == 1);
	vgrouprec gr;
	REQUIRE(b.get_payload(cr.vgroup_begin, 0, gr));
	CHECK(refname(b, gr.owner_id) == "Derived");
	CHECK(gr.this_offset == 0);
	CHECK(gr.addr_point == 2);
	REQUIRE(gr.slots_count == 2);
	uint32_t s0 = 0, s1 = 0;
	REQUIRE(b.get_word(gr.slots_begin, 0, s0));
	REQUIRE(b.get_word(gr.slots_begin, 1, s1));
	CHECK(std::string(b.strings.c_str(s0)) == "foo");
	CHECK(std::string(b.strings.c_str(s1)) == "bar");
	assert_no_primitive_records(b);

	delete mv;
	delete fd;
}

// ---- read a snapshot segment back into a typed vector (the load-side copy). ----
static std::vector<uint8_t> read_raw(const snapshot_reader &rd, uint32_t seg_id)
{
	std::vector<uint8_t> raw;
	const snapshot_segment *s = rd.find(seg_id);
	if ( s ) rd.read_segment(*s, raw);
	return raw;
}
template <typename T> static std::vector<T> read_seg(const snapshot_reader &rd, uint32_t seg_id)
{
	std::vector<uint8_t> raw = read_raw(rd, seg_id);
	std::vector<T> out(raw.size() / sizeof(T));
	if ( !out.empty() ) std::memcpy(out.data(), raw.data(), out.size() * sizeof(T));
	return out;
}
static std::string fdname(const FrozenDefArena &f, uint32_t type_id)
{
	if ( arena_id_is_pinned(type_id) )
	{
		DataDef *p = madc_type_from_id(type_id);
		return p ? p->name : "<pin?>";
	}
	defrec r;
	if ( !f.get_def_at(type_id, r) ) return "<oob>";
	return std::string(f.c_str(r.name_id));
}

TEST_CASE("B3 arena: REAL byte round-trip through the snapshot container (not a copy)")
{
	id_table<DataDef> proj(MADC_TYPEID_PROJECT_BASE);
	DataDef *ii = arena_test_setup(proj);

	// The richest case — a class with all three flattenings — into a live arena.
	DataDefCLASS base("Base", 0, DataType::dtRESERVED); base.finalize();
	DataDefCLASS der("Derived", 0, DataType::dtRESERVED); der.finalize();
	BaseSpec bs; bs.base = &base; bs.offset = 8; bs.is_virtual = true; bs.access = 0; bs.is_primary = false;
	der.bases.push_back(bs);
	der.vbase_offset[&base] = 16;
	FuncDef *fd = new FuncDef(*ii); fd->name = "foo"; fd->parameters.push_back(ii);
	Variable *mv = new Variable("foo", *fd); der.methods.push_back(mv);
	DataDefCLASS::VtableGroup vg; vg.owner = &der; vg.this_offset = 0;
	vg.slots.push_back("foo"); vg.slots.push_back("bar"); vg.addr_point = 2;
	der.vtable_groups.push_back(vg);
	der.has_vtable = true; der.nvsize = 8; der.class_align = 8;

	DefArena a;
	std::set<DataDef *> done;
	uint32_t tid = arena_ensure(a, &der, done);

	// ---- SERIALIZE the three blocks to a real byte blob (codec None -> bind-in-place). ----
	snapshot_writer w;
	REQUIRE(w.add_segment(1, SNAP_KIND_CONSUMER + 0,
			      a.defs.data(), a.defs.size() * sizeof(uint32_t), PchCompression::None));
	REQUIRE(w.add_segment(2, SNAP_KIND_CONSUMER + 1,
			      a.payload.data(), a.payload.size() * sizeof(uint32_t), PchCompression::None));
	REQUIRE(w.add_segment(3, SNAP_KIND_INTERN_BYTES,
			      a.strings.bytes_data(), a.strings.bytes_size(), PchCompression::None));
	REQUIRE(w.add_segment(4, SNAP_KIND_INTERN_ENTRIES,
			      a.strings.entries_data(),
			      a.strings.entries_size() * sizeof(intern_table::Entry), PchCompression::None));
	REQUIRE(w.add_segment(5, SNAP_KIND_INTERN_BUCKETS,
			      a.strings.buckets_data(),
			      a.strings.buckets_size() * sizeof(uint32_t), PchCompression::None));
	std::vector<uint8_t> blob;
	REQUIRE(w.build(blob));
	CHECK(blob.size() > 0);

	// ---- RELOAD from the bytes (a fresh reader; the live arena `a` is now ignored). The
	//      typed vectors below own the reloaded blocks; FrozenDefArena binds pointers into
	//      them (they outlive the assertions in this scope). ----
	snapshot_reader rd;
	REQUIRE(rd.open(blob.data(), blob.size()));

	std::vector<uint32_t>            defs2  = read_seg<uint32_t>(rd, 1);
	std::vector<uint32_t>            pay2   = read_seg<uint32_t>(rd, 2);
	std::vector<char>                ibytes = read_seg<char>(rd, 3);
	std::vector<intern_table::Entry> ients  = read_seg<intern_table::Entry>(rd, 4);
	std::vector<uint32_t>            ibkts  = read_seg<uint32_t>(rd, 5);

	FrozenDefArena f;
	f.strings.bind(ibytes.data(), ibytes.size(), ients.data(), ients.size(), ibkts.data(), ibkts.size());
	REQUIRE(f.strings.valid());		// structural gate over the reloaded intern blocks
	f.bind_defs(defs2.data(), defs2.size());
	f.bind_payload(pay2.data(), pay2.size());

	// ---- the class + all three flattenings survived a REAL serialize + reload ----
	defrec cr;
	REQUIRE(f.get_def_at(tid, cr));
	CHECK(cr.kind == DK_CLASS);
	CHECK(std::string(f.c_str(cr.name_id)) == "Derived");
	CHECK((cr.flags & DF_HAS_VTABLE) != 0);
	CHECK(cr.nvsize == 8);

	REQUIRE(cr.bases_count == 1);
	baserec br; REQUIRE(f.get_payload(cr.bases_begin, 0, br));
	CHECK(fdname(f, br.base_id) == "Base");
	CHECK(br.offset == 8);
	CHECK((br.flags & BSF_VIRTUAL) != 0);

	REQUIRE(cr.methods_count == 1);
	methodrec md; REQUIRE(f.get_payload(cr.methods_begin, 0, md));
	CHECK(std::string(f.c_str(md.name_id)) == "foo");
	defrec mfr; REQUIRE(f.get_def_at(md.func_id, mfr));
	CHECK(mfr.kind == DK_FUNC);
	CHECK(fdname(f, mfr.ref0) == "int");	// return type by pinned id, resolved cross-process

	REQUIRE(cr.vbase_count == 1);
	vbaserec vbr; REQUIRE(f.get_payload(cr.vbase_begin, 0, vbr));
	CHECK(fdname(f, vbr.class_id) == "Base");
	CHECK(vbr.offset == 16);

	REQUIRE(cr.vgroup_count == 1);
	vgrouprec gr; REQUIRE(f.get_payload(cr.vgroup_begin, 0, gr));
	CHECK(fdname(f, gr.owner_id) == "Derived");
	CHECK(gr.addr_point == 2);
	REQUIRE(gr.slots_count == 2);
	uint32_t s0 = 0, s1 = 0;
	REQUIRE(f.get_word(gr.slots_begin, 0, s0));
	REQUIRE(f.get_word(gr.slots_begin, 1, s1));
	CHECK(std::string(f.c_str(s0)) == "foo");
	CHECK(std::string(f.c_str(s1)) == "bar");

	delete mv;
	delete fd;
}

// ---- SLICE 1c: the live write-through. getPointerType is the single memoized funnel where a
//      NEW project DataDefPTR is born; with forest_arena_enabled it also populates the arena
//      record, WITHOUT disturbing the pointer's read interface (base_type stays the read-cache).
TEST_CASE("B3 arena: getPointerType write-through records a new project pointer (reads unchanged)")
{
	// widget declared FIRST so it outlives `delete prog` (prog.project_types holds &widget).
	DataDefSTRUCT widget("Widget", 0);
	widget.finalize();

	Program *prog = new Program();
	prog->forest_arena_enabled = true;

	// A project (non-well-known) base: getPointerType must CREATE a new DataDefPTR (not a
	// cached / pinned-global return like int*/char*), so the write-through fires.
	DataDefPTR *p = prog->getPointerType(&widget);
	REQUIRE(p != NULL);

	// --- reads are UNCHANGED (base_type stays the live read-cache) ---
	CHECK(p->is_pointer());
	CHECK(p->base_type == &widget);
	CHECK(p->rawtype() == widget.rawtype());
	CHECK(p->size == 8);

	// --- the arena record is populated at the pointer's own project-id slot ---
	uint32_t ptid = prog->type_id_for(p);
	CHECK(arena_id_is_project(ptid));
	defrec r;
	REQUIRE(prog->forest_arena.get_def_at(ptid, r));
	CHECK(r.kind == DK_PTR);
	CHECK(std::string(prog->forest_arena.strings.c_str(r.name_id)) == p->name);	// "Widget*"
	CHECK(r.size == 8);
	CHECK(r.ref0 == prog->type_id_for(&widget));	// pointee stored as its (project) type-id
	CHECK(arena_id_is_project(r.ref0));

	// idempotent: a second call returns the SAME cached pointer (no duplicate write-through).
	DataDefPTR *p2 = prog->getPointerType(&widget);
	CHECK(p2 == p);

	delete prog;	// while widget is still in scope
}

// ---- SLICE 1d: the reference + const funnels write-through the same way (DK_REF / DK_CONST),
//      via the generalized forest_arena_record_unary. Reads (is_reference/is_const/base_type)
//      stay unchanged; only the record is dual-populated.
TEST_CASE("B3 arena: getReferenceType / getConstType write-through (DK_REF / DK_CONST)")
{
	DataDefSTRUCT widget("Widget", 0);	// declared first → outlives `delete prog`
	widget.finalize();

	Program *prog = new Program();
	prog->forest_arena_enabled = true;

	// --- reference (Widget&) ---
	DataDefREF *rf = prog->getReferenceType(&widget);
	REQUIRE(rf != NULL);
	CHECK(rf->is_reference());			// reads unchanged
	CHECK(rf->base_type == &widget);
	uint32_t rtid = prog->type_id_for(rf);
	CHECK(arena_id_is_project(rtid));
	defrec rr;
	REQUIRE(prog->forest_arena.get_def_at(rtid, rr));
	CHECK(rr.kind == DK_REF);
	CHECK(rr.ref0 == prog->type_id_for(&widget));	// referee, by project id

	// --- const (const Widget) ---
	DataDefCONST *cst = prog->getConstType(&widget);
	REQUIRE(cst != NULL);
	CHECK(cst->is_const());				// reads unchanged
	CHECK(cst->base_type == &widget);
	uint32_t ctid = prog->type_id_for(cst);
	CHECK(arena_id_is_project(ctid));
	defrec cr;
	REQUIRE(prog->forest_arena.get_def_at(ctid, cr));
	CHECK(cr.kind == DK_CONST);
	CHECK(cr.ref0 == prog->type_id_for(&widget));	// unqualified base, by project id

	CHECK(rtid != ctid);				// distinct derived types get distinct slots

	delete prog;	// while widget is still in scope
}

// ---- SLICE 1e: the AGGREGATE write-through, driven by a REAL parse (the honest test of the
//      HOOKS, not just the encoder). tokenize+parse a program defining a struct + two classes
//      (a base and a derived-with-member-and-method) with forest_arena_enabled; then assert the
//      arena carries a DK_STRUCT + DK_CLASS record at each aggregate's project-id slot, with the
//      member/base/method runs populated and cross-refs = the operands' type-ids. The forest
//      gates cover the flag-OFF byte-identity; this covers the flag-ON population end to end.
TEST_CASE("B3 arena: aggregate write-through records DK_STRUCT/DK_CLASS from a real parse")
{
	Program *prog = new Program();
	prog->forest_arena_enabled = true;	// opt in BEFORE parse so the hooks fire

	const char *src =
		"struct Point { int x; int y; };\n"
		"class Base { public: int bb; };\n"
		"class Widget : public Base { public: int w; int get() { return w; } };\n"
		"int main() { Point p; Widget wd; return p.x + wd.w + wd.get(); }\n";
	TokenProgram *tp = prog->tokenize_buffer(src, "arena_1e.mad");
	REQUIRE(tp != NULL);
	REQUIRE(prog->parse(tp));

	// --- Point: a plain struct (no object member) -> DK_STRUCT with 2 int members ---
	datadef_map_citer pit = prog->struct_map.find("Point");
	REQUIRE(pit != prog->struct_map.end());
	DataDefSTRUCT *pt = dynamic_cast<DataDefSTRUCT *>(pit->second);
	REQUIRE(pt != NULL);
	uint32_t ptid = prog->type_id_for(pt);
	CHECK(arena_id_is_project(ptid));
	defrec pr;
	REQUIRE(prog->forest_arena.get_def_at(ptid, pr));
	CHECK(pr.kind == DK_STRUCT);
	CHECK(std::string(prog->forest_arena.strings.c_str(pr.name_id)) == "Point");
	CHECK(pr.size == pt->size);
	REQUIRE(pr.members_count == 2);
	memberrec mx, my;
	REQUIRE(prog->forest_arena.get_payload(pr.members_begin, 0, mx));
	REQUIRE(prog->forest_arena.get_payload(pr.members_begin, 1, my));
	CHECK(std::string(prog->forest_arena.strings.c_str(mx.name_id)) == "x");
	CHECK(std::string(prog->forest_arena.strings.c_str(my.name_id)) == "y");
	CHECK(mx.offset == 0);
	CHECK(my.offset == 4);
	// `int` resolves to a PINNED primitive slot (int32 == slot 9, not the generic int slot);
	// assert the spine invariant — pinned, shared, resolvable, and NEVER recorded — rather
	// than a specific slot number.
	CHECK(arena_id_is_pinned(mx.type_id));
	CHECK(mx.type_id == my.type_id);			// both members share the one primitive
	DataDef *int_prim = madc_type_from_id(mx.type_id);	// resolves to the process global
	CHECK(int_prim != NULL);
	CHECK(prog->forest_arena.has_def(mx.type_id) == false);	// a pinned primitive has NO record

	// --- Base: a class -> DK_CLASS carrying member "bb" ---
	datadef_map_citer bit = prog->struct_map.find("Base");
	REQUIRE(bit != prog->struct_map.end());
	DataDefCLASS *base = dynamic_cast<DataDefCLASS *>(bit->second);
	REQUIRE(base != NULL);
	uint32_t btid = prog->type_id_for(base);
	CHECK(arena_id_is_project(btid));
	defrec br;
	REQUIRE(prog->forest_arena.get_def_at(btid, br));
	CHECK(br.kind == DK_CLASS);
	CHECK(std::string(prog->forest_arena.strings.c_str(br.name_id)) == "Base");
	bool base_has_bb = false;
	for (uint32_t i = 0; i < br.members_count; ++i) {
		memberrec m;
		REQUIRE(prog->forest_arena.get_payload(br.members_begin, i, m));
		if (std::string(prog->forest_arena.strings.c_str(m.name_id)) == "bb")
			base_has_bb = true;
	}
	CHECK(base_has_bb);

	// --- Widget: DK_CLASS with a base (-> Base's type-id), a member "w", and >=1 method ---
	datadef_map_citer wit = prog->struct_map.find("Widget");
	REQUIRE(wit != prog->struct_map.end());
	DataDefCLASS *widget = dynamic_cast<DataDefCLASS *>(wit->second);
	REQUIRE(widget != NULL);
	uint32_t wtid = prog->type_id_for(widget);
	CHECK(arena_id_is_project(wtid));
	defrec wr;
	REQUIRE(prog->forest_arena.get_def_at(wtid, wr));
	CHECK(wr.kind == DK_CLASS);
	CHECK(std::string(prog->forest_arena.strings.c_str(wr.name_id)) == "Widget");

	// base cross-ref is Base's PROJECT type-id, and Base's own record resolves through it
	REQUIRE(wr.bases_count == 1);
	baserec wb;
	REQUIRE(prog->forest_arena.get_payload(wr.bases_begin, 0, wb));
	CHECK(arena_id_is_project(wb.base_id));
	CHECK(wb.base_id == btid);				// same Base object the class inherits
	defrec base_via_ref;
	REQUIRE(prog->forest_arena.get_def_at(wb.base_id, base_via_ref));
	CHECK(base_via_ref.kind == DK_CLASS);
	CHECK(std::string(prog->forest_arena.strings.c_str(base_via_ref.name_id)) == "Base");

	// member "w" is present (own member; inherited members may also be flattened in)
	bool widget_has_w = false;
	for (uint32_t i = 0; i < wr.members_count; ++i) {
		memberrec m;
		REQUIRE(prog->forest_arena.get_payload(wr.members_begin, i, m));
		if (std::string(prog->forest_arena.strings.c_str(m.name_id)) == "w")
			widget_has_w = true;
	}
	CHECK(widget_has_w);

	// >=1 method recorded; each method's func_id is a cross-ref type-id (the DK_FUNC record
	// itself is filled by slice 1f, so a forward/unwritten slot is fine here).
	REQUIRE(wr.methods_count >= 1);
	for (uint32_t i = 0; i < wr.methods_count; ++i) {
		methodrec md;
		REQUIRE(prog->forest_arena.get_payload(wr.methods_begin, i, md));
		CHECK(md.name_id != 0);
		bool func_ref_ok = (md.func_id == (uint32_t)MADC_TYPEID_INVALID)
				 || arena_id_is_project(md.func_id)
				 || arena_id_is_pinned(md.func_id);
		CHECK(func_ref_ok);
	}

	// no primitive was ever recorded (the type-id spine invariant holds through a real parse)
	assert_no_primitive_records(prog->forest_arena);

	delete prog;
}

// ---- SLICE 1f-a: a class method's FuncDef is now recorded as its OWN DK_FUNC record (return +
//      params run), closing the methodrec.func_id forward-ref that 1e left open. Driven by a real
//      parse: every method's func_id now resolves to a DK_FUNC record; a method with an explicit
//      parameter carries __this (param 0) + that parameter.
TEST_CASE("B3 arena: method FuncDef records resolve to DK_FUNC (return + params) via a real parse")
{
	Program *prog = new Program();
	prog->forest_arena_enabled = true;

	const char *src =
		"class Widget { public: int w; int get() { return w; } int add(int k) { return w + k; } };\n"
		"int main() { Widget wd; return wd.get() + wd.add(2); }\n";
	TokenProgram *tp = prog->tokenize_buffer(src, "arena_1f.mad");
	REQUIRE(tp != NULL);
	REQUIRE(prog->parse(tp));

	datadef_map_citer wit = prog->struct_map.find("Widget");
	REQUIRE(wit != prog->struct_map.end());
	DataDefCLASS *widget = dynamic_cast<DataDefCLASS *>(wit->second);
	REQUIRE(widget != NULL);
	uint32_t wtid = prog->type_id_for(widget);
	defrec wr;
	REQUIRE(prog->forest_arena.get_def_at(wtid, wr));
	REQUIRE(wr.methods_count >= 2);		// get + add (plus any implicit members)

	// Every method whose func_id is a project id now resolves to a DK_FUNC record (1f-a closed
	// the forward-ref). At least one method carries an explicit (non-__this) parameter: add(int k).
	int func_records = 0, with_explicit_param = 0;
	for (uint32_t i = 0; i < wr.methods_count; ++i) {
		methodrec md;
		REQUIRE(prog->forest_arena.get_payload(wr.methods_begin, i, md));
		if (!arena_id_is_project(md.func_id))
			continue;
		defrec fr;
		REQUIRE(prog->forest_arena.get_def_at(md.func_id, fr));
		CHECK(fr.kind == DK_FUNC);
		++func_records;
		// return type-id resolves as a pinned primitive (int/void), a project type, or invalid
		bool ret_ok = arena_id_is_pinned(fr.ref0) || arena_id_is_project(fr.ref0)
			    || fr.ref0 == (uint32_t)MADC_TYPEID_INVALID;
		CHECK(ret_ok);
		if (fr.params_count >= 1) {
			// param 0 of a non-static method is the hidden __this = a pointer to the
			// class (a project DK_PTR recorded by the unary write-through).
			paramrec p0;
			REQUIRE(prog->forest_arena.get_payload(fr.params_begin, 0, p0));
			CHECK(arena_id_is_project(p0.type_id));
		}
		if (fr.params_count >= 2)		// __this + an explicit param
			++with_explicit_param;
	}
	CHECK(func_records >= 2);
	CHECK(with_explicit_param >= 1);	// add(int k)

	assert_no_primitive_records(prog->forest_arena);
	delete prog;
}

// ---- CHUNK 1 (flip fidelity): ANONYMOUS sub-aggregate population. addAnonymousAggregate
//      flattens a nameless union/struct's members into the parent DURING the body parse, so
//      the sub-aggregate never reaches a registration hook of its own — the parent's
//      completion write-through must (a) record the SUB as its own def and (b) relink the
//      grouping via the parent's anonrec run (without it a bound anon union's overlap is
//      lost — the v11 silent miscompile). Uses the forest_bind_gate anon case's shape.
TEST_CASE("B3 arena: an anonymous union records its own def + the parent's anonrec run")
{
	Program *prog = new Program();
	prog->forest_arena_enabled = true;

	const char *src =
		"struct S { int tag; union { int i; char buf[8]; }; };\n"
		"int main() { S s; s.tag = 7; s.i = 5; return s.buf[0]; }\n";
	TokenProgram *tp = prog->tokenize_buffer(src, "arena_anon.mad");
	REQUIRE(tp != NULL);
	REQUIRE(prog->parse(tp));

	datadef_map_citer sit = prog->struct_map.find("S");
	REQUIRE(sit != prog->struct_map.end());
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(sit->second);
	REQUIRE(sdd != NULL);
	REQUIRE(sdd->anonymous_aggregates.size() == 1);	// the live grouping the arena mirrors

	uint32_t stid = prog->type_id_for(sdd);
	defrec sr;
	REQUIRE(prog->forest_arena.get_def_at(stid, sr));
	CHECK(sr.kind == DK_STRUCT);
	bool has_anon_flag = (sr.flags & DF_HAS_ANON_AGG) != 0;
	CHECK(has_anon_flag);
	REQUIRE(sr.anon_count == 1);

	// The anonrec mirrors the live AnonymousAggregateInfo verbatim.
	anonrec ar;
	REQUIRE(prog->forest_arena.get_payload(sr.anon_begin, 0, ar));
	CHECK(ar.first_member == sdd->anonymous_aggregates[0].first_member);
	CHECK(ar.member_count == sdd->anonymous_aggregates[0].member_count);
	CHECK(ar.member_count == 2);			// i + buf
	CHECK(ar.offset == sdd->anonymous_aggregates[0].offset);

	// The grouped members overlap at the union's base offset in the PARENT's member run.
	REQUIRE(ar.first_member + ar.member_count <= sr.members_count);
	memberrec mi, mb;
	REQUIRE(prog->forest_arena.get_payload(sr.members_begin, ar.first_member, mi));
	REQUIRE(prog->forest_arena.get_payload(sr.members_begin, ar.first_member + 1, mb));
	CHECK(std::string(prog->forest_arena.strings.c_str(mi.name_id)) == "i");
	CHECK(std::string(prog->forest_arena.strings.c_str(mb.name_id)) == "buf");
	CHECK(mi.offset == ar.offset);
	CHECK(mb.offset == ar.offset);			// union overlap, not sequential layout

	// The nameless sub-aggregate has its OWN record: a DK_UNION with the same 2 members
	// (offsets 0-based within the union), reached through the anonrec cross-ref.
	REQUIRE(arena_id_is_project(ar.sub_type_id));
	defrec ur;
	REQUIRE(prog->forest_arena.get_def_at(ar.sub_type_id, ur));
	CHECK(ur.kind == DK_UNION);
	bool sub_union_layout = (ur.flags & DF_UNION_LAYOUT) != 0;
	CHECK(sub_union_layout);
	bool sub_anonymous = (ur.flags & DF_IS_ANONYMOUS) != 0;
	CHECK(sub_anonymous);
	REQUIRE(ur.members_count == 2);
	memberrec ui;
	REQUIRE(prog->forest_arena.get_payload(ur.members_begin, 0, ui));
	CHECK(std::string(prog->forest_arena.strings.c_str(ui.name_id)) == "i");
	CHECK(ui.offset == 0);

	assert_no_primitive_records(prog->forest_arena);
	delete prog;
}
