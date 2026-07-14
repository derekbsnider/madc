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

// v27 producer-config gate: the ONE derivation shared by freeze (stamp) and
// bind (compare). A container parsed under one language standard / -D set
// must never bind into a compile under another — header CONTENT differs (C
// vs C++ surface, __STRICT_ANSI__, feature macros). Per-compile state, so it
// cannot ride the process-invariant context hash above.
uint32_t madc_forest_config_word(const Program *prog)
{
	return (uint32_t)prog->language_std
	     | ((uint32_t)(prog->gnu_dialect ? 1 : 0) << 16);
}

uint32_t madc_forest_defines_hash(const Program *prog)
{
	if (prog->cli_defines.empty())
		return 0;
	uint32_t h = 2166136261u;	// FNV-1a over "NAME=VALUE\n" in CLI order
	for (const auto &d : prog->cli_defines) {
		for (char c : d.first)  { h ^= (uint8_t)c; h *= 16777619u; }
		h ^= (uint8_t)'=';        h *= 16777619u;
		for (char c : d.second) { h ^= (uint8_t)c; h *= 16777619u; }
		h ^= (uint8_t)'\n';       h *= 16777619u;
	}
	return h ? h : 1;	// 0 is reserved for "no defines"
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
	// recording the location lets cir_forest_arena_complete flag the method as
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
		// Env-gated probe (MADC_MTI_PROBE=<substr>): each partitioned
		// func-def symbol indexed for body stamping.
		{
			static const char *mtp = ::getenv("MADC_MTI_PROBE");
			if (mtp && *mtp && strstr(id->u.s.s, mtp))
				fprintf(stderr, "MTIPROBE funcdef_loc sym=%s unit=%u"
					" idx=%u\n",
					id->u.s.s, it->second.unit, it->second.idx);
		}
		// v21: the def's OWN source file (origin token) — an instantiated
		// definition lands in the main-file unit but its tokens carry the
		// template's header origin; body stamping fences by this (v25:
		// root-vs-include, the v24 discriminator).
		const char *deffile = NULL;
		if (it->first->origin_id) {
			TokenBase *tb = madc_token_for_slot(it->first->origin_id);
			if (tb && tb->file)
				deffile = tb->file;
		}
		out.funcdef_files[id->u.s.s] = deffile;
	}

	// v20: index every top-level EXTERN declaration's symbol -> (unit, idx).
	// These are the typed (or deliberately unprototyped) extern decls the
	// producer's lowering emitted (need_output_extern: operator new/delete
	// manglings, dlsym-fallback libc fns, the __madc_* runtime). A LOADED
	// body's pre-built call to such a symbol never passes the lowering site
	// that would declare it, so bind loads the producer's OWN decl node —
	// verbatim state, never a re-derived signature.
	if (TokenBase::_active_strpool) {
		madc::dis::intern_table &pool = *TokenBase::_active_strpool;
		std::set<std::string> seen_externs;
		for (std::map<cir_node *, cir_freeze_loc>::iterator it = where.begin();
		     it != where.end(); ++it) {
			node_t sd = it->first->as_node();
			if (sd->code != N_SPEC_DECL)
				continue;
			node_t share = c2mir_node_first_op(sd);	// op0: N_SHARE(spec list)
			node_t specs = share && share->code == N_SHARE
				     ? c2mir_node_first_op(share) : share;
			bool is_extern = false;
			for (node_t s = specs ? c2mir_node_first_op(specs) : NULL;
			     s; s = c2mir_node_next_op(s))
				if (s->code == N_EXTERN) { is_extern = true; break; }
			if (!is_extern)
				continue;
			node_t decl = share ? c2mir_node_next_op(share) : NULL;
			if (!decl || decl->code != N_DECL)
				continue;
			node_t id = c2mir_node_first_op(decl);
			if (!id || id->code != N_ID || !id->u.s.s)
				continue;
			if (!seen_externs.insert(id->u.s.s).second)
				continue;	// first decl wins (dedup)
			cir_forest_extern_loc xl;
			xl.name_id = pool.intern(id->u.s.s);
			xl.unit    = it->second.unit;
			xl.idx     = it->second.idx;
			out.extern_locs.push_back(xl);
		}
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

// Pack-time zstd level for the CURRENT cir_forest_write call. Set from its
// zstd_level parameter (the freeze path is single-threaded): the RELEASE
// pack passes a high level — compression is paid once per release build,
// and zstd DEcompression speed is essentially level-independent — while
// dev/standalone freezes keep the fast default (0 -> codec default 3) so
// the drain-ladder loop never pays high-level compression. The pack level
// itself is budgeted against the ~120s per-process CPU kill on the dev
// box (see the selection comment at the madc_cir.cpp call site).
static int cir_forest_zstd_level = 0;

// Pack-time intern-spine compression for the CURRENT cir_forest_write call
// (owner-approved 2026-07-14, task #37): the RELEASE pack compresses the
// three intern blocks (3.74 -> 0.81 MB stored — the difference between a
// ~12.8 and a ~9.5 MB packed binary) and its consumers rebind through the
// forest_pool_block owned-buffer fallback at ~7ms decode once per process.
// Dev/standalone freezes keep the spine raw so their binds stay zero-copy
// (the drain-ladder loop's instant rebinds).
static bool cir_forest_compress_intern = false;

// Zero-length payloads take codec None (nothing to compress); the writer
// still needs a non-NULL byte pointer.
// INTERN pool blocks (both the container pool and the arena's — keyed on
// KIND, not seg-id) stay codec None outside the release pack: they are the
// bind-in-place spine the reader binds zero-copy from the image
// (forest_pool_block/raw_ptr — the path the per-segment codec field exists
// for). Everything else reaches consumers through a copying read_segment
// (per-unit payloads via the load-on-demand unit_segment), so compression
// there trades a memcpy for a zstd frame decode of only what a consumer
// actually loads.
//
// Per-KIND transform policy (declared format vocabulary; measured on the
// packed corpus, task #37 2026-07-14): CHILDREN are near-sequential record
// indices (depth-first record layout) — a forward delta turns them into
// tiny runs (1.68 -> 0.09 MB stored). RECORDS are fixed-stride structs —
// byte-plane transposition groups like fields into columns so zstd sees
// the cross-record redundancy (2.05 -> 0.90 MB stored, and compresses ~3x
// faster than row order). Trained ZDICT dictionaries were measured the
// same day and rejected: net ~-0.3 MB for +13s pack CPU and a dictionary
// segment + reader wiring — the transforms dominate them on every axis.
static bool add_seg(madc::dis::snapshot_writer &w, uint32_t seg_id,
		    uint32_t kind, const void *bytes, size_t len,
		    PchCompression codec)
{
	if (!cir_forest_compress_intern
	    && (kind == madc::dis::SNAP_KIND_INTERN_BYTES
		|| kind == madc::dis::SNAP_KIND_INTERN_ENTRIES
		|| kind == madc::dis::SNAP_KIND_INTERN_BUCKETS))
		codec = PchCompression::None;
	uint32_t xform = 0;
	if (kind == SNAP_KIND_CIR_CHILDREN)
		xform = madc::dis::snap_xform_flags(madc::dis::SNAP_XFORM_DELTA32);
	else if (kind == SNAP_KIND_CIR_RECORDS)
		xform = madc::dis::snap_xform_flags(
			madc::dis::SNAP_XFORM_BYTEPLANE,
			(uint32_t)sizeof(cir_frozen_record));
	return w.add_segment(seg_id, kind, len ? bytes : (const void *)"",
			     len, len ? codec : PchCompression::None,
			     cir_forest_zstd_level, len ? xform : 0);
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
		      PchCompression codec, int zstd_level, bool compress_intern)
{
	cir_forest_zstd_level = zstd_level;
	cir_forest_compress_intern = compress_intern;
	madc::dis::intern_table *pool = TokenBase::_active_strpool;
	if (f.units.empty() || !pool)
		return false;

	// Everything below interns BEFORE the pool blocks are staged, so the
	// serialized pool contains every handle the container references.
	std::vector<uint32_t> lib_ids;
	for (size_t i = 0; i < f.libs.size(); ++i)
		lib_ids.push_back(pool->intern(f.libs[i]));

	// Directory payload: header + unit table + lib name ids.
	cir_forest_dir_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.version    = CIR_FOREST_FORMAT_VERSION;
	hdr.unit_count = (uint32_t)f.units.size();
	hdr.root_unit  = f.root_unit;
	hdr.root_idx   = f.root_idx;
	hdr.lib_count  = (uint32_t)f.libs.size();
	hdr.language_std = f.language_std;	// v27 producer-config gate
	hdr.defines_hash = f.defines_hash;
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
	// v13 container-global: file-scope global VARIABLE definitions (zero-length
	// when the freeze recorded none).
	if (!add_seg(w, CIR_FOREST_SEG_GLOBALS, SNAP_KIND_CIR_GLOBALS,
		     f.globals.data(),
		     f.globals.size() * sizeof(cir_forest_global_record), codec))
		return false;
	// B3 (v18): the DefArena dump — defrec[] + payload u32[] + the arena's OWN
	// intern strings (distinct seg-ids, INTERN_* kinds). THE type-graph
	// serialization: populated during parse from Program::forest_arena,
	// refreshed + completed at freeze, reconstructed by materialize_from_arena
	// on load. Zero-length segments = a type-less freeze.
	if (!add_seg(w, CIR_FOREST_SEG_ARENA_DEFS, SNAP_KIND_CIR_ARENA_DEFS,
		     f.arena.defs.data(),
		     f.arena.defs.size() * sizeof(uint32_t), codec)
	    || !add_seg(w, CIR_FOREST_SEG_ARENA_PAYLOAD, SNAP_KIND_CIR_ARENA_PAYLOAD,
			f.arena.payload.data(),
			f.arena.payload.size() * sizeof(uint32_t), codec)
	    || !add_seg(w, CIR_FOREST_SEG_ARENA_STR_BYTES, madc::dis::SNAP_KIND_INTERN_BYTES,
			f.arena.strings.bytes_data(), f.arena.strings.bytes_size(), codec)
	    || !add_seg(w, CIR_FOREST_SEG_ARENA_STR_ENTRIES, madc::dis::SNAP_KIND_INTERN_ENTRIES,
			f.arena.strings.entries_data(),
			f.arena.strings.entries_size() * sizeof(madc::dis::intern_table::Entry), codec)
	    || !add_seg(w, CIR_FOREST_SEG_ARENA_STR_BUCKETS, madc::dis::SNAP_KIND_INTERN_BUCKETS,
			f.arena.strings.buckets_data(),
			f.arena.strings.buckets_size() * sizeof(uint32_t), codec))
		return false;
	// v23: the arena's raw token-byte block (param-default expression runs,
	// .madh record form; zero-length when no defaults were captured).
	if (!add_seg(w, CIR_FOREST_SEG_ARENA_TOKBYTES, SNAP_KIND_CIR_ARENA_TOKBYTES,
		     f.arena.tokbytes.data(), f.arena.tokbytes.size(), codec))
		return false;
	// v20 container-global: template-NAME state (zero-length when the freeze
	// recorded no template patterns).
	if (!add_seg(w, CIR_FOREST_SEG_TEMPLATES, SNAP_KIND_CIR_TEMPLATES,
		     f.templates.data(),
		     f.templates.size() * sizeof(cir_forest_template_record), codec)
	    || !add_seg(w, CIR_FOREST_SEG_TEMPLATE_PAYLOAD, SNAP_KIND_CIR_TEMPLATE_PAYLOAD,
			f.template_payload.data(),
			f.template_payload.size() * sizeof(uint32_t), codec)
	    || !add_seg(w, CIR_FOREST_SEG_TEMPLATE_TOKENS, SNAP_KIND_CIR_TEMPLATE_TOKENS,
			f.template_tokens.data(), f.template_tokens.size(), codec)
	    || !add_seg(w, CIR_FOREST_SEG_EXTERN_LOCS, SNAP_KIND_CIR_EXTERN_LOCS,
			f.extern_locs.data(),
			f.extern_locs.size() * sizeof(cir_forest_extern_loc), codec))
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
	: _blob(std::move(blob)), _record_count(_blob.records.size()),
	  _forest(NULL), _c2m(c2m)
{
	_mat.assign(_record_count, (cir_node *)NULL);
	_seg = madc_cir_register_segment(this);
}

CirFrozenSegment::CirFrozenSegment(cir_forest_unit &&unit, CirFrozenForest *forest,
				   c2m_ctx_t c2m)
	: _blob(std::move(unit.blob)), _record_count(_blob.records.size()),
	  _connectors(std::move(unit.connectors)),
	  _positions(std::move(unit.positions)), _forest(forest), _c2m(c2m)
{
	_mat.assign(_record_count, (cir_node *)NULL);
	_seg = madc_cir_register_segment(this);
}

CirFrozenSegment::CirFrozenSegment(cir_forest_unit &&unit,
				   std::vector<uint8_t> &&record_planes,
				   size_t record_count, CirFrozenForest *forest,
				   c2m_ctx_t c2m)
	: _blob(std::move(unit.blob)), _record_planes(std::move(record_planes)),
	  _record_count(record_count), _connectors(std::move(unit.connectors)),
	  _positions(std::move(unit.positions)), _forest(forest), _c2m(c2m)
{
	_mat.assign(_record_count, (cir_node *)NULL);
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

bool CirFrozenSegment::record_at(uint32_t idx, cir_frozen_record &out) const
{
	if (idx >= _record_count)
		return false;
	if (_record_planes.empty()) {
		out = _blob.records[idx];
		return true;
	}
	uint8_t *dst = reinterpret_cast<uint8_t *>(&out);
	for (size_t b = 0; b < sizeof(out); ++b)
		dst[b] = _record_planes[b * _record_count + idx];
	return true;
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
	cir_frozen_record r;
	if (!record_at(idx, r))
		return NULL;
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
	if (idx0 >= s0->record_count())
		return NULL;
	if (s0->_mat[idx0])
		return s0->_mat[idx0];

	// Phase A: shells for every cold record reachable from (s0, idx0) —
	// one iterative worklist ACROSS units (a connector hop is a worklist
	// step, not a recursion; memoized shells terminate shares and cycles,
	// including cycles that cross units).
	std::vector<std::pair<CirFrozenSegment *, uint32_t> > created;
	if (!s0->shell(idx0))
		return NULL;
	created.push_back(std::make_pair(s0, idx0));
	for (size_t w = 0; w < created.size(); ++w) {
		CirFrozenSegment *s = created[w].first;
		cir_frozen_record r;
		if (!s->record_at(created[w].second, r))
			return NULL;
		for (uint32_t k = 0; k < r.nchildren; ++k) {
			CirFrozenSegment *cs = NULL;
			uint32_t cidx = 0;
			if (!cir_child_target(s, s->_blob.children[r.child_base + k],
					      cs, cidx))
				return NULL;
			if (!cs->_mat[cidx]) {
				if (!cs->shell(cidx))
					return NULL;
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
		cir_frozen_record r;
		if (!s->record_at(created[w].second, r))
			return NULL;
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

bool CirFrozenForest::open(const void *image, size_t len, c2m_ctx_t c2m,
			   bool quiet_missing)
{
	_c2m = c2m;
	if (!TokenBase::_active_strpool) {
		fprintf(stderr, "madc: forest thaw requires a live string pool\n");
		return false;
	}
	if (!_reader.open(image, len)) {
		if (!quiet_missing)
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
	_language_std = hdr.language_std;	// v27 producer config (bind gate)
	_defines_hash = hdr.defines_hash;

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

	// v13 container-global: file-scope global VARIABLE definitions (zero-length
	// = none; materialize_from_arena swizzles each type_id -> DataDef*).
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
	// v20 container-global: template-NAME state (records + u32 payload + token
	// bytes; zero-length = a template-less freeze). materialize_from_arena
	// resolves names / swizzles owner ids into _restored_templates; the token
	// runs deserialize on the parser side (forest_restore_decls).
	if (const madc::dis::snapshot_segment *ts =
		_reader.find(CIR_FOREST_SEG_TEMPLATES)) {
		std::vector<uint8_t> d;
		if (ts->kind != SNAP_KIND_CIR_TEMPLATES
		    || !_reader.read_segment(*ts, d)
		    || d.size() % sizeof(cir_forest_template_record)) {
			fprintf(stderr, "madc: forest template table corrupt\n");
			return false;
		}
		_templates.resize(d.size() / sizeof(cir_forest_template_record));
		if (!d.empty())
			memcpy(_templates.data(), d.data(), d.size());
	}
	if (const madc::dis::snapshot_segment *tp =
		_reader.find(CIR_FOREST_SEG_TEMPLATE_PAYLOAD)) {
		std::vector<uint8_t> d;
		if (tp->kind != SNAP_KIND_CIR_TEMPLATE_PAYLOAD
		    || !_reader.read_segment(*tp, d) || d.size() % sizeof(uint32_t)) {
			fprintf(stderr, "madc: forest template payload corrupt\n");
			return false;
		}
		_template_payload.resize(d.size() / sizeof(uint32_t));
		if (!d.empty())
			memcpy(_template_payload.data(), d.data(), d.size());
	}
	if (const madc::dis::snapshot_segment *tt =
		_reader.find(CIR_FOREST_SEG_TEMPLATE_TOKENS)) {
		if (tt->kind != SNAP_KIND_CIR_TEMPLATE_TOKENS
		    || !_reader.read_segment(*tt, _template_tokens)) {
			fprintf(stderr, "madc: forest template tokens corrupt\n");
			return false;
		}
	}
	// v20 container-global: the extern-decl index (symbol -> frozen loc).
	if (const madc::dis::snapshot_segment *xs =
		_reader.find(CIR_FOREST_SEG_EXTERN_LOCS)) {
		std::vector<uint8_t> d;
		if (xs->kind != SNAP_KIND_CIR_EXTERN_LOCS
		    || !_reader.read_segment(*xs, d)
		    || d.size() % sizeof(cir_forest_extern_loc)) {
			fprintf(stderr, "madc: forest extern index corrupt\n");
			return false;
		}
		size_t n = d.size() / sizeof(cir_forest_extern_loc);
		for (size_t i = 0; i < n; ++i) {
			cir_forest_extern_loc xl;
			memcpy(&xl, d.data() + i * sizeof(xl), sizeof(xl));
			uint32_t nlen = 0;
			const char *nm = pool_cstr(xl.name_id, nlen);
			if (!nm || !nlen)
				continue;
			_extern_by_name[std::string(nm, nlen)] =
				std::make_pair(xl.unit, xl.idx);
		}
	}
	// v18 container-global: the B3 DefArena dump — THE type-graph serialization.
	// Bind the five arena segments read-only (defrec/payload uint32 spans + the
	// arena's OWN intern blocks; the container 16-aligns payloads, satisfying the
	// uint32/Entry casts). A type-less freeze dumps zero-length segments -> the
	// view stays empty; a malformed set is rejected like the other container
	// globals. The typeid->name closure (type_name_for) derives from the bound
	// records; the DataDef objects materialize lazily at bind
	// (materialize_from_arena), never at open — so --run-frozen (which never
	// binds) pays nothing.
	{
		size_t nd = 0, np = 0, nb = 0, ne = 0, nk = 0;
		const uint8_t *pd = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_DEFS,
						      SNAP_KIND_CIR_ARENA_DEFS,
						      _arena_defs, nd);
		const uint8_t *pp = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_PAYLOAD,
						      SNAP_KIND_CIR_ARENA_PAYLOAD,
						      _arena_payload, np);
		const uint8_t *ab = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_STR_BYTES,
						      madc::dis::SNAP_KIND_INTERN_BYTES,
						      _arena_sbytes, nb);
		const uint8_t *ae = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_STR_ENTRIES,
						      madc::dis::SNAP_KIND_INTERN_ENTRIES,
						      _arena_sentries, ne);
		const uint8_t *ak = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_STR_BUCKETS,
						      madc::dis::SNAP_KIND_INTERN_BUCKETS,
						      _arena_sbuckets, nk);
		if (!ak)
			nk = 0;	// buckets are derivable; never bind NULL with a count
		// v23: the param-default token-byte block (absent/zero-length on a
		// defaults-less freeze — the runs then cleanly lack).
		size_t ntk = 0;
		const uint8_t *tk = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_TOKBYTES,
						      SNAP_KIND_CIR_ARENA_TOKBYTES,
						      _arena_tokbytes, ntk);
		if (tk && ntk)
			_arena.bind_tokbytes(tk, ntk);
		if (pd && pp && ab && ae && nd
		    && !(nd % sizeof(uint32_t)) && !(np % sizeof(uint32_t))
		    && !(ne % sizeof(madc::dis::intern_table::Entry))
		    && !(nk % sizeof(uint32_t))) {
			_arena.bind_defs((const uint32_t *)pd, nd / sizeof(uint32_t));
			_arena.bind_payload((const uint32_t *)pp, np / sizeof(uint32_t));
			_arena.strings.bind((const char *)ab, nb,
					    (const madc::dis::intern_table::Entry *)ae,
					    ne / sizeof(madc::dis::intern_table::Entry),
					    (const uint32_t *)ak,
					    nk / sizeof(uint32_t));
			if (!_arena.strings.valid()) {
				fprintf(stderr, "madc: forest arena string pool"
					" failed validation\n");
				return false;
			}
			// Derive the typeid -> name closure (type_name_for) from
			// the arena records — the arena is id-addressed by project
			// slot, so a def's arena id IS the freeze-time typeid.
			for (uint32_t s = 0; s < _arena.def_slots(); ++s) {
				uint32_t tid = madc::dis::arena_id_of(s);
				madc::dis::defrec r;
				if (!_arena.get_def_at(tid, r) || !r.name_id)
					continue;
				if (const char *nm = _arena.c_str(r.name_id))
					_type_names[tid] = nm;
			}
		}
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

// ---------------------------------------------------------------------------
// B3 (v18) — reconstruct the type graph from the dumped DefArena.
//
// THE load path: the arena (defrec/memberrec/baserec/methodrec/anonrec/
// paramrec + its own intern pool) is the type-graph serialization. The retired
// v6 freeze FILTERED at save time (its fixpoint recorded an aggregate only
// when every member/base type was serializable; it skipped polymorphic /
// vbase / union-layout classes, template methods, and symbol-less non-dtor
// specials). The arena stores EVERYTHING the parse produced, so the SAME
// selection runs here at LOAD time — the flip kept bind behavior identical.
// Widening to the arena's full fidelity is a post-flip step, gated against
// LIVE, never against v6.
// ---------------------------------------------------------------------------

// A member / param / return / typedef-underlying type-id resolves iff its
// derived chain (pointer / reference / const ref0 links) bottoms out in a
// pinned primitive or a recordable aggregate — `self` allowed, the
// self-referential `Node *next` case. Reproduces the retired v6 save-side
// test (an enum / func / other kind fails, exactly as v6 bailed).
static bool arena_chain_ok(const madc::dis::FrozenDefArena &a, uint32_t tid,
			   uint32_t self_id, const std::set<uint32_t> &recordable)
{
	for (int depth = 0; depth < 64; ++depth) {
		if (!tid)
			return false;
		if (madc::dis::arena_id_is_pinned(tid))
			return true;
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r))
			return false;
		switch (r.kind) {
		case madc::dis::DK_PTR:
		case madc::dis::DK_REF:
		case madc::dis::DK_CONST:
		case madc::dis::DK_CARRAY:	// v25: element chain, like the other unaries
			tid = r.ref0;
			continue;
		case madc::dis::DK_STRUCT:
		case madc::dis::DK_UNION:
		case madc::dis::DK_CLASS:
			return tid == self_id || recordable.count(tid) != 0;
		case madc::dis::DK_ENUM:
			return true;	// v21: enums materialize in pass 1a (leaves)
		case madc::dis::DK_FPTR:
			return true;	// v22: fn-ptrs materialize in pass 1b (the
					// target signature never gates member layout)
		default:
			return false;
		}
	}
	return false;
}

// Swizzle an arena type-id to the reconstructed DataDef*: a pinned primitive
// resolves process-invariantly; anything else must be in by_id (aggregates
// from pass 1, derived types from pass 1b).
static DataDef *arena_swizzle(uint32_t tid,
			      const std::map<uint32_t, DataDef *> &by_id)
{
	if (tid && tid < MADC_TYPEID_PRIMITIVE_END)
		return madc_type_from_id(tid);
	std::map<uint32_t, DataDef *>::const_iterator it = by_id.find(tid);
	return it != by_id.end() ? it->second : NULL;
}

const std::vector<CirRestoredType> &CirFrozenForest::materialize_from_arena()
{
	if (_types_materialized)
		return _restored;
	_types_materialized = true;

	// A type-less freeze binds an empty arena view: every aggregate pass
	// below no-ops (nslots == 0) and only pinned-typed globals restore.
	const madc::dis::FrozenDefArena &a = _arena;
	uint32_t nslots = a.def_slots();

	// Recordability closure — the v6 save-side fixpoint reproduced at load as
	// a pure analysis over the records: an aggregate is recordable iff every
	// member's type chain resolves (self allowed), every base is recordable
	// (base-before-derived) and every anonymous sub-aggregate is recordable.
	// v22 (iostream): POLYMORPHIC classes (vtable / vptr slot) and
	// virtual-base-carrying classes are ADMITTED — the records carry the full
	// vtable state (own_block_off, vbaserec runs, vgrouprec runs) and pass 2
	// restores it verbatim. Union-layout classes stay fenced (their own
	// follow-on), and their dependents cleanly lack, like the v6 fixpoint.
	std::vector<uint32_t> agg_ids;		// slot order == id-stamp order
	for (uint32_t s = 0; s < nslots; ++s) {
		uint32_t tid = madc::dis::arena_id_of(s);
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r))
			continue;
		if (r.kind != madc::dis::DK_STRUCT && r.kind != madc::dis::DK_UNION
		    && r.kind != madc::dis::DK_CLASS)
			continue;
		// v21: an INCOMPLETE record restores only in the payload-less empty
		// shape (a synthesized recursion tail like _Tuple_impl<N> — live
		// registers, inherits from, and emits it without completing it).
		// Anything half-parsed WITH members/bases stays dropped.
		if (!(r.flags & madc::dis::DF_IS_COMPLETE)
		    && (r.members_count || r.bases_count))
			continue;
		if (r.kind == madc::dis::DK_CLASS
		    && (r.flags & madc::dis::DF_UNION_LAYOUT))
			continue;
		agg_ids.push_back(tid);
	}
	// GREATEST fixpoint (v22): start from ALL candidates and iteratively
	// REMOVE any aggregate with an unresolvable member / base / anon group
	// until stable. The old additive (least) fixpoint could not admit a
	// POINTER CYCLE — basic_ios<char> holds a basic_ostream<char>* member
	// (_M_tie) while basic_ostream derives from basic_ios, so neither could
	// ever be inserted first and the whole iostream hierarchy dropped. A
	// through-pointer reference only needs its target IN the surviving set
	// (pass 1 allocates every survivor before pass 1b resolves the pointer),
	// exactly the C incomplete-type rule; a failure still cascades to its
	// dependents through the removal rounds.
	std::set<uint32_t> recordable(agg_ids.begin(), agg_ids.end());
	bool removed = true;
	while (removed) {
		removed = false;
		for (size_t i = 0; i < agg_ids.size(); ++i) {
			uint32_t tid = agg_ids[i];
			if (!recordable.count(tid))
				continue;
			madc::dis::defrec r;
			if (!a.get_def_at(tid, r)) {
				recordable.erase(tid);
				removed = true;
				continue;
			}
			bool ok = true;
			for (uint32_t m = 0; ok && m < r.members_count; ++m) {
				madc::dis::memberrec mr;
				if (!a.get_payload(r.members_begin, m, mr)
				    || !arena_chain_ok(a, mr.type_id, tid, recordable))
					ok = false;
			}
			if (ok && r.kind == madc::dis::DK_CLASS) {
				for (uint32_t b = 0; ok && b < r.bases_count; ++b) {
					madc::dis::baserec br;
					if (!a.get_payload(r.bases_begin, b, br)
					    || !recordable.count(br.base_id))
						ok = false;
				}
			}
			for (uint32_t g = 0; ok && g < r.anon_count; ++g) {
				madc::dis::anonrec ar;
				if (!a.get_payload(r.anon_begin, g, ar)
				    || !recordable.count(ar.sub_type_id))
					ok = false;
			}
			if (!ok) {
				recordable.erase(tid);
				removed = true;
			}
		}
	}

	// Permanent -v diagnostic (the load-side twin of the freeze-completeness
	// probe): name every candidate aggregate the closure DROPPED and the
	// first blocking member / base / anon group — "what is the LOADED state
	// missing" is always a measurement, never a guess.
	DBG(for (size_t i = 0; i < agg_ids.size(); ++i) {
		uint32_t tid = agg_ids[i];
		if (recordable.count(tid))
			continue;
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r))
			continue;
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		std::string why = "?";
		for (uint32_t m = 0; m < r.members_count; ++m) {
			madc::dis::memberrec mr;
			if (!a.get_payload(r.members_begin, m, mr)
			    || !arena_chain_ok(a, mr.type_id, tid, recordable)) {
				const char *mn = mr.name_id ? a.c_str(mr.name_id) : "?";
				why = std::string("member ") + (mn ? mn : "?");
				break;
			}
		}
		if (why == "?" && r.kind == madc::dis::DK_CLASS)
			for (uint32_t b = 0; b < r.bases_count; ++b) {
				madc::dis::baserec br;
				if (!a.get_payload(r.bases_begin, b, br)
				    || !recordable.count(br.base_id)) {
					madc::dis::defrec bd;
					const char *bn =
						a.get_def_at(br.base_id, bd) && bd.name_id
						? a.c_str(bd.name_id) : "?";
					why = std::string("base ") + (bn ? bn : "?");
					break;
				}
			}
		if (why == "?")
			why = "anon group";
		std::cout << "materialize closure: DROPPED " << (nm ? nm : "?")
			  << " (" << why << ")" << std::endl;
	});

	// Rung 2a (closure-filtered materialization): with a demand filter
	// installed, only records the TU's bound-include closure declares build
	// DataDefs. Verdicts mirror the item-5 registration filter EXACTLY
	// (forest_restore_decls): qualified ns::name first, then bare name;
	// unindexed = derived entity = keep; a `<`-bearing canonical spelling is
	// judged by its HEAD (the declaring template's name). Tri-state so the
	// free-function walk below can reproduce registration's display-name
	// refinement (which applies only when the primary key is UNINDEXED).
	auto verdict_str = [&](const char *nm, const char *ns) -> int {
		// +1 declared by a bound unit, -1 declared only by unbound units,
		// 0 unindexed (derived entity)
		if (!nm || !*nm)
			return 0;
		const std::unordered_map<std::string, bool> &db =
			_mat_filter.declared_bound;
		std::unordered_map<std::string, bool>::const_iterator it = db.end();
		if (ns && *ns)
			it = db.find(std::string(ns) + "::" + nm);
		if (it == db.end())
			it = db.find(nm);
		if (it != db.end())
			return it->second ? 1 : -1;
		return 0;
	};
	std::unordered_map<uint64_t, int> verdict_memo;
	auto name_verdict = [&](uint32_t name_id, uint32_t ns_id) -> int {
		if (!name_id)
			return 0;
		uint64_t key = ((uint64_t)ns_id << 32) | name_id;
		std::unordered_map<uint64_t, int>::iterator mi =
			verdict_memo.find(key);
		if (mi != verdict_memo.end())
			return mi->second;
		int v = verdict_str(a.c_str(name_id),
				    ns_id ? a.c_str(ns_id) : NULL);
		verdict_memo[key] = v;
		return v;
	};
	std::unordered_map<std::string, int> head_memo;
	auto record_kept = [&](const madc::dis::defrec &r) -> bool {
		if (!_mat_filter.active)
			return true;
		// A record carries up to TWO name surfaces, judged INDEPENDENTLY
		// (mirrors registration's ns_ok/flat_ok split, parser.cpp ~12597):
		// the freeze stamps a globally-defined tag's ONE record with the
		// namespace that ALIASES it (ctime's `using ::timespec` marks
		// glibc's struct timespec ns=std), so the qualified and bare forms
		// answer to DIFFERENT declaring units. The record survives if
		// EITHER surface is permitted; registration then gates each
		// surface separately.
		bool ns_ok = name_verdict(r.name_id, r.ns_id) >= 0;
		bool flat_ok = r.ns_id ? name_verdict(r.name_id, 0) >= 0 : ns_ok;
		if (!ns_ok && !flat_ok)
			return false;
		if (!r.canon_id)
			return true;
		const char *cs = a.c_str(r.canon_id);
		const char *lt = cs ? strchr(cs, '<') : NULL;
		if (!lt)
			return true;
		std::string head(cs, (size_t)(lt - cs));
		while (!head.empty() && head[head.size() - 1] == ' ')
			head.erase(head.size() - 1);
		std::unordered_map<std::string, int>::iterator hi =
			head_memo.find(head);
		if (hi == head_memo.end()) {
			std::unordered_map<std::string, bool>::const_iterator di =
				_mat_filter.declared_bound.find(head);
			int hv = di == _mat_filter.declared_bound.end()
				 ? 0 : (di->second ? 1 : -1);
			hi = head_memo.insert(std::make_pair(head, hv)).first;
		}
		return hi->second >= 0;
	};

	// Admitted set: filter-kept recordable aggregates seed a REFERENCE-PULL
	// closure — every recordable aggregate (and enum) an admitted record
	// reaches through member / base / anon / method-signature chains is
	// admitted too, so a bound record never loses a referent to the filter
	// (the plan's guard; cross-unit references are already inside the bound
	// closure by unit-edge construction, so pulls are the residue, not the
	// rule). Inactive filter = passes below skip the admitted check.
	std::set<uint32_t> admitted;
	std::set<uint32_t> pulled_enums;
	if (_mat_filter.active) {
		std::vector<uint32_t> work;	// admitted aggregates to expand
		std::vector<uint32_t> tq;	// type-ids whose chains to chase
		for (size_t i = 0; i < agg_ids.size(); ++i) {
			uint32_t tid = agg_ids[i];
			if (!recordable.count(tid))
				continue;
			madc::dis::defrec r;
			if (!a.get_def_at(tid, r))
				continue;
			if (record_kept(r)) {
				admitted.insert(tid);
				work.push_back(tid);
			}
		}
		// Every OTHER kept restore surface seeds the chase too: pre-2a its
		// referents always existed (whole-container materialization), and
		// the producer's frozen trees bake in emission names whose shape
		// depends on the full set — dropping std::pmr::string's target
		// changed the consumer's colliding-alias typedef_emit_name choice
		// out from under the frozen trees (packed-suite 8-test regression).
		// Each surface is judged by ITS registration predicate: typedef
		// aliases pull their ref0 chain, free functions their return +
		// param chains, file-scope globals their type chain, template
		// records their owner class.
		for (uint32_t s = 0; s < nslots; ++s) {
			madc::dis::defrec r;
			if (!a.get_def_at(madc::dis::arena_id_of(s), r))
				continue;
			if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
				continue;
			if (r.kind == madc::dis::DK_TYPEDEF) {
				if (name_verdict(r.name_id, r.ns_id) >= 0)
					tq.push_back(r.ref0);
			} else if (r.kind == madc::dis::DK_FUNC
				   && (r.flags & madc::dis::DF_IS_FREE_FUNC)) {
				int v = name_verdict(r.name_id, 0);
				if (v == 0 && r.disp_id && r.ns_id)
					v = name_verdict(r.disp_id, r.ns_id);
				if (v < 0)
					continue;
				tq.push_back(r.ref0);
				for (uint32_t p = 0; p < r.params_count; ++p) {
					madc::dis::paramrec pr;
					if (a.get_payload(r.params_begin, p, pr))
						tq.push_back(pr.type_id);
				}
			}
		}
		for (size_t i = 0; i < _globals.size(); ++i) {
			const cir_forest_global_record &g = _globals[i];
			if (g.gflags & CIR_GLOBALF_TU_ROOT)
				continue;
			uint32_t nl = 0, sl = 0;
			const char *nm = pool_cstr(g.name_id, nl);
			const char *ns = g.ns_id ? pool_cstr(g.ns_id, sl) : NULL;
			if (verdict_str(nm, ns) >= 0)
				tq.push_back(g.type_id);
		}
		for (size_t i = 0; i < _templates.size(); ++i) {
			const cir_forest_template_record &t = _templates[i];
			if ((t.flags & CIR_TMPLF_TU_ROOT) || !t.owner_type_id)
				continue;
			uint32_t kl = 0, sl = 0;
			const char *key = pool_cstr(t.key_id, kl);
			const char *ns = t.ns_id ? pool_cstr(t.ns_id, sl) : NULL;
			if (verdict_str(key, ns) >= 0)
				tq.push_back(t.owner_type_id);
		}
		auto admit_agg = [&](uint32_t tid) {
			if (recordable.count(tid) && !admitted.count(tid)) {
				admitted.insert(tid);
				work.push_back(tid);
			}
		};
		while (!work.empty() || !tq.empty()) {
			if (!tq.empty()) {
				uint32_t tid = tq.back();
				tq.pop_back();
				for (int depth = 0; depth < 64 && tid; ++depth) {
					if (madc::dis::arena_id_is_pinned(tid))
						break;
					madc::dis::defrec r;
					if (!a.get_def_at(tid, r))
						break;
					if (r.kind == madc::dis::DK_PTR
					    || r.kind == madc::dis::DK_REF
					    || r.kind == madc::dis::DK_CONST
					    || r.kind == madc::dis::DK_CARRAY) {
						tid = r.ref0;
						continue;
					}
					if (r.kind == madc::dis::DK_STRUCT
					    || r.kind == madc::dis::DK_UNION
					    || r.kind == madc::dis::DK_CLASS)
						admit_agg(tid);
					else if (r.kind == madc::dis::DK_ENUM)
						pulled_enums.insert(tid);
					else if (r.kind == madc::dis::DK_FPTR) {
						madc::dis::defrec fr;
						if (r.ref0
						    && a.get_def_at(r.ref0, fr)
						    && fr.kind == madc::dis::DK_FUNC) {
							tq.push_back(fr.ref0);
							for (uint32_t p = 0; p < fr.params_count; ++p) {
								madc::dis::paramrec pr;
								if (a.get_payload(fr.params_begin, p, pr))
									tq.push_back(pr.type_id);
							}
						}
					}
					break;
				}
				continue;
			}
			uint32_t tid = work.back();
			work.pop_back();
			madc::dis::defrec r;
			if (!a.get_def_at(tid, r))
				continue;
			for (uint32_t m = 0; m < r.members_count; ++m) {
				madc::dis::memberrec mr;
				if (a.get_payload(r.members_begin, m, mr))
					tq.push_back(mr.type_id);
			}
			for (uint32_t b = 0; b < r.bases_count; ++b) {
				madc::dis::baserec br;
				if (a.get_payload(r.bases_begin, b, br))
					admit_agg(br.base_id);
			}
			for (uint32_t g = 0; g < r.anon_count; ++g) {
				madc::dis::anonrec ar;
				if (a.get_payload(r.anon_begin, g, ar))
					admit_agg(ar.sub_type_id);
			}
			for (uint32_t m = 0; m < r.methods_count; ++m) {
				madc::dis::methodrec mr;
				madc::dis::defrec fr;
				if (!a.get_payload(r.methods_begin, m, mr)
				    || !mr.func_id
				    || !a.get_def_at(mr.func_id, fr)
				    || fr.kind != madc::dis::DK_FUNC)
					continue;
				tq.push_back(fr.ref0);
				for (uint32_t p = 0; p < fr.params_count; ++p) {
					madc::dis::paramrec pr;
					if (a.get_payload(fr.params_begin, p, pr))
						tq.push_back(pr.type_id);
				}
			}
		}
		DBG(std::cout << "materialize filter: " << admitted.size()
			      << " of " << recordable.size()
			      << " recordable aggregates admitted ("
			      << pulled_enums.size() << " enums pulled)"
			      << std::endl);
	}

	// Pass 1: allocate a DataDef per recordable aggregate, so forward member /
	// base ids resolve in pass 2.
	std::map<uint32_t, DataDef *> by_id;
	for (size_t i = 0; i < agg_ids.size(); ++i) {
		uint32_t tid = agg_ids[i];
		if (!recordable.count(tid))
			continue;
		if (_mat_filter.active && !admitted.count(tid))
			continue;
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r))
			continue;
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		if (!nm || !*nm)
			continue;
		// Env-gated probe (MADC_MTI_PROBE_CLASS=<substr>): every pass-1
		// aggregate allocation for a matching name — the duplicate-record
		// diagnostic (two tids for one name = producer identity split).
		{
			static const char *mtp = ::getenv("MADC_MTI_PROBE_CLASS");
			if (mtp && *mtp && strstr(nm, mtp))
				fprintf(stderr, "MTIPROBE pass1 name=%s tid=%u\n",
					nm, tid);
		}
		DataDefSTRUCT *sdd;
		if (r.kind == madc::dis::DK_CLASS)
			sdd = new DataDefCLASS(std::string(nm), r.size,
					       DataType::dtRESERVED);
		else {
			sdd = new DataDefSTRUCT(std::string(nm), r.size);
			sdd->union_layout = (r.flags & madc::dis::DF_UNION_LAYOUT) != 0;
		}
		_mat_storage.push_back(sdd);
		by_id[tid] = sdd;
	}

	// Pass 1a (v21): ENUM types — leaves of the type graph, allocated before
	// the derived fixpoint so pointer/const chains over an enum resolve.
	// Size + canonical spelling load verbatim (a scoped `: size_t` enum is 8
	// bytes, not the ctor's int default).
	for (uint32_t s = 0; s < nslots; ++s) {
		uint32_t tid = madc::dis::arena_id_of(s);
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r) || r.kind != madc::dis::DK_ENUM)
			continue;
		// Rung 2a: an enum outside the bound closure skips unless an
		// admitted record's chain pulled it.
		if (_mat_filter.active && !pulled_enums.count(tid)
		    && !record_kept(r))
			continue;
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		if (!nm || !*nm)
			continue;
		DataDefENUM *edd = new DataDefENUM(std::string(nm));
		if (r.size)
			edd->size = r.size;
		if (r.canon_id)
			if (const char *cn = a.c_str(r.canon_id))
				edd->set_canonical_spelling(cn);
		_mat_storage.push_back(edd);
		by_id[tid] = edd;
	}

	// Pass 1b: derived types (pointer / reference / const / fn-ptr), operand-
	// before-derived fixpoint — chains and the self-referential pointer resolve.
	bool dprog = true;
	while (dprog) {
		dprog = false;
		for (uint32_t s = 0; s < nslots; ++s) {
			uint32_t tid = madc::dis::arena_id_of(s);
			if (by_id.count(tid))
				continue;
			madc::dis::defrec r;
			if (!a.get_def_at(tid, r))
				continue;
			// v22: a DK_FPTR rebuilds its target signature (a
			// declaration-only FuncDef from the DK_FUNC record — return
			// + params through the same swizzle) then DataDefFPTR.
			// A signature type not ready this round retries; one that
			// never resolves cleanly lacks (its dependents drop with
			// the closure diagnostic naming the member).
			if (r.kind == madc::dis::DK_FPTR) {
				madc::dis::defrec fr;
				if (!r.ref0 || !madc::dis::arena_id_is_project(r.ref0)
				    || !a.get_def_at(r.ref0, fr)
				    || fr.kind != madc::dis::DK_FUNC)
					continue;
				DataDef *ret = arena_swizzle(fr.ref0, by_id);
				if (!ret)
					continue;	// not ready this round
				bool pok = true;
				std::vector<DataDef *> ps(fr.params_count);
				for (uint32_t p = 0; p < fr.params_count; ++p) {
					madc::dis::paramrec pr;
					if (!a.get_payload(fr.params_begin, p, pr)
					    || !(ps[p] = arena_swizzle(pr.type_id, by_id))) {
						pok = false;
						break;
					}
				}
				if (!pok)
					continue;	// not ready this round
				FuncDef *tfd = new FuncDef(*ret);
				_mat_storage.push_back(tfd);
				for (uint32_t p = 0; p < fr.params_count; ++p)
					tfd->parameters.push_back(ps[p]);
				tfd->is_varargs =
					(fr.flags & madc::dis::DF_IS_VARARGS) != 0;
				tfd->is_void_params =
					(fr.flags & madc::dis::DF_IS_VOID_PARAMS) != 0;
				tfd->declaration_only = true;
				if (fr.name_id)
					if (const char *fn = a.c_str(fr.name_id))
						tfd->name = fn;
				DataDefFPTR *fpd = new DataDefFPTR(tfd);
				fpd->ptr_syntax =
					(r.flags & madc::dis::DF_FPTR_PTR_SYNTAX) != 0;
				if (r.name_id)
					if (const char *nn = a.c_str(r.name_id))
						fpd->name = nn;
				_mat_storage.push_back(fpd);
				by_id[tid] = fpd;
				dprog = true;
				continue;
			}
			if (r.kind != madc::dis::DK_PTR && r.kind != madc::dis::DK_REF
			    && r.kind != madc::dis::DK_CONST
			    && r.kind != madc::dis::DK_CARRAY)
				continue;
			DataDef *operand = arena_swizzle(r.ref0, by_id);
			if (!operand)
				continue;	// not ready this round (or dropped aggregate)
			DataDef *d;
			if (r.kind == madc::dis::DK_REF)
				d = new DataDefREF(*operand);
			else if (r.kind == madc::dis::DK_CONST)
				d = new DataDefCONST(*operand);
			else if (r.kind == madc::dis::DK_CARRAY) {
				// v25: rebuild the fixed-size array VERBATIM (name +
				// folded count; a runtime-sized array is never recorded).
				uint64_t cnt = (uint64_t)r.carray_count_lo
					     | ((uint64_t)r.carray_count_hi << 32);
				const char *an = r.name_id ? a.c_str(r.name_id) : NULL;
				d = new DataDefCArray(*operand,
						      an ? std::string(an) : operand->name,
						      (size_t)cnt, NULL);
			}
			else
				d = new DataDefPTR(*operand);
			_mat_storage.push_back(d);
			by_id[tid] = d;
			dprog = true;
		}
	}

	// Method-identity sharing (using-decl imports): a methodrec whose
	// Variable name is NOT prefixed by its class's own name is a BASE
	// method imported via a using-declaration (__gnu_cxx::__alloc_traits's
	// `using _Base_type::construct;`). Live pushes the base's Variable —
	// the SAME object — into the importer's methods/method_map; building a
	// fresh FuncDef per methodrec split that identity (the importer's copy
	// missed the member-template pattern hydration, so a bound consumer's
	// tsubst re-resolution routed to the un-hydrated shadow and bailed
	// "calls un-emittable symbol"). Definers build first, keyed by the
	// methodrec's DK_FUNC id (one live FuncDef == one id == one record);
	// importers resolve to the shared object in a post-pass below.
	std::map<uint32_t, Variable *> method_by_func_id;
	struct PendingMethodImport {
		DataDefCLASS *cdd;
		uint32_t      func_id;
		std::string   disp;
		uint32_t      mflags;
	};
	std::vector<PendingMethodImport> pending_method_imports;

	// Pass 2: fill each aggregate VERBATIM (offsets / counts / access /
	// origin / bitfields as stored, no finalize, no re-derivation) — then
	// class extras + methods under the v6 selection rules.
	for (size_t i = 0; i < agg_ids.size(); ++i) {
		uint32_t tid = agg_ids[i];
		std::map<uint32_t, DataDef *>::iterator ai = by_id.find(tid);
		if (ai == by_id.end())
			continue;
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(ai->second);
		if (!sdd)
			continue;
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r))
			continue;
		bool ok = true;
		for (uint32_t m = 0; m < r.members_count; ++m) {
			madc::dis::memberrec mr;
			if (!a.get_payload(r.members_begin, m, mr)) { ok = false; break; }
			DataDef *mdd = arena_swizzle(mr.type_id, by_id);
			const char *mnm = mr.name_id ? a.c_str(mr.name_id) : NULL;
			if (!mdd || !mnm) { ok = false; break; }
			sdd->members.push_back(memberpair_t(std::string(mnm), mdd));
			sdd->member_offsets.push_back(mr.offset);
			sdd->member_counts.push_back(mr.count ? mr.count : 1);
			// v6 parity: array flag derives from count (not the stored bit).
			sdd->member_array_flags.push_back(mr.count > 1);
			sdd->member_access.push_back(mr.access);
			sdd->member_origin.push_back((int)mr.origin);
			DataDefSTRUCT::BitFieldInfo bf;
			if (mr.bf_flags & 1u) {
				bf.is_bitfield     = true;
				bf.is_unsigned     = (mr.bf_flags & 2u) != 0;
				bf.reverse_storage = (mr.bf_flags & 4u) != 0;
				bf.bit_offset      = mr.bf_bit_offset;
				bf.bit_width       = mr.bf_bit_width;
				bf.storage_offset  = mr.bf_storage_offset;
				bf.storage_size    = mr.bf_storage_size;
			}
			sdd->member_bitfields.push_back(bf);
		}
		if (!ok)
			continue;		// defensive: the closure should have dropped it
		sdd->size        = r.size;
		sdd->max_align   = r.max_align ? r.max_align : 1;
		// Verbatim: a restored empty recursion tail stays INCOMPLETE,
		// exactly as live leaves it (everything else here is complete).
		sdd->is_complete = (r.flags & madc::dis::DF_IS_COMPLETE) != 0;
		// A spared CONCRETE forward tag re-stamps its placeholder-ness
		// so the consumer's dependence classification matches a live
		// parse (datadef_involves_placeholder reads it).
		if (r.flags & madc::dis::DF_OPAQUE_TAG)
			if (DataDefCLASS *tagc = dynamic_cast<DataDefCLASS *>(sdd)) {
				tagc->is_dependent_placeholder = true;
				tagc->opaque_concrete_tag = true;
			}
		// v20: canonical C++ spelling — template ARG-SPELLING identity.
		// template_type_arg_spelling reads it to build an instantiation's
		// key fragment ("std::allocator<int32_t>" -> std__allocator_...);
		// left empty, a restored product's spelling fell back to its bare
		// name, the consumer's key diverged from the producer's, and the
		// memo MISSED — the consumer re-instantiated a duplicate product
		// under the wrong name instead of reusing the restored one.
		if (r.canon_id)
			if (const char *cs = a.c_str(r.canon_id))
				sdd->set_canonical_spelling(cs);
		sdd->pack                   = r.pack;
		sdd->tag_explicit_align     = r.tag_explicit_align;
		sdd->is_anonymous           = (r.flags & madc::dis::DF_IS_ANONYMOUS) != 0;
		sdd->reverse_scalar_storage = (r.flags & madc::dis::DF_REVERSE_SCALAR) != 0;
		sdd->has_anon_aggregate     = (r.flags & madc::dis::DF_HAS_ANON_AGG) != 0;
		for (uint32_t g = 0; g < r.anon_count; ++g) {
			madc::dis::anonrec ar;
			if (!a.get_payload(r.anon_begin, g, ar))
				break;
			DataDefSTRUCT *subs = dynamic_cast<DataDefSTRUCT *>(
				arena_swizzle(ar.sub_type_id, by_id));
			if (!subs)
				continue;
			sdd->anonymous_aggregates.push_back(
				DataDefSTRUCT::AnonymousAggregateInfo(
					ar.first_member, ar.member_count, subs,
					ar.offset));
		}
		sdd->type_id = 0;		// re-stamp in the consuming Program

		if (DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(sdd)) {
			// v6 restores class_align = max_align = the saved alignment();
			// the arena stores the raw fields. The consumer-visible
			// alignment() agrees either way (class_align wins when set).
			cdd->class_align        = r.class_align ? r.class_align : 1;
			cdd->nvsize             = r.nvsize;
			cdd->from_system_header = (r.flags & madc::dis::DF_FROM_SYSTEM_HDR) != 0;
			cdd->has_user_ctor      = (r.flags & madc::dis::DF_HAS_USER_CTOR) != 0;
			cdd->has_user_dtor      = (r.flags & madc::dis::DF_HAS_USER_DTOR) != 0;
			// v22 (iostream): the polymorphic-class state, verbatim from
			// the runs the aggregate encoder writes (own_block_off +
			// vbaserec (class_id,offset) + vgrouprec incl. the slot-name
			// id run) — the vgroup owner / vbase classes swizzle through
			// the same by_id as every other cross-ref.
			cdd->has_vtable    = (r.flags & madc::dis::DF_HAS_VTABLE) != 0;
			cdd->has_vptr_slot = (r.flags & madc::dis::DF_HAS_VPTR_SLOT) != 0;
			cdd->own_block_off = r.own_block_off;
			for (uint32_t vb = 0; vb < r.vbase_count; ++vb) {
				madc::dis::vbaserec vr;
				if (!a.get_payload(r.vbase_begin, vb, vr))
					break;
				DataDefCLASS *vbc = dynamic_cast<DataDefCLASS *>(
					arena_swizzle(vr.class_id, by_id));
				if (vbc)
					cdd->vbase_offset[vbc] = vr.offset;
			}
			for (uint32_t vg = 0; vg < r.vgroup_count; ++vg) {
				madc::dis::vgrouprec gr;
				if (!a.get_payload(r.vgroup_begin, vg, gr))
					break;
				DataDefCLASS::VtableGroup g;
				g.owner = dynamic_cast<DataDefCLASS *>(
					arena_swizzle(gr.owner_id, by_id));
				g.this_offset = gr.this_offset;
				g.addr_point  = gr.addr_point;
				for (uint32_t sl = 0; sl < gr.slots_count; ++sl) {
					uint32_t nid = 0;
					if (!a.get_word(gr.slots_begin, sl, nid))
						break;
					const char *sn = a.c_str(nid);
					g.slots.push_back(sn ? sn : "");
				}
				cdd->vtable_groups.push_back(g);
			}
			bool bok = true;
			for (uint32_t b = 0; b < r.bases_count; ++b) {
				madc::dis::baserec br;
				if (!a.get_payload(r.bases_begin, b, br)) { bok = false; break; }
				DataDefCLASS *bc = dynamic_cast<DataDefCLASS *>(
					arena_swizzle(br.base_id, by_id));
				if (!bc) { bok = false; break; }
				BaseSpec bs;
				bs.base       = bc;
				bs.offset     = br.offset;
				bs.is_virtual = (br.flags & madc::dis::BSF_VIRTUAL) != 0;
				bs.is_primary = (br.flags & madc::dis::BSF_PRIMARY) != 0;
				bs.access     = br.flags >> madc::dis::BSF_ACCESS_SHIFT;
				cdd->bases.push_back(bs);
			}
			if (!bok)
				continue;
			cdd->base_class = cdd->bases.empty() ? NULL : cdd->bases[0].base;

			// Methods: the arena run holds EVERY parsed method; apply the
			// v6 save-side selection here so the restored surface matches
			// (symbol-less non-dtor specials, display-less plain methods,
			// and unresolvable signatures all lack). MEMBER-TEMPLATE
			// records restore VERBATIM: the decl-only placeholders at
			// their saved __oN ranks (live registers each pattern as one —
			// register_skipped_class_template_function; the flush hydrates
			// the pattern fields from the CIR_TMPLK_MEMBER tokens) and the
			// bodied instantiations (a loaded _Rb_tree body calls
			// pair(...)__oN directly — without the def the MIR link dies
			// on an undefined import).
			for (uint32_t mi = 0; mi < r.methods_count; ++mi) {
				madc::dis::methodrec md;
				if (!a.get_payload(r.methods_begin, mi, md))
					break;
				if (!md.name_id)
					continue;
				madc::dis::defrec fr;
				if (!madc::dis::arena_id_is_project(md.func_id)
				    || !a.get_def_at(md.func_id, fr)
				    || fr.kind != madc::dis::DK_FUNC)
					continue;
				bool is_mtmpl = (fr.flags & madc::dis::DF_IS_MEMBER_TEMPLATE) != 0;
				bool is_ctor  = (md.flags & madc::dis::MF_CTOR) != 0;
				bool is_dtor  = (md.flags & madc::dis::MF_DTOR) != 0;
				bool m_static = (md.flags & madc::dis::MF_STATIC) != 0;
				const char *dispkey =
					md.disp_key_id ? a.c_str(md.disp_key_id) : NULL;
				// Using-decl import: the Variable name always carries
				// the DEFINING class's prefix (parseFunction's
				// owner->name + "__" parse_id), so a foreign prefix
				// here == a base method shared into this class. Stage
				// it; the post-pass binds the definer's object.
				{
					const char *mnm0 = a.c_str(md.name_id);
					if (mnm0 && *mnm0
					    && strncmp(mnm0, (cdd->name + "__").c_str(),
						       cdd->name.size() + 2) != 0) {
						PendingMethodImport pi;
						pi.cdd     = cdd;
						pi.func_id = md.func_id;
						pi.disp    = dispkey ? dispkey : "";
						pi.mflags  = md.flags;
						pending_method_imports.push_back(pi);
						continue;
					}
				}
				bool is_operator = dispkey
					&& !strncmp(dispkey, "operator", 8);
				bool has_body =
					(fr.flags & madc::dis::DF_HAS_FOREST_BODY) != 0;
				// v27 (the never-ODR-used-producer shape): a symbol-less
				// body-less ctor/operator was SKIPPED here (the v12 rule;
				// v15 lifted it for dtors only). A producer that never
				// uses the class leaves EVERY inline special member in
				// that state — the body rides the v26 DK_DEFBODY token
				// family instead — so the skip emptied cdd->ctors and a
				// consumer's first construction died NO-MATCH. Live
				// registers every parsed method declaration
				// (parseFunction's tail) with the body deferred; the
				// restore now does the same. Members with neither a
				// special-member classification nor a display key still
				// skip — they have no live registration surface.
				if (!is_ctor && !is_dtor && !is_operator
				    && (!dispkey || !*dispkey))
					continue;
				if (!m_static && !fr.params_count)
					continue;	// no __this slot: malformed
				DataDef *ret = arena_swizzle(fr.ref0, by_id);
				if (!ret)
					continue;
				FuncDef *fd = new FuncDef(*ret);
				_mat_storage.push_back(fd);
				if (!m_static) {
					// v6 parity: rebuild __this fresh (a pointer to
					// this class); the stored param 0 flips in at the
					// post-flip widening, not here.
					DataDefPTR *thisp = new DataDefPTR(*cdd);
					_mat_storage.push_back(thisp);
					fd->parameters.push_back(thisp);
				}
				bool pok = true;
				// v23: a param's default-argument token run rides its
				// paramrec (def_tok_*) — collect (index, run) pairs for
				// the flush's parseExpression re-run. Index space ==
				// the stored/restored parameter slot (incl. __this).
				std::vector<std::pair<uint32_t, CirRestoredTemplateRun> >
					def_runs;
				for (uint32_t p = m_static ? 0u : 1u;
				     p < fr.params_count; ++p) {
					madc::dis::paramrec pr;
					if (!a.get_payload(fr.params_begin, p, pr)) {
						pok = false; break;
					}
					DataDef *pd = arena_swizzle(pr.type_id, by_id);
					if (!pd) { pok = false; break; }
					fd->parameters.push_back(pd);
					const uint8_t *db =
						a.tok_run(pr.def_tok_off, pr.def_tok_bytes);
					if (db && pr.def_tok_count) {
						CirRestoredTemplateRun run;
						run.bytes = db;
						run.len   = pr.def_tok_bytes;
						run.count = pr.def_tok_count;
						run.file  = pr.def_file_id
							  ? a.c_str(pr.def_file_id) : NULL;
						def_runs.push_back(std::make_pair(p, run));
					}
				}
				if (!pok)
					continue;
				std::string dispname =
					dispkey ? std::string(dispkey) : std::string();
				fd->method_display_name = dispname;	// v6 parity: the KEY rides here
				if (fr.emit_symbol_id)
					if (const char *es = a.c_str(fr.emit_symbol_id))
						fd->emit_symbol = es;
				fd->is_const_method =
					(fr.flags & madc::dis::DF_IS_CONST_METHOD) != 0;
				fd->is_varargs =
					(fr.flags & madc::dis::DF_IS_VARARGS) != 0;
				fd->is_void_params =
					(fr.flags & madc::dis::DF_IS_VOID_PARAMS) != 0;
				// A member-template record keeps live's flag; the flush
				// hydrates the pattern fields (param names, spellings,
				// retained decl tokens) from its CIR_TMPLK_MEMBER twin,
				// keyed by this funcdef symbol.
				fd->is_member_template = is_mtmpl;
				// v27: the captured body's parse context (see the
				// free-fn arm) — a DEFBODY re-run reproduces it.
				fd->forest_body_in_instantiation =
					(fr.flags & madc::dis::DF_BODY_IN_INSTANTIATION) != 0;
				if (has_body) {
					fd->has_forest_body  = true;
					fd->forest_body_unit = fr.body_unit;
					fd->forest_body_idx  = fr.body_idx;
					fd->declaration_only = false;
				} else {
					fd->declaration_only = true;
				}
				const char *mnm = a.c_str(md.name_id);
				// Live's overload-disambiguation invariant: a method whose
				// registered symbol was disambiguated away from the canonical
				// ClassName__display scheme (the `_un` unary peer, `__oN`
				// type-overloads) carries that symbol on local_emit_name — the
				// call emitter (call_emit_symbol) reads it. Without it a bound
				// `++it` emitted the canonical ClassName__operator++ (an
				// undefined symbol) instead of the restored _un definition.
				if (mnm && !dispname.empty()
				    && std::string(mnm) != cdd->name + "__" + dispname)
					fd->local_emit_name = mnm;
				// A CTOR has no display name; its canonical scheme is
				// ClassName__ClassName (ctor_call_symbol's default). Live
				// stamps a rank-disambiguated ctor's registered symbol
				// (__oN) on local_emit_name at the ctor registration site,
				// and the FuncDef-only ctor emitters (ctor_call_symbol —
				// the shim/global/default-construct paths hold no Variable)
				// read it. Without it a bound shim's converting-ctor call
				// degraded to the canonical symbol — naming whichever ctor
				// holds the canonical rank (basic_string's __sv_wrapper
				// ctor) instead of the selected char* overload.
				else if (mnm && dispname.empty() && is_ctor
				    && std::string(mnm) != cdd->name + "__" + cdd->name)
					fd->local_emit_name = mnm;
				Variable *mv = new Variable(std::string(mnm ? mnm : ""),
							    *fd, 1, NULL, false);
				// Env-gated probe (MADC_MTI_PROBE=<substr>): every
				// methodrec materialization of a matching symbol —
				// the duplicate-class-copy diagnostic.
				{
					static const char *mtp = ::getenv("MADC_MTI_PROBE");
					if (mtp && *mtp && mnm
					    && strstr(mnm, mtp))
						fprintf(stderr, "MTIPROBE matrec sym=%s"
							" mv=%p fd=%p cls=%p(%s) mt=%d"
							" body=%d unit=%u idx=%u"
							" forest=%p\n",
							mnm, (void *)mv, (void *)fd,
							(void *)cdd, cdd->name.c_str(),
							(int)is_mtmpl,
							(int)fd->has_forest_body,
							fd->forest_body_unit,
							fd->forest_body_idx,
							(void *)this);
				}
				if (m_static)
					mv->flags |= vfSTATIC;
				// Live parity (parseFunction's tail): every method
				// Variable carries a Method whose owner_class is the
				// class. findMethodOverload derives the hidden-__this
				// skip from it — with data==NULL every restored
				// overload ranked at the wrong arity and resolution
				// fell to the LAST method_map slot (the
				// append(initializer_list<char>) mispick).
				Method *mm = new Method(*mv);
				mm->owner_class = cdd;
				mv->data = (void *)mm;
				// v26: the Method's NAMED parameter Variables (the
				// aliasrec run on the DK_FUNC record — live parity
				// with parseFunction's temp_param_method scope). A
				// deferred body's re-parse resolves `__n` against
				// these; a run with an unresolvable type cleanly
				// leaves that slot out.
				for (uint32_t ai = 0; ai < fr.alias_count; ++ai) {
					madc::dis::aliasrec ar;
					if (!a.get_payload(fr.alias_begin, ai, ar)
					    || !ar.name_id)
						continue;
					const char *pn = a.c_str(ar.name_id);
					DataDef *ptp = ar.type_id
						     ? arena_swizzle(ar.type_id, by_id)
						     : NULL;
					if (!pn || !*pn || !ptp)
						continue;
					Variable *pv = new Variable(std::string(pn),
								    *ptp, 1, NULL, false);
					pv->flags |= vfPARAM | vfLOCAL;
					_mat_vars.push_back(pv);
					mm->parameters.push_back(pv);
				}
				_mat_vars.push_back(mv);
				cdd->methods.push_back(mv);
				method_by_func_id[md.func_id] = mv;
				if (!dispname.empty())
					cdd->method_map[dispname] = mv;
				if (is_ctor)
					cdd->ctors.push_back(mv);
				// v23: stage the method's default-arg token runs for
				// the flush (parseExpression re-runs inside the
				// owner's class + namespace scope). v24: not for the
				// program's own (fenced) classes — they never register.
				if (!def_runs.empty()
				    && !(r.flags & madc::dis::DF_TU_ROOT_ORIGIN)) {
					CirRestoredFuncDefaults rd;
					rd.fd    = fd;
					rd.owner = cdd;
					rd.ns    = r.ns_id ? a.c_str(r.ns_id) : NULL;
					rd.runs.swap(def_runs);
					_restored_param_defaults.push_back(rd);
				}
			}

			// v20 (widening slice 2): class-scope name maps — type
			// aliases (`typename _Alloc::value_type` resolution reads
			// type_aliases), static member types, and integral
			// static-const values (`X<T>::value`, the integral_constant
			// fold). Loaded VERBATIM; an entry whose type did not
			// restore cleanly lacks (same per-entry discipline as a
			// method with an unresolvable signature).
			for (uint32_t ai = 0; ai < r.alias_count; ++ai) {
				madc::dis::aliasrec ar;
				if (!a.get_payload(r.alias_begin, ai, ar))
					break;
				const char *an = ar.name_id ? a.c_str(ar.name_id) : NULL;
				DataDef *ad = arena_swizzle(ar.type_id, by_id);
				if (!an || !*an || !ad)
					continue;
				// Env-gated probe (MADC_MTI_PROBE_CLASS=<substr>):
				// every restored class-scope type alias whose OWNER
				// matches — the wrong-alias-target diagnostic.
				{
					static const char *mtp =
						::getenv("MADC_MTI_PROBE_CLASS");
					if (mtp && *mtp
					    && cdd->name.find(mtp) != std::string::npos)
						fprintf(stderr, "MTIPROBE restore-alias"
							" owner=%s name=%s tid=%u -> %s\n",
							cdd->name.c_str(), an,
							ar.type_id, ad->name.c_str());
				}
				cdd->type_aliases[an] = ad;
			}
			for (uint32_t si = 0; si < r.statty_count; ++si) {
				madc::dis::aliasrec ar;
				if (!a.get_payload(r.statty_begin, si, ar))
					break;
				const char *an = ar.name_id ? a.c_str(ar.name_id) : NULL;
				DataDef *ad = arena_swizzle(ar.type_id, by_id);
				if (!an || !*an || !ad)
					continue;
				cdd->static_member_types[an] = ad;
			}
			for (uint32_t ci = 0; ci < r.constval_count; ++ci) {
				madc::dis::constvalrec cr;
				if (!a.get_payload(r.constval_begin, ci, cr))
					break;
				const char *an = cr.name_id ? a.c_str(cr.name_id) : NULL;
				if (!an || !*an)
					continue;
				cdd->static_member_const_values[an] =
					(int64_t)(((uint64_t)cr.val_hi << 32) | cr.val_lo);
			}
		}
		if (r.flags & madc::dis::DF_IS_ANONYMOUS)
			continue;	// nameless sub-aggregate: never surfaced standalone
		if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
			continue;	// v24: the program's own type — the forest holds
					// #include state only; the consumer parses it fresh
					// (the object stays in by_id for cross-refs +
					// run-frozen's closure)
		CirRestoredType rt;
		rt.name       = a.c_str(r.name_id);
		rt.kind       = r.kind == madc::dis::DK_CLASS ? CIR_TYPEK_CLASS
			      : (r.kind == madc::dis::DK_UNION ? CIR_TYPEK_UNION
							       : CIR_TYPEK_STRUCT);
		rt.dd         = sdd;
		rt.underlying = NULL;
		rt.ns         = r.ns_id ? a.c_str(r.ns_id) : NULL;
		rt.flat_alias = false;
		rt.tag_alias  = false;
		_restored.push_back(rt);
	}

	// Post-pass: bind the staged using-decl method imports to the definer's
	// built object — the live import handler's exact effect (the importer's
	// methods + first-wins method_map display binding hold the SAME
	// Variable as the base). A definer the selection rules dropped cleanly
	// lacks, like every other unresolvable entry.
	for (size_t i = 0; i < pending_method_imports.size(); ++i) {
		PendingMethodImport &pi = pending_method_imports[i];
		std::map<uint32_t, Variable *>::iterator mi =
			method_by_func_id.find(pi.func_id);
		if (mi == method_by_func_id.end() || !mi->second)
			continue;
		Variable *mv = mi->second;
		bool present = false;
		for (size_t j = 0; j < pi.cdd->methods.size(); ++j)
			if (pi.cdd->methods[j] == mv) { present = true; break; }
		if (!present)
			pi.cdd->methods.push_back(mv);
		if (!pi.disp.empty()
		    && pi.cdd->method_map.find(pi.disp) == pi.cdd->method_map.end())
			pi.cdd->method_map[pi.disp] = mv;
		if (pi.mflags & madc::dis::MF_CTOR)
			pi.cdd->ctors.push_back(mv);
	}

	// Pass 3: DK_TYPEDEF records (flat + namespaced aliases, at their
	// synthetic slots) -> (name, ns, underlying).
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r)
		    || r.kind != madc::dis::DK_TYPEDEF)
			continue;
		if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
			continue;	// v24: the program's own typedef — fenced
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		if (!nm || !*nm)
			continue;
		DataDef *underlying = arena_swizzle(r.ref0, by_id);
		if (!underlying)
			continue;
		CirRestoredType rt;
		rt.name       = nm;
		rt.kind       = CIR_TYPEK_TYPEDEF;
		rt.dd         = NULL;
		rt.underlying = underlying;
		rt.ns         = r.ns_id ? a.c_str(r.ns_id) : NULL;
		rt.flat_alias =
			(r.flags & madc::dis::DF_TYPEDEF_FLAT_ALIAS) != 0;
		rt.tag_alias  =
			(r.flags & madc::dis::DF_TYPEDEF_TAG_ALIAS) != 0;
		_restored.push_back(rt);
	}

	// Pass 3a (v25): namespace-surface records — inline-namespace links
	// (NSLINK: the flush re-runs the live mirror) and using-declaration fn
	// imports (NSBIND: the flush rebinds namespace_map[ns][name] to the
	// restored fn's Variable).
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r))
			continue;
		if (r.kind == madc::dis::DK_NSLINK) {
			CirRestoredNsLink l;
			l.parent = r.ns_id ? a.c_str(r.ns_id) : NULL;
			l.child  = r.name_id ? a.c_str(r.name_id) : NULL;
			if (l.child && *l.child)
				_restored_nslinks.push_back(l);
		} else if (r.kind == madc::dis::DK_NSBIND) {
			CirRestoredNsBind b;
			b.ns   = r.ns_id ? a.c_str(r.ns_id) : NULL;
			b.name = r.name_id ? a.c_str(r.name_id) : NULL;
			b.key  = r.disp_id ? a.c_str(r.disp_id) : NULL;
			if (b.ns && *b.ns && b.name && *b.name && b.key && *b.key)
				_restored_nsbinds.push_back(b);
		} else if (r.kind == madc::dis::DK_DEFBODY) {
			// v26: a deferred method body — four token runs (word
			// quads: off/bytes/count/file_id) at params_begin.
			if (r.params_count != 4 || !r.name_id)
				continue;
			CirRestoredDeferredBody d;
			d.key   = a.c_str(r.name_id);
			d.owner = NULL;
			if (r.ref0) {
				DataDef *od = arena_swizzle(r.ref0, by_id);
				d.owner = od ? dynamic_cast<DataDefCLASS *>(od) : NULL;
				if (!d.owner)
					continue;	// owner dropped -> body cleanly lacks
			}
			d.file   = r.disp_id ? a.c_str(r.disp_id) : NULL;
			d.line   = (int)r.body_unit;
			d.column = (int)r.body_idx;
			d.full_definition =
				(r.flags & madc::dis::DF_DEFBODY_FULL_DEFINITION) != 0;
			bool ok = d.key && *d.key;
			for (int sx = 0; ok && sx < 4; ++sx) {
				uint32_t off = r.params_begin + (uint32_t)sx * 4u;
				if (off + 4 > a.payload_words) { ok = false; break; }
				CirRestoredTemplateRun &run = d.runs[sx];
				run.bytes = NULL;
				run.len   = a.payload[off + 1];
				run.count = a.payload[off + 2];
				run.file  = a.payload[off + 3]
					  ? a.c_str(a.payload[off + 3]) : NULL;
				if (run.count)
					run.bytes = a.tok_run(a.payload[off], run.len);
			}
			if (ok)
				_restored_defbodies.push_back(d);
		}
	}

	// Pass 3b (v21): DK_ENUM records -> (name, ns, dd, enumerators). The
	// DataDefENUM was allocated in pass 1a; the scoped enumerator values ride
	// the record's constvalrec run and rebuild as constant Variables in the
	// tag's pseudo-namespace on the parser side.
	for (uint32_t s = 0; s < nslots; ++s) {
		uint32_t tid = madc::dis::arena_id_of(s);
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r) || r.kind != madc::dis::DK_ENUM)
			continue;
		if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
			continue;	// v24: the program's own enum — fenced
		std::map<uint32_t, DataDef *>::iterator ei = by_id.find(tid);
		if (ei == by_id.end())
			continue;
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		if (!nm || !*nm)
			continue;
		CirRestoredType rt;
		rt.name       = nm;
		rt.kind       = CIR_TYPEK_ENUM;
		rt.dd         = ei->second;
		rt.underlying = NULL;
		rt.ns         = r.ns_id ? a.c_str(r.ns_id) : NULL;
		rt.flat_alias = false;
		rt.tag_alias  = false;
		for (uint32_t c = 0; c < r.constval_count; ++c) {
			madc::dis::constvalrec cv;
			if (!a.get_payload(r.constval_begin, c, cv))
				break;
			const char *en = cv.name_id ? a.c_str(cv.name_id) : NULL;
			if (!en || !*en)
				continue;
			int64_t val = (int64_t)(((uint64_t)cv.val_hi << 32)
					      | (uint64_t)cv.val_lo);
			rt.enumerators.push_back(std::make_pair(en, val));
		}
		_restored.push_back(rt);
	}

	// RC2: restore file-scope FREE-FUNCTION declarations — every DK_FUNC
	// flagged DF_IS_FREE_FUNC reconstructs as a declaration-only FuncDef
	// (return + ALL params swizzled through the same by_id; a free function
	// has no hidden __this, so the params run is consumed whole). A signature
	// the arena cannot resolve cleanly LACKS — the bound call then keeps
	// today's dlsym-fallback behavior, exactly like every partial-restore
	// case. Template / member-template PATTERNS were filtered at record time;
	// v21 additionally restores the skipped-ns-fn-template PLACEHOLDERS
	// (declaration-only + function_display_name + namespace_name — the
	// resolution chokepoint a qualified `std::_Destroy(...)` call needs).
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r)
		    || r.kind != madc::dis::DK_FUNC
		    || !(r.flags & madc::dis::DF_IS_FREE_FUNC))
			continue;
		if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
			continue;	// v24: the program's own prototype — fenced
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		if (!nm || !*nm)
			continue;
		// Rung 2a: mirror registration's free-function predicate — the
		// funcdef_map key decides; a key in NO unit's index refines by
		// the ns::display source form (flush does the same); still
		// unindexed = derived entity = keep.
		if (_mat_filter.active) {
			int v = name_verdict(r.name_id, 0);
			if (v == 0 && r.disp_id && r.ns_id)
				v = name_verdict(r.disp_id, r.ns_id);
			if (v < 0)
				continue;
		}
		// v20: a BODIED free function (an instantiated __mti / __ns_*__oN
		// definition) restores WITH its forest body — the loaded method
		// bodies call its symbol, and the m&l fixpoint materializes the
		// definition exactly like a bound method's. One that got NO body
		// location (a producer root like main, or a non-system origin)
		// cleanly lacks — a consumer never inherits the producer's roots.
		bool was_bodied = (r.flags & madc::dis::DF_WAS_BODIED) != 0;
		bool has_body   = (r.flags & madc::dis::DF_HAS_FOREST_BODY) != 0;
		// v26 piece (a): a bodied fn with NO translated def but WITH a
		// captured raw-body run (an ownerless DK_DEFBODY twin) restores
		// its DECLARATION — the flush plants the deferred body and the
		// m&l fixpoint materializes it on first ODR-use.
		if (was_bodied && !has_body
		    && !(r.flags & madc::dis::DF_FUNC_DEF_TOKENS))
			continue;
		DataDef *ret = arena_swizzle(r.ref0, by_id);
		if (!ret)
			continue;
		FuncDef *fd = new FuncDef(*ret);
		_mat_storage.push_back(fd);
		bool pok = true;
		// v23: default-arg token runs (see the method loop above).
		std::vector<std::pair<uint32_t, CirRestoredTemplateRun> > def_runs;
		for (uint32_t p = 0; p < r.params_count; ++p) {
			madc::dis::paramrec pr;
			if (!a.get_payload(r.params_begin, p, pr)) {
				pok = false; break;
			}
			DataDef *pd = arena_swizzle(pr.type_id, by_id);
			if (!pd) { pok = false; break; }
			fd->parameters.push_back(pd);
			// v21: the C++ param spelling rides the record — the
			// Itanium mangle of a declaration-only ns function
			// (storage_alias_name at flush) reads it.
			const char *ps = pr.cpp_spelling_id
				       ? a.c_str(pr.cpp_spelling_id) : NULL;
			fd->param_cpp_spellings.push_back(ps ? ps : "");
			const uint8_t *db = a.tok_run(pr.def_tok_off, pr.def_tok_bytes);
			if (db && pr.def_tok_count) {
				CirRestoredTemplateRun run;
				run.bytes = db;
				run.len   = pr.def_tok_bytes;
				run.count = pr.def_tok_count;
				run.file  = pr.def_file_id ? a.c_str(pr.def_file_id) : NULL;
				def_runs.push_back(std::make_pair(p, run));
			}
		}
		if (!pok)
			continue;
		if (r.emit_symbol_id)
			if (const char *es = a.c_str(r.emit_symbol_id))
				fd->emit_symbol = es;
		fd->is_varargs       = (r.flags & madc::dis::DF_IS_VARARGS) != 0;
		fd->is_void_params   = (r.flags & madc::dis::DF_IS_VOID_PARAMS) != 0;
		// v21: the free-function source identity + FuncDef-intrinsic
		// template state the live registration sets. On a DF_IS_FREE_FUNC
		// record disp_id carries function_display_name; ns_id carries
		// namespace_name. A skipped-ns-fn-template placeholder needs these
		// for its namespace_map binding + overload-set seed (flush side);
		// an instantiated __oN definition carries them in live state too.
		if (r.disp_id)
			if (const char *dn = a.c_str(r.disp_id))
				fd->function_display_name = dn;
		if (r.ns_id)
			if (const char *nn = a.c_str(r.ns_id))
				fd->namespace_name = nn;
		if (r.builtin_kind_id)
			if (const char *bk = a.c_str(r.builtin_kind_id))
				fd->inline_builtin_kind = bk;
		if (r.tret_name_id)
			if (const char *tn = a.c_str(r.tret_name_id))
				fd->template_return_param_name = tn;
		fd->template_return_deduce_arg_index   = (int)r.tret_arg_index;
		fd->template_return_deduce_from_pointer =
			(r.flags & madc::dis::DF_TRET_FROM_POINTER) != 0;
		fd->template_return_ref =
			(r.flags & madc::dis::DF_TRET_REF) != 0;
		if (has_body) {
			fd->has_forest_body  = true;
			fd->forest_body_unit = r.body_unit;
			fd->forest_body_idx  = r.body_idx;
			fd->declaration_only = false;
		} else if (r.flags & madc::dis::DF_FUNC_DEF_TOKENS) {
			// v26 piece (a): the body arrives via the ownerless
			// DK_DEFBODY entry the flush plants — live's
			// bodied-DEFERRED state (parseFunction parsed a body;
			// materialization waits for first ODR-use). NOT
			// declaration-only: the flush's Itanium
			// storage_alias_name stamp applies only to concrete
			// declaration-only ns fns, and stamping it here made a
			// bound call go MANGLED-DIRECT to a header-inline fn
			// with no .so export (std::stod → undefined MIR
			// import) instead of materializing the DEFBODY.
			fd->declaration_only = false;
			// v27: reproduce the body's original parse context.
			fd->forest_body_in_instantiation =
				(r.flags & madc::dis::DF_BODY_IN_INSTANTIATION) != 0;
		} else {
			fd->declaration_only = true;
		}
		CirRestoredFunc rf;
		rf.name = nm;
		rf.fd   = fd;
		// v26 piece (a): the fn's NAMED parameters (aliasrec run) — the
		// flush fills Method::parameters so a deferred body's re-parse
		// resolves its parameter names.
		for (uint32_t ai = 0; ai < r.alias_count; ++ai) {
			madc::dis::aliasrec ar;
			if (!a.get_payload(r.alias_begin, ai, ar) || !ar.name_id)
				continue;
			const char *pn = a.c_str(ar.name_id);
			DataDef *ptp = ar.type_id
				     ? arena_swizzle(ar.type_id, by_id) : NULL;
			if (!pn || !*pn || !ptp)
				continue;
			rf.mparams.push_back(std::make_pair(pn, ptp));
		}
		_restored_funcs.push_back(rf);
		if (!def_runs.empty()) {
			CirRestoredFuncDefaults rd;
			rd.fd    = fd;
			rd.owner = NULL;
			rd.ns    = r.ns_id ? a.c_str(r.ns_id) : NULL;
			rd.runs.swap(def_runs);
			_restored_param_defaults.push_back(rd);
		}
	}

	// v13/v14: restore file-scope global VARIABLE definitions — swizzle each
	// record's type_id back to a DataDef* through the arena reconstruct (the
	// same pinned-or-by_id dispatch as a member / base / param reference; the
	// globals themselves stay on the CIR_GLOBALS segment). forest_restore_decls
	// rebuilds a Variable + dkGlobalVar TopDecl from each, so the existing
	// passes emit the global + queue its ctor into __madc_global_init (class)
	// or emit its constant data item (scalar init_value).
	for (size_t i = 0; i < _globals.size(); ++i) {
		const cir_forest_global_record &g = _globals[i];
		if (g.gflags & CIR_GLOBALF_TU_ROOT)
			continue;		// v24: the program's own global — fenced
		uint32_t nlen = 0;
		const char *nm = pool_cstr(g.name_id, nlen);
		DataDef *ty = arena_swizzle(g.type_id, by_id);
		if (!nm || !ty)
			continue;		// unresolved type -> bind cleanly lacks it
		CirRestoredGlobal rg;
		rg.name       = nm;
		rg.type       = ty;
		rg.flags      = g.flags;
		rg.gflags     = g.gflags;
		uint32_t nslen = 0;
		rg.ns         = g.ns_id ? pool_cstr(g.ns_id, nslen) : NULL;	// v22
		rg.init_value = g.init_value;
		// v25: the ctor-args raw-token run (arena tokbytes span).
		rg.ctor_bytes = NULL;
		rg.ctor_len   = 0;
		rg.ctor_count = 0;
		rg.ctor_file  = NULL;
		if ((g.gflags & CIR_GLOBALF_CTOR_ARG_TOKENS) && g.ctor_tok_count) {
			rg.ctor_bytes = a.tok_run(g.ctor_tok_off, g.ctor_tok_bytes);
			rg.ctor_len   = g.ctor_tok_bytes;
			rg.ctor_count = g.ctor_tok_count;
			rg.ctor_file  = g.ctor_file_id ? a.c_str(g.ctor_file_id) : NULL;
		}
		_restored_globals.push_back(rg);
	}

	// v20 (widening slice 2): build the restored TEMPLATE-NAME view — names
	// resolved against the container pool, the owner class swizzled through
	// the same by_id, each token run exposed as a span into the loaded
	// TEMPLATE_TOKENS bytes. Token materialization happens on the parser side
	// (forest_restore_decls), where the active spelling pool + intern_file
	// live. A record whose owner class did not restore cleanly LACKS (a
	// member-template pattern without its class cannot instantiate anyway).
	{
		auto run_at = [&](uint32_t word_off) -> CirRestoredTemplateRun {
			CirRestoredTemplateRun rr;
			rr.bytes = NULL;
			rr.len = 0;
			rr.count = 0;
			rr.file = NULL;
			cir_forest_token_run tr;
			if (!madc::dis::pod_read(_template_payload, word_off, tr))
				return rr;
			if (tr.tok_count
			    && (size_t)tr.tok_off + tr.tok_bytes <= _template_tokens.size()) {
				rr.bytes = _template_tokens.data() + tr.tok_off;
				rr.len   = tr.tok_bytes;
				rr.count = tr.tok_count;
			}
			if (tr.file_id) {
				uint32_t flen = 0;
				rr.file = pool_cstr(tr.file_id, flen);
			}
			return rr;
		};
		const size_t pw = madc::dis::pod_words<cir_forest_template_param>();
		const size_t rw = madc::dis::pod_words<cir_forest_token_run>();
		for (size_t i = 0; i < _templates.size(); ++i) {
			const cir_forest_template_record &t = _templates[i];
			if (t.flags & CIR_TMPLF_TU_ROOT)
				continue;	// v24: the program's own pattern — fenced
			// v24: a pattern owned by a FENCED class (a member template
			// of the program's own class) rides its owner's fence.
			if (t.owner_type_id) {
				madc::dis::defrec od;
				if (a.get_def_at(t.owner_type_id, od)
				    && (od.flags & madc::dis::DF_TU_ROOT_ORIGIN))
					continue;
			}
			uint32_t klen = 0;
			const char *key = pool_cstr(t.key_id, klen);
			if (!key || !*key)
				continue;
			CirRestoredTemplate rt;
			rt.kind  = t.kind;
			rt.key   = key;
			rt.name  = t.name_id ? pool_str(t.name_id) : NULL;
			rt.ns    = t.ns_id ? pool_str(t.ns_id) : NULL;
			rt.extra = t.extra_id ? pool_str(t.extra_id) : NULL;
			rt.owner = NULL;
			rt.flags = t.flags;
			if (t.owner_type_id) {
				DataDefCLASS *oc = dynamic_cast<DataDefCLASS *>(
					arena_swizzle(t.owner_type_id, by_id));
				if (!oc)
					continue;	// owner did not restore -> cleanly lack
				rt.owner = oc;
			}
			bool ok = true;
			for (uint32_t p = 0; p < t.param_count; ++p) {
				cir_forest_template_param pr;
				if (!madc::dis::pod_read(_template_payload,
							 t.param_begin + p * pw, pr)) {
					ok = false; break;
				}
				uint32_t plen = 0;
				const char *pn = pool_cstr(pr.name_id, plen);
				if (!pn) { ok = false; break; }
				rt.params.push_back(std::make_pair(pn, pr.pflags));
			}
			if (!ok)
				continue;
			// Positional run table: body, constraint, per-param
			// defaults, per-slot spec patterns.
			uint32_t ro = t.run_begin;
			rt.body       = run_at(ro); ro += (uint32_t)rw;
			rt.constraint = run_at(ro); ro += (uint32_t)rw;
			for (uint32_t p = 0; p < t.param_count; ++p) {
				rt.defaults.push_back(run_at(ro));
				ro += (uint32_t)rw;
			}
			for (uint32_t sp = 0; sp < t.spec_count; ++sp) {
				rt.spec.push_back(run_at(ro));
				ro += (uint32_t)rw;
			}
			_restored_templates.push_back(rt);
		}
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
	bool records_columnar =
		madc::dis::snap_xform_id(recs->flags)
			== madc::dis::SNAP_XFORM_BYTEPLANE
		&& madc::dis::snap_xform_param(recs->flags)
			== sizeof(cir_frozen_record);
	bool records_ok = records_columnar
		? _reader.read_segment_transformed(*recs, rb)
		: _reader.read_segment(*recs, rb);
	if (!records_ok || !_reader.read_segment(*kids, kb)
	    || !_reader.read_segment(*conn, cb) || !_reader.read_segment(*poss, pb)
	    || rb.size() % sizeof(cir_frozen_record) || kb.size() % sizeof(uint32_t)
	    || cb.size() % sizeof(uint64_t) || pb.size() % sizeof(cir_frozen_pos)) {
		fprintf(stderr, "madc: forest unit %u payload corrupt\n", unit);
		return NULL;
	}

	cir_forest_unit fu;
	fu.unit_name_id = _units[unit].unit_name_id;
	size_t record_count = rb.size() / sizeof(cir_frozen_record);
	if (!records_columnar)
		fu.blob.records.resize(record_count);
	fu.blob.children.resize(kb.size() / sizeof(uint32_t));
	fu.connectors.resize(cb.size() / sizeof(uint64_t));
	fu.positions.resize(pb.size() / sizeof(cir_frozen_pos));
	if (!records_columnar && !rb.empty())
		memcpy(fu.blob.records.data(), rb.data(), rb.size());
	if (!kb.empty()) memcpy(fu.blob.children.data(), kb.data(), kb.size());
	if (!cb.empty()) memcpy(fu.connectors.data(), cb.data(), cb.size());
	if (!pb.empty()) memcpy(fu.positions.data(), pb.data(), pb.size());

	bool ok = record_count == _units[unit].record_count
	       && fu.connectors.size() == _units[unit].connector_count
	       && fu.positions.size() == record_count;
	const uint8_t *nchildren_planes[sizeof(uint32_t)] = {};
	const uint8_t *child_base_planes[sizeof(uint64_t)] = {};
	if (records_columnar && record_count) {
		const uint8_t *plane_data = rb.data();
		for (size_t b = 0; b < sizeof(uint32_t); ++b)
			nchildren_planes[b] = plane_data
				+ (offsetof(cir_frozen_record, nchildren) + b)
				  * record_count;
		for (size_t b = 0; b < sizeof(uint64_t); ++b)
			child_base_planes[b] = plane_data
				+ (offsetof(cir_frozen_record, child_base) + b)
				  * record_count;
	}
	for (size_t i = 0; ok && i < record_count; ++i) {
		uint32_t nchildren = 0;
		uint64_t child_base = 0;
		if (records_columnar) {
			uint8_t *nchildren_bytes =
				reinterpret_cast<uint8_t *>(&nchildren);
			uint8_t *child_base_bytes =
				reinterpret_cast<uint8_t *>(&child_base);
			for (size_t b = 0; b < sizeof(nchildren); ++b)
				nchildren_bytes[b] = nchildren_planes[b][i];
			for (size_t b = 0; b < sizeof(child_base); ++b)
				child_base_bytes[b] = child_base_planes[b][i];
		} else {
			const cir_frozen_record &rec = fu.blob.records[i];
			nchildren = rec.nchildren;
			child_base = rec.child_base;
		}
		if (!ok || child_base > fu.blob.children.size()
		    || nchildren > fu.blob.children.size() - child_base) {
			ok = false;
			break;
		}
		for (uint32_t k = 0; ok && k < nchildren; ++k) {
			uint32_t ci = fu.blob.children[child_base + k];
			if (ci & CIR_FROZEN_CHILD_CONNECTOR_BIT)
				ok = (ci & ~CIR_FROZEN_CHILD_CONNECTOR_BIT)
				     < fu.connectors.size();
			else
				ok = ci < record_count;
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
		    unit, record_count));
	if (records_columnar)
		_segs[unit] = new CirFrozenSegment(std::move(fu), std::move(rb),
						   record_count, this, _c2m);
	else
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
