/* test_cir_arena.cpp — B3 step-2: prove a DataDef's state survives the arena
 * round-trip (encode -> copy-the-blocks [== dump/load, since everything is
 * index/offset relative with zero live pointers] -> decode) losslessly.
 *
 * This is the empirical validation of the record SCHEMA before any of the ~548
 * live mutation sites are touched. If these shapes cannot round-trip byte-stably
 * here, B3's storage model is wrong and we learn it now.
 *
 * Covered:
 *   - DK_PRIM (a primitive scalar), DK_PTR (index cross-ref)
 *   - DK_STRUCT (payload (begin,count) slice + per-member type indices + offsets)
 *   - NESTED struct (proves the resolve-first payload discipline: a member whose
 *     type is itself an aggregate must NOT interleave its member run with ours)
 *   - DK_CLASS with the three hard-tier FLATTENINGS: bases, methods, the
 *     pointer-keyed vbase_offset map -> a sorted (class_idx,offset) run, and the
 *     nested vtable_groups -> a two-level (vgrouprec + slot-id) slice
 *   - DK_FUNC (FuncDef return + parameters run)
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

// ---- encode: a live DataDef* -> a defrec in the arena, returning its def index.
// Two-phase (reserve slot, record in `seen`, then fill) so a (transitively) self-
// referencing type terminates. PAYLOAD DISCIPLINE: every run resolves ALL of its
// child def indices FIRST (a recursive encode may append payload) and only THEN
// appends its own contiguous run — otherwise a nested aggregate's run interleaves.
static uint32_t arena_encode(DefArena &a, DataDef *dd, std::map<DataDef *, uint32_t> &seen)
{
	if ( !dd ) return 0;
	auto it = seen.find(dd);
	if ( it != seen.end() ) return it->second;

	defrec r;
	std::memset(&r, 0, sizeof(r));
	r.name_id  = a.strings.intern(dd->name.c_str());
	r.canon_id = dd->canonical_cpp_spelling.empty()
		     ? 0u : a.strings.intern(dd->canonical_cpp_spelling.c_str());
	r.size     = (uint32_t)dd->size;
	r.datatype = (uint32_t)dd->rawtype();

	uint32_t idx = a.add_def(r);	// reserve + record BEFORE recursion
	seen[dd] = idx;

	if ( DataDefSTRUCT *s = dynamic_cast<DataDefSTRUCT *>(dd) )
	{
		// --- members (resolve types first, then append the contiguous run) ---
		std::vector<uint32_t> mt(s->members.size());
		for ( size_t i = 0; i < s->members.size(); ++i )
			mt[i] = arena_encode(a, s->members[i].second, seen);
		uint32_t mbegin = (uint32_t)a.payload.size();
		for ( size_t i = 0; i < s->members.size(); ++i )
		{
			memberrec m;
			std::memset(&m, 0, sizeof(m));
			m.name_id    = a.strings.intern(s->members[i].first.c_str());
			m.type_idx   = mt[i];
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

			// --- bases (resolve base indices first) ---
			std::vector<uint32_t> bidx(c->bases.size());
			for ( size_t i = 0; i < c->bases.size(); ++i )
				bidx[i] = arena_encode(a, c->bases[i].base, seen);
			uint32_t bbegin = (uint32_t)a.payload.size();
			for ( size_t i = 0; i < c->bases.size(); ++i )
			{
				baserec b;
				b.base_idx = bidx[i];
				b.offset   = (uint32_t)c->bases[i].offset;
				b.flags    = (c->bases[i].is_virtual ? BSF_VIRTUAL : 0u)
					   | (c->bases[i].is_primary ? BSF_PRIMARY : 0u)
					   | (c->bases[i].access << BSF_ACCESS_SHIFT);
				a.add_payload(b);
			}
			r.bases_begin = bbegin;
			r.bases_count = (uint32_t)c->bases.size();

			// --- methods (resolve each method's FuncDef index first) ---
			std::vector<uint32_t> fidx(c->methods.size());
			for ( size_t i = 0; i < c->methods.size(); ++i )
				fidx[i] = arena_encode(a, c->methods[i]->type, seen);
			uint32_t mmbegin = (uint32_t)a.payload.size();
			for ( size_t i = 0; i < c->methods.size(); ++i )
			{
				methodrec md;
				md.name_id  = a.strings.intern(c->methods[i]->name.c_str());
				md.func_idx = fidx[i];
				md.flags    = 0;
				a.add_payload(md);
			}
			r.methods_begin = mmbegin;
			r.methods_count = (uint32_t)c->methods.size();

			// --- vbase_offset: flatten pointer-keyed map -> sorted (idx,offset) run ---
			std::vector<std::pair<uint32_t, uint32_t> > vb;
			for ( std::map<DataDefCLASS *, size_t>::const_iterator vi = c->vbase_offset.begin();
			      vi != c->vbase_offset.end(); ++vi )
				vb.push_back(std::make_pair(arena_encode(a, vi->first, seen),
							    (uint32_t)vi->second));
			std::sort(vb.begin(), vb.end());
			uint32_t vbbegin = (uint32_t)a.payload.size();
			for ( size_t i = 0; i < vb.size(); ++i )
			{
				vbaserec vr; vr.class_idx = vb[i].first; vr.offset = vb[i].second;
				a.add_payload(vr);
			}
			r.vbase_begin = vbbegin;
			r.vbase_count = (uint32_t)vb.size();

			// --- vtable_groups: nested vector -> two-level slice. Resolve owners
			//     first; append each group's slot-id run; THEN append the vgrouprec
			//     run contiguously (slot runs carry no recursion). ---
			std::vector<uint32_t> owners(c->vtable_groups.size());
			for ( size_t g = 0; g < c->vtable_groups.size(); ++g )
				owners[g] = arena_encode(a, c->vtable_groups[g].owner, seen);
			std::vector<vgrouprec> vgs(c->vtable_groups.size());
			for ( size_t g = 0; g < c->vtable_groups.size(); ++g )
			{
				const DataDefCLASS::VtableGroup &vg = c->vtable_groups[g];
				uint32_t sbegin = (uint32_t)a.payload.size();
				for ( size_t k = 0; k < vg.slots.size(); ++k )
					a.add_word(a.strings.intern(vg.slots[k].c_str()));
				vgs[g].owner_idx   = owners[g];
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
		r.ref0 = arena_encode(a, &fd->returns, seen);	// return type
		std::vector<uint32_t> pt(fd->parameters.size());
		for ( size_t i = 0; i < fd->parameters.size(); ++i )
			pt[i] = arena_encode(a, fd->parameters[i], seen);
		uint32_t pbegin = (uint32_t)a.payload.size();
		for ( size_t i = 0; i < fd->parameters.size(); ++i )
		{
			paramrec p;
			p.type_idx        = pt[i];
			p.flags           = (i < fd->const_params.size() && fd->const_params[i]) ? 1u : 0u;
			p.cpp_spelling_id = (i < fd->param_cpp_spellings.size() && !fd->param_cpp_spellings[i].empty())
				? a.strings.intern(fd->param_cpp_spellings[i].c_str()) : 0u;
			a.add_payload(p);
		}
		r.params_begin = pbegin;
		r.params_count = (uint32_t)fd->parameters.size();
		if ( fd->is_varargs )     r.flags |= DF_IS_VARARGS;
		if ( fd->is_void_params ) r.flags |= DF_IS_VOID_PARAMS;
		if ( fd->declaration_only ) r.flags |= DF_DECLARATION_ONLY;
	}
	else if ( DataDefCONST *k = dynamic_cast<DataDefCONST *>(dd) )
	{
		r.kind = DK_CONST;
		r.ref0 = arena_encode(a, k->base_type, seen);
	}
	else if ( DataDefREF *rf = dynamic_cast<DataDefREF *>(dd) )
	{
		r.kind = DK_REF;
		r.ref0 = arena_encode(a, rf->base_type, seen);
	}
	else if ( DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd) )
	{
		r.kind = DK_PTR;
		r.ref0 = arena_encode(a, p->base_type, seen);
	}
	else if ( dynamic_cast<DataDefENUM *>(dd) )
	{
		r.kind = DK_ENUM;
	}
	else
	{
		r.kind = (dd->rawtype() == DataType::dtVOID) ? DK_VOID : DK_PRIM;
	}

	a.set_def(idx, r);	// rewrite with kind + operands/slices now known
	return idx;
}

// Simulate dump/load: the three blocks are index/offset-relative with no live pointers,
// so a plain value copy is byte-for-byte what a dump-then-load produces.
static DefArena roundtrip(const DefArena &a)
{
	DefArena b;
	b.strings = a.strings;
	b.defs    = a.defs;
	b.payload = a.payload;
	return b;
}

static std::string dname(const DefArena &a, uint32_t idx)
{
	defrec r;
	if ( !a.get_def(idx, r) ) return "<oob>";
	return std::string(a.strings.c_str(r.name_id));
}

TEST_CASE("B3 arena: a primitive scalar round-trips")
{
	DataDefINT ii;
	DefArena a;
	std::map<DataDef *, uint32_t> seen;
	uint32_t idx = arena_encode(a, &ii, seen);

	DefArena b = roundtrip(a);
	defrec r;
	REQUIRE(b.get_def(idx, r));
	CHECK(r.kind == DK_PRIM);
	CHECK(std::string(b.strings.c_str(r.name_id)) == "int");
	CHECK(r.size == 4);
	CHECK(r.datatype == (uint32_t)DataType::dtINT);
}

TEST_CASE("B3 arena: a pointer round-trips its pointee by INDEX (no live ptr)")
{
	DataDefINT ii;
	DataDefPTR pp(ii);
	DefArena a;
	std::map<DataDef *, uint32_t> seen;
	uint32_t idx = arena_encode(a, &pp, seen);

	DefArena b = roundtrip(a);
	defrec pr;
	REQUIRE(b.get_def(idx, pr));
	CHECK(pr.kind == DK_PTR);
	CHECK(dname(b, pr.ref0) == "int");
}

TEST_CASE("B3 arena: a plain struct round-trips (payload slice + member indices + offsets)")
{
	DataDefINT ii;
	DataDefSTRUCT pt("Point", 0);
	pt.addMember("x", ii, 1);
	pt.addMember("y", ii, 1);
	pt.finalize();
	REQUIRE(pt.members.size() == 2);

	DefArena a;
	std::map<DataDef *, uint32_t> seen;
	uint32_t idx = arena_encode(a, &pt, seen);

	DefArena b = roundtrip(a);
	defrec sr;
	REQUIRE(b.get_def(idx, sr));
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
	CHECK(m0.type_idx == m1.type_idx);	// shared int def, by index
	CHECK(dname(b, m0.type_idx) == "int");
}

TEST_CASE("B3 arena: NESTED struct round-trips (resolve-first payload discipline)")
{
	DataDefINT ii;
	DataDefSTRUCT pt("Point", 0);
	pt.addMember("x", ii, 1);
	pt.addMember("y", ii, 1);
	pt.finalize();
	DataDefSTRUCT ln("Line", 0);
	ln.addMember("a", pt, 1);	// a member whose TYPE is itself an aggregate
	ln.addMember("b", pt, 1);
	ln.finalize();

	DefArena a;
	std::map<DataDef *, uint32_t> seen;
	uint32_t lidx = arena_encode(a, &ln, seen);

	DefArena b = roundtrip(a);
	defrec lr;
	REQUIRE(b.get_def(lidx, lr));
	CHECK(lr.kind == DK_STRUCT);
	REQUIRE(lr.members_count == 2);	// NOT corrupted by Point's member run interleaving

	memberrec a0, a1;
	REQUIRE(b.get_payload(lr.members_begin, 0, a0));
	REQUIRE(b.get_payload(lr.members_begin, 1, a1));
	CHECK(std::string(b.strings.c_str(a0.name_id)) == "a");
	CHECK(a0.offset == 0);
	CHECK(std::string(b.strings.c_str(a1.name_id)) == "b");
	CHECK(a1.offset == 8);
	CHECK(a0.type_idx == a1.type_idx);	// both are Point

	// The nested Point def is intact with its OWN 2 members.
	defrec pr;
	REQUIRE(b.get_def(a0.type_idx, pr));
	CHECK(std::string(b.strings.c_str(pr.name_id)) == "Point");
	REQUIRE(pr.members_count == 2);
	memberrec px, py;
	REQUIRE(b.get_payload(pr.members_begin, 0, px));
	REQUIRE(b.get_payload(pr.members_begin, 1, py));
	CHECK(std::string(b.strings.c_str(px.name_id)) == "x");
	CHECK(std::string(b.strings.c_str(py.name_id)) == "y");
}

TEST_CASE("B3 arena: a FuncDef round-trips its return + parameter run")
{
	DataDefINT ii;
	FuncDef *add = new FuncDef(ii);	// returns int
	add->name = "add";
	add->parameters.push_back(&ii);
	add->parameters.push_back(&ii);

	DefArena a;
	std::map<DataDef *, uint32_t> seen;
	uint32_t idx = arena_encode(a, add, seen);

	DefArena b = roundtrip(a);
	defrec fr;
	REQUIRE(b.get_def(idx, fr));
	CHECK(fr.kind == DK_FUNC);
	CHECK(std::string(b.strings.c_str(fr.name_id)) == "add");
	CHECK(dname(b, fr.ref0) == "int");	// return type by index
	REQUIRE(fr.params_count == 2);
	paramrec p0, p1;
	REQUIRE(b.get_payload(fr.params_begin, 0, p0));
	REQUIRE(b.get_payload(fr.params_begin, 1, p1));
	CHECK(dname(b, p0.type_idx) == "int");
	CHECK(dname(b, p1.type_idx) == "int");
	delete add;
}

TEST_CASE("B3 arena: DK_CLASS round-trips all three hard-tier flattenings")
{
	DataDefINT ii;
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
	FuncDef *fd = new FuncDef(ii);
	fd->name = "foo";
	fd->parameters.push_back(&ii);
	Variable *mv = new Variable("foo", *fd);
	der.methods.push_back(mv);
	// nested vtable_groups
	DataDefCLASS::VtableGroup vg;
	vg.owner = &der; vg.this_offset = 0; vg.slots.push_back("foo"); vg.slots.push_back("bar"); vg.addr_point = 2;
	der.vtable_groups.push_back(vg);
	der.has_vtable = true;
	der.nvsize = 8; der.class_align = 8;

	DefArena a;
	std::map<DataDef *, uint32_t> seen;
	uint32_t idx = arena_encode(a, &der, seen);

	DefArena b = roundtrip(a);
	defrec cr;
	REQUIRE(b.get_def(idx, cr));
	CHECK(cr.kind == DK_CLASS);
	CHECK(std::string(b.strings.c_str(cr.name_id)) == "Derived");
	CHECK((cr.flags & DF_HAS_VTABLE) != 0);
	CHECK(cr.nvsize == 8);
	CHECK(cr.class_align == 8);

	// bases
	REQUIRE(cr.bases_count == 1);
	baserec br;
	REQUIRE(b.get_payload(cr.bases_begin, 0, br));
	CHECK(dname(b, br.base_idx) == "Base");
	CHECK(br.offset == 8);
	CHECK((br.flags & BSF_VIRTUAL) != 0);
	CHECK((br.flags & BSF_PRIMARY) == 0);

	// methods -> FuncDef by index
	REQUIRE(cr.methods_count == 1);
	methodrec md;
	REQUIRE(b.get_payload(cr.methods_begin, 0, md));
	CHECK(std::string(b.strings.c_str(md.name_id)) == "foo");
	defrec mfr;
	REQUIRE(b.get_def(md.func_idx, mfr));
	CHECK(mfr.kind == DK_FUNC);
	CHECK(dname(b, mfr.ref0) == "int");
	REQUIRE(mfr.params_count == 1);

	// vbase_offset flattening
	REQUIRE(cr.vbase_count == 1);
	vbaserec vbr;
	REQUIRE(b.get_payload(cr.vbase_begin, 0, vbr));
	CHECK(dname(b, vbr.class_idx) == "Base");
	CHECK(vbr.offset == 16);

	// vtable_groups flattening (two-level slice)
	REQUIRE(cr.vgroup_count == 1);
	vgrouprec gr;
	REQUIRE(b.get_payload(cr.vgroup_begin, 0, gr));
	CHECK(dname(b, gr.owner_idx) == "Derived");
	CHECK(gr.this_offset == 0);
	CHECK(gr.addr_point == 2);
	REQUIRE(gr.slots_count == 2);
	uint32_t s0 = 0, s1 = 0;
	REQUIRE(b.get_word(gr.slots_begin, 0, s0));
	REQUIRE(b.get_word(gr.slots_begin, 1, s1));
	CHECK(std::string(b.strings.c_str(s0)) == "foo");
	CHECK(std::string(b.strings.c_str(s1)) == "bar");

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
static std::string fdname(const FrozenDefArena &f, uint32_t idx)
{
	defrec r;
	if ( !f.get_def(idx, r) ) return "<oob>";
	return std::string(f.c_str(r.name_id));
}

TEST_CASE("B3 arena: REAL byte round-trip through the snapshot container (not a copy)")
{
	// The richest case — a class with all three flattenings — into a live arena.
	DataDefINT ii;
	DataDefCLASS base("Base", 0, DataType::dtRESERVED); base.finalize();
	DataDefCLASS der("Derived", 0, DataType::dtRESERVED); der.finalize();
	BaseSpec bs; bs.base = &base; bs.offset = 8; bs.is_virtual = true; bs.access = 0; bs.is_primary = false;
	der.bases.push_back(bs);
	der.vbase_offset[&base] = 16;
	FuncDef *fd = new FuncDef(ii); fd->name = "foo"; fd->parameters.push_back(&ii);
	Variable *mv = new Variable("foo", *fd); der.methods.push_back(mv);
	DataDefCLASS::VtableGroup vg; vg.owner = &der; vg.this_offset = 0;
	vg.slots.push_back("foo"); vg.slots.push_back("bar"); vg.addr_point = 2;
	der.vtable_groups.push_back(vg);
	der.has_vtable = true; der.nvsize = 8; der.class_align = 8;

	DefArena a;
	std::map<DataDef *, uint32_t> seen;
	uint32_t idx = arena_encode(a, &der, seen);

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
	REQUIRE(f.get_def(idx, cr));
	CHECK(cr.kind == DK_CLASS);
	CHECK(std::string(f.c_str(cr.name_id)) == "Derived");
	CHECK((cr.flags & DF_HAS_VTABLE) != 0);
	CHECK(cr.nvsize == 8);

	REQUIRE(cr.bases_count == 1);
	baserec br; REQUIRE(f.get_payload(cr.bases_begin, 0, br));
	CHECK(fdname(f, br.base_idx) == "Base");
	CHECK(br.offset == 8);
	CHECK((br.flags & BSF_VIRTUAL) != 0);

	REQUIRE(cr.methods_count == 1);
	methodrec md; REQUIRE(f.get_payload(cr.methods_begin, 0, md));
	CHECK(std::string(f.c_str(md.name_id)) == "foo");
	defrec mfr; REQUIRE(f.get_def(md.func_idx, mfr));
	CHECK(mfr.kind == DK_FUNC);
	CHECK(fdname(f, mfr.ref0) == "int");

	REQUIRE(cr.vbase_count == 1);
	vbaserec vbr; REQUIRE(f.get_payload(cr.vbase_begin, 0, vbr));
	CHECK(fdname(f, vbr.class_idx) == "Base");
	CHECK(vbr.offset == 16);

	REQUIRE(cr.vgroup_count == 1);
	vgrouprec gr; REQUIRE(f.get_payload(cr.vgroup_begin, 0, gr));
	CHECK(fdname(f, gr.owner_idx) == "Derived");
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
