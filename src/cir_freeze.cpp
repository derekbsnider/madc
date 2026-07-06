/* cir_freeze.cpp — freeze/thaw a cir_node sub-DAG through the madc::dis
 * pool-snapshot container (forest Phase 2+3 / data-substrate Track B2+B3).
 * See cir_freeze.h for the contract, the forest format, and the closure
 * story.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <queue>	// madc.h uses std::queue (included below) and relies on the TU providing it
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
// mir-code-alloc.h (reached via cir_node.h -> c2mir headers) redefines
// MAP_FAILED to NULL for its own allocator; drop the glibc define so that
// redefinition is fresh, and compare mmap() results against the real value
// explicitly below.
#undef MAP_FAILED

extern thread_local bool madc_verbose;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"	// Variable (Phase 6 3d: reconstruct method Variables on load)
#include "madc.h"		// FuncDef (Phase 6 3d: reconstruct method FuncDefs on load)
#include "token_arena.h"
#include "madcdis/id_table.h"
#include "cir_freeze.h"

// ---------------------------------------------------------------------------
// Payload classification
// ---------------------------------------------------------------------------
// Mirrors c2mir's ext_node_is_leaf single source of truth: leaves are
// code <= N_ID plus the complex-constant trio (N_CF/N_CD/N_CLD, numerically
// above N_ID). Interior nodes own the ops DLIST; reading their union as a
// payload (or vice versa) aliases garbage. c2mir_node_first_op self-guards
// on leaves, so child walks below need no separate interior test.

enum cir_payload_class { CIR_PAYLOAD_NONE, CIR_PAYLOAD_SCALAR, CIR_PAYLOAD_STR };

static cir_payload_class cir_payload_class_for(uint32_t code)
{
	switch (code) {
	case N_I: case N_L: case N_LL: case N_U: case N_UL: case N_ULL:
	case N_F: case N_D: case N_LD:
	case N_CH: case N_CH16: case N_CH32:
	case N_CF: case N_CD: case N_CLD:
		return CIR_PAYLOAD_SCALAR;
	case N_STR: case N_STR16: case N_STR32:
	case N_ID:
		return CIR_PAYLOAD_STR;
	default:
		return CIR_PAYLOAD_NONE;	// N_IGNORE + every interior code
	}
}

// ---------------------------------------------------------------------------
// The context-hash pin
// ---------------------------------------------------------------------------
// Folds everything a frozen forest's byte layout and id spaces depend on:
// the madc release, the record/position PODs, the c2mir node-code enum tail
// (fork reorderings renumber every serialized `code`), and the typeid
// primitive tail (pinned slots). A mismatch means "built by a different
// madc" — readers reject, they never guess.

#ifndef MADC_VERSION_STR
#define MADC_VERSION_STR "0.0.0"
#endif

uint64_t madc_cir_context_hash()
{
	static uint64_t cached = 0;
	if (!cached) {
		char sig[256];
		snprintf(sig, sizeof(sig),
			 "madc-forest-v%u|%s|rec%u|pos%u|nc%d|tid%d|snap%u",
			 (unsigned)CIR_FOREST_FORMAT_VERSION, MADC_VERSION_STR,
			 (unsigned)sizeof(cir_frozen_record),
			 (unsigned)sizeof(cir_frozen_pos),
			 (int)N_INT128, (int)MADC_TYPEID_PRIMITIVE_LAST,
			 (unsigned)madc::dis::SNAPSHOT_FORMAT_VERSION);
		cached = madc_pch::hash_content(sig, strlen(sig));
		if (!cached)
			cached = 1;	// 0 means "unpinned" in the container header
	}
	return cached;
}

// ---------------------------------------------------------------------------
// Freeze
// ---------------------------------------------------------------------------

// One record's payload + extension block (everything except child linkage,
// which is partition-dependent). False on an out-of-format string or a
// missing string pool.
static bool cir_fill_record(cir_node *n, cir_frozen_record &r)
{
	memset(&r, 0, sizeof(r));
	r.code = (uint32_t)n->base.code;

	switch (cir_payload_class_for(r.code)) {
	case CIR_PAYLOAD_SCALAR:
		// Raw union image: arena nodes are zero-initialized, so
		// the unused union tail is deterministically zero.
		memcpy(r.payload, &n->base.u, sizeof(r.payload));
		break;
	case CIR_PAYLOAD_STR: {
		const c2mir_str_t &s = n->base.u.s;
		if (s.len > 0xffffffffu)
			return false;	// record field is u32
		r.str_len = (uint32_t)s.len;
		if (s.s && s.len) {
			if (!TokenBase::_active_strpool) {
				fprintf(stderr, "madc internal: cir freeze"
					" with no active string pool\n");
				return false;
			}
			r.str_id = TokenBase::_active_strpool->intern(
				s.s, (uint32_t)s.len);
		}
		break;
	}
	case CIR_PAYLOAD_NONE:
		break;	// children are the caller's (partition-dependent)
	}

	// B1 extension block — already position-independent.
	r.origin_id            = n->origin_id;
	r.datadef_id           = n->datadef_id;
	r.typedef_name_id      = n->typedef_name_id;
	r.error_msg_id         = n->error_msg_id;
	r.tsubst_pack_index    = n->tsubst_pack_index;
	r.tsubst_pack_value_id = n->tsubst_pack_value_id;
	r.tree1_origin         = n->tree1_origin;
	r.src_lang             = (uint8_t)n->src_lang;
	r.flags                = (n->synth_from_origin ? CIR_FROZEN_SYNTH_FROM_ORIGIN : 0)
			       | (n->tsubst_pack_expand ? CIR_FROZEN_PACK_EXPAND : 0);
	return true;
}

// The ONE freeze walk (B2 single-blob and B3 forest are the same pass with
// different partitioning). Pass 1 discovers the reachable sub-DAG assigning
// dense (unit, record) at FIRST touch (share/cycle-safe — the dump-walker
// discipline; iterative, real trees exceed 800 deep); a node's unit is its
// origin token's source file, an origin-less node inherits its discovering
// parent's unit, the root falls back to the main unit. Pass 2 emits the
// records + per-unit CSR child pools; a child in another unit becomes a
// connector-pool entry. Single-unit mode (by_unit=false) reproduces the B2
// format exactly: one unit, no connectors, no positions, no unit names.
struct cir_freeze_loc { uint32_t unit, idx; };

static bool cir_freeze_partitioned(cir_node *root, const char *main_unit_name,
				   bool by_unit, cir_frozen_forest &out)
{
	if (!root)
		return false;
	out.units.clear();
	out.libs.clear();

	std::map<cir_node *, cir_freeze_loc> where;
	std::vector<std::vector<cir_node *> > order;
	std::map<std::string, uint32_t> unit_by_name;
	std::vector<std::string> unit_names;

	// Unit for a source path (by_unit mode), creating it on first sight.
	// Unit 0 is always the main unit so readers see a stable convention.
	auto unit_for = [&](const char *fname) -> uint32_t {
		std::string key(fname);
		std::map<std::string, uint32_t>::iterator it = unit_by_name.find(key);
		if (it != unit_by_name.end())
			return it->second;
		uint32_t u = (uint32_t)order.size();
		unit_by_name[key] = u;
		unit_names.push_back(key);
		order.push_back(std::vector<cir_node *>());
		return u;
	};

	if (by_unit)
		unit_for(main_unit_name);	// reserve unit 0 = main
	else
		order.push_back(std::vector<cir_node *>());

	// Assign (unit, idx) at first touch and enqueue.
	auto place = [&](cir_node *n, uint32_t unit) -> bool {
		if (order[unit].size() & CIR_FROZEN_CHILD_CONNECTOR_BIT)
			return false;	// 2^31 records in one unit: out of format
		cir_freeze_loc loc;
		loc.unit = unit;
		loc.idx = (uint32_t)order[unit].size();
		where[n] = loc;
		order[unit].push_back(n);
		return true;
	};

	// Pass 1: discovery.
	{
		const char *rf = by_unit ? root->src_file() : NULL;
		uint32_t ru = by_unit ? unit_for(rf ? rf : main_unit_name) : 0;
		if (!place(root, ru))
			return false;
	}
	std::vector<cir_node *> stack;
	stack.push_back(root);
	while (!stack.empty()) {
		cir_node *n = stack.back();
		stack.pop_back();
		uint32_t nu = where[n].unit;
		for (node_t op = c2mir_node_first_op(n->as_node()); op;
		     op = c2mir_node_next_op(op)) {
			cir_node *c = CIR_NODE(op);
			if (where.count(c))
				continue;
			uint32_t cu = nu;
			if (by_unit) {
				const char *cf = c->src_file();
				if (cf)
					cu = unit_for(cf);
			}
			if (!place(c, cu))
				return false;
			stack.push_back(c);
		}
	}

	// Pass 2: emit records + child pools (+ connectors/positions by_unit).
	out.units.resize(order.size());
	out.root_unit = where[root].unit;
	out.root_idx  = where[root].idx;
	for (uint32_t u = 0; u < order.size(); ++u) {
		cir_forest_unit &fu = out.units[u];
		fu.unit_name_id = by_unit
			? TokenBase::_active_strpool->intern(unit_names[u])
			: 0;
		fu.blob.records.resize(order[u].size());
		if (by_unit)
			fu.positions.resize(order[u].size());
		std::map<uint64_t, uint32_t> conn_ix;	// packed target -> pool idx

		for (size_t i = 0; i < order[u].size(); ++i) {
			cir_node *n = order[u][i];
			cir_frozen_record &r = fu.blob.records[i];
			if (!cir_fill_record(n, r))
				return false;

			if (cir_payload_class_for(r.code) == CIR_PAYLOAD_NONE) {
				r.child_base = fu.blob.children.size();
				for (node_t op = c2mir_node_first_op(n->as_node()); op;
				     op = c2mir_node_next_op(op)) {
					cir_freeze_loc cl = where[CIR_NODE(op)];
					uint32_t entry;
					if (cl.unit == u) {
						entry = cl.idx;
					} else {
						uint64_t key = ((uint64_t)cl.unit << 32)
							     | cl.idx;
						std::map<uint64_t, uint32_t>::iterator ci =
							conn_ix.find(key);
						uint32_t cix;
						if (ci != conn_ix.end()) {
							cix = ci->second;
						} else {
							cix = (uint32_t)fu.connectors.size();
							if (cix & CIR_FROZEN_CHILD_CONNECTOR_BIT)
								return false;
							fu.connectors.push_back(key);
							conn_ix[key] = cix;
						}
						entry = CIR_FROZEN_CHILD_CONNECTOR_BIT | cix;
					}
					fu.blob.children.push_back(entry);
					++r.nchildren;
				}
			}

			if (by_unit) {
				cir_frozen_pos &p = fu.positions[i];
				p.fname_id = p.line = p.col = 0;
				if (n->origin_id) {
					TokenBase *tb = madc_token_for_slot(n->origin_id);
					if (tb && tb->file) {
						p.fname_id = TokenBase::_active_strpool
							->intern(tb->file);
						p.line = (uint32_t)tb->line;
						p.col  = (uint32_t)tb->column;
					}
				}
			}
		}
	}

	// Index every N_FUNC_DEF's emit symbol -> its (unit, idx). A method's INLINE
	// body is the func-def whose declarator ID equals the method's mangled symbol;
	// recording the location lets cir_forest_append_methods flag the method as
	// body-bearing so bind copies the saved Tree-1 body into the consumer's Tree-2
	// on use. (A symbol absent here is a LIBRARY method — body in a .so, no def.)
	for (std::map<cir_node *, cir_freeze_loc>::iterator it = where.begin();
	     it != where.end(); ++it) {
		node_t fn = it->first->as_node();
		if (fn->code != N_FUNC_DEF)
			continue;
		node_t o = c2mir_node_first_op(fn);		// op0: return spec list
		node_t decl = o ? c2mir_node_next_op(o) : NULL;	// op1: N_DECL
		if (!decl || decl->code != N_DECL)
			continue;
		node_t id = c2mir_node_first_op(decl);		// N_DECL op0: N_ID/N_IGNORE
		if (!id || id->code != N_ID || !id->u.s.s)
			continue;
		out.funcdef_locs[id->u.s.s] =
			std::make_pair(it->second.unit, it->second.idx);
	}
	return true;
}

bool cir_freeze_subtree(cir_node *root, cir_frozen_blob &out)
{
	cir_frozen_forest f;
	if (!cir_freeze_partitioned(root, NULL, false, f))
		return false;
	out = std::move(f.units[0].blob);
	return true;
}

bool cir_freeze_forest(cir_node *root, const char *main_unit_name,
		       cir_frozen_forest &out)
{
	if (!TokenBase::_active_strpool) {
		fprintf(stderr, "madc internal: cir forest freeze with no"
			" active string pool\n");
		return false;
	}
	return cir_freeze_partitioned(root, main_unit_name ? main_unit_name
							   : "<main>",
				      true, out);
}

// ---------------------------------------------------------------------------
// Container glue
// ---------------------------------------------------------------------------

// Zero-length payloads take codec None (nothing to compress); the writer
// still needs a non-NULL byte pointer.
static bool add_seg(madc::dis::snapshot_writer &w, uint32_t seg_id,
		    uint32_t kind, const void *bytes, size_t len,
		    PchCompression codec)
{
	return w.add_segment(seg_id, kind, len ? bytes : (const void *)"",
			     len, len ? codec : PchCompression::None);
}

bool cir_freeze_write(const cir_frozen_blob &blob,
		      madc::dis::snapshot_writer &w, uint32_t seg_id_base,
		      PchCompression codec)
{
	if (blob.records.empty())
		return false;
	if (!add_seg(w, seg_id_base + 0, SNAP_KIND_CIR_RECORDS,
		     blob.records.data(),
		     blob.records.size() * sizeof(cir_frozen_record), codec))
		return false;
	return add_seg(w, seg_id_base + 1, SNAP_KIND_CIR_CHILDREN,
		       blob.children.data(),
		       blob.children.size() * sizeof(uint32_t), codec);
}

bool cir_freeze_read(const madc::dis::snapshot_reader &r, uint32_t seg_id_base,
		     cir_frozen_blob &out)
{
	const madc::dis::snapshot_segment *recs = r.find(seg_id_base + 0);
	const madc::dis::snapshot_segment *kids = r.find(seg_id_base + 1);
	if (!recs || !kids)
		return false;
	if (recs->kind != SNAP_KIND_CIR_RECORDS || kids->kind != SNAP_KIND_CIR_CHILDREN)
		return false;
	std::vector<uint8_t> rbytes, kbytes;
	if (!r.read_segment(*recs, rbytes) || !r.read_segment(*kids, kbytes))
		return false;
	if (rbytes.size() % sizeof(cir_frozen_record) || kbytes.size() % sizeof(uint32_t))
		return false;
	out.records.resize(rbytes.size() / sizeof(cir_frozen_record));
	out.children.resize(kbytes.size() / sizeof(uint32_t));
	if (!rbytes.empty())
		memcpy(out.records.data(), rbytes.data(), rbytes.size());
	if (!kbytes.empty())
		memcpy(out.children.data(), kbytes.data(), kbytes.size());
	// Bounds validation: every child ref and child range must be in-blob,
	// so a corrupt container fails HERE, not as a wild read at materialize.
	// Standalone blobs carry no connector pool, so the connector form is
	// out of format here (the forest reader below accepts it).
	for (size_t i = 0; i < out.records.size(); ++i) {
		const cir_frozen_record &rec = out.records[i];
		if (rec.child_base + rec.nchildren > out.children.size())
			return false;
		for (uint32_t k = 0; k < rec.nchildren; ++k) {
			uint32_t ci = out.children[rec.child_base + k];
			if ((ci & CIR_FROZEN_CHILD_CONNECTOR_BIT)
			    || ci >= out.records.size())
				return false;
		}
	}
	return true;
}

bool cir_forest_write(const cir_frozen_forest &f, madc::dis::snapshot_writer &w,
		      PchCompression codec)
{
	madc::dis::intern_table *pool = TokenBase::_active_strpool;
	if (f.units.empty() || !pool)
		return false;

	// Everything below interns BEFORE the pool blocks are staged, so the
	// serialized pool contains every handle the container references.
	std::vector<uint32_t> lib_ids;
	for (size_t i = 0; i < f.libs.size(); ++i)
		lib_ids.push_back(pool->intern(f.libs[i]));

	// Phase 6: the complete type-table serialization. f.type_records holds the
	// FULL-content records (structs / typedefs) built by
	// cir_forest_fill_type_records (madc_cir.cpp) — each DataDef's content with
	// pointer fields as ids, swizzled back on load. Here we ALSO emit a name-only
	// CIR_TYPEK_OTHER record for every OTHER named project type (classes + derived
	// types not yet fully serialized) so the typeid->name closure (type_name_for)
	// stays complete — the old typeid->name behavior, now unified into the one
	// record stream and the widening path (OTHER -> full as kinds are added).
	std::vector<cir_forest_type_record> type_recs = f.type_records;
	{
		std::set<uint32_t> have;
		for (size_t i = 0; i < type_recs.size(); ++i)
			if (type_recs[i].type_id)
				have.insert(type_recs[i].type_id);
		if (madc_active_project_types) {
			uint32_t base = madc_active_project_types->base();
			for (uint32_t i = 0;
			     i < (uint32_t)madc_active_project_types->size(); ++i) {
				uint32_t tid = base + i;
				if (have.count(tid))
					continue;
				DataDef *dd = madc_active_project_types->get(tid);
				if (!dd || dd->name.empty())
					continue;
				cir_forest_type_record r;
				memset(&r, 0, sizeof(r));
				r.type_id = tid;
				r.kind    = CIR_TYPEK_OTHER;
				r.name_id = pool->intern(dd->name);
				type_recs.push_back(r);
			}
		}
	}

	// Directory payload: header + unit table + lib name ids.
	cir_forest_dir_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.version    = CIR_FOREST_FORMAT_VERSION;
	hdr.unit_count = (uint32_t)f.units.size();
	hdr.root_unit  = f.root_unit;
	hdr.root_idx   = f.root_idx;
	hdr.lib_count  = (uint32_t)f.libs.size();
	std::vector<uint8_t> dir(sizeof(hdr));
	memcpy(dir.data(), &hdr, sizeof(hdr));
	for (size_t u = 0; u < f.units.size(); ++u) {
		const cir_forest_unit &fu = f.units[u];
		cir_forest_dir_unit du;
		du.unit_name_id    = fu.unit_name_id;
		du.record_count    = (uint32_t)fu.blob.records.size();
		du.connector_count = (uint32_t)fu.connectors.size();
		// v2: anchor = decl-index entry count when the unit carries a
		// grove payload; ANCHOR_NONE marks a module-only unit.
		du.anchor_idx      = fu.token_payload.empty()
				   ? CIR_FOREST_ANCHOR_NONE
				   : (uint32_t)fu.decl_index.size();
		size_t off = dir.size();
		dir.resize(off + sizeof(du));
		memcpy(dir.data() + off, &du, sizeof(du));
	}
	if (!lib_ids.empty()) {
		size_t off = dir.size();
		dir.resize(off + lib_ids.size() * sizeof(uint32_t));
		memcpy(dir.data() + off, lib_ids.data(),
		       lib_ids.size() * sizeof(uint32_t));
	}

	w.set_context_hash(madc_cir_context_hash());
	if (!add_seg(w, CIR_FOREST_SEG_DIR, SNAP_KIND_CIR_FOREST_DIR,
		     dir.data(), dir.size(), codec))
		return false;
	if (!add_seg(w, CIR_FOREST_SEG_STR_BYTES, madc::dis::SNAP_KIND_INTERN_BYTES,
		     pool->bytes_data(), pool->bytes_size(), codec)
	    || !add_seg(w, CIR_FOREST_SEG_STR_ENTRIES, madc::dis::SNAP_KIND_INTERN_ENTRIES,
			pool->entries_data(),
			pool->entries_size() * sizeof(madc::dis::intern_table::Entry), codec)
	    || !add_seg(w, CIR_FOREST_SEG_STR_BUCKETS, madc::dis::SNAP_KIND_INTERN_BUCKETS,
			pool->buckets_data(),
			pool->buckets_size() * sizeof(uint32_t), codec))
		return false;
	// v6 container-global: the complete type-table serialization (Phase 6) —
	// one record per project/system DataDef (full content, pointer fields as
	// ids) plus the member/base payload stream, swizzled to DataDef* on load.
	if (!add_seg(w, CIR_FOREST_SEG_TYPE_RECORDS, SNAP_KIND_CIR_TYPE_RECORDS,
		     type_recs.data(),
		     type_recs.size() * sizeof(cir_forest_type_record), codec)
	    || !add_seg(w, CIR_FOREST_SEG_TYPE_PAYLOAD, SNAP_KIND_CIR_TYPE_PAYLOAD,
			f.type_payload.data(),
			f.type_payload.size() * sizeof(uint32_t), codec))
		return false;
	// v13 container-global: file-scope global VARIABLE definitions (zero-length
	// when the freeze recorded none).
	if (!add_seg(w, CIR_FOREST_SEG_GLOBALS, SNAP_KIND_CIR_GLOBALS,
		     f.globals.data(),
		     f.globals.size() * sizeof(cir_forest_global_record), codec))
		return false;
	// v2 container-global payloads (zero-length when the freeze recorded
	// nothing — a module-only freeze).
	if (!add_seg(w, CIR_FOREST_SEG_BRANCH_MACROS, SNAP_KIND_CIR_BRANCH_MACROS,
		     f.branch_macros.data(),
		     f.branch_macros.size() * sizeof(uint32_t), codec)
	    || !add_seg(w, CIR_FOREST_SEG_CANON_ORDER, SNAP_KIND_CIR_CANON_ORDER,
			f.canon_order.data(),
			f.canon_order.size() * sizeof(uint32_t), codec))
		return false;

	for (size_t u = 0; u < f.units.size(); ++u) {
		const cir_forest_unit &fu = f.units[u];
		uint32_t base = CIR_FOREST_SEG_UNIT_BASE
			      + (uint32_t)u * CIR_FOREST_SEGS_PER_UNIT;
		// v2: a unit may be token-only (a macro-only header), or even
		// empty of both records and tokens (a PP-export-only carrier,
		// or an edge target whose tokens live under another display
		// name) — all are legal directory entries; connectors never
		// reference an empty unit and its anchor writes as NONE.
		if (!add_seg(w, base + 0, SNAP_KIND_CIR_RECORDS,
			     fu.blob.records.data(),
			     fu.blob.records.size() * sizeof(cir_frozen_record), codec)
		    || !add_seg(w, base + 1, SNAP_KIND_CIR_CHILDREN,
				fu.blob.children.data(),
				fu.blob.children.size() * sizeof(uint32_t), codec)
		    || !add_seg(w, base + 2, SNAP_KIND_CIR_CONNECTORS,
				fu.connectors.data(),
				fu.connectors.size() * sizeof(uint64_t), codec)
		    || !add_seg(w, base + 3, SNAP_KIND_CIR_POSITIONS,
				fu.positions.data(),
				fu.positions.size() * sizeof(cir_frozen_pos), codec))
			return false;
		// v2 grove payload slots. Token slice = u32 token count, then
		// the .madh record bytes.
		std::vector<uint8_t> toks;
		if (!fu.token_payload.empty()) {
			toks.resize(sizeof(uint32_t) + fu.token_payload.size());
			memcpy(toks.data(), &fu.token_count, sizeof(uint32_t));
			memcpy(toks.data() + sizeof(uint32_t),
			       fu.token_payload.data(), fu.token_payload.size());
		}
		if (!add_seg(w, base + 4, SNAP_KIND_CIR_UNIT_TOKENS,
			     toks.data(), toks.size(), codec)
		    || !add_seg(w, base + 5, SNAP_KIND_CIR_DECL_INDEX,
				fu.decl_index.data(),
				fu.decl_index.size() * sizeof(cir_forest_decl_entry), codec)
		    || !add_seg(w, base + 6, SNAP_KIND_CIR_PP_EXPORTS,
				fu.pp_events.data(),
				fu.pp_events.size() * sizeof(uint32_t), codec)
		    || !add_seg(w, base + 7, SNAP_KIND_CIR_UNIT_EDGES,
				fu.edges.data(),
				fu.edges.size() * sizeof(uint32_t), codec))
			return false;
	}
	return true;
}

bool cir_forest_map_image(const char *path, const void *&image, size_t &len)
{
	char selfbuf[4096];
	if (!path) {
		ssize_t n = readlink("/proc/self/exe", selfbuf, sizeof(selfbuf) - 1);
		if (n <= 0)
			return false;
		selfbuf[n] = '\0';
		path = selfbuf;
	}
	int fd = ::open(path, O_RDONLY);
	if (fd < 0)
		return false;
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return false;
	}
	void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (m == (void *)-1)	// the real MAP_FAILED (see #undef above)
		return false;
	// Deliberately never munmap'd: thawed segments read string bytes and
	// pool blocks from the mapping for the process lifetime.
	image = m;
	len = (size_t)st.st_size;
	return true;
}

// ---------------------------------------------------------------------------
// Thaw: CirFrozenSegment
// ---------------------------------------------------------------------------

CirFrozenSegment::CirFrozenSegment(cir_frozen_blob &&blob, c2m_ctx_t c2m)
	: _blob(std::move(blob)), _forest(NULL), _c2m(c2m)
{
	_mat.assign(_blob.records.size(), (cir_node *)NULL);
	_seg = madc_cir_register_segment(this);
}

CirFrozenSegment::CirFrozenSegment(cir_forest_unit &&unit, CirFrozenForest *forest,
				   c2m_ctx_t c2m)
	: _blob(std::move(unit.blob)), _connectors(std::move(unit.connectors)),
	  _positions(std::move(unit.positions)), _forest(forest), _c2m(c2m)
{
	_mat.assign(_blob.records.size(), (cir_node *)NULL);
	_seg = madc_cir_register_segment(this);
}

CirFrozenSegment::~CirFrozenSegment()
{
	madc_cir_unregister_segment(_seg);
}

size_t CirFrozenSegment::materialized_count() const
{
	size_t n = 0;
	for (size_t i = 0; i < _mat.size(); ++i)
		if (_mat[i])
			++n;
	return n;
}

// Phase A: materialize ONE record as a childless node shell — code, uid,
// payload, extension block, self ref, source position — memoized before any
// child work so shares and cycles terminate. Mirrors CirBuilder::make().
// Forest units resolve strings/positions against the CONTAINER's closure
// (its own pool + position side-car) and re-intern extension string ids
// into the live pool — in-process that dedups back to the identical id;
// cross-process it yields the fresh valid id.
cir_node *CirFrozenSegment::shell(uint32_t idx)
{
	const cir_frozen_record &r = _blob.records[idx];
	_nodes.emplace_back();
	cir_node *cn = &_nodes.back();
	memset(cn, 0, sizeof(cir_node));
	cn->base.code = (node_code_t)r.code;
	cn->base.uid = c2mir_next_uid(_c2m);
	c2mir_init_node_ops(cn->as_node());

	switch (cir_payload_class_for(r.code)) {
	case CIR_PAYLOAD_SCALAR:
		memcpy(&cn->base.u, r.payload, sizeof(r.payload));
		break;
	case CIR_PAYLOAD_STR: {
		const char *s = NULL;
		if (r.str_id) {
			if (_forest) {
				uint32_t plen = 0;
				s = _forest->pool_cstr(r.str_id, plen);
			} else if (TokenBase::_active_strpool) {
				s = TokenBase::_active_strpool->c_str(r.str_id);
			}
		}
		if (s) {
			cn->base.u.s.s = c2mir_uniq_str(_c2m, s, r.str_len);
			cn->base.u.s.len = r.str_len;
		}
		break;
	}
	case CIR_PAYLOAD_NONE:
		break;	// children appended in phase B
	}

	cn->origin_id            = r.origin_id;
	cn->datadef_id           = r.datadef_id;
	cn->tsubst_pack_index    = r.tsubst_pack_index;
	if (_forest) {
		cn->typedef_name_id      = _forest->live_str_id(r.typedef_name_id);
		cn->error_msg_id         = _forest->live_str_id(r.error_msg_id);
		cn->tsubst_pack_value_id = _forest->live_str_id(r.tsubst_pack_value_id);
	} else {
		cn->typedef_name_id      = r.typedef_name_id;
		cn->error_msg_id         = r.error_msg_id;
		cn->tsubst_pack_value_id = r.tsubst_pack_value_id;
	}
	cn->tree1_origin         = r.tree1_origin;
	cn->src_lang             = (CirSourceLang)r.src_lang;
	cn->synth_from_origin    = (r.flags & CIR_FROZEN_SYNTH_FROM_ORIGIN) != 0;
	cn->tsubst_pack_expand   = (r.flags & CIR_FROZEN_PACK_EXPAND) != 0;
	cn->self.seg = _seg;
	cn->self.idx = idx;

	// Position: forest units read the side-car (valid without the freezing
	// process's token arena); standalone segments derive it from the origin
	// token exactly as CirBuilder::make does for a live build.
	if (_forest) {
		const cir_frozen_pos &p = _positions[idx];
		if (p.fname_id) {
			uint32_t flen = 0;
			const char *fn = _forest->pool_cstr(p.fname_id, flen);
			if (fn) {
				c2mir_pos_t pos = { c2mir_uniq_str(_c2m, fn, flen),
						    (int)p.line, (int)p.col };
				c2mir_set_node_pos(_c2m, cn->as_node(), pos);
			}
		}
	} else if (r.origin_id) {
		TokenBase *tb = madc_token_for_slot(r.origin_id);
		if (tb) {
			c2mir_pos_t pos = { tb->file, tb->line, tb->column };
			c2mir_set_node_pos(_c2m, cn->as_node(), pos);
		}
	}

	_mat[idx] = cn;
	return cn;
}

// Resolve a child entry to its owning segment + record index. A connector
// entry crossing into a not-yet-loaded unit triggers that unit's
// decompress+register here (forest SETTLED #6: load-on-demand falls out of
// reference resolution).
static bool cir_child_target(CirFrozenSegment *s, uint32_t entry,
			     CirFrozenSegment *&cs, uint32_t &cidx)
{
	if (!(entry & CIR_FROZEN_CHILD_CONNECTOR_BIT)) {
		cs = s;
		cidx = entry;
		return true;
	}
	CirFrozenForest *forest = s->forest();
	if (!forest)
		return false;	// standalone blobs have no connector pool
	uint64_t conn = s->connector(entry & ~CIR_FROZEN_CHILD_CONNECTOR_BIT);
	cs = forest->unit_segment((uint32_t)(conn >> 32));
	cidx = (uint32_t)conn;
	return cs != NULL && cidx < cs->record_count();
}

cir_node *CirFrozenSegment::resolve(CirFrozenSegment *s0, uint32_t idx0)
{
	if (idx0 >= s0->_blob.records.size())
		return NULL;
	if (s0->_mat[idx0])
		return s0->_mat[idx0];

	// Phase A: shells for every cold record reachable from (s0, idx0) —
	// one iterative worklist ACROSS units (a connector hop is a worklist
	// step, not a recursion; memoized shells terminate shares and cycles,
	// including cycles that cross units).
	std::vector<std::pair<CirFrozenSegment *, uint32_t> > created;
	s0->shell(idx0);
	created.push_back(std::make_pair(s0, idx0));
	for (size_t w = 0; w < created.size(); ++w) {
		CirFrozenSegment *s = created[w].first;
		const cir_frozen_record &r = s->_blob.records[created[w].second];
		for (uint32_t k = 0; k < r.nchildren; ++k) {
			CirFrozenSegment *cs = NULL;
			uint32_t cidx = 0;
			if (!cir_child_target(s, s->_blob.children[r.child_base + k],
					      cs, cidx))
				return NULL;
			if (!cs->_mat[cidx]) {
				cs->shell(cidx);
				created.push_back(std::make_pair(cs, cidx));
			}
		}
	}

	// Phase B: append children for the records created THIS call only —
	// previously-materialized nodes already own their child lists. A
	// shared child appended under several parents reproduces the live
	// builder's last-appender-owns-links DLIST state (arity-1 N_SHARE
	// parents read head-only, so every parent still sees its child).
	for (size_t w = 0; w < created.size(); ++w) {
		CirFrozenSegment *s = created[w].first;
		const cir_frozen_record &r = s->_blob.records[created[w].second];
		cir_node *parent = s->_mat[created[w].second];
		for (uint32_t k = 0; k < r.nchildren; ++k) {
			CirFrozenSegment *cs = NULL;
			uint32_t cidx = 0;
			cir_child_target(s, s->_blob.children[r.child_base + k],
					 cs, cidx);	// memoized: cannot fail now
			c2mir_op_append(s->_c2m, parent->as_node(),
					cs->_mat[cidx]->as_node());
		}
	}

	return s0->_mat[idx0];
}

cir_node *CirFrozenSegment::node_at(uint32_t idx)
{
	return resolve(this, idx);
}

// ---------------------------------------------------------------------------
// Thaw: CirFrozenForest
// ---------------------------------------------------------------------------

CirFrozenForest::CirFrozenForest()
	: _c2m(NULL), _root_unit(0), _root_idx(0), _types_materialized(false)
{
}

CirFrozenForest::~CirFrozenForest()
{
	for (size_t u = 0; u < _segs.size(); ++u)
		delete _segs[u];
	// Free the DataDef objects materialized from the type table (forest-owned;
	// the parser's symbol tables held non-owning pointers into these).
	for (size_t i = 0; i < _mat_storage.size(); ++i)
		delete _mat_storage[i];
	// Free the reconstructed method Variables (forest-owned; the classes'
	// method_map/methods held non-owning pointers).
	for (size_t i = 0; i < _mat_vars.size(); ++i)
		delete _mat_vars[i];
}

// Bind one pool block: in place from the image when stored uncompressed
// (the zero-copy path the per-segment codec field exists for), otherwise
// decompressed into `own`.
static const uint8_t *forest_pool_block(const madc::dis::snapshot_reader &r,
					uint32_t seg_id, uint32_t kind,
					std::vector<uint8_t> &own, size_t &len)
{
	const madc::dis::snapshot_segment *s = r.find(seg_id);
	if (!s || s->kind != kind)
		return NULL;
	len = (size_t)s->raw_size;
	if (const uint8_t *p = r.raw_ptr(*s))
		return p;
	if (!r.read_segment(*s, own))
		return NULL;
	return own.data();
}

bool CirFrozenForest::open(const void *image, size_t len, c2m_ctx_t c2m)
{
	_c2m = c2m;
	if (!TokenBase::_active_strpool) {
		fprintf(stderr, "madc: forest thaw requires a live string pool\n");
		return false;
	}
	if (!_reader.open(image, len)) {
		fprintf(stderr, "madc: no forest container found\n");
		return false;
	}
	if (_reader.context_hash() != madc_cir_context_hash()) {
		fprintf(stderr, "madc: frozen forest was built by a different"
			" madc (context-hash mismatch) — re-freeze it\n");
		return false;
	}

	// Directory.
	const madc::dis::snapshot_segment *ds = _reader.find(CIR_FOREST_SEG_DIR);
	std::vector<uint8_t> dir;
	if (!ds || ds->kind != SNAP_KIND_CIR_FOREST_DIR
	    || !_reader.read_segment(*ds, dir)
	    || dir.size() < sizeof(cir_forest_dir_header)) {
		fprintf(stderr, "madc: forest directory missing or corrupt\n");
		return false;
	}
	cir_forest_dir_header hdr;
	memcpy(&hdr, dir.data(), sizeof(hdr));
	if (hdr.version != CIR_FOREST_FORMAT_VERSION
	    || dir.size() != sizeof(hdr)
			      + (size_t)hdr.unit_count * sizeof(cir_forest_dir_unit)
			      + (size_t)hdr.lib_count * sizeof(uint32_t)
	    || hdr.unit_count == 0) {
		fprintf(stderr, "madc: forest directory malformed\n");
		return false;
	}
	_units.resize(hdr.unit_count);
	memcpy(_units.data(), dir.data() + sizeof(hdr),
	       hdr.unit_count * sizeof(cir_forest_dir_unit));

	// The container's own string pool (A1 frozen view).
	size_t nbytes = 0, nentries = 0, nbuckets = 0;
	const uint8_t *pb = forest_pool_block(_reader, CIR_FOREST_SEG_STR_BYTES,
					      madc::dis::SNAP_KIND_INTERN_BYTES,
					      _pool_bytes, nbytes);
	const uint8_t *pe = forest_pool_block(_reader, CIR_FOREST_SEG_STR_ENTRIES,
					      madc::dis::SNAP_KIND_INTERN_ENTRIES,
					      _pool_entries, nentries);
	const uint8_t *pk = forest_pool_block(_reader, CIR_FOREST_SEG_STR_BUCKETS,
					      madc::dis::SNAP_KIND_INTERN_BUCKETS,
					      _pool_buckets, nbuckets);
	if (!pk)
		nbuckets = 0;	// buckets are derivable; never bind NULL with a count
	if (!pb || !pe || nentries % sizeof(madc::dis::intern_table::Entry)
	    || (nbuckets % sizeof(uint32_t))) {
		fprintf(stderr, "madc: forest string pool missing or corrupt\n");
		return false;
	}
	_pool.bind((const char *)pb, nbytes,
		   (const madc::dis::intern_table::Entry *)pe,
		   nentries / sizeof(madc::dis::intern_table::Entry),
		   (const uint32_t *)pk, nbuckets / sizeof(uint32_t));
	if (!_pool.valid()) {
		fprintf(stderr, "madc: forest string pool failed validation\n");
		return false;
	}

	// Root sanity against the directory.
	_root_unit = hdr.root_unit;
	_root_idx  = hdr.root_idx;
	if (_root_unit >= hdr.unit_count
	    || _root_idx >= _units[_root_unit].record_count) {
		fprintf(stderr, "madc: forest root out of range\n");
		return false;
	}

	// Required libraries (the link-environment closure).
	const uint8_t *libp = dir.data() + sizeof(hdr)
			    + hdr.unit_count * sizeof(cir_forest_dir_unit);
	for (uint32_t i = 0; i < hdr.lib_count; ++i) {
		uint32_t id;
		memcpy(&id, libp + i * sizeof(uint32_t), sizeof(uint32_t));
		uint32_t slen = 0;
		const char *s = pool_cstr(id, slen);
		if (!s) {
			fprintf(stderr, "madc: forest library list corrupt\n");
			return false;
		}
		_libs.push_back(std::string(s, slen));
	}

	// v6 container-global: the complete type-table serialization (Phase 6).
	// Records load here; the type_name_for closure is DERIVED from them; the
	// DataDef objects materialize lazily at bind (materialize_types), never at
	// open — so --run-frozen (which never binds) pays nothing.
	if (const madc::dis::snapshot_segment *ts =
		_reader.find(CIR_FOREST_SEG_TYPE_RECORDS)) {
		std::vector<uint8_t> d;
		if (ts->kind != SNAP_KIND_CIR_TYPE_RECORDS
		    || !_reader.read_segment(*ts, d)
		    || d.size() % sizeof(cir_forest_type_record)) {
			fprintf(stderr, "madc: forest type records corrupt\n");
			return false;
		}
		_type_records.resize(d.size() / sizeof(cir_forest_type_record));
		if (!d.empty())
			memcpy(_type_records.data(), d.data(), d.size());
		// Derive the typeid -> name closure (type_name_for) from the records.
		for (size_t i = 0; i < _type_records.size(); ++i) {
			const cir_forest_type_record &r = _type_records[i];
			if (!r.type_id)
				continue;
			uint32_t slen = 0;
			if (const char *s = pool_cstr(r.name_id, slen))
				_type_names[r.type_id] = s;
		}
	}
	if (const madc::dis::snapshot_segment *ps =
		_reader.find(CIR_FOREST_SEG_TYPE_PAYLOAD)) {
		std::vector<uint8_t> d;
		if (ps->kind != SNAP_KIND_CIR_TYPE_PAYLOAD
		    || !_reader.read_segment(*ps, d)
		    || d.size() % sizeof(uint32_t)) {
			fprintf(stderr, "madc: forest type payload corrupt\n");
			return false;
		}
		_type_payload.resize(d.size() / sizeof(uint32_t));
		if (!d.empty())
			memcpy(_type_payload.data(), d.data(), d.size());
	}
	// v13 container-global: file-scope global VARIABLE definitions (zero-length
	// = none; materialize_types swizzles each type_id -> DataDef*).
	if (const madc::dis::snapshot_segment *gs =
		_reader.find(CIR_FOREST_SEG_GLOBALS)) {
		std::vector<uint8_t> d;
		if (gs->kind != SNAP_KIND_CIR_GLOBALS
		    || !_reader.read_segment(*gs, d)
		    || d.size() % sizeof(cir_forest_global_record)) {
			fprintf(stderr, "madc: forest global-var table corrupt\n");
			return false;
		}
		_globals.resize(d.size() / sizeof(cir_forest_global_record));
		if (!d.empty())
			memcpy(_globals.data(), d.data(), d.size());
	}

	// v2 container-global payloads (zero-length segments = empty).
	if (const madc::dis::snapshot_segment *bs =
		_reader.find(CIR_FOREST_SEG_BRANCH_MACROS)) {
		std::vector<uint8_t> b;
		if (bs->kind != SNAP_KIND_CIR_BRANCH_MACROS
		    || !_reader.read_segment(*bs, b) || b.size() % sizeof(uint32_t)) {
			fprintf(stderr, "madc: forest branch-macro set corrupt\n");
			return false;
		}
		_branch_macros.resize(b.size() / sizeof(uint32_t));
		if (!b.empty())
			memcpy(_branch_macros.data(), b.data(), b.size());
	}
	if (const madc::dis::snapshot_segment *cs =
		_reader.find(CIR_FOREST_SEG_CANON_ORDER)) {
		std::vector<uint8_t> c;
		if (cs->kind != SNAP_KIND_CIR_CANON_ORDER
		    || !_reader.read_segment(*cs, c) || c.size() % sizeof(uint32_t)) {
			fprintf(stderr, "madc: forest canonical-order table corrupt\n");
			return false;
		}
		_canon_order.resize(c.size() / sizeof(uint32_t));
		if (!c.empty())
			memcpy(_canon_order.data(), c.data(), c.size());
		for (size_t i = 0; i < _canon_order.size(); ++i)
			if (_canon_order[i] >= hdr.unit_count) {
				fprintf(stderr, "madc: forest canonical order out of range\n");
				return false;
			}
	}
	// Reverse directory: unit-name spelling -> index (Phase 6 bind lookup).
	// The writer dedups unit names, so a name maps to one unit; a stray
	// duplicate keeps the first (bind is order-insensitive for a conforming
	// closure).
	for (uint32_t u = 0; u < hdr.unit_count; ++u) {
		uint32_t slen = 0;
		const char *nm = pool_cstr(_units[u].unit_name_id, slen);
		if (nm)
			_unit_by_name.emplace(std::string(nm, slen), u);
	}

	_segs.assign(hdr.unit_count, (CirFrozenSegment *)NULL);
	return true;
}

int CirFrozenForest::find_unit(const std::string &name) const
{
	std::map<std::string, uint32_t>::const_iterator it =
		_unit_by_name.find(name);
	return it == _unit_by_name.end() ? -1 : (int)it->second;
}

// --- grove payload v2 readers (B4a) ----------------------------------------

bool CirFrozenForest::read_unit_seg(uint32_t unit, uint32_t slot, uint32_t kind,
				    std::vector<uint8_t> &out) const
{
	if (unit >= _units.size())
		return false;
	uint32_t base = CIR_FOREST_SEG_UNIT_BASE + unit * CIR_FOREST_SEGS_PER_UNIT;
	const madc::dis::snapshot_segment *s = _reader.find(base + slot);
	if (!s || s->kind != kind || !s->raw_size)
		return false;
	return _reader.read_segment(*s, out);
}

uint32_t CirFrozenForest::unit_anchor(uint32_t unit) const
{
	return unit < _units.size() ? _units[unit].anchor_idx
				    : CIR_FOREST_ANCHOR_NONE;
}

bool CirFrozenForest::unit_tokens(uint32_t unit, std::vector<uint8_t> &madh_payload,
				  uint32_t &token_count)
{
	std::vector<uint8_t> raw;
	if (!read_unit_seg(unit, 4, SNAP_KIND_CIR_UNIT_TOKENS, raw)
	    || raw.size() < sizeof(uint32_t))
		return false;
	memcpy(&token_count, raw.data(), sizeof(uint32_t));
	madh_payload.assign(raw.begin() + sizeof(uint32_t), raw.end());
	return true;
}

bool CirFrozenForest::unit_decl_index(uint32_t unit,
				      std::vector<cir_forest_decl_entry> &out)
{
	std::vector<uint8_t> raw;
	if (!read_unit_seg(unit, 5, SNAP_KIND_CIR_DECL_INDEX, raw)
	    || raw.size() % sizeof(cir_forest_decl_entry))
		return false;
	out.resize(raw.size() / sizeof(cir_forest_decl_entry));
	if (!raw.empty())
		memcpy(out.data(), raw.data(), raw.size());
	return true;
}

bool CirFrozenForest::unit_pp_events(uint32_t unit, std::vector<uint32_t> &out)
{
	std::vector<uint8_t> raw;
	if (!read_unit_seg(unit, 6, SNAP_KIND_CIR_PP_EXPORTS, raw)
	    || raw.size() % sizeof(uint32_t))
		return false;
	out.resize(raw.size() / sizeof(uint32_t));
	if (!raw.empty())
		memcpy(out.data(), raw.data(), raw.size());
	return true;
}

bool CirFrozenForest::unit_edges(uint32_t unit, std::vector<uint32_t> &out)
{
	std::vector<uint8_t> raw;
	if (!read_unit_seg(unit, 7, SNAP_KIND_CIR_UNIT_EDGES, raw)
	    || raw.size() % sizeof(uint32_t))
		return false;
	out.resize(raw.size() / sizeof(uint32_t));
	if (!raw.empty())
		memcpy(out.data(), raw.data(), raw.size());
	return true;
}

const char *CirFrozenForest::pool_cstr(uint32_t id, uint32_t &len) const
{
	if (id > _pool.count())
		return NULL;
	len = _pool.length(id);
	return _pool.c_str(id);
}

uint32_t CirFrozenForest::live_str_id(uint32_t frozen_id)
{
	if (!frozen_id)
		return 0;
	std::map<uint32_t, uint32_t>::iterator it = _live_ids.find(frozen_id);
	if (it != _live_ids.end())
		return it->second;
	uint32_t slen = 0;
	const char *s = pool_cstr(frozen_id, slen);
	uint32_t live = s ? TokenBase::_active_strpool->intern(s, slen) : 0;
	_live_ids[frozen_id] = live;
	return live;
}

const char *CirFrozenForest::unit_name(uint32_t unit) const
{
	if (unit >= _units.size())
		return NULL;
	uint32_t slen = 0;
	return pool_cstr(_units[unit].unit_name_id, slen);
}

// Swizzle a serialized typeid back to its DataDef* on load: a pinned primitive
// (id < MADC_TYPEID_PRIMITIVE_END) resolves process-invariantly via
// madc_type_from_id; any other id is a forest record, found in by_id (populated
// by materialize_types passes 1 / 1b). NULL if not (yet) resolvable. This is the
// one place the "primitive-or-by_id" lookup lives — DataDef-aware, so it stays on
// the madc side (not a madc::dis export), but shared across every member / base /
// method / typedef reference the way pod_read is shared across every POD read.
static DataDef *forest_swizzle_type(uint32_t tid,
				    const std::map<uint32_t, DataDef *> &by_id)
{
	if (tid < MADC_TYPEID_PRIMITIVE_END)
		return madc_type_from_id(tid);
	std::map<uint32_t, DataDef *>::const_iterator it = by_id.find(tid);
	return it != by_id.end() ? it->second : NULL;
}

const std::vector<CirRestoredType> &CirFrozenForest::materialize_types()
{
	if (_types_materialized)
		return _restored;
	_types_materialized = true;

	const size_t MSTRIDE = madc::dis::pod_words<cir_forest_type_member>();

	// Pass 1: allocate a DataDef object per struct/union record BEFORE filling
	// any, so member type ids (which may forward-reference a later record)
	// resolve in pass 2. Keyed by the record's freeze-time typeid.
	std::map<uint32_t, DataDef *> by_id;
	for (size_t i = 0; i < _type_records.size(); ++i) {
		const cir_forest_type_record &r = _type_records[i];
		if ((r.kind != CIR_TYPEK_STRUCT && r.kind != CIR_TYPEK_UNION
		     && r.kind != CIR_TYPEK_CLASS) || !r.type_id)
			continue;			// typedef: pass 3
		uint32_t nlen = 0;
		const char *nm = pool_cstr(r.name_id, nlen);
		if (!nm)
			continue;
		DataDefSTRUCT *sdd;
		if (r.kind == CIR_TYPEK_CLASS)
			sdd = new DataDefCLASS(std::string(nm, nlen), r.size,
					       DataType::dtRESERVED);
		else {
			sdd = new DataDefSTRUCT(std::string(nm, nlen), r.size);
			sdd->union_layout = (r.flags & CIR_TYPEF_UNION) != 0;
		}
		_mat_storage.push_back(sdd);
		by_id[r.type_id] = sdd;
	}

	// Pass 1b: reconstruct derived types (pointer / reference / const) into by_id.
	// A record's ref0 is its operand's freeze-time typeid — a primitive
	// (madc_type_from_id) or another record (by_id, populated by pass 1). A fixpoint
	// converges operand-before-derived, so a chain (T**, const T*) and a
	// self-referential pointer (Node *next — its operand aggregate is already
	// allocated in pass 1) both resolve. Same swizzle discipline as a member/base id.
	bool dprog = true;
	while (dprog) {
		dprog = false;
		for (size_t i = 0; i < _type_records.size(); ++i) {
			const cir_forest_type_record &r = _type_records[i];
			if (r.kind != CIR_TYPEK_POINTER && r.kind != CIR_TYPEK_REFERENCE
			    && r.kind != CIR_TYPEK_CONST)
				continue;
			if (!r.type_id || by_id.count(r.type_id))
				continue;
			DataDef *operand = forest_swizzle_type(r.ref0, by_id);
			if (!operand)
				continue;		// operand not ready this round (or ever)
			DataDef *d;
			if (r.kind == CIR_TYPEK_REFERENCE)
				d = new DataDefREF(*operand);
			else if (r.kind == CIR_TYPEK_CONST)
				d = new DataDefCONST(*operand);
			else
				d = new DataDefPTR(*operand);
			_mat_storage.push_back(d);
			by_id[r.type_id] = d;
			dprog = true;
		}
	}

	// Pass 2: fill each struct's members VERBATIM (offset / count / access /
	// origin / bitfield loaded as stored — no finalize, no re-derivation),
	// swizzling each member's type id -> DataDef* (primitive via
	// madc_type_from_id, forest aggregate via by_id).
	for (size_t i = 0; i < _type_records.size(); ++i) {
		const cir_forest_type_record &r = _type_records[i];
		if ((r.kind != CIR_TYPEK_STRUCT && r.kind != CIR_TYPEK_UNION
		     && r.kind != CIR_TYPEK_CLASS) || !r.type_id)
			continue;
		std::map<uint32_t, DataDef *>::iterator ai = by_id.find(r.type_id);
		if (ai == by_id.end())
			continue;
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(ai->second);
		if (!sdd)
			continue;
		bool ok = true;
		for (uint32_t m = 0; m < r.member_count; ++m) {
			size_t base = (size_t)r.member_begin + (size_t)m * MSTRIDE;
			cir_forest_type_member tm;
			if (!madc::dis::pod_read(_type_payload, base, tm)) { ok = false; break; }
			DataDef *mdd = forest_swizzle_type(tm.type_id, by_id);
			uint32_t mnlen = 0;
			const char *mnm = pool_cstr(tm.name_id, mnlen);
			if (!mdd || !mnm) { ok = false; break; }
			sdd->members.push_back(memberpair_t(std::string(mnm, mnlen), mdd));
			sdd->member_offsets.push_back(tm.offset);
			sdd->member_counts.push_back(tm.count ? tm.count : 1);
			sdd->member_array_flags.push_back(tm.count > 1);
			sdd->member_access.push_back(tm.access);
			sdd->member_origin.push_back((int)tm.origin);
			DataDefSTRUCT::BitFieldInfo bf;
			if (tm.bf_flags & 1u) {
				bf.is_bitfield     = true;
				bf.is_unsigned     = (tm.bf_flags & 2u) != 0;
				bf.reverse_storage = (tm.bf_flags & 4u) != 0;
				bf.bit_offset      = tm.bf_bit_offset;
				bf.bit_width       = tm.bf_bit_width;
				bf.storage_offset  = tm.bf_storage_offset;
				bf.storage_size    = tm.bf_storage_size;
			}
			sdd->member_bitfields.push_back(bf);
		}
		if (!ok)
			continue;		// unresolvable member -> bind cleanly lacks it
		sdd->size        = r.size;		// verbatim total size
		sdd->max_align   = r.align ? r.align : 1;
		sdd->is_complete = true;
		// Layout scalars VERBATIM (part of the aggregate's complete state).
		sdd->pack                   = r.pack;
		sdd->tag_explicit_align     = r.tag_align;
		sdd->is_anonymous           = (r.flags & CIR_TYPEF_ANON) != 0;
		sdd->reverse_scalar_storage = (r.flags & CIR_TYPEF_REVERSE) != 0;
		sdd->has_anon_aggregate     = (r.flags & CIR_TYPEF_HAS_ANONAGG) != 0;
		// Rebuild anonymous_aggregates VERBATIM: the group's flattened members
		// are already filled above; relink the grouping (first_member / count /
		// offset) and swizzle the nameless sub-aggregate's typeid to its restored
		// DataDefSTRUCT*, so emission re-nests the anon union/struct and c2mir
		// reproduces the overlap (never a flat sequential relayout).
		{
			const size_t ASTRIDE =
				madc::dis::pod_words<cir_forest_type_anon>();
			for (uint32_t ai = 0; ai < r.anon_count; ++ai) {
				size_t ab = (size_t)r.anon_begin + (size_t)ai * ASTRIDE;
				cir_forest_type_anon ta;
				if (!madc::dis::pod_read(_type_payload, ab, ta))
					break;
				DataDefSTRUCT *subs = dynamic_cast<DataDefSTRUCT *>(
					forest_swizzle_type(ta.aggregate_type_id, by_id));
				if (!subs)
					continue;	// sub-aggregate unresolved -> skip this group
				sdd->anonymous_aggregates.push_back(
					DataDefSTRUCT::AnonymousAggregateInfo(
						ta.first_member, ta.member_count,
						subs, ta.offset));
			}
		}
		sdd->type_id     = 0;			// re-stamp a fresh id in the consuming Program
		// CLASS extra state (Phase 6 3c): flags + bases VERBATIM. Each base
		// base_type_id swizzles to the DataDefCLASS* allocated in pass 1;
		// offset/access/is_primary load as stored (no compute_layout re-run).
		if (DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(sdd)) {
			cdd->class_align        = r.align ? r.align : 1;
			cdd->from_system_header = (r.flags & CIR_TYPEF_SYSHDR) != 0;
			cdd->has_user_ctor      = (r.flags & CIR_TYPEF_USER_CTOR) != 0;
			cdd->has_user_dtor      = (r.flags & CIR_TYPEF_USER_DTOR) != 0;
			cdd->has_vtable         = (r.flags & CIR_TYPEF_HAS_VTABLE) != 0;
			cdd->has_vptr_slot      = (r.flags & CIR_TYPEF_HAS_VPTR) != 0;
			const size_t BSTRIDE = madc::dis::pod_words<cir_forest_type_base>();
			bool bok = true;
			for (uint32_t b = 0; b < r.base_count; ++b) {
				size_t bb = (size_t)r.base_begin + (size_t)b * BSTRIDE;
				cir_forest_type_base tb;
				if (!madc::dis::pod_read(_type_payload, bb, tb)) { bok = false; break; }
				DataDefCLASS *bc = dynamic_cast<DataDefCLASS *>(
					forest_swizzle_type(tb.base_type_id, by_id));
				if (!bc) { bok = false; break; }
				BaseSpec bs;
				bs.base       = bc;
				bs.offset     = tb.offset;
				bs.is_virtual = (tb.flags & 1u) != 0;
				bs.access     = tb.access;
				bs.is_primary = (tb.flags & 2u) != 0;
				cdd->bases.push_back(bs);
			}
			if (!bok)
				continue;		// unresolvable base -> bind cleanly lacks it
			cdd->base_class = cdd->bases.empty() ? NULL : cdd->bases[0].base;

			// Methods (Phase 6 3d/v12): rebuild each non-virtual method DECLARATION
			// as a FuncDef + Variable and attach to method_map/methods, so a member
			// call resolves. The hidden __this (param 0 of a non-static method) is
			// rebuilt as a pointer to this class. Bodies are NOT here — an inline
			// method's body rides the grove (emitted by the producer), an external
			// method binds emit_symbol — so the FuncDef is declaration_only (the
			// consumer emits no body; the call links to the grove def / real symbol).
			// v12: a CIR_METHF_CTOR method ALSO joins cdd->ctors (so default/overload
			// construction resolves) and a CIR_METHF_DTOR method's display_id is the
			// "~" method_map key class_own_dtor scans for (its own display is empty);
			// a concrete ctor has no key (display_id 0) — no method_map entry.
			const size_t METHSTRIDE = madc::dis::pod_words<cir_forest_type_method>();
			for (uint32_t mi = 0; mi < r.method_count; ++mi) {
				size_t mb = (size_t)r.method_begin + (size_t)mi * METHSTRIDE;
				cir_forest_type_method tm;
				if (!madc::dis::pod_read(_type_payload, mb, tm))
					break;
				DataDef *ret = forest_swizzle_type(tm.ret_type_id, by_id);
				uint32_t mnl = 0, mdl = 0;
				const char *mnm = pool_cstr(tm.name_id, mnl);
				// display_id 0 => no method_map key (a concrete ctor); otherwise the
				// key (plain method / operator display, or the dtor's "~" tag).
				const char *mdp = tm.display_id ? pool_cstr(tm.display_id, mdl) : NULL;
				if (!ret || !mnm)
					continue;
				bool m_static = (tm.flags & CIR_METHF_STATIC) != 0;
				FuncDef *fd = new FuncDef(*ret);
				_mat_storage.push_back(fd);
				if (!m_static) {
					DataDefPTR *thisp = new DataDefPTR(*cdd);
					_mat_storage.push_back(thisp);
					fd->parameters.push_back(thisp);
				}
				bool pok = true;
				for (uint32_t p = 0; p < tm.param_count; ++p) {
					size_t pb = (size_t)tm.param_begin + p;
					if (pb >= _type_payload.size()) { pok = false; break; }
					DataDef *pd = forest_swizzle_type(_type_payload[pb], by_id);
					if (!pd) { pok = false; break; }
					fd->parameters.push_back(pd);
				}
				if (!pok)
					continue;	// unserializable param -> method cleanly lacks
				std::string dispname = mdp ? std::string(mdp, mdl) : std::string();
				fd->method_display_name = dispname;
				if (tm.emit_symbol_id) {
					uint32_t el = 0;
					const char *es = pool_cstr(tm.emit_symbol_id, el);
					if (es) fd->emit_symbol = std::string(es, el);
				}
				fd->is_const_method = (tm.flags & CIR_METHF_CONST) != 0;
				fd->is_varargs      = (tm.flags & CIR_METHF_VARARGS) != 0;
				fd->is_void_params  = (tm.flags & CIR_METHF_VOIDPARAMS) != 0;
				if (tm.flags & CIR_METHF_HAS_BODY) {
					// INLINE method: its body is a Tree-1 func-def in this
					// container. Record where, so ODR-use copies it into the
					// consumer's Tree-2 (like a template instantiation). NOT
					// declaration_only — the consumer emits the body itself.
					fd->has_forest_body  = true;
					fd->forest_body_unit = tm.body_unit;
					fd->forest_body_idx  = tm.body_idx;
					fd->declaration_only = false;
				} else {
					// LIBRARY method: body lives in a .so; the call links to
					// emit_symbol. Declaration-only, no body emitted.
					fd->declaration_only = true;
				}
				Variable *mv = new Variable(std::string(mnm, mnl), *fd, 1, NULL, false);
				if (m_static)
					mv->flags |= vfSTATIC;
				_mat_vars.push_back(mv);
				cdd->methods.push_back(mv);
				if (!dispname.empty())
					cdd->method_map[dispname] = mv;
				if (tm.flags & CIR_METHF_CTOR)
					cdd->ctors.push_back(mv);
			}
		}
		// A nameless anonymous sub-aggregate stays in by_id (so the enclosing
		// aggregate's rebuilt anonymous_aggregates can reference it) but is NOT
		// surfaced to forest_restore_decls — the live path never registers/emits
		// it standalone (it exists only nested inside its parent), so surfacing
		// it would emit a bogus top-level `struct __anon_N` and diverge from live.
		if (r.flags & CIR_TYPEF_ANON)
			continue;
		uint32_t nlen = 0, nslen = 0;
		CirRestoredType rt;
		rt.name       = pool_cstr(r.name_id, nlen);
		rt.kind       = r.kind;
		rt.dd         = sdd;
		rt.underlying = NULL;
		rt.ns         = r.namespace_id ? pool_cstr(r.namespace_id, nslen) : NULL;
		_restored.push_back(rt);
	}

	// Pass 3: typedef alias records -> (name, underlying DataDef*).
	for (size_t i = 0; i < _type_records.size(); ++i) {
		const cir_forest_type_record &r = _type_records[i];
		if (r.kind != CIR_TYPEK_TYPEDEF)
			continue;
		uint32_t nlen = 0;
		const char *nm = pool_cstr(r.name_id, nlen);
		if (!nm)
			continue;
		DataDef *underlying = forest_swizzle_type(r.ref0, by_id);
		if (!underlying)
			continue;
		uint32_t nslen = 0;
		CirRestoredType rt;
		rt.name       = nm;
		rt.kind       = CIR_TYPEK_TYPEDEF;
		rt.dd         = NULL;
		rt.underlying = underlying;
		rt.ns         = r.namespace_id ? pool_cstr(r.namespace_id, nslen) : NULL;
		_restored.push_back(rt);
	}

	// v13: restore file-scope global VARIABLE definitions — swizzle each record's
	// type_id back to a DataDef* (reusing the same by_id map + primitive resolver).
	// forest_restore_decls rebuilds a Variable + dkGlobalVar TopDecl from each, so
	// the existing passes emit the global + queue its ctor into __madc_global_init.
	for (size_t i = 0; i < _globals.size(); ++i) {
		const cir_forest_global_record &g = _globals[i];
		uint32_t nlen = 0;
		const char *nm = pool_cstr(g.name_id, nlen);
		DataDef *ty = forest_swizzle_type(g.type_id, by_id);
		if (!nm || !ty)
			continue;		// unresolved type -> bind cleanly lacks it
		CirRestoredGlobal rg;
		rg.name  = nm;
		rg.type  = ty;
		rg.flags = g.flags;
		_restored_globals.push_back(rg);
	}

	return _restored;
}

const char *CirFrozenForest::type_name_for(uint32_t type_id) const
{
	std::map<uint32_t, const char *>::const_iterator it = _type_names.find(type_id);
	return it != _type_names.end() ? it->second : NULL;
}

size_t CirFrozenForest::units_loaded() const
{
	size_t n = 0;
	for (size_t u = 0; u < _segs.size(); ++u)
		if (_segs[u])
			++n;
	return n;
}

// The load-on-demand step: decompress one unit's four payload segments,
// validate them against the directory, and register the segment. Every
// bound (child entry, connector target, position count) is checked HERE so
// a corrupt container fails at unit load, never as a wild read at
// materialize.
CirFrozenSegment *CirFrozenForest::unit_segment(uint32_t unit)
{
	if (unit >= _units.size())
		return NULL;
	if (_segs[unit])
		return _segs[unit];

	uint32_t base = CIR_FOREST_SEG_UNIT_BASE + unit * CIR_FOREST_SEGS_PER_UNIT;
	const madc::dis::snapshot_segment *recs = _reader.find(base + 0);
	const madc::dis::snapshot_segment *kids = _reader.find(base + 1);
	const madc::dis::snapshot_segment *conn = _reader.find(base + 2);
	const madc::dis::snapshot_segment *poss = _reader.find(base + 3);
	if (!recs || !kids || !conn || !poss
	    || recs->kind != SNAP_KIND_CIR_RECORDS
	    || kids->kind != SNAP_KIND_CIR_CHILDREN
	    || conn->kind != SNAP_KIND_CIR_CONNECTORS
	    || poss->kind != SNAP_KIND_CIR_POSITIONS) {
		fprintf(stderr, "madc: forest unit %u segments missing\n", unit);
		return NULL;
	}
	std::vector<uint8_t> rb, kb, cb, pb;
	if (!_reader.read_segment(*recs, rb) || !_reader.read_segment(*kids, kb)
	    || !_reader.read_segment(*conn, cb) || !_reader.read_segment(*poss, pb)
	    || rb.size() % sizeof(cir_frozen_record) || kb.size() % sizeof(uint32_t)
	    || cb.size() % sizeof(uint64_t) || pb.size() % sizeof(cir_frozen_pos)) {
		fprintf(stderr, "madc: forest unit %u payload corrupt\n", unit);
		return NULL;
	}

	cir_forest_unit fu;
	fu.unit_name_id = _units[unit].unit_name_id;
	fu.blob.records.resize(rb.size() / sizeof(cir_frozen_record));
	fu.blob.children.resize(kb.size() / sizeof(uint32_t));
	fu.connectors.resize(cb.size() / sizeof(uint64_t));
	fu.positions.resize(pb.size() / sizeof(cir_frozen_pos));
	if (!rb.empty()) memcpy(fu.blob.records.data(), rb.data(), rb.size());
	if (!kb.empty()) memcpy(fu.blob.children.data(), kb.data(), kb.size());
	if (!cb.empty()) memcpy(fu.connectors.data(), cb.data(), cb.size());
	if (!pb.empty()) memcpy(fu.positions.data(), pb.data(), pb.size());

	bool ok = fu.blob.records.size() == _units[unit].record_count
	       && fu.connectors.size() == _units[unit].connector_count
	       && fu.positions.size() == fu.blob.records.size();
	for (size_t i = 0; ok && i < fu.blob.records.size(); ++i) {
		const cir_frozen_record &rec = fu.blob.records[i];
		if (rec.child_base + rec.nchildren > fu.blob.children.size()) {
			ok = false;
			break;
		}
		for (uint32_t k = 0; ok && k < rec.nchildren; ++k) {
			uint32_t ci = fu.blob.children[rec.child_base + k];
			if (ci & CIR_FROZEN_CHILD_CONNECTOR_BIT)
				ok = (ci & ~CIR_FROZEN_CHILD_CONNECTOR_BIT)
				     < fu.connectors.size();
			else
				ok = ci < fu.blob.records.size();
		}
	}
	for (size_t c = 0; ok && c < fu.connectors.size(); ++c) {
		uint32_t tu = (uint32_t)(fu.connectors[c] >> 32);
		uint32_t ti = (uint32_t)fu.connectors[c];
		ok = tu < _units.size() && tu != unit
		     && ti < _units[tu].record_count;
	}
	if (!ok) {
		fprintf(stderr, "madc: forest unit %u failed validation\n", unit);
		return NULL;
	}

	DBG(fprintf(stderr, "forest: loading unit %u (%zu records)\n",
		    unit, fu.blob.records.size()));
	_segs[unit] = new CirFrozenSegment(std::move(fu), this, _c2m);
	return _segs[unit];
}

cir_node *CirFrozenForest::node_for(uint32_t unit, uint32_t idx)
{
	CirFrozenSegment *s = unit_segment(unit);
	return s ? s->node_at(idx) : NULL;
}

// ---------------------------------------------------------------------------
// Structural identity oracle
// ---------------------------------------------------------------------------

static bool str_payload_equal(const c2mir_str_t &x, const c2mir_str_t &y)
{
	if (x.len != y.len)
		return false;
	if (x.len == 0)
		return true;	// content-equal regardless of pointer nullness
	if (!x.s || !y.s)
		return x.s == y.s;
	return memcmp(x.s, y.s, x.len) == 0;
}

static bool scalar_payload_equal(uint32_t code, const struct node *x,
				 const struct node *y)
{
	switch (code) {
	case N_I: case N_L:   return x->u.l == y->u.l;
	case N_LL:            return x->u.ll == y->u.ll;
	case N_U: case N_UL:  return x->u.ul == y->u.ul;
	case N_ULL:           return x->u.ull == y->u.ull;
	case N_F: case N_CF:  return memcmp(&x->u.f, &y->u.f, sizeof(x->u.f)) == 0;
	case N_D: case N_CD:  return memcmp(&x->u.d, &y->u.d, sizeof(x->u.d)) == 0;
	case N_LD: case N_CLD: return memcmp(&x->u.ld, &y->u.ld, sizeof(x->u.ld)) == 0;
	case N_CH: case N_CH16: case N_CH32: return x->u.ch == y->u.ch;
	default:              return true;
	}
}

bool cir_trees_structurally_identical(node_t a, node_t b)
{
	std::vector<std::pair<node_t, node_t> > work;
	std::set<std::pair<node_t, node_t> > seen;
	work.push_back(std::make_pair(a, b));
	while (!work.empty()) {
		node_t x = work.back().first;
		node_t y = work.back().second;
		work.pop_back();
		if (!x || !y) {
			if (x != y)
				return false;
			continue;
		}
		if (!seen.insert(std::make_pair(x, y)).second)
			continue;	// shared pair / cycle: already compared
		if (x->code != y->code)
			return false;

		switch (cir_payload_class_for((uint32_t)x->code)) {
		case CIR_PAYLOAD_SCALAR:
			if (!scalar_payload_equal((uint32_t)x->code, x, y))
				return false;
			break;
		case CIR_PAYLOAD_STR:
			if (!str_payload_equal(x->u.s, y->u.s))
				return false;
			break;
		case CIR_PAYLOAD_NONE:
			break;
		}

		cir_node *cx = CIR_NODE(x);
		cir_node *cy = CIR_NODE(y);
		if (cx->origin_id != cy->origin_id
		    || cx->datadef_id != cy->datadef_id
		    || cx->tsubst_pack_index != cy->tsubst_pack_index
		    || !(cx->tree1_origin == cy->tree1_origin)
		    || cx->src_lang != cy->src_lang
		    || cx->synth_from_origin != cy->synth_from_origin
		    || cx->tsubst_pack_expand != cy->tsubst_pack_expand)
			return false;
		// Extension STRING ids compare by CONTENT via the accessors,
		// not by raw id — a forest thaw legitimately re-interns them
		// into the live pool (identical ids in-process; fresh ids
		// cross-process). The accessors resolve both sides against
		// the live pool; NULL/NULL is equal.
		const char *tx = cx->typedef_name(), *ty = cy->typedef_name();
		if ((tx != NULL) != (ty != NULL) || (tx && strcmp(tx, ty) != 0))
			return false;
		const char *ex = cx->error_msg(), *ey = cy->error_msg();
		if ((ex != NULL) != (ey != NULL) || (ex && strcmp(ex, ey) != 0))
			return false;
		const char *px = cx->tsubst_pack_value_name(), *py = cy->tsubst_pack_value_name();
		if ((px != NULL) != (py != NULL) || (px && strcmp(px, py) != 0))
			return false;

		// Children: identical sequences (c2mir_node_first_op
		// self-guards on leaves).
		node_t ox = c2mir_node_first_op(x);
		node_t oy = c2mir_node_first_op(y);
		while (ox && oy) {
			work.push_back(std::make_pair(ox, oy));
			ox = c2mir_node_next_op(ox);
			oy = c2mir_node_next_op(oy);
		}
		if (ox || oy)
			return false;	// arity mismatch
	}
	return true;
}
