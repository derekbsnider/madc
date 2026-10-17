/* cir_freeze.cpp — freeze/thaw a cir_node sub-DAG through the madc::dis
 * pool-snapshot container (forest Phase 2+3 / data-substrate Track B2+B3).
 * See cir_freeze.h for the contract, the forest format, and the closure
 * story.
 */

#include <algorithm>	// std::stable_sort — the directory's sorted name index
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>	// unique_ptr — the cache's lazy BLOCKED-segment readers
#include <mutex>	// the process-level decoded-segment cache's guard
#include <queue>	// madc.h uses std::queue (included below) and relies on the TU providing it
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <sys/time.h>
#ifdef __APPLE__
#include <mach-o/getsect.h>	// self-image forest carrier: __MADC,__forest section
#include <mach-o/dyld.h>	// _dyld_get_image_header (the main executable's header)
#endif
#include "madc_posix_io.h"	// map_file_readonly — the one file-mapping owner
				// (also why no <sys/mman.h> here: MIR's
				// mir-code-alloc.h redefines MAP_FAILED, which
				// used to force an #undef + (void *)-1 compare)

extern thread_local bool madc_verbose;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"	// Variable (Phase 6 3d: reconstruct method Variables on load)
#include "madc.h"		// FuncDef (Phase 6 3d: reconstruct method FuncDefs on load)
#include "madc_sys_includes.h"	// stdlib flavor table (v34 config-word: flavor is config identity)
#include "token_arena.h"
#include "madcdis/id_table.h"
#include "cir_freeze.h"
#include "cir_arena_recordable.h"

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
// bind (compare). A container parsed under one language standard / -D set /
// stdlib FLAVOR must never bind into a compile under another — header
// CONTENT differs (C vs C++ surface, __STRICT_ANSI__, feature macros; a
// libstdc++-parsed corpus bound into a -stdlib=libc++ compile served the
// wrong <stddef.h> and tripped libc++'s <cstddef> #error). Per-compile
// state, so it cannot ride the process-invariant context hash above.
uint32_t madc_forest_config_word(const Program *prog)
{
	uint32_t word = (uint32_t)prog->language_std
		      | ((uint32_t)(prog->posix_compat_enabled() ? 1 : 0) << 15)
		      | ((uint32_t)(prog->gnu_dialect ? 1 : 0) << 16);
	// The EFFECTIVE flavor name, not the CLI spelling: an explicit
	// -stdlib=<build default> is the same config as no flag at all.
	const char *flavor = prog->stdlib_flavor && prog->stdlib_flavor->name
			   ? prog->stdlib_flavor->name
			   : madc_default_stdlib_flavor;
	if (flavor) {
		uint32_t h = 2166136261u;	// FNV-1a, folded into bits 17..31
		for (const char *c = flavor; *c; ++c) {
			h ^= (uint8_t)*c;
			h *= 16777619u;
		}
		word ^= ((h ^ (h >> 15)) & 0x7fffu) << 17;
	}
	return word;
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
			       | (n->tsubst_pack_expand ? CIR_FROZEN_PACK_EXPAND : 0)
			       | (n->tsubst_arg_uncoerced ? CIR_FROZEN_ARG_UNCOERCED : 0);
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
				   bool by_unit, cir_frozen_forest &out,
				   cir_unit_owner_fn owner_override = NULL,
				   void *owner_ctx = NULL,
				   const cir_freeze_placement *placement = NULL)
{
	if (!root)
		return false;
	out.units.clear();
	out.libs.clear();

	std::map<cir_node *, cir_freeze_loc> where;
	std::vector<std::vector<cir_node *> > order;
	std::map<madc::dis::istring, uint32_t> unit_by_name;
	std::vector<madc::dis::istring> unit_names;

	// Unit for a source path (by_unit mode), creating it on first sight.
	// Unit 0 is always the main unit so readers see a stable convention.
	auto unit_for = [&](const char *fname) -> uint32_t {
		madc::dis::istring key(fname);
		std::map<madc::dis::istring, uint32_t>::iterator it = unit_by_name.find(key);
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

	// A node's unit is its origin token's source file — unless the owner
	// override reassigns it (a __need protocol serving's nodes belong to
	// the INCLUDER's unit; see cir_unit_owner_fn in cir_freeze.h).
	auto node_unit_file = [&](cir_node *n) -> const char * {
		if (placement) {
			std::map<const cir_node *, const char *>::const_iterator pi =
				placement->unit_of.find(n);
			if (pi != placement->unit_of.end() && pi->second)
				return pi->second;
		}
		if (owner_override) {
			const char *own = owner_override(owner_ctx, n->origin_id);
			if (own)
				return own;
		}
		return n->src_file();
	};

	// Pass 1: discovery.
	{
		const char *rf = by_unit ? node_unit_file(root) : NULL;
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
				const char *cf = node_unit_file(c);
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

	// v66: report the nodes the caller asked to locate.
	out.located.clear();
	if (placement)
		for (std::set<const cir_node *>::const_iterator li =
			 placement->locate.begin();
		     li != placement->locate.end(); ++li) {
			std::map<cir_node *, cir_freeze_loc>::const_iterator wi =
				where.find(const_cast<cir_node *>(*li));
			if (wi != where.end())
				out.located[*li] = std::make_pair(wi->second.unit,
								  wi->second.idx);
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
		std::set<madc::dis::istring> seen_externs;
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
		       cir_frozen_forest &out,
		       cir_unit_owner_fn owner_override, void *owner_ctx,
		       const cir_freeze_placement *placement)
{
	if (!TokenBase::_active_strpool) {
		fprintf(stderr, "madc internal: cir forest freeze with no"
			" active string pool\n");
		return false;
	}
	return cir_freeze_partitioned(root, main_unit_name ? main_unit_name
							   : "<main>",
				      true, out, owner_override, owner_ctx,
				      placement);
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
// intern BYTES (v59: blocked, decoded on first touch; the offsets block
// stays raw — 4 bytes an id, read in place). Dev/standalone freezes keep the
// spine raw so their binds stay zero-copy (the drain-ladder loop's instant
// rebinds).
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
// A unit that carries nothing but (possibly) its source text: no records,
// tokens, declarations, PP exports, edges or branch dependencies.
static bool forest_unit_source_only(const cir_forest_unit &fu)
{
	return fu.blob.records.empty() && fu.blob.children.empty()
	    && fu.connectors.empty() && fu.positions.empty()
	    && fu.token_payload.empty() && fu.decl_index.empty()
	    && fu.pp_events.empty() && fu.edges.empty()
	    && fu.branch_deps.empty();
}

// The SHARED tables a bind reads record by record (both string pools' bytes,
// the arena's defs / payload / token bytes, the global and template record
// tables, the template payload and token bytes) are stored BLOCKED when
// compressed
// (pack-slices design §3): fixed-size blocks, each one frame, decoded on
// first touch through the lazy views — a bind decodes the blocks holding
// what it reads, never the whole table.
static bool forest_kind_blocked(uint32_t kind)
{
	switch (kind) {
	case madc::dis::SNAP_KIND_INTERN_BYTES:
	case SNAP_KIND_CIR_ARENA_DEFS:
	case SNAP_KIND_CIR_ARENA_PAYLOAD:
	case SNAP_KIND_CIR_ARENA_TOKBYTES:
	case SNAP_KIND_CIR_TEMPLATE_PAYLOAD:
	case SNAP_KIND_CIR_TEMPLATE_TOKENS:
	case SNAP_KIND_CIR_GLOBALS:
	case SNAP_KIND_CIR_TEMPLATES:
	// A unit's node records, child entries, connectors and positions
	// (format 61): a bind materializes a few hundred of a unit's records —
	// 2,121 of the 569,504 its 16 node units hold for cxx_stl.cpp — so
	// each is read block by block too.
	case SNAP_KIND_CIR_RECORDS:
	case SNAP_KIND_CIR_CHILDREN:
	case SNAP_KIND_CIR_CONNECTORS:
	case SNAP_KIND_CIR_POSITIONS:
		return true;
	default:
		return false;
	}
}

// log2 of the shared tables' block size: 16 KB. Measured 2026-10-13 on the
// packed corpus — 4 KB blocks cost +0.67 MB (12,785,936 bytes) and made the
// 8-header C workload slower (more blocks touched, each with frame overhead).
// A node unit's tables (format 61) use 128 KB: a bind's reads of them cluster
// (cxx_stl.cpp decodes 96 of the 3,381 16 KB blocks of the 16 units it
// loads), so the block size moves the pack, not the bind — measured
// 2026-10-14 on the release, 16 / 64 / 128 KB: pack 14.61 / 13.65 / 13.35 MB,
// cxx_stl 1,436.4M / 1,442.0M / 1,447.2M Ir at the same wall time (owner:
// 128 KB, pack cap 15 MB).
enum : uint32_t { FOREST_BLOCK_SHIFT = 14, FOREST_UNIT_BLOCK_SHIFT = 17 };

// A blocked kind's flags: its block size, and its inner transform — node
// records keep their byte-plane recoding and child entries their delta, now
// inside each block.
static uint32_t forest_blocked_flags(uint32_t kind)
{
	static_assert(sizeof(cir_frozen_record) <= 0xff,
		      "a blocked byte-plane stride is one byte (snap_blocked_flags)");
	switch (kind) {
	case SNAP_KIND_CIR_RECORDS:
		return madc::dis::snap_blocked_flags(FOREST_UNIT_BLOCK_SHIFT,
						     madc::dis::SNAP_XFORM_BYTEPLANE,
						     (uint32_t)sizeof(cir_frozen_record));
	case SNAP_KIND_CIR_CHILDREN:
		return madc::dis::snap_blocked_flags(FOREST_UNIT_BLOCK_SHIFT,
						     madc::dis::SNAP_XFORM_DELTA32);
	case SNAP_KIND_CIR_CONNECTORS:
	case SNAP_KIND_CIR_POSITIONS:
		return madc::dis::snap_blocked_flags(FOREST_UNIT_BLOCK_SHIFT);
	default:
		return madc::dis::snap_blocked_flags(FOREST_BLOCK_SHIFT);
	}
}

static bool add_seg(madc::dis::snapshot_writer &w, uint32_t seg_id,
		    uint32_t kind, const void *bytes, size_t len,
		    PchCompression codec)
{
	if (!cir_forest_compress_intern
	    && kind == madc::dis::SNAP_KIND_INTERN_BYTES)
		codec = PchCompression::None;
	// The transforms serve the codec (a delta or a byte plane compresses
	// better); a payload stored with no codec keeps its own bytes, so a bind
	// reads it in place (a raw role's container, B1).
	uint32_t xform = 0;
	if (codec == PchCompression::None)
		xform = 0;
	else if (forest_kind_blocked(kind))
		xform = forest_blocked_flags(kind);
	else if (kind == SNAP_KIND_CIR_CHILDREN)
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
	madc::dis::decode_bytes rbytes, kbytes;
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
	// v57: the name index — the unit names as text, so a lookup reads the
	// directory alone (see cir_forest_dir_header).
	const size_t nunits = f.units.size();
	std::vector<uint32_t> name_off(nunits + 1, 0);
	for (size_t u = 0; u < nunits; ++u)
		name_off[u + 1] = name_off[u]
				+ pool->length(f.units[u].unit_name_id) + 1;
	std::vector<uint32_t> sorted(nunits);
	for (size_t u = 0; u < nunits; ++u)
		sorted[u] = (uint32_t)u;
	std::stable_sort(sorted.begin(), sorted.end(),
			 [&](uint32_t a, uint32_t b) {
		return strcmp(pool->c_str(f.units[a].unit_name_id),
			      pool->c_str(f.units[b].unit_name_id)) < 0;
	});
	size_t off = dir.size();
	dir.resize(off + (2 * nunits + 1) * sizeof(uint32_t) + name_off[nunits]);
	memcpy(dir.data() + off, name_off.data(), (nunits + 1) * sizeof(uint32_t));
	off += (nunits + 1) * sizeof(uint32_t);
	memcpy(dir.data() + off, sorted.data(), nunits * sizeof(uint32_t));
	off += nunits * sizeof(uint32_t);
	for (size_t u = 0; u < nunits; ++u) {
		const uint32_t id = f.units[u].unit_name_id;
		memcpy(dir.data() + off + name_off[u], pool->c_str(id),
		       pool->length(id));
		dir[off + name_off[u + 1] - 1] = 0;
	}

	w.set_context_hash(madc_cir_context_hash());
	// Uncompressed: the reader binds the index in place in the mapped image.
	if (!add_seg(w, CIR_FOREST_SEG_DIR, SNAP_KIND_CIR_FOREST_DIR,
		     dir.data(), dir.size(), PchCompression::None))
		return false;
	// B1: the profile's role — written only for a role other than the
	// system-header profile's, which stays exactly the container it was.
	if (f.role != CIR_FOREST_ROLE_SYSTEM
	    && !add_seg(w, CIR_FOREST_SEG_ROLE, SNAP_KIND_CIR_ROLE,
			&f.role, sizeof(f.role), PchCompression::None))
		return false;
	// v57: the bare function names every unit's decl index declares (kind
	// pdkFunction; a qualified form is C++ surface), sorted and unique —
	// count, count + 1 offsets, NUL-terminated bytes; read in place.
	// v62: only the ones the adoption probe can adopt (PACK_DECL_ADOPTABLE).
	std::vector<const char *> fns;
	for (size_t u = 0; u < nunits; ++u)
		for (const cir_forest_decl_entry &e : f.units[u].decl_index) {
			const char *nm = pool->c_str(e.name_id);
			if (e.kind == Program::pdkFunction
			    && (e.aux & Program::PACK_DECL_ADOPTABLE)
			    && *nm && !strchr(nm, ':'))
				fns.push_back(nm);
		}
	std::sort(fns.begin(), fns.end(), [](const char *a, const char *b) {
		return strcmp(a, b) < 0;
	});
	fns.erase(std::unique(fns.begin(), fns.end(), [](const char *a, const char *b) {
		return strcmp(a, b) == 0;
	}), fns.end());
	std::vector<uint8_t> fn_index((fns.size() + 2) * sizeof(uint32_t));
	uint32_t fn_at = 0;
	const uint32_t fn_count = (uint32_t)fns.size();
	memcpy(fn_index.data(), &fn_count, sizeof(fn_count));
	for (size_t i = 0; i <= fns.size(); ++i) {
		memcpy(fn_index.data() + (i + 1) * sizeof(uint32_t), &fn_at, sizeof(fn_at));
		if (i < fns.size())
			fn_at += (uint32_t)strlen(fns[i]) + 1;
	}
	for (size_t i = 0; i < fns.size(); ++i)
		fn_index.insert(fn_index.end(), fns[i], fns[i] + strlen(fns[i]) + 1);
	if (!add_seg(w, CIR_FOREST_SEG_FUNC_NAMES, SNAP_KIND_CIR_FUNC_NAMES,
		     fn_index.data(), fn_index.size(), PchCompression::None))
		return false;
	// v58: the seed index (cir_forest_build_seeds; absent on a freeze that
	// records no decl index — the bind then walks the whole container).
	// v59: stored raw and read in place — every bind consults it first.
	if (!f.decl_names.empty()
	    && !add_seg(w, CIR_FOREST_SEG_DECL_NAMES, SNAP_KIND_CIR_DECL_NAMES,
			f.decl_names.data(), f.decl_names.size(), PchCompression::None))
		return false;
	if (!f.seeds.empty()
	    && !add_seg(w, CIR_FOREST_SEG_SEEDS, SNAP_KIND_CIR_SEEDS,
			f.seeds.data(), f.seeds.size() * sizeof(uint32_t),
			PchCompression::None))
		return false;
	if (!f.recordable.empty()
	    && !add_seg(w, CIR_FOREST_SEG_RECORDABLE, SNAP_KIND_CIR_RECORDABLE,
			f.recordable.data(), f.recordable.size() * sizeof(uint32_t),
			PchCompression::None))
		return false;
	// The container's string pool: its records and (v59) the raw offsets
	// block (intern_table::frozen_blocks; v60 records carry their length).
	std::vector<char> pool_bytes, arena_bytes;
	std::vector<uint32_t> pool_offs, arena_offs;
	pool->frozen_blocks(pool_bytes, pool_offs);
	f.arena.strings.frozen_blocks(arena_bytes, arena_offs);
	if (!add_seg(w, CIR_FOREST_SEG_STR_BYTES, madc::dis::SNAP_KIND_INTERN_BYTES,
		     pool_bytes.data(), pool_bytes.size(), codec)
	    || !add_seg(w, CIR_FOREST_SEG_STR_OFFSETS, madc::dis::SNAP_KIND_INTERN_OFFSETS,
			pool_offs.data(), pool_offs.size() * sizeof(uint32_t),
			PchCompression::None))
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
			arena_bytes.data(), arena_bytes.size(), codec)
	    || !add_seg(w, CIR_FOREST_SEG_ARENA_STR_OFFSETS, madc::dis::SNAP_KIND_INTERN_OFFSETS,
			arena_offs.data(), arena_offs.size() * sizeof(uint32_t),
			PchCompression::None))
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
			f.template_tokens.data(), f.template_tokens.size(), codec))
		return false;
	// v65: the extern-decl index as a ForestDeclNames table — each symbol's
	// run is its (unit, idx) — stored raw and read in place (extern_loc_for).
	// The freeze recorded each symbol's first decl only.
	std::map<madc::dis::istring, std::vector<uint32_t> > extern_names;
	for (const cir_forest_extern_loc &xl : f.extern_locs) {
		const char *nm = pool->c_str(xl.name_id);
		if (!nm || !*nm)
			continue;
		std::vector<uint32_t> &run = extern_names[madc::dis::istring(nm)];
		if (run.empty()) {
			run.push_back(xl.unit);
			run.push_back(xl.idx);
		}
	}
	std::vector<uint8_t> extern_index;
	ForestDeclNames::build(extern_names, extern_index);
	if (!add_seg(w, CIR_FOREST_SEG_EXTERN_LOCS, SNAP_KIND_CIR_EXTERN_LOCS,
		     extern_index.data(), extern_index.size(), PchCompression::None))
		return false;
	// v2 container-global payloads (zero-length when the freeze recorded
	// nothing — a module-only freeze). Every open reads both and they are
	// small, so they are stored raw: no decode on open.
	if (!add_seg(w, CIR_FOREST_SEG_BRANCH_MACROS, SNAP_KIND_CIR_BRANCH_MACROS,
		     f.branch_macros.data(),
		     f.branch_macros.size() * sizeof(uint32_t), PchCompression::None)
	    || !add_seg(w, CIR_FOREST_SEG_CANON_ORDER, SNAP_KIND_CIR_CANON_ORDER,
			f.canon_order.data(),
			f.canon_order.size() * sizeof(uint32_t), PchCompression::None))
		return false;

	for (size_t u = 0; u < f.units.size(); ++u) {
		const cir_forest_unit &fu = f.units[u];
		uint32_t base = CIR_FOREST_SEG_UNIT_BASE
			      + (uint32_t)u * CIR_FOREST_SEGS_PER_UNIT;
		// v56: a SOURCE-ONLY unit (the pack's source arm, a dedup-skipped
		// first include) writes its source slot alone. An absent slot
		// reads as "no payload" everywhere such a unit is asked about,
		// and it is never bound (unit_has_frozen_surface) — while nine
		// empty slots per header made every container open index ten
		// times the segments.
		if (forest_unit_source_only(fu)) {
			if (!add_seg(w, base + 9, SNAP_KIND_CIR_UNIT_SOURCE,
				     fu.source_payload.data(),
				     fu.source_payload.size(), codec))
				return false;
			continue;
		}
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
				fu.edges.size() * sizeof(uint32_t), codec)
		    || !add_seg(w, base + 8, SNAP_KIND_CIR_BRANCH_DEPS,
				fu.branch_deps.data(),
				fu.branch_deps.size() * sizeof(uint32_t), codec)
		    || !add_seg(w, base + 9, SNAP_KIND_CIR_UNIT_SOURCE,
				fu.source_payload.data(),
				fu.source_payload.size(), codec))
			return false;
	}
	return true;
}

// Mach-O FILE carrier probe: a signed Mach-O cannot carry an EOF trailer,
// so a packed image (a hosted madc binary, or a --pack-forest emitted
// executable) holds the container in its __MADC,__forest section. This is
// the FILE arm — pure byte parsing of the mapped image, host-neutral (the
// Linux cross madcs verify emitted Mach-O images with --dump-forest); the
// RUNNING self-image on darwin is served by getsectiondata below (slid,
// zero-copy). Little-endian MH_MAGIC_64 only — exactly what the fork
// writer and the hosted toolchain produce. False = not a Mach-O or no
// section; the caller falls through to the EOF-footer probe.
static bool cir_macho_find_forest(const uint8_t *m, size_t sz,
				  const void *&image, size_t &len)
{
	// Local values, not the <mach-o/loader.h> names: on darwin hosted
	// builds that header is already included above and defines them as
	// macros, which would clobber any same-named declaration here.
	const uint32_t macho_magic_64 = 0xfeedfacfu;
	const uint32_t macho_lc_segment_64 = 0x19u;
	if (sz < 32)
		return false;
	uint32_t magic, ncmds, sizeofcmds;
	memcpy(&magic, m, 4);
	if (magic != macho_magic_64)
		return false;
	memcpy(&ncmds, m + 16, 4);
	memcpy(&sizeofcmds, m + 20, 4);
	if ((uint64_t)32 + sizeofcmds > sz)
		return false;
	size_t off = 32;
	for (uint32_t c = 0; c < ncmds; c++) {
		if (off + 8 > 32 + (size_t)sizeofcmds)
			return false;
		uint32_t cmd, cmdsize;
		memcpy(&cmd, m + off, 4);
		memcpy(&cmdsize, m + off + 4, 4);
		if (cmdsize < 8 || off + cmdsize > 32 + (size_t)sizeofcmds)
			return false;
		if (cmd == macho_lc_segment_64 && cmdsize >= 72
		    && memcmp(m + off + 8, "__MADC\0\0\0\0\0\0\0\0\0\0", 16) == 0) {
			uint32_t nsects;
			memcpy(&nsects, m + off + 64, 4);
			if ((uint64_t)72 + (uint64_t)nsects * 80 > cmdsize)
				return false;
			for (uint32_t s = 0; s < nsects; s++) {
				const uint8_t *sec = m + off + 72 + (size_t)s * 80;
				if (memcmp(sec, "__forest\0\0\0\0\0\0\0\0", 16) != 0)
					continue;
				uint64_t ssize;
				uint32_t soff;
				memcpy(&ssize, sec + 40, 8);
				memcpy(&soff, sec + 48, 4);
				if (ssize == 0 || soff >= sz || ssize > sz - soff)
					return false;
				image = m + soff;
				len = (size_t)ssize;
				return true;
			}
		}
		off += cmdsize;
	}
	return false;
}

bool cir_forest_map_image(const char *path, const void *&image, size_t &len)
{
	madc::dis::istring selfpath;
	if (!path) {
#ifdef __APPLE__
		// Darwin self-image carrier: signed Mach-O forbids appended
		// bytes (the file must end exactly at the code signature), so
		// the forest rides a __MADC,__forest section placed at link
		// time (-sectcreate) or by the post-link packer. The section
		// data IS the container (footer at its end), already mapped
		// and slid in the running image — zero-copy, process-lifetime.
		// The probe reads the main executable's image (dyld's image 0),
		// whether the engine is linked into it or loaded from
		// libmadc-0.dylib: the link-time _mh_execute_header exists only
		// in an executable, so the dylib could not name it. The
		// forest-in-dylib shape gets its own dladdr-based arm in the
		// carriers track S4.
		unsigned long seclen = 0;
		uint8_t *sec = getsectiondata(
			(const struct mach_header_64 *)_dyld_get_image_header(0),
			"__MADC", "__forest", &seclen);
		if (sec && seclen) {
			image = sec;
			len = (size_t)seclen;
			return true;
		}
		// No section: fall through to the file probe (an unpacked dev
		// binary silently fails the footer check -> live parse).
#endif
		selfpath = madc_self_exe_path();
		if (selfpath.empty())
			return false;
		path = selfpath.c_str();
	}
	// Process-level mapping cache: ONE read-only mapping per carrier file.
	// The mappings were already deliberately never munmap'd (thawed
	// segments read string bytes and pool blocks from them for the
	// process lifetime) — but each Program re-mapping the same file
	// multiplied identical page tables AND gave the same blob a
	// different base address per TU, defeating the decoded-segment
	// cache's (blob base, offset) keys. Mutex-guarded; entries are
	// immutable once inserted (the thread contract the decode cache
	// states).
	static std::mutex map_mu;
	static std::map<madc::dis::istring, std::pair<void *, size_t> > map_cache;
	void *m = NULL;
	size_t maplen = 0;
	{
		std::lock_guard<std::mutex> lk(map_mu);
		std::map<madc::dis::istring, std::pair<void *, size_t> >::iterator mi =
			map_cache.find(path);
		if (mi == map_cache.end()) {
			m = madc::detail::map_file_readonly(path, maplen);
			if (!m)
				return false;
			mi = map_cache.emplace(madc::dis::istring(path),
					       std::make_pair(m, maplen)).first;
		}
		m = mi->second.first;
		maplen = mi->second.second;
	}
	// Mach-O file: the container is the __MADC,__forest section slice
	// (page-aligned in file, so 16-aligned payload offsets hold), not an
	// EOF trailer. Anything else keeps the whole file (footer at EOF).
	if (cir_macho_find_forest((const uint8_t *)m, maplen, image, len))
		return true;
	image = m;
	len = maplen;
	return true;
}

// ---------------------------------------------------------------------------
// Thaw: CirFrozenSegment
// ---------------------------------------------------------------------------

CirFrozenSegment::CirFrozenSegment(cir_frozen_blob &&blob, c2m_ctx_t c2m)
	: _blob(std::move(blob)), _record_count(_blob.records.size()),
	  _forest(NULL), _c2m(c2m), _unit(0)
{
	_mat.resize((_record_count + MAT_CHUNK - 1) >> MAT_CHUNK_SHIFT);
	_seg = madc_cir_register_segment(this);
}

CirFrozenSegment::CirFrozenSegment(uint32_t unit, const Table &recs, const Table &kids,
				   const Table &conns, const Table &poss,
				   CirFrozenForest *forest, c2m_ctx_t c2m)
	: _recs(recs), _kids(kids), _conns(conns), _poss(poss),
	  _record_count(recs.n), _forest(forest), _c2m(c2m), _unit(unit)
{
	_mat.resize((_record_count + MAT_CHUNK - 1) >> MAT_CHUNK_SHIFT);
	_seg = madc_cir_register_segment(this);
}

CirFrozenSegment::~CirFrozenSegment()
{
	madc_cir_unregister_segment(_seg);
}

void CirFrozenSegment::begin_compile(c2m_ctx_t c2m)
{
	_c2m = c2m;
	_mat.clear();
	_mat.resize((_record_count + MAT_CHUNK - 1) >> MAT_CHUNK_SHIFT);
}

void CirFrozenSegment::set_mat(uint32_t idx, cir_node *cn)
{
	std::unique_ptr<cir_node *[]> &c = _mat[idx >> MAT_CHUNK_SHIFT];
	if (!c)
		c.reset(new cir_node *[MAT_CHUNK]());
	c[idx & (MAT_CHUNK - 1)] = cn;
}

size_t CirFrozenSegment::materialized_count() const
{
	size_t n = 0;
	for (size_t c = 0; c < _mat.size(); ++c)
		if (_mat[c])
			for (uint32_t i = 0; i < MAT_CHUNK; ++i)
				if (_mat[c][i])
					++n;
	return n;
}

void CirFrozenSegment::block_counts(size_t &decoded, size_t &total) const
{
	decoded = total = 0;
	const Table *t[4] = { &_recs, &_kids, &_conns, &_poss };
	for (int k = 0; k < 4; ++k)
		if (t[k]->lz) {
			decoded += t[k]->lz->blocks_decoded();
			total += t[k]->lz->block_count();
		}
}

// Element i of a forest unit's table: its block decoded on first touch.
template <class T>
static bool frozen_table_read(const CirFrozenSegment::Table &t, uint64_t i, T &out)
{
	if (i >= t.n)
		return false;
	const size_t off = (size_t)i * sizeof(T);
	if (!madc::dis::lazy_ensure(t.lz, off, sizeof(T)))
		return false;
	memcpy(&out, t.p + off, sizeof(T));
	return true;
}

bool CirFrozenSegment::record_at(uint32_t idx, cir_frozen_record &out) const
{
	if (_forest)
		return frozen_table_read(_recs, idx, out);
	if (idx >= _record_count)
		return false;
	out = _blob.records[idx];
	return true;
}

bool CirFrozenSegment::child_entry(uint64_t i, uint32_t &out) const
{
	if (_forest)
		return frozen_table_read(_kids, i, out);
	if (i >= _blob.children.size())
		return false;
	out = _blob.children[(size_t)i];
	return true;
}

bool CirFrozenSegment::connector_at(uint32_t i, uint64_t &out) const
{
	return _forest && frozen_table_read(_conns, i, out);
}

bool CirFrozenSegment::position_at(uint32_t idx, cir_frozen_pos &out) const
{
	return _forest && frozen_table_read(_poss, idx, out);
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
	cn->tsubst_arg_uncoerced = (r.flags & CIR_FROZEN_ARG_UNCOERCED) != 0;
	cn->self.seg = _seg;
	cn->self.idx = idx;

	// Position: forest units read the side-car (valid without the freezing
	// process's token arena); standalone segments derive it from the origin
	// token exactly as CirBuilder::make does for a live build.
	cir_frozen_pos p;
	if (_forest) {
		if (position_at(idx, p) && p.fname_id) {
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

	set_mat(idx, cn);
	return cn;
}

// Resolve a child entry to its owning segment + record index, bounds checked
// (a corrupt entry fails the resolve, never reads wild). A connector entry
// crossing into a not-yet-loaded unit loads it here (forest SETTLED #6:
// load-on-demand falls out of reference resolution).
static bool cir_child_target(CirFrozenSegment *s, uint32_t entry,
			     CirFrozenSegment *&cs, uint32_t &cidx)
{
	if (!(entry & CIR_FROZEN_CHILD_CONNECTOR_BIT)) {
		cs = s;
		cidx = entry;
		return cidx < s->record_count();
	}
	CirFrozenForest *forest = s->forest();
	uint64_t conn = 0;
	if (!forest || !s->connector(entry & ~CIR_FROZEN_CHILD_CONNECTOR_BIT, conn))
		return false;	// standalone blobs have no connector pool
	cs = forest->unit_segment((uint32_t)(conn >> 32));
	cidx = (uint32_t)conn;
	return cs != NULL && cidx < cs->record_count();
}

cir_node *CirFrozenSegment::resolve(CirFrozenSegment *s0, uint32_t idx0)
{
	if (idx0 >= s0->record_count())
		return NULL;
	if (cir_node *done = s0->mat(idx0))
		return done;

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
			uint32_t entry = 0, cidx = 0;
			if (!s->child_entry(r.child_base + k, entry)
			    || !cir_child_target(s, entry, cs, cidx)) {
				fprintf(stderr, "madc: forest unit %u record %u failed validation\n",
					s->_unit, created[w].second);
				return NULL;
			}
			if (!cs->mat(cidx)) {
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
		cir_node *parent = s->mat(created[w].second);
		for (uint32_t k = 0; k < r.nchildren; ++k) {
			CirFrozenSegment *cs = NULL;
			uint32_t entry = 0, cidx = 0;
			s->child_entry(r.child_base + k, entry);	// read in phase A:
			cir_child_target(s, entry, cs, cidx);		// cannot fail now
			c2mir_op_append(s->_c2m, parent->as_node(),
					cs->mat(cidx)->as_node());
		}
	}

	return s0->mat(idx0);
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

void CirFrozenForest::begin_compile(c2m_ctx_t c2m)
{
	_c2m = c2m;
	for (size_t u = 0; u < _segs.size(); ++u)
		if (_segs[u])
			_segs[u]->begin_compile(c2m);
}

// The --forest-touch-log sink (record_touches): the first read of each
// (segment, offset) becomes one "seg off len" line, flushed as written — the
// recording bind may end without tearing the forest down. Only the pack
// step's recording binds create one; it serializes its own state (a bind may
// read from worker threads).
class ForestTouchRecorder : public madc::dis::lazy_touch_sink
{
	FILE *_f;
	std::mutex _mu;
	std::map<const madc::dis::lazy_blocks *, uint32_t> _seg;
	std::set<uint64_t> _seen;
public:
	explicit ForestTouchRecorder(FILE *f) : _f(f) {}
	~ForestTouchRecorder()
	{
		for (std::map<const madc::dis::lazy_blocks *, uint32_t>::const_iterator it =
		     _seg.begin(); it != _seg.end(); ++it)
			it->first->observe(NULL);
		fclose(_f);
	}
	void attach(uint32_t seg_id, const madc::dis::lazy_blocks *lz)
	{
		{
			std::lock_guard<std::mutex> g(_mu);
			if (!_seg.insert(std::make_pair(lz, seg_id)).second)
				return;
		}
		lz->observe(this);
	}
	void touched(const madc::dis::lazy_blocks *span, size_t off,
		     size_t len) override
	{
		std::lock_guard<std::mutex> g(_mu);
		std::map<const madc::dis::lazy_blocks *, uint32_t>::const_iterator it =
			_seg.find(span);
		if (it == _seg.end()
		    || !_seen.insert(((uint64_t)it->second << 40) | off).second)
			return;
		fprintf(_f, "%u %zu %zu\n", it->second, off, len);
		fflush(_f);
	}
};

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
	delete _touch;
}

// Bind one pool block: in place from the image when stored uncompressed
// (the zero-copy path the per-segment codec field exists for), otherwise
// decompressed into `own`.
// Process-level decoded-segment cache (S1, the JIT Python-contender plan,
// docs/plans/2026-08-21-project-prelude-forest.md). Forest images are
// process-lifetime mappings (cir_forest_map_image deliberately never
// munmaps), and a compressed container-global segment decodes IDENTICALLY
// for every Program that opens the same blob — so the owned-buffer zstd
// decode happens ONCE per process per segment instead of once per TU
// Program (an 11-TU project launch paid the ~14-frame / 11.6 MB global
// decode eleven times; --show-stats now shows the decode on the FIRST
// Program only, which is the truth). Keyed (blob base, segment offset):
// profile stacks append several blobs to one image, so the blob base — not
// the image base — identifies a directory's segments.
//
// Thread contract (thread-safety.md): the map is mutex-guarded; a cached
// buffer is IMMUTABLE after insertion and never evicted (process
// lifetime), so readers hold const pointers into it across threads safely
// (the C++ stdlib convention). The entries deliberately outlive every
// CirFrozenForest — materialized state pointing into them can never
// dangle.
//
// A BLOCKED segment caches its lazy reader instead (snapshot_lazy_segment —
// the same process lifetime and immutability, per block): blocks decode on
// first touch through the views that hold it.
struct ForestCachedSegment
{
	madc::dis::decode_bytes bytes;		// a whole decode (unblocked)
	std::unique_ptr<madc::dis::snapshot_lazy_segment> lazy;	// BLOCKED
};

static const ForestCachedSegment *forest_decoded_segment_cached(
	const madc::dis::snapshot_reader &r,
	const madc::dis::snapshot_segment &s)
{
	static std::mutex mu;
	static std::map<std::pair<const void *, uint64_t>,
			ForestCachedSegment> cache;
	std::lock_guard<std::mutex> lk(mu);
	std::pair<const void *, uint64_t> key(r.blob_base(), s.offset);
	std::map<std::pair<const void *, uint64_t>,
		 ForestCachedSegment>::iterator it = cache.find(key);
	bool miss = it == cache.end();
	if (miss) {
		ForestCachedSegment c;
		if (madc::dis::snap_xform_id(s.flags)
		    == madc::dis::SNAP_XFORM_BLOCKED) {
			c.lazy.reset(r.open_lazy(s));
			if (!c.lazy)
				return NULL;
		} else if (!r.read_segment(s, c.bytes))
			return NULL;
		it = cache.emplace(key, std::move(c)).first;
	}
	// Env-gated probe (MADC_FOREST_CACHE_PROBE=1): per-segment hit/miss
	// evidence — the observable for "N Programs, one decode".
	if (::getenv("MADC_FOREST_CACHE_PROBE"))
		fprintf(stderr, "[forest-cache] %s seg=%u off=%llu raw=%llu%s\n",
			miss ? "MISS" : "hit", s.seg_id,
			(unsigned long long)s.offset,
			(unsigned long long)s.raw_size,
			it->second.lazy ? " blocked" : "");
	return &it->second;
}

// The one segment-bytes resolver: zero-copy from the mapped image when the
// segment is stored raw (no codec, no transform), the process-level decoded
// cache otherwise. Returned bytes live for the PROCESS (image mapping or
// cache entry — both outlive every CirFrozenForest), so consumers bind spans
// instead of owning per-forest copies. A BLOCKED segment hands its lazy
// reader to a caller that passes `lz` (the bytes are then valid only as it
// ensures them); a caller that does not gets every block decoded.
static const uint8_t *forest_segment_bytes(const madc::dis::snapshot_reader &r,
					   const madc::dis::snapshot_segment &s,
					   size_t &len,
					   const madc::dis::lazy_blocks **lz = NULL)
{
	len = (size_t)s.raw_size;
	if (lz)
		*lz = NULL;
	if (const uint8_t *p = r.raw_ptr(s))
		return p;
	const ForestCachedSegment *c = forest_decoded_segment_cached(r, s);
	if (!c)
		return NULL;
	if (!c->lazy)
		return c->bytes.data();
	if (lz)
		*lz = c->lazy.get();
	else if (!c->lazy->ensure(0, len))
		return NULL;
	return c->lazy->data();
}

static const uint8_t *forest_pool_block(const madc::dis::snapshot_reader &r,
					uint32_t seg_id, uint32_t kind,
					size_t &len,
					const madc::dis::lazy_blocks **lz)
{
	const madc::dis::snapshot_segment *s = r.find(seg_id);
	if (!s || s->kind != kind)
		return NULL;
	return forest_segment_bytes(r, *s, len, lz);
}

// --show-stats wall clock (R0 startup instrumentation).
static double forest_now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

bool CirFrozenForest::open(const void *image, size_t len, c2m_ctx_t c2m,
			   bool quiet_missing)
{
	if (!open_header(image, len, quiet_missing))
		return false;
	return complete_open(c2m);
}

// R1 stage 1: footer + context-hash pin + directory only. Cheap (one small
// segment decode) — the bind path config-gates on language_std()/
// defines_hash() after this and skips complete_open() on a mismatch.
bool CirFrozenForest::open_header(const void *image, size_t len,
				  bool quiet_missing,
				  madc::dis::snapshot_reader *opened)
{
	double _t0 = forest_now();
	if (opened)
		_reader.adopt(*opened);
	else if (!_reader.open(image, len)) {
		if (!quiet_missing)
			fprintf(stderr, "madc: no forest container found\n");
		return false;
	}
	_image = image;
	_image_len = len;
	if (_reader.context_hash() != madc_cir_context_hash()) {
		fprintf(stderr, "madc: frozen forest was built by a different"
			" madc (context-hash mismatch) — re-freeze it\n");
		return false;
	}

	// Directory: bound in place in the mapped image (written uncompressed,
	// v57); a decoded copy in _dir_bytes otherwise, which then owns the bytes
	// the name index points into.
	const madc::dis::snapshot_segment *ds = _reader.find(CIR_FOREST_SEG_DIR);
	const uint8_t *dir = NULL;
	size_t dir_len = 0;
	if (ds && ds->kind == SNAP_KIND_CIR_FOREST_DIR) {
		dir = _reader.raw_ptr(*ds);
		dir_len = ds->raw_size;
		if (!dir && _reader.read_segment(*ds, _dir_bytes)) {
			dir = _dir_bytes.data();
			dir_len = _dir_bytes.size();
		}
	}
	if (!dir || dir_len < sizeof(cir_forest_dir_header)) {
		fprintf(stderr, "madc: forest directory missing or corrupt\n");
		return false;
	}
	cir_forest_dir_header hdr;
	memcpy(&hdr, dir, sizeof(hdr));
	const size_t fixed = sizeof(hdr)
			   + (size_t)hdr.unit_count * sizeof(cir_forest_dir_unit)
			   + (size_t)hdr.lib_count * sizeof(uint32_t);
	const size_t index = (2 * (size_t)hdr.unit_count + 1) * sizeof(uint32_t);
	if (hdr.version != CIR_FOREST_FORMAT_VERSION || hdr.unit_count == 0
	    || dir_len < fixed + index
	    || !bind_name_index(dir + fixed, hdr.unit_count,
				dir_len - fixed - index)) {
		fprintf(stderr, "madc: forest directory malformed\n");
		return false;
	}
	_units.resize(hdr.unit_count);
	memcpy(_units.data(), dir + sizeof(hdr),
	       hdr.unit_count * sizeof(cir_forest_dir_unit));
	_language_std = hdr.language_std;	// v27 producer config (bind gate)
	_defines_hash = hdr.defines_hash;
	// The configs a pack proved this profile also serves: directory-level
	// state like the header's own pair, since the bind gate reads it before
	// complete_open.
	_shared_configs.clear();
	if (const madc::dis::snapshot_segment *ss = _reader.find(CIR_FOREST_SEG_SHARED_CONFIGS)) {
		madc::dis::decode_bytes shared;
		if (ss->kind != SNAP_KIND_CIR_SHARED_CONFIGS
		    || !_reader.read_segment(*ss, shared)
		    || shared.size() % sizeof(cir_forest_shared_config) != 0) {
			fprintf(stderr, "madc: forest shared-config labels malformed\n");
			return false;
		}
		_shared_configs.resize(shared.size() / sizeof(cir_forest_shared_config));
		if (!_shared_configs.empty())
			memcpy(_shared_configs.data(), shared.data(), shared.size());
	}
	// B1: the role, directory-level state too (a profile walk judges it
	// before complete_open).
	if (!cir_forest_read_role(_reader, _role)) {
		fprintf(stderr, "madc: forest role malformed\n");
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

	// Required-library name IDS (the link-environment closure) — the names
	// resolve through the container pool in complete_open.
	const uint8_t *libp = dir + sizeof(hdr)
			    + hdr.unit_count * sizeof(cir_forest_dir_unit);
	for (uint32_t i = 0; i < hdr.lib_count; ++i) {
		uint32_t id;
		memcpy(&id, libp + i * sizeof(uint32_t), sizeof(uint32_t));
		_lib_ids.push_back(id);
	}

	_header_opened = true;
	_stat_open_secs += forest_now() - _t0;
	return true;
}

bool CirFrozenForest::serves_config(uint32_t config, uint32_t defines) const
{
	if (config == _language_std && defines == _defines_hash)
		return true;
	for (const cir_forest_shared_config &c : _shared_configs)
		if (c.language_std == config && c.defines_hash == defines)
			return true;
	return false;
}

// --- the pack layout (format 60; design doc §3) --------------------------
//
// The tables the layout pass reorders, decoded whole: the two string pools
// (bytes + offsets block) and the arena records with their positions (slot
// order when the container carries no ARENA_DEF_POS).
struct ForestLayoutTables
{
	struct Pool
	{
		uint32_t bytes_seg, offs_seg;
		std::vector<uint8_t> bytes;
		std::vector<uint32_t> offs;	// ids + 1 (the closing size)
		size_t ids() const { return offs.empty() ? 0 : offs.size() - 1; }
	};
	Pool pools[2];
	std::vector<uint8_t> defs;
	std::vector<uint32_t> pos;	// slot -> record position
	size_t record_bytes = 0;
	size_t slots() const { return record_bytes ? defs.size() / record_bytes : 0; }
};

static bool forest_layout_seg(uint32_t seg_id)
{
	return seg_id == CIR_FOREST_SEG_STR_BYTES || seg_id == CIR_FOREST_SEG_STR_OFFSETS
	    || seg_id == CIR_FOREST_SEG_ARENA_STR_BYTES
	    || seg_id == CIR_FOREST_SEG_ARENA_STR_OFFSETS
	    || seg_id == CIR_FOREST_SEG_ARENA_DEFS;
}

static bool forest_read_u32s(const madc::dis::snapshot_reader &r, uint32_t seg_id,
			     std::vector<uint32_t> &out)
{
	const madc::dis::snapshot_segment *s = r.find(seg_id);
	std::vector<uint8_t> b;
	if (!s || !r.read_segment(*s, b) || b.size() % sizeof(uint32_t))
		return false;
	out.resize(b.size() / sizeof(uint32_t));
	if (!b.empty())
		memcpy(out.data(), b.data(), b.size());
	return true;
}

static bool forest_read_layout_tables(const madc::dis::snapshot_reader &r,
				      ForestLayoutTables &t)
{
	t.pools[0].bytes_seg = CIR_FOREST_SEG_STR_BYTES;
	t.pools[0].offs_seg = CIR_FOREST_SEG_STR_OFFSETS;
	t.pools[1].bytes_seg = CIR_FOREST_SEG_ARENA_STR_BYTES;
	t.pools[1].offs_seg = CIR_FOREST_SEG_ARENA_STR_OFFSETS;
	for (ForestLayoutTables::Pool &p : t.pools) {
		const madc::dis::snapshot_segment *bs = r.find(p.bytes_seg);
		if (!bs || !r.read_segment(*bs, p.bytes)
		    || !forest_read_u32s(r, p.offs_seg, p.offs) || p.offs.empty())
			return false;
	}
	const madc::dis::snapshot_segment *ds = r.find(CIR_FOREST_SEG_ARENA_DEFS);
	if (!ds || !r.read_segment(*ds, t.defs))
		return false;
	t.record_bytes = madc::dis::DefArena::def_stride() * sizeof(uint32_t);
	if (t.defs.size() % t.record_bytes)
		return false;
	if (r.find(CIR_FOREST_SEG_ARENA_DEF_POS)) {
		if (!forest_read_u32s(r, CIR_FOREST_SEG_ARENA_DEF_POS, t.pos)
		    || t.pos.size() != t.slots())
			return false;
		for (size_t i = 0; i < t.pos.size(); ++i)
			if (t.pos[i] >= t.slots())
				return false;
	} else {
		t.pos.resize(t.slots());
		for (size_t i = 0; i < t.pos.size(); ++i)
			t.pos[i] = (uint32_t)i;
	}
	return true;
}

// The byte length of id's record in a pool (its LEB128 length, the spelling,
// the NUL); 0 when the record runs past the bytes.
static size_t forest_pool_record_bytes(const ForestLayoutTables::Pool &p, uint32_t id)
{
	size_t at = p.offs[id];
	uint32_t n = 0;
	for (unsigned sh = 0; ; sh += 7) {
		if (sh > 28 || at >= p.bytes.size())
			return 0;
		const uint8_t b = p.bytes[at++];
		n |= (uint32_t)(b & 0x7f) << sh;
		if (!(b & 0x80))
			break;
	}
	if (n >= p.bytes.size() - at)
		return 0;
	return at + n + 1 - p.offs[id];
}

// A pool's records laid out in `order` (every id once), with the offsets
// block that finds them. A malformed record is written as the empty one.
static void forest_pool_in_order(const ForestLayoutTables::Pool &p,
				 const std::vector<uint32_t> &order,
				 std::vector<uint8_t> &bytes, std::vector<uint32_t> &offs)
{
	bytes.clear();
	bytes.reserve(p.bytes.size());
	offs.assign(p.ids() + 1, 0);
	for (size_t k = 0; k < order.size(); ++k) {
		const uint32_t id = order[k];
		const size_t n = forest_pool_record_bytes(p, id);
		offs[id] = (uint32_t)bytes.size();
		if (n)
			bytes.insert(bytes.end(), p.bytes.begin() + p.offs[id],
				     p.bytes.begin() + p.offs[id] + n);
		else {
			bytes.push_back(0);	// length 0
			bytes.push_back(0);	// its NUL
		}
	}
	offs[p.ids()] = (uint32_t)bytes.size();
}

// The arena records laid out in `order` (every slot once): position k holds
// slot order[k]; `pos` maps each slot to its position.
static void forest_defs_in_order(const ForestLayoutTables &t,
				 const std::vector<uint32_t> &order,
				 std::vector<uint8_t> &defs, std::vector<uint32_t> &pos)
{
	defs.resize(t.defs.size());
	pos.assign(t.slots(), 0);
	for (size_t k = 0; k < order.size(); ++k) {
		const uint32_t slot = order[k];
		memcpy(defs.data() + k * t.record_bytes,
		       t.defs.data() + (size_t)t.pos[slot] * t.record_bytes, t.record_bytes);
		pos[slot] = (uint32_t)k;
	}
}

static void forest_identity_order(size_t n, std::vector<uint32_t> &order)
{
	order.resize(n);
	for (size_t i = 0; i < n; ++i)
		order[i] = (uint32_t)i;
}

// A layout segment's LOGICAL bytes — the pools and records in id order — so a
// laid-out container digests as the freeze that produced it.
static void forest_logical_bytes(const ForestLayoutTables &t, uint32_t seg_id,
				 std::vector<uint8_t> &out)
{
	std::vector<uint32_t> order, words;
	for (const ForestLayoutTables::Pool &p : t.pools) {
		if (seg_id != p.bytes_seg && seg_id != p.offs_seg)
			continue;
		forest_identity_order(p.ids(), order);
		forest_pool_in_order(p, order, out, words);
		if (seg_id == p.offs_seg) {
			out.resize(words.size() * sizeof(uint32_t));
			memcpy(out.data(), words.data(), out.size());
		}
		return;
	}
	forest_identity_order(t.slots(), order);
	forest_defs_in_order(t, order, out, words);
}

// The roles (dialect per-name plan B1), one row each: a new role is a row and
// a code, never a format change. The dialect image is raw (pack-slices design
// §7: never decompressed, read in place).
static const cir_forest_role_row forest_roles[] = {
	{ CIR_FOREST_ROLE_SYSTEM,  "system",  false },
	{ CIR_FOREST_ROLE_DIALECT, "dialect", true },
};

const cir_forest_role_row *cir_forest_role_info(uint32_t role)
{
	for (const cir_forest_role_row &row : forest_roles)
		if ((uint32_t)row.role == role)
			return &row;
	return NULL;
}

bool cir_forest_role_named(const char *name, CirForestRole &out)
{
	for (const cir_forest_role_row &row : forest_roles)
		if (name && strcmp(row.name, name) == 0) {
			out = row.role;
			return true;
		}
	return false;
}

PchCompression cir_forest_storage_codec(uint32_t role, PchCompression packed)
{
	const cir_forest_role_row *row = cir_forest_role_info(role);
	return row && row->raw ? PchCompression::None : packed;
}

bool cir_forest_read_role(const madc::dis::snapshot_reader &r, uint32_t &role)
{
	role = CIR_FOREST_ROLE_SYSTEM;
	const madc::dis::snapshot_segment *s = r.find(CIR_FOREST_SEG_ROLE);
	if (!s)
		return true;
	uint32_t code = 0;
	if (s->kind != SNAP_KIND_CIR_ROLE || s->raw_size != sizeof(code)
	    || !r.read_segment_into(*s, (uint8_t *)&code, sizeof(code)))
		return false;
	role = code;
	return true;
}

bool cir_forest_profiles(const void *image, size_t len,
			 std::vector<cir_forest_profile> &out)
{
	out.clear();
	size_t at = len;
	for (;;) {
		madc::dis::snapshot_reader r;
		if (!r.open(image, at))
			break;
		cir_forest_profile p;
		if (!cir_forest_read_role(r, p.role))
			break;
		p.offset = r.blob_offset();
		p.bytes = at - p.offset;
		p.compressed = 0;
		for (uint32_t i = 0; i < r.segment_count(); ++i) {
			const madc::dis::snapshot_segment *s = r.segment_at(i);
			if (s && s->codec != (uint32_t)PchCompression::None)
				++p.compressed;
		}
		out.push_back(p);
		if (!r.previous_image_len(at))
			break;
	}
	return !out.empty();
}

bool cir_forest_profile_role(const void *image, size_t len, uint32_t &role,
			     size_t *previous)
{
	madc::dis::snapshot_reader r;
	if (!r.open(image, len) || !cir_forest_read_role(r, role))
		return false;
	if (previous && !r.previous_image_len(*previous))
		*previous = 0;
	return true;
}

bool cir_forest_content_digests(const void *image, size_t len,
				std::map<uint32_t, uint64_t> &out)
{
	out.clear();
	madc::dis::snapshot_reader r;
	if (!r.open(image, len))
		return false;
	ForestLayoutTables t;
	bool have_tables = false;
	for (uint32_t i = 0; i < r.segment_count(); ++i) {
		const madc::dis::snapshot_segment *s = r.segment_at(i);
		if (!s)
			return false;	// a corrupt directory record
		if (s->seg_id == CIR_FOREST_SEG_MIR_MODULE
		    || s->seg_id == CIR_FOREST_SEG_LEDGER
		    || s->seg_id == CIR_FOREST_SEG_SHARED_CONFIGS
		    || s->seg_id == CIR_FOREST_SEG_ROLE)
			continue;
		std::vector<uint8_t> bytes;
		if (s->seg_id == CIR_FOREST_SEG_ARENA_DEF_POS)
			continue;	// layout, not content (folded into the records)
		if (forest_layout_seg(s->seg_id)) {
			if (!have_tables) {
				if (!forest_read_layout_tables(r, t))
					return false;
				have_tables = true;
			}
			forest_logical_bytes(t, s->seg_id, bytes);
		} else if (!r.read_segment(*s, bytes))
			return false;
		if (s->seg_id == CIR_FOREST_SEG_DIR) {
			if (bytes.size() < sizeof(cir_forest_dir_header))
				return false;
			cir_forest_dir_header hdr;
			memcpy(&hdr, bytes.data(), sizeof(hdr));
			hdr.language_std = 0;
			hdr.defines_hash = 0;
			memcpy(bytes.data(), &hdr, sizeof(hdr));
		}
		// The kind rides the digest: the same bytes under another segment
		// kind are not the same content.
		uint64_t h = madc_pch::hash_content((const char *)&s->kind, sizeof(s->kind));
		h ^= madc_pch::hash_content((const char *)bytes.data(), bytes.size());
		out[s->seg_id] = h;
	}
	return true;
}

// One table's reads across the recorded root binds: per item (a string id or
// an arena slot) the roots that read it, in root order, and when it was first
// read (a global sequence, so ties keep the order a bind read them in).
struct ForestLayoutReads
{
	struct Sig
	{
		std::vector<uint32_t> roots;
		uint64_t first;
	};
	std::map<uint32_t, Sig> items;
	void note(uint32_t item, uint32_t root, uint64_t seq)
	{
		std::map<uint32_t, Sig>::iterator it = items.find(item);
		if (it == items.end()) {
			Sig g;
			g.roots.push_back(root);
			g.first = seq;
			items.insert(std::make_pair(item, g));
		} else if (it->second.roots.back() != root)
			it->second.roots.push_back(root);
	}
	// The read items by signature, then the unread ones in id order; `pin`
	// (when given) leads.
	void order(size_t n, std::vector<uint32_t> &out, const uint32_t *pin) const
	{
		std::vector<std::pair<const Sig *, uint32_t> > read;
		for (std::map<uint32_t, Sig>::const_iterator it = items.begin();
		     it != items.end(); ++it)
			if (!pin || it->first != *pin)
				read.push_back(std::make_pair(&it->second, it->first));
		std::sort(read.begin(), read.end(),
			  [](const std::pair<const Sig *, uint32_t> &a,
			     const std::pair<const Sig *, uint32_t> &b) {
			if (a.first->roots != b.first->roots)
				return a.first->roots < b.first->roots;
			return a.first->first < b.first->first;
		});
		out.clear();
		if (pin)
			out.push_back(*pin);
		for (size_t i = 0; i < read.size(); ++i)
			out.push_back(read[i].second);
		for (uint32_t i = 0; i < n; ++i)
			if ((!pin || i != *pin) && !items.count(i))
				out.push_back(i);
	}
};

bool cir_forest_relayout(const void *image, size_t image_len,
			 const std::vector<std::string> &logs,
			 std::vector<uint8_t> &blob, size_t &blob_offset,
			 size_t &hot_blocks, std::string &err)
{
	madc::dis::snapshot_reader r;
	if (!r.open(image, image_len)) {
		err = "no forest container";
		return false;
	}
	if (r.context_hash() != madc_cir_context_hash()) {
		err = "the container was built by a different madc";
		return false;
	}
	if (r.find(CIR_FOREST_SEG_ARENA_DEF_POS)) {
		err = "the container is already laid out";
		return false;
	}
	ForestLayoutTables t;
	if (!forest_read_layout_tables(r, t)) {
		err = "the container's string pools or arena records are unreadable";
		return false;
	}

	// A read names an item by where it starts: a pool record's first byte,
	// a record's position (slot order here — the container is not laid out).
	std::map<uint32_t, uint32_t> start_id[2];
	for (int k = 0; k < 2; ++k)
		for (uint32_t id = 0; id < t.pools[k].ids(); ++id)
			start_id[k][t.pools[k].offs[id]] = id;
	ForestLayoutReads reads[3];	// pool, arena pool, records
	// The roots reading each block of a BLOCKED segment the layout leaves in
	// place (its offsets are the new layout's too); the reordered tables'
	// blocks are counted from `reads` once they are laid out.
	typedef std::map<std::pair<uint32_t, uint64_t>, std::set<uint32_t> > BlockRoots;
	BlockRoots block_roots;
	auto note_block_span = [&](uint32_t seg, uint32_t shift, uint64_t off, uint64_t len,
				   const uint32_t *roots, size_t n) {
		for (uint64_t b = off >> shift; len && b <= (off + len - 1) >> shift; ++b)
			block_roots[std::make_pair(seg, b)].insert(roots, roots + n);
	};
	uint64_t seq = 0;
	// Each log ends `root <header>` (forest_pack_layout.sh): the dialect
	// surface's roots are their own population (pack-slices design §7).
	std::vector<bool> dialect_root(logs.size(), false);
	for (size_t i = 0; i < logs.size(); ++i) {
		FILE *lf = fopen(logs[i].c_str(), "r");
		if (!lf) {
			err = "cannot read " + logs[i];
			return false;
		}
		// A log with no read is a root that binds no frozen table (a
		// pragma-only header, served as source alone): it counts in its
		// population and reads no block. forest_pack_layout.sh fails a root
		// whose bind wrote no log — one that never opened the container.
		unsigned seg = 0;
		unsigned long long off = 0, len = 0;
		while (fscanf(lf, "%u %llu %llu", &seg, &off, &len) == 3) {
			if (seg == CIR_FOREST_SEG_STR_BYTES || seg == CIR_FOREST_SEG_ARENA_STR_BYTES) {
				const int k = seg == CIR_FOREST_SEG_STR_BYTES ? 0 : 1;
				std::map<uint32_t, uint32_t>::const_iterator it =
					start_id[k].find((uint32_t)off);
				if (it != start_id[k].end())
					reads[k].note(it->second, (uint32_t)i, seq++);
			} else if (seg == CIR_FOREST_SEG_ARENA_DEFS) {
				if (off % t.record_bytes == 0)
					reads[2].note((uint32_t)(off / t.record_bytes), (uint32_t)i, seq++);
			} else if (const madc::dis::snapshot_segment *bs = r.find(seg))
				if (madc::dis::snap_xform_id(bs->flags) == madc::dis::SNAP_XFORM_BLOCKED) {
					const uint32_t root = (uint32_t)i;
					note_block_span(seg, madc::dis::snap_blocked_shift(bs->flags),
							off, len, &root, 1);
				}
		}
		char root_name[512];
		if (fscanf(lf, " root %511s", root_name) == 1)
			dialect_root[i] = embedded_dialect_fragment_p(madc::dis::istring(root_name));
		fclose(lf);
	}

	// The reordered tables.
	std::vector<uint8_t> pool_bytes[2], defs;
	std::vector<uint32_t> pool_offs[2], pos, order;
	const uint32_t empty_id = 0;	// the empty spelling stays at byte 0
	for (int k = 0; k < 2; ++k) {
		reads[k].order(t.pools[k].ids(), order, &empty_id);
		forest_pool_in_order(t.pools[k], order, pool_bytes[k], pool_offs[k]);
	}
	reads[2].order(t.slots(), order, NULL);
	forest_defs_in_order(t, order, defs, pos);

	// The reordered tables' blocks, counted from the reads in their new places.
	uint32_t shift = 0;
	auto blocked_shift = [&](uint32_t seg) -> bool {
		const madc::dis::snapshot_segment *s = r.find(seg);
		if (!s || madc::dis::snap_xform_id(s->flags) != madc::dis::SNAP_XFORM_BLOCKED)
			return false;
		shift = madc::dis::snap_blocked_shift(s->flags);
		return true;
	};
	for (int k = 0; k < 2; ++k)
		if (blocked_shift(t.pools[k].bytes_seg))
			for (std::map<uint32_t, ForestLayoutReads::Sig>::const_iterator it =
			     reads[k].items.begin(); it != reads[k].items.end(); ++it)
				note_block_span(t.pools[k].bytes_seg, shift, pool_offs[k][it->first],
						forest_pool_record_bytes(t.pools[k], it->first),
						it->second.roots.data(), it->second.roots.size());
	if (blocked_shift(CIR_FOREST_SEG_ARENA_DEFS))
		for (std::map<uint32_t, ForestLayoutReads::Sig>::const_iterator it =
		     reads[2].items.begin(); it != reads[2].items.end(); ++it)
			note_block_span(CIR_FOREST_SEG_ARENA_DEFS, shift,
					(uint64_t)pos[it->first] * t.record_bytes, t.record_bytes,
					it->second.roots.data(), it->second.roots.size());
	// A block at least half the C/C++ root binds read is stored raw: a compile
	// including any of those roots copies it instead of decoding it. The
	// dialect surface is never decompressed (pack-slices design §7): a block
	// ANY dialect root reads is stored raw. The populations count apart, so
	// the dialect roots never dilute the C/C++ hot set.
	size_t dialect_roots = 0;
	for (size_t i = 0; i < dialect_root.size(); ++i)
		dialect_roots += dialect_root[i];
	const size_t header_roots = logs.size() - dialect_roots;
	std::map<uint32_t, std::vector<bool> > raw_blocks;	// seg -> per block
	hot_blocks = 0;
	for (BlockRoots::const_iterator it = block_roots.begin(); it != block_roots.end(); ++it) {
		size_t by_dialect = 0;
		for (std::set<uint32_t>::const_iterator ri = it->second.begin();
		     ri != it->second.end(); ++ri)
			by_dialect += dialect_root[*ri];
		const size_t by_header = it->second.size() - by_dialect;
		if (!(header_roots && by_header * 2 >= header_roots)
		    && !by_dialect)
			continue;
		std::vector<bool> &v = raw_blocks[it->first.first];
		if (v.size() <= it->first.second)
			v.resize((size_t)it->first.second + 1, false);
		v[(size_t)it->first.second] = true;
		++hot_blocks;
	}

	madc::dis::snapshot_writer w;
	w.set_context_hash(r.context_hash());
	for (uint32_t i = 0; i < r.segment_count(); ++i) {
		const madc::dis::snapshot_segment *s = r.segment_at(i);
		if (!s) {
			err = "corrupt directory record " + std::to_string(i);
			return false;
		}
		const void *bytes = NULL;
		size_t n = 0;
		std::map<uint32_t, std::vector<bool> >::const_iterator rb =
			raw_blocks.find(s->seg_id);
		const std::vector<bool> *raw = rb == raw_blocks.end() ? NULL : &rb->second;
		std::vector<uint8_t> whole;	// a segment left in place, re-blocked
		if (raw && !forest_layout_seg(s->seg_id)) {
			if (!r.read_segment(*s, whole) || whole.empty()) {
				err = "cannot read segment " + std::to_string(s->seg_id);
				return false;
			}
			bytes = whole.data();
			n = whole.size();
		}
		for (int k = 0; k < 2; ++k)
			if (s->seg_id == t.pools[k].bytes_seg) {
				bytes = pool_bytes[k].data();
				n = pool_bytes[k].size();
			} else if (s->seg_id == t.pools[k].offs_seg) {
				bytes = pool_offs[k].data();
				n = pool_offs[k].size() * sizeof(uint32_t);
			}
		if (s->seg_id == CIR_FOREST_SEG_ARENA_DEFS) {
			bytes = defs.data();
			n = defs.size();
		}
		const bool ok = bytes
			? w.add_segment(s->seg_id, s->kind, n ? bytes : (const void *)"", n,
					n ? (PchCompression)s->codec : PchCompression::None,
					CIR_FOREST_PACK_ZSTD_LEVEL, n ? s->flags : 0, raw)
			: w.add_encoded_segment(*s, r.stored_payload(*s));
		if (!ok) {
			err = "cannot re-encode segment " + std::to_string(s->seg_id);
			return false;
		}
	}
	if (!w.add_segment(CIR_FOREST_SEG_ARENA_DEF_POS, SNAP_KIND_CIR_ARENA_DEF_POS,
			   pos.empty() ? (const void *)"" : (const void *)pos.data(),
			   pos.size() * sizeof(uint32_t), PchCompression::None, 0, 0)
	    || !w.build(blob)) {
		err = "cannot assemble the laid-out container";
		return false;
	}
	blob_offset = r.blob_offset();
	return true;
}

// The --show-stats block census: every lazy span a bind bound, by segment.
void CirFrozenForest::note_lazy_span(uint32_t seg_id,
				     const madc::dis::lazy_blocks *lz) const
{
	if (lz)
		_lazy_spans.push_back(std::make_pair(seg_id, lz));
	if (lz && _touch)
		_touch->attach(seg_id, lz);
}

bool CirFrozenForest::record_touches(const char *path)
{
	if (_touch)
		return true;
	FILE *f = fopen(path, "w");
	if (!f)
		return false;
	_touch = new ForestTouchRecorder(f);
	for (size_t i = 0; i < _lazy_spans.size(); ++i)
		_touch->attach(_lazy_spans[i].first, _lazy_spans[i].second);
	return true;
}

std::string CirFrozenForest::block_stats(unsigned long long &decoded,
					 unsigned long long &total) const
{
	std::string rows;
	decoded = total = 0;
	for (size_t i = 0; i < _lazy_spans.size(); ++i) {
		const madc::dis::lazy_blocks *lz = _lazy_spans[i].second;
		decoded += lz->blocks_decoded();
		total += lz->block_count();
		char row[64];
		snprintf(row, sizeof row, "%s%u:%zu/%zu", rows.empty() ? "" : " ",
			 _lazy_spans[i].first, lz->blocks_decoded(),
			 lz->block_count());
		rows += row;
	}
	return rows;
}

// R1 stage 2: everything open() historically did past the directory —
// string-pool/arena binds, the container-global segments. Memoized; needs the
// live string pool. Neither name index is built here: the unit names are the
// directory's own (open_header), and the extern-decl index builds on the first
// extern_loc_for query.
bool CirFrozenForest::complete_open(c2m_ctx_t c2m)
{
	if (_fully_opened)
		return true;
	if (!_header_opened)
		return false;
	double _t0 = forest_now();
	_c2m = c2m;
	if (!TokenBase::_active_strpool) {
		fprintf(stderr, "madc: forest thaw requires a live string pool\n");
		return false;
	}

	// The container's own string pool (A1 frozen view): its bytes (lazy when
	// blocked) and the raw offsets block, read in place.
	size_t nbytes = 0, noffs = 0;
	const madc::dis::lazy_blocks *lzb = NULL, *lzo = NULL;
	const uint8_t *pb = forest_pool_block(_reader, CIR_FOREST_SEG_STR_BYTES,
					      madc::dis::SNAP_KIND_INTERN_BYTES,
					      nbytes, &lzb);
	const uint8_t *po = forest_pool_block(_reader, CIR_FOREST_SEG_STR_OFFSETS,
					      madc::dis::SNAP_KIND_INTERN_OFFSETS,
					      noffs, &lzo);
	if (!pb || !po || lzo || noffs % sizeof(uint32_t)) {
		fprintf(stderr, "madc: forest string pool missing or corrupt\n");
		return false;
	}
	_pool.bind((const char *)pb, nbytes, (const uint32_t *)po,
		   noffs / sizeof(uint32_t), lzb);
	if (!_pool.valid()) {
		fprintf(stderr, "madc: forest string pool failed validation\n");
		return false;
	}
	note_lazy_span(CIR_FOREST_SEG_STR_BYTES, lzb);

	// Required libraries (the link-environment closure): the header-stage
	// name IDS are range-checked here and spelled by libs() — a bind never
	// reads them, so their pool block is not decoded for one.
	for (size_t i = 0; i < _lib_ids.size(); ++i)
		if (_lib_ids[i] > _pool.count()) {
			fprintf(stderr, "madc: forest library list corrupt\n");
			return false;
		}

	// v13 container-global: file-scope global VARIABLE definitions (zero-length
	// = none; materialize_from_arena swizzles each type_id -> DataDef*).
	{
		size_t n = 0;
		const madc::dis::lazy_blocks *lz = NULL;
		const madc::dis::snapshot_segment *gs = _reader.find(CIR_FOREST_SEG_GLOBALS);
		const uint8_t *p = gs ? forest_pool_block(_reader, CIR_FOREST_SEG_GLOBALS,
							  SNAP_KIND_CIR_GLOBALS, n, &lz) : NULL;
		if (gs && (!p || n % sizeof(cir_forest_global_record))) {
			fprintf(stderr, "madc: forest global-var table corrupt\n");
			return false;
		}
		if (p)
			_globals.bind(p, n / sizeof(cir_forest_global_record), lz);
		note_lazy_span(CIR_FOREST_SEG_GLOBALS, lz);
	}
	// v20 container-global: template-NAME state (records + u32 payload + token
	// bytes; zero-length = a template-less freeze). materialize_from_arena
	// resolves names / swizzles owner ids into _restored_templates; the token
	// runs deserialize on the parser side (forest_restore_decls).
	{
		size_t n = 0;
		const madc::dis::lazy_blocks *lz = NULL;
		const madc::dis::snapshot_segment *ts = _reader.find(CIR_FOREST_SEG_TEMPLATES);
		const uint8_t *p = ts ? forest_pool_block(_reader, CIR_FOREST_SEG_TEMPLATES,
							  SNAP_KIND_CIR_TEMPLATES, n, &lz) : NULL;
		if (ts && (!p || n % sizeof(cir_forest_template_record))) {
			fprintf(stderr, "madc: forest template table corrupt\n");
			return false;
		}
		if (p)
			_templates.bind(p, n / sizeof(cir_forest_template_record), lz);
		note_lazy_span(CIR_FOREST_SEG_TEMPLATES, lz);
	}
	// R1: the template payload + token segments (the heavy pair) are
	// shape-validated here (directory metadata only, no decode) and
	// decoded lazily by ensure_template_payload() on first demand.
	if (const madc::dis::snapshot_segment *tp =
		_reader.find(CIR_FOREST_SEG_TEMPLATE_PAYLOAD)) {
		if (tp->kind != SNAP_KIND_CIR_TEMPLATE_PAYLOAD
		    || tp->raw_size % sizeof(uint32_t)) {
			fprintf(stderr, "madc: forest template payload corrupt\n");
			return false;
		}
	}
	if (const madc::dis::snapshot_segment *tt =
		_reader.find(CIR_FOREST_SEG_TEMPLATE_TOKENS)) {
		if (tt->kind != SNAP_KIND_CIR_TEMPLATE_TOKENS) {
			fprintf(stderr, "madc: forest template tokens corrupt\n");
			return false;
		}
	}
	// v20 container-global: the extern-decl index. R1: kind-check only
	// (directory metadata, no decode) — the table binds, validated whole, on
	// the first extern_loc_for query (v65), so a compile that never asks pays
	// nothing.
	if (const madc::dis::snapshot_segment *xs =
		_reader.find(CIR_FOREST_SEG_EXTERN_LOCS)) {
		if (xs->kind != SNAP_KIND_CIR_EXTERN_LOCS) {
			fprintf(stderr, "madc: forest extern index corrupt\n");
			return false;
		}
	}
	// v18 container-global: the B3 DefArena dump — THE type-graph serialization.
	// Bind the arena segments read-only (defrec/payload uint32 spans + the
	// arena's OWN string pool — bytes and the v59 raw offsets block; the
	// container 16-aligns payloads, satisfying the uint32 casts). A type-less
	// freeze dumps zero-length segments -> the view stays empty; a malformed
	// set is rejected like the other container globals. The typeid->name
	// closure (type_name_for) derives from the bound records; the DataDef
	// objects materialize lazily at bind (materialize_from_arena), never at
	// open — so --run-frozen (which never binds) pays nothing.
	{
		size_t nd = 0, np = 0, nb = 0, no = 0, ntk = 0;
		const madc::dis::lazy_blocks *lzd = NULL, *lzp = NULL,
			*lzab = NULL, *lzao = NULL, *lztk = NULL;
		const uint8_t *pd = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_DEFS,
						      SNAP_KIND_CIR_ARENA_DEFS,
						      nd, &lzd);
		const uint8_t *pp = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_PAYLOAD,
						      SNAP_KIND_CIR_ARENA_PAYLOAD,
						      np, &lzp);
		const uint8_t *ab = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_STR_BYTES,
						      madc::dis::SNAP_KIND_INTERN_BYTES,
						      nb, &lzab);
		const uint8_t *ao = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_STR_OFFSETS,
						      madc::dis::SNAP_KIND_INTERN_OFFSETS,
						      no, &lzao);
		// v23: the param-default token-byte block (absent/zero-length on a
		// defaults-less freeze — the runs then cleanly lack).
		const uint8_t *tk = forest_pool_block(_reader, CIR_FOREST_SEG_ARENA_TOKBYTES,
						      SNAP_KIND_CIR_ARENA_TOKBYTES,
						      ntk, &lztk);
		if (tk && ntk)
			_arena.bind_tokbytes(tk, ntk, lztk);
		if (pd && pp && ab && ao && nd && !lzao
		    && !(nd % sizeof(uint32_t)) && !(np % sizeof(uint32_t))
		    && !(no % sizeof(uint32_t))) {
			_arena.bind_defs((const uint32_t *)pd, nd / sizeof(uint32_t), lzd);
			// v60: the records' layout (raw, in place; absent = slot
			// order). One position per slot; a position past the table
			// reads as absent (get_def_at's bound).
			const madc::dis::snapshot_segment *ps =
				_reader.find(CIR_FOREST_SEG_ARENA_DEF_POS);
			size_t npos = 0;
			const uint8_t *pos = ps ? forest_segment_bytes(_reader, *ps, npos) : NULL;
			if (ps && (ps->kind != SNAP_KIND_CIR_ARENA_DEF_POS || !pos
				   || npos != (size_t)_arena.def_slots() * sizeof(uint32_t))) {
				fprintf(stderr, "madc: forest record layout corrupt\n");
				return false;
			}
			if (pos)
				_arena.bind_def_positions((const uint32_t *)pos,
							  npos / sizeof(uint32_t));
			_arena.bind_payload((const uint32_t *)pp, np / sizeof(uint32_t), lzp);
			_arena.strings.bind((const char *)ab, nb, (const uint32_t *)ao,
					    no / sizeof(uint32_t), lzab);
			if (!_arena.strings.valid()) {
				fprintf(stderr, "madc: forest arena string pool"
					" failed validation\n");
				return false;
			}
			note_lazy_span(CIR_FOREST_SEG_ARENA_DEFS, lzd);
			note_lazy_span(CIR_FOREST_SEG_ARENA_PAYLOAD, lzp);
			note_lazy_span(CIR_FOREST_SEG_ARENA_STR_BYTES, lzab);
			note_lazy_span(CIR_FOREST_SEG_ARENA_TOKBYTES, lztk);
			// (The typeid -> name closure derives per-query in
			// type_name_for — the arena is id-addressed by project
			// slot, so a def's arena id IS the freeze-time typeid;
			// no eager whole-arena map (R1).)
		}
	}

	// v2 container-global payloads (zero-length segments = empty).
	if (const madc::dis::snapshot_segment *bs =
		_reader.find(CIR_FOREST_SEG_BRANCH_MACROS)) {
		madc::dis::decode_bytes b;
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
		madc::dis::decode_bytes c;
		if (cs->kind != SNAP_KIND_CIR_CANON_ORDER
		    || !_reader.read_segment(*cs, c) || c.size() % sizeof(uint32_t)) {
			fprintf(stderr, "madc: forest canonical-order table corrupt\n");
			return false;
		}
		_canon_order.resize(c.size() / sizeof(uint32_t));
		if (!c.empty())
			memcpy(_canon_order.data(), c.data(), c.size());
		for (size_t i = 0; i < _canon_order.size(); ++i)
			if (_canon_order[i] >= _units.size()) {
				fprintf(stderr, "madc: forest canonical order out of range\n");
				return false;
			}
	}
	_segs.assign(_units.size(), (CirFrozenSegment *)NULL);
	_fully_opened = true;
	_stat_open_secs += forest_now() - _t0;
	return true;
}

// Bind the directory's name index (v57) in place and validate it whole, so a
// corrupt container fails at open, never as a wild read at lookup: offsets
// rising from 0 to exactly `name_bytes`, each name NUL-terminated, every
// sorted entry a unit number and the run in byte order.
bool CirFrozenForest::bind_name_index(const uint8_t *p, uint32_t nunits,
				      size_t name_bytes)
{
	const uint32_t *off = (const uint32_t *)p;
	const uint32_t *sorted = off + nunits + 1;
	const char *bytes = (const char *)(sorted + nunits);
	// Every read stays in bounds: names ascend and end in a NUL, and the
	// sorted index names units. Their ORDER is not re-checked — a misordered
	// index can only make find_unit miss (a hit is an exact strcmp match).
	if (off[0] != 0 || off[nunits] != name_bytes)
		return false;
	for (uint32_t u = 0; u < nunits; ++u)
		if (off[u + 1] <= off[u] || bytes[off[u + 1] - 1] != 0 || sorted[u] >= nunits)
			return false;
	_name_off = off;
	_name_sorted = sorted;
	_name_bytes = bytes;
	return true;
}

void CirFrozenForest::bind_seed_index() const
{
	if (_seed_index_bound)
		return;
	_seed_index_bound = true;
	size_t len = 0;
	const madc::dis::snapshot_segment *s;
	if ((s = _reader.find(CIR_FOREST_SEG_DECL_NAMES)) && s->kind == SNAP_KIND_CIR_DECL_NAMES)
		if (const uint8_t *p = forest_segment_bytes(_reader, *s, len))
			_decl_names.bind(p, len);
	if ((s = _reader.find(CIR_FOREST_SEG_SEEDS)) && s->kind == SNAP_KIND_CIR_SEEDS)
		if (const uint8_t *p = forest_segment_bytes(_reader, *s, len))
			_seeds.bind((const uint32_t *)p, len / sizeof(uint32_t));
	if ((s = _reader.find(CIR_FOREST_SEG_RECORDABLE)) && s->kind == SNAP_KIND_CIR_RECORDABLE)
		if (const uint8_t *p = forest_segment_bytes(_reader, *s, len))
			_recordable_bits.bind((const uint32_t *)p, len / sizeof(uint32_t));
}

// Ascending, each value once.
template <class V>
static void forest_sort_unique(V &v)
{
	std::sort(v.begin(), v.end());
	v.erase(std::unique(v.begin(), v.end()), v.end());
}

// Any unit of a declaring-unit run inside `units`.
static bool forest_run_meets(const uint32_t *run, uint32_t n,
			     const std::set<uint32_t> &units)
{
	for (uint32_t i = 0; i < n; ++i)
		if (units.count(run[i]))
			return true;
	return false;
}

// Any unit of a declaring-unit run the filter binds (bound or unseeded).
static bool forest_run_bound(const uint32_t *run, uint32_t n,
			     const CirMaterializeFilter &f)
{
	for (uint32_t i = 0; i < n; ++i)
		if (f.binds(run[i]))
			return true;
	return false;
}

int CirFrozenForest::name_verdict(const char *nm, const char *ns,
				  const CirMaterializeFilter &f) const
{
	if (!f.active)
		return 1;
	if (!nm || !*nm)
		return 0;
	const ForestDeclNames &dn = decl_names();
	uint32_t n = 0;
	if (ns && *ns) {
		std::string q = std::string(ns) + "::" + nm;
		if (!f.extra_names.empty() && f.extra_names.count(q))
			return 1;
		if (!f.exact) {
			const uint32_t *run = dn.units_of(q.c_str(), n);
			if (n)
				return forest_run_bound(run, n, f) ? 1 : -1;
		}
	}
	if (!f.extra_names.empty() && f.extra_names.count(nm))
		return 1;
	if (!f.exact) {
		const uint32_t *run = dn.units_of(nm, n);
		if (n)
			return forest_run_bound(run, n, f) ? 1 : -1;
	}
	return f.exact ? -1 : 0;
}

int CirFrozenForest::head_verdict(const char *canon,
				  const CirMaterializeFilter &f) const
{
	const char *lt = canon ? strchr(canon, '<') : NULL;
	if (!lt)
		return 1;
	std::string head(canon, (size_t)(lt - canon));
	while (!head.empty() && head[head.size() - 1] == ' ')
		head.erase(head.size() - 1);
	if (head.empty())
		return !f.active ? 1 : f.exact ? -1 : 0;
	return name_verdict(head.c_str(), NULL, f);
}

bool CirFrozenForest::declared_system(const char *nm, const char *ns,
				      const CirMaterializeFilter &f) const
{
	if (!f.active || f.system_units.empty() || !nm || !*nm)
		return false;
	const ForestDeclNames &dn = decl_names();
	uint32_t n = 0;
	const uint32_t *run = dn.units_of(nm, n);
	if (n && forest_run_meets(run, n, f.system_units))
		return true;
	if (ns && *ns) {
		run = dn.units_of((std::string(ns) + "::" + nm).c_str(), n);
		if (n && forest_run_meets(run, n, f.system_units))
			return true;
	}
	return false;
}

bool CirFrozenForest::may_adopt_function(const madc::dis::istring &name) const
{
	if (!_fn_index_bound) {
		_fn_index_bound = true;
		const madc::dis::snapshot_segment *s = _reader.find(CIR_FOREST_SEG_FUNC_NAMES);
		const uint8_t *p = s && s->kind == SNAP_KIND_CIR_FUNC_NAMES
				 ? _reader.raw_ptr(*s) : NULL;
		uint32_t n = 0;
		if (p && s->raw_size >= 2 * sizeof(uint32_t)) {
			memcpy(&n, p, sizeof(n));
			const uint32_t *off = (const uint32_t *)(p + sizeof(uint32_t));
			const char *bytes = (const char *)(off + n + 1);
			// Validated whole (a corrupt segment answers "none", never a
			// wild read).
			bool ok = s->raw_size >= ((uint64_t)n + 2) * sizeof(uint32_t)
			       && off[0] == 0
			       && off[n] == s->raw_size - (n + 2) * sizeof(uint32_t);
			for (uint32_t i = 0; ok && i < n; ++i)
				ok = off[i + 1] > off[i] && bytes[off[i + 1] - 1] == 0
				  && (!i || strcmp(bytes + off[i - 1], bytes + off[i]) < 0);
			if (ok) {
				_fn_count = n;
				_fn_off = off;
				_fn_bytes = bytes;
			}
		}
	}
	const uint32_t *it = std::lower_bound(_fn_off, _fn_off + _fn_count, name.c_str(),
		[this](uint32_t o, const char *key) {
			return strcmp(_fn_bytes + o, key) < 0;
		});
	return it != _fn_off + _fn_count && strcmp(_fn_bytes + *it, name.c_str()) == 0;
}

// The index lookup: a binary search over the sorted names, read in place. A
// duplicate name (the writer dedups; a stray one) answers its lower unit.
int CirFrozenForest::find_unit(const madc::dis::istring &name) const
{
	if (!_name_sorted)
		return -1;
	const uint32_t *first = _name_sorted;
	const uint32_t *last = _name_sorted + _units.size();
	const uint32_t *it = std::lower_bound(first, last, name.c_str(),
		[this](uint32_t u, const char *key) {
			return strcmp(_name_bytes + _name_off[u], key) < 0;
		});
	return it != last && strcmp(_name_bytes + _name_off[*it], name.c_str()) == 0
	     ? (int)*it : -1;
}

// Path-tail lookup: the unit whose (path-)name ends in "/<incfile>".
// Filesystem-frozen units are keyed by the PRODUCER's full header path,
// which a consumer on a different machine can never spell — the hosted
// darwin binaries carry groves frozen from the build container's libc++
// tree, and the Mac's resolver has nothing to resolve against (that is
// the point: run-only Macs have no headers). The include SPELLING is the
// machine-portable identity: match it against the name's tail component
// path. First hit in unit order (the grove's units precede the source arm's);
// -1 when none or when the spelling is itself an absolute path. A LAST
// resort: once the pack holds every system header a spelling ends many names
// (`search.h` ends libc++'s __algorithm/search.h too), so a consumer first
// asks its build's include roots (Program::forest_unit_under_build_roots).
int CirFrozenForest::find_unit_path_tail(const madc::dis::istring &incfile) const
{
	if (incfile.empty() || madc::detail::host_path_absolute(incfile))
		return -1;
	const size_t n = incfile.size();
	for (size_t u = 0; _name_off && u < _units.size(); ++u) {
		const size_t len = _name_off[u + 1] - _name_off[u] - 1;
		const char *nm = _name_bytes + _name_off[u];
		if (len > n && nm[len - n - 1] == '/'
		    && memcmp(nm + len - n, incfile.data(), n) == 0)
			return (int)u;
	}
	return -1;
}

// --- grove payload v2 readers (B4a) ----------------------------------------

bool CirFrozenForest::read_unit_seg(uint32_t unit, uint32_t slot, uint32_t kind,
				    madc::dis::decode_bytes &out) const
{
	if (unit >= _units.size())
		return false;
	// slice D: every on-demand aux-segment decode (edges / PP events /
	// decl index / token runs) rides this choke — one frame covers them.
	ForestWorkFrame _fw(_work_secs, _work_depth);
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

bool CirFrozenForest::mir_module_bytes(std::vector<uint8_t> &out) const
{
	const madc::dis::snapshot_segment *s =
		_reader.find(CIR_FOREST_SEG_MIR_MODULE);
	if (!s || s->kind != SNAP_KIND_CIR_MIR_MODULE
	    || s->raw_size <= sizeof(cir_forest_mir_header))
		return false;	// no cache segment: the normal blob-less case
	madc::dis::decode_bytes payload;
	if (!_reader.read_segment(*s, payload)
	    || payload.size() <= sizeof(cir_forest_mir_header)) {
		fprintf(stderr, "madc: mir cache: malformed segment — "
			"falling back to node materialization\n");
		return false;
	}
	cir_forest_mir_header mh;
	memcpy(&mh, payload.data(), sizeof(mh));
	uint32_t api_x100 = (uint32_t)(_MIR_get_api_version() * 100.0 + 0.5);
	if (mh.forest_version != CIR_FOREST_FORMAT_VERSION
	    || mh.mir_api_x100 != api_x100) {
		// A coupling bug, not routine fallback: the container-level
		// version pin + context hash should have rejected this first.
		fprintf(stderr, "madc: mir cache: stamp mismatch (forest v%u "
			"vs v%u, MIR api %u vs %u) — falling back\n",
			mh.forest_version, (unsigned)CIR_FOREST_FORMAT_VERSION,
			mh.mir_api_x100, api_x100);
		return false;
	}
	out.assign(payload.begin() + sizeof(cir_forest_mir_header),
		   payload.end());
	return true;
}

// AOT ledger read-back (S5). Layout: header, module_count directory entries,
// then the name / symbol / MIR blocks in directory order. Every length is
// bounds-checked against the remaining payload — a truncated segment is a
// loud refusal, never a partial ledger (a half-read ledger would look like a
// Tier-B program and refuse for the wrong reason).
bool CirFrozenForest::ledger_modules(std::vector<cir_ledger_module> &out) const
{
	out.clear();
	const madc::dis::snapshot_segment *s =
		_reader.find(CIR_FOREST_SEG_LEDGER);
	if (!s || s->kind != SNAP_KIND_CIR_LEDGER
	    || s->raw_size < sizeof(cir_forest_ledger_header))
		return false;	// no ledger segment: the normal case
	madc::dis::decode_bytes payload;
	if (!_reader.read_segment(*s, payload)
	    || payload.size() < sizeof(cir_forest_ledger_header)) {
		fprintf(stderr, "madc: ledger: malformed segment\n");
		return false;
	}
	cir_forest_ledger_header lh;
	memcpy(&lh, payload.data(), sizeof(lh));
	uint32_t api_x100 = (uint32_t)(_MIR_get_api_version() * 100.0 + 0.5);
	if (lh.forest_version != CIR_FOREST_FORMAT_VERSION
	    || lh.mir_api_x100 != api_x100) {
		fprintf(stderr, "madc: ledger: stamp mismatch (forest v%u vs v%u,"
			" MIR api %u vs %u) — ledger ignored\n",
			lh.forest_version, (unsigned)CIR_FOREST_FORMAT_VERSION,
			lh.mir_api_x100, api_x100);
		return false;
	}
	size_t pos = sizeof(lh);
	size_t dir_bytes = (size_t)lh.module_count * sizeof(cir_forest_ledger_entry);
	if (lh.module_count == 0 || payload.size() < pos + dir_bytes) {
		fprintf(stderr, "madc: ledger: truncated directory (%u module(s))\n",
			lh.module_count);
		return false;
	}
	std::vector<cir_forest_ledger_entry> dir(lh.module_count);
	memcpy(dir.data(), payload.data() + pos, dir_bytes);
	pos += dir_bytes;
	for (const cir_forest_ledger_entry &e : dir) {
		size_t need = (size_t)e.name_bytes + e.sym_bytes + e.mir_bytes;
		if (payload.size() - pos < need) {
			fprintf(stderr, "madc: ledger: truncated payload\n");
			out.clear();
			return false;
		}
		cir_ledger_module m;
		m.name = madc::dis::istring((const char *)payload.data() + pos, e.name_bytes);
		pos += e.name_bytes;
		// NUL-separated symbol names (a trailing NUL per name, so the
		// final entry ends the block cleanly).
		for (size_t i = 0, start = 0; i < e.sym_bytes; i++)
			if (payload[pos + i] == '\0') {
				if (i > start)
					m.syms.push_back(madc::dis::istring(
						(const char *)payload.data() + pos + start,
						i - start));
				start = i + 1;
			}
		pos += e.sym_bytes;
		m.bytes.assign(payload.begin() + pos,
			       payload.begin() + pos + e.mir_bytes);
		pos += e.mir_bytes;
		out.push_back(std::move(m));
	}
	return true;
}

bool CirFrozenForest::unit_tokens(uint32_t unit, std::vector<uint8_t> &madh_payload,
				  uint32_t &token_count)
{
	madc::dis::decode_bytes raw;
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
	out.clear();
	if (unit >= _units.size())
		return false;
	uint32_t base = CIR_FOREST_SEG_UNIT_BASE
		      + unit * CIR_FOREST_SEGS_PER_UNIT;
	const madc::dis::snapshot_segment *s = _reader.find(base + 5);
	if (!s || s->kind != SNAP_KIND_CIR_DECL_INDEX)
		return false;
	// An empty declaration surface is a valid grove unit (macro-only /
	// edge-only headers and task #57's conditional husks). read_unit_seg
	// deliberately treats zero raw bytes as "no payload" for its other
	// callers; this API must distinguish that valid EMPTY index from a
	// missing or wrong-kind segment so bind eligibility can classify it.
	if (!s->raw_size)
		return true;
	madc::dis::decode_bytes raw;
	if (!read_unit_seg(unit, 5, SNAP_KIND_CIR_DECL_INDEX, raw)
	    || raw.size() % sizeof(cir_forest_decl_entry))
		return false;
	out.resize(raw.size() / sizeof(cir_forest_decl_entry));
	if (!raw.empty())
		memcpy(out.data(), raw.data(), raw.size());
	return true;
}

// Each record kind carries its entity's namespace in its own ns field. The
// decl-entry record builder (cir_forest_build_seeds, 4b) gives a function
// entry a free function's DK_FUNC record, else — a method's call symbol —
// its owner class; a template record with an owner is a member template.
// Neither member names anything at namespace scope. A variable entry's
// record is a global's, or — a scoped enumerator — its enum's.
bool CirFrozenForest::decl_record_facts(uint32_t kind, uint32_t record_id,
					CirDeclRecordFacts &f) const
{
	f = CirDeclRecordFacts();
	if (record_id == CIR_DECL_RECORD_NONE)
		return false;
	const uint32_t idx = cir_seed_item_index(record_id);
	uint32_t len = 0;
	switch (cir_seed_item_kind(record_id)) {
	case CIR_SEED_DEP_ARENA: {
		madc::dis::defrec r;
		if (!_arena.get_def_at(madc::dis::arena_id_of(idx), r))
			return false;
		const bool free_fn = r.kind == madc::dis::DK_FUNC
				  && (r.flags & madc::dis::DF_IS_FREE_FUNC);
		if (kind == Program::pdkFunction && !free_fn)
			return false;
		f.ns = r.ns_id ? _arena.c_str(r.ns_id) : "";
		// A free function's disp_id is its function_display_name.
		if (free_fn && r.disp_id)
			f.display = _arena.c_str(r.disp_id);
		if (r.name_id)
			f.name = f.leaf = _arena.c_str(r.name_id);
		switch (r.kind) {
		case madc::dis::DK_FUNC:
			if (free_fn)
				f.shape = CirDeclRecordFacts::FREE_FUNCTION;
			break;
		case madc::dis::DK_STRUCT:
		case madc::dis::DK_UNION:
		case madc::dis::DK_CLASS:
		case madc::dis::DK_TYPEDEF:
			f.shape = CirDeclRecordFacts::TYPE;
			break;
		case madc::dis::DK_ENUM:
			f.shape = CirDeclRecordFacts::TYPE;
			f.is_enum = true;
			break;
		default:
			break;
		}
		break;
	}
	case CIR_SEED_DEP_TEMPLATE: {
		if (idx >= _templates.size())
			return false;
		const cir_forest_template_record t = _templates[idx];
		if (t.owner_type_id)
			return false;
		f.ns = t.ns_id ? pool_cstr(t.ns_id, len) : "";
		f.shape = CirDeclRecordFacts::TEMPLATE;
		f.name = f.leaf = t.key_id ? pool_cstr(t.key_id, len) : NULL;
		// A namespace-scope pattern's key is qualified (`php::print_r`).
		const size_t nl = f.ns ? strlen(f.ns) : 0;
		if (f.name && nl && !strncmp(f.name, f.ns, nl)
		    && f.name[nl] == ':' && f.name[nl + 1] == ':')
			f.leaf = f.name + nl + 2;
		break;
	}
	case CIR_SEED_DEP_GLOBAL: {
		if (idx >= _globals.size())
			return false;
		const cir_forest_global_record g = _globals[idx];
		f.ns = g.ns_id ? pool_cstr(g.ns_id, len) : "";
		f.shape = CirDeclRecordFacts::OBJECT;
		f.name = f.leaf = g.name_id ? pool_cstr(g.name_id, len) : NULL;
		f.gflags = g.gflags;
		// An enumerator constant's type is its enum.
		madc::dis::defrec er;
		if ((g.gflags & CIR_GLOBALF_CONST_SCALAR)
		    && madc::dis::arena_id_is_project(g.type_id)
		    && _arena.get_def_at(g.type_id, er) && er.kind == madc::dis::DK_ENUM)
			f.enum_record = (CIR_SEED_DEP_ARENA << 30)
				      | madc::dis::arena_slot_of(g.type_id);
		break;
	}
	default:
		return false;
	}
	return f.ns != NULL;
}

bool CirFrozenForest::unit_pp_events(uint32_t unit, std::vector<uint32_t> &out)
{
	madc::dis::decode_bytes raw;
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
	madc::dis::decode_bytes raw;
	if (!read_unit_seg(unit, 7, SNAP_KIND_CIR_UNIT_EDGES, raw)
	    || raw.size() % sizeof(uint32_t))
		return false;
	out.resize(raw.size() / sizeof(uint32_t));
	if (!raw.empty())
		memcpy(out.data(), raw.data(), raw.size());
	return true;
}

// v40: [name_id, flags, value_id, definer_unit] tuples — see
// SNAP_KIND_CIR_BRANCH_DEPS.
bool CirFrozenForest::unit_branch_deps(uint32_t unit, std::vector<uint32_t> &out)
{
	madc::dis::decode_bytes raw;
	if (!read_unit_seg(unit, 8, SNAP_KIND_CIR_BRANCH_DEPS, raw)
	    || raw.size() % (4 * sizeof(uint32_t)))
		return false;
	out.resize(raw.size() / sizeof(uint32_t));
	if (!raw.empty())
		memcpy(out.data(), raw.data(), raw.size());
	return true;
}

bool CirFrozenForest::unit_source(uint32_t unit, std::string &out) const
{
	madc::dis::decode_bytes raw;
	if (!read_unit_seg(unit, 9, SNAP_KIND_CIR_UNIT_SOURCE, raw))
		return false;
	out.assign((const char *)raw.data(), raw.size());
	return true;
}

bool CirFrozenForest::unit_has_source(uint32_t unit) const
{
	if (unit >= _units.size())
		return false;
	uint32_t base = CIR_FOREST_SEG_UNIT_BASE
		      + unit * CIR_FOREST_SEGS_PER_UNIT;
	const madc::dis::snapshot_segment *s = _reader.find(base + 9);
	return s && s->kind == SNAP_KIND_CIR_UNIT_SOURCE && s->raw_size != 0;
}

// Does the unit carry anything an #include binds — tokens, PP exports or
// include edges? A unit with none was packed for its SOURCE alone (the pack's
// source arm, or a dedup-skipped first include): a consumer live-parses its
// text instead. A macro-only header still binds through its PP exports. A
// directory/segment probe like unit_has_source: nothing is decompressed.
bool CirFrozenForest::unit_has_frozen_surface(uint32_t unit) const
{
	if (unit >= _units.size())
		return false;
	const uint32_t base = CIR_FOREST_SEG_UNIT_BASE
			    + unit * CIR_FOREST_SEGS_PER_UNIT;
	static const struct { uint32_t slot, kind; } surface[] = {
		{ 4, SNAP_KIND_CIR_UNIT_TOKENS },
		{ 6, SNAP_KIND_CIR_PP_EXPORTS },
		{ 7, SNAP_KIND_CIR_UNIT_EDGES },
	};
	for (size_t i = 0; i < sizeof(surface) / sizeof(surface[0]); ++i) {
		const madc::dis::snapshot_segment *s =
			_reader.find(base + surface[i].slot);
		if (s && s->kind == surface[i].kind && s->raw_size != 0)
			return true;
	}
	return false;
}

const char *CirFrozenForest::pool_cstr(uint32_t id, uint32_t &len) const
{
	if (id > _pool.count())
		return NULL;
	if (const char *s = _pool.spelling(id, len))
		return s;
	len = 0;	// unreadable: the empty spelling, as c_str()
	return "";
}

const std::vector<madc::dis::istring> &CirFrozenForest::libs()
{
	if (_libs.size() != _lib_ids.size()) {
		_libs.clear();
		for (size_t i = 0; i < _lib_ids.size(); ++i) {
			uint32_t slen = 0;
			const char *s = pool_cstr(_lib_ids[i], slen);
			_libs.push_back(s ? madc::dis::istring(s, slen)
					  : madc::dis::istring());
		}
	}
	return _libs;
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
	if (unit >= _units.size() || !_name_off)
		return NULL;
	return _name_bytes + _name_off[unit];
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


// Swizzle an arena type-id to the reconstructed DataDef*: a pinned primitive
// resolves process-invariantly; an aggregate or enum is the object its pass
// allocated; a DERIVED type builds here on its first use (pack slices design
// §6). The retired pass 1b built every derived record of the arena up front;
// derived objects are reachable only through this map, so building one on
// first use yields the same object for every reader at a fraction of the work.
DataDef *CirFrozenForest::swizzle(uint32_t tid) const
{
	if (tid && tid < MADC_TYPEID_PRIMITIVE_END)
		return madc_type_from_id(tid);
	std::map<uint32_t, DataDef *>::const_iterator it = _defs_by_tid.find(tid);
	if (it != _defs_by_tid.end())
		return it->second;
	if (!tid || !madc::dis::arena_id_is_project(tid))
		return NULL;
	madc::dis::defrec r;
	if (!_arena.get_def_at(tid, r))
		return NULL;
	switch (r.kind) {
	case madc::dis::DK_PTR:
	case madc::dis::DK_REF:
	case madc::dis::DK_CONST:
	case madc::dis::DK_CARRAY:
	case madc::dis::DK_FPTR:
	case madc::dis::DK_MEMBERPTR:
		break;
	default:
		return NULL;	// aggregates and enums come from their passes
	}
	if (!_derived_building.insert(tid).second)
		return NULL;	// a cycle through derived records alone
	DataDef *d = build_derived(tid, r);
	_derived_building.erase(tid);
	if (!d) {
		report_unresolved_derived(tid, r);
		return NULL;
	}
	_mat_storage.push_back(d);
	_defs_by_tid[tid] = d;
	return d;
}

// One derived record over its operand (NULL while the operand does not
// restore).
DataDef *CirFrozenForest::build_derived(uint32_t tid, const madc::dis::defrec &r) const
{
	(void)tid;
	const madc::dis::FrozenDefArena &a = _arena;
	// v22: a DK_FPTR rebuilds its target signature (a declaration-only
	// FuncDef from the DK_FUNC record) then DataDefFPTR.
	if (r.kind == madc::dis::DK_FPTR) {
		FuncDef *tfd = restore_signature(r.ref0);
		if (!tfd)
			return NULL;
		DataDefFPTR *fpd = new DataDefFPTR(tfd);
		fpd->ptr_syntax = (r.flags & madc::dis::DF_FPTR_PTR_SYNTAX) != 0;
		if (r.name_id)
			if (const char *nn = a.c_str(r.name_id))
				fpd->name = nn;
		return fpd;
	}
	// v53: a pointer to member — its owner as the parse resolved it (NULL
	// when it had none, or when the owner did not restore: the spelling
	// stays, as for an unresolved owner), then the member type or the
	// signature.
	if (r.kind == madc::dis::DK_MEMBERPTR) {
		DataDef *owner = r.body_unit ? swizzle(r.body_unit) : NULL;
		const char *on = r.disp_id ? a.c_str(r.disp_id) : NULL;
		madc::dis::istring owner_name = on ? on : "";
		DataDef *d;
		if (r.flags & madc::dis::DF_MEMBERPTR_FUNCTION) {
			FuncDef *tfd = restore_signature(r.ref0);
			if (!tfd)
				return NULL;
			d = new DataDefMemberFnPtr(owner, owner_name, tfd,
				(r.flags & madc::dis::DF_MEMBERPTR_CONST_METHOD) != 0);
		} else {
			DataDef *member = swizzle(r.ref0);
			if (!member)
				return NULL;
			d = new DataDefMemberPtr(owner, owner_name, *member);
		}
		if (r.name_id)
			if (const char *nn = a.c_str(r.name_id))
				d->name = nn;
		return d;
	}
	DataDef *operand = swizzle(r.ref0);
	if (!operand)
		return NULL;	// operand outside the restored set
	if (r.kind == madc::dis::DK_REF)
		return new DataDefREF(*operand, (r.flags & 1u) != 0);
	if (r.kind == madc::dis::DK_CONST)
		// The cv mask rides flags; a pre-mask record (0) was const.
		return new DataDefQUAL(*operand, r.flags ? r.flags : cvCONST);
	if (r.kind == madc::dis::DK_CARRAY) {
		// v25: rebuild the fixed-size array VERBATIM (name + folded
		// count; a runtime-sized array is never recorded).
		uint64_t cnt = (uint64_t)r.carray_count_lo
			     | ((uint64_t)r.carray_count_hi << 32);
		const char *an = r.name_id ? a.c_str(r.name_id) : NULL;
		return new DataDefCArray(*operand,
					 an ? madc::dis::istring(an) : operand->name,
					 (size_t)cnt, NULL);
	}
	return new DataDefPTR(*operand);
}

// Permanent -v diagnostic: every DERIVED type asked for that did NOT build.
// arena_chain_ok admits an fn-ptr chain unconditionally ("the target
// signature never gates member layout"), but its build can still fail — and
// pass 2 then drops every aggregate with such a member. Without naming it here
// the loss surfaces only as a class that is simply absent, layers later (task
// #64: std::ios_base died on `event_callback *__fn_`, and took the whole
// iostream family with it). An EXACT pass asks only for what one name needs —
// nothing to census.
void CirFrozenForest::report_unresolved_derived(uint32_t tid,
						const madc::dis::defrec &r) const
{
	DBG(do {
		if (_mat_filter.exact)
			break;
		// Once per forest, across widened re-passes (see the member).
		if (!_derived_unresolved_reported.insert(tid).second)
			break;
		const madc::dis::FrozenDefArena &a = _arena;
		// Built already (a pinned primitive, or in the map) — the reader
		// below names the first operand that is not, without building.
		auto built = [&](uint32_t id) -> bool {
			return (id && id < MADC_TYPEID_PRIMITIVE_END)
			       || _defs_by_tid.count(id) != 0;
		};
		const char *dk = NULL;
		if (r.kind == madc::dis::DK_PTR)		dk = "ptr";
		else if (r.kind == madc::dis::DK_REF)		dk = "ref";
		else if (r.kind == madc::dis::DK_CONST)		dk = "const";
		else if (r.kind == madc::dis::DK_CARRAY)	dk = "carray";
		else if (r.kind == madc::dis::DK_FPTR)		dk = "fptr";
		else if (r.kind == madc::dis::DK_MEMBERPTR)	dk = "memberptr";
		if (!dk)
			break;
		std::string why;
		// A member-function pointer's ref0 is a signature, as a fn-ptr's.
		if (r.kind == madc::dis::DK_FPTR
		    || (r.kind == madc::dis::DK_MEMBERPTR
			&& (r.flags & madc::dis::DF_MEMBERPTR_FUNCTION))) {
			madc::dis::defrec fr;
			if (!r.ref0 || !madc::dis::arena_id_is_project(r.ref0)
			    || !a.get_def_at(r.ref0, fr)
			    || fr.kind != madc::dis::DK_FUNC)
				why = "no signature record";
			else if (!built(fr.ref0))
				why = "return type";
			else
				for (uint32_t p = 0; p < fr.params_count; ++p) {
					madc::dis::paramrec pr;
					if (a.get_payload(fr.params_begin, p, pr)
					    && built(pr.type_id))
						continue;
					why = "param #" + std::to_string(p)
					    + " tid=" + std::to_string(pr.type_id);
					madc::dis::defrec pd;
					if (!a.get_def_at(pr.type_id, pd))
						why += " (no record)";
					else {
						why += " kind=" + std::to_string((int)pd.kind);
						if (pd.name_id && a.c_str(pd.name_id))
							why += std::string(" ")
							     + a.c_str(pd.name_id);
					}
					break;
				}
		} else {
			// Spell the operand's record kind, exactly like the fn-ptr
			// param arm above. An id whose record is DK_NONE (kind=0)
			// is task #64's fingerprint — stamped by
			// forest_serialize_type_id as a cross-reference, never
			// written by any record walk — and `kind=0` is the token
			// the pack degradation gate ratchets. A DK_NONE operand
			// printed as a bare "operand" would read like the routine
			// outside-the-closure case.
			madc::dis::defrec od;
			why = "operand";
			if (!r.ref0 || !a.get_def_at(r.ref0, od))
				why += " (no record)";
			else {
				why += " kind=" + std::to_string((int)od.kind);
				if (od.name_id && a.c_str(od.name_id))
					why += std::string(" ")
					     + a.c_str(od.name_id);
			}
		}
		std::cout << "materialize derived: UNRESOLVED " << dk << " "
			  << (r.name_id && a.c_str(r.name_id)
			      ? a.c_str(r.name_id) : "?")
			  << " (" << why << ")" << std::endl;
	} while (0));
}

DataDef *CirFrozenForest::restored_def_by_tid(uint32_t tid) const
{
	return swizzle(tid);
}

// The anon-tag gensym floor: max __anon_N across the arena's named records,
// computed once at freeze (RECORDABLE's header word).
size_t CirFrozenForest::max_restored_anon_tag()
{
	return recordable_bits().max_anon_tag();
}

const std::vector<CirRestoredType> &CirFrozenForest::materialize_from_arena()
{
	if (_types_materialized)
		return _restored;
	_types_materialized = true;
	materialize_pass();
	return _restored;
}

// S2 (R4-lite): monotone widening — see the declaration. The union is per
// unit, per name and per record. Growth = a bound or unseeded unit the
// installed filter lacks, exact -> closure, widening to the whole container,
// or an asked-for name the current filter judges declared-only-by-unbound-
// units (one it already keeps — a bound or undeclared name under a closure
// filter — changes nothing; under an exact filter every new name is growth):
// a generation that re-visits every list. A new asked record alone is a
// generation over the new records only.
const std::vector<CirRestoredType> &CirFrozenForest::materialize_for(
	const CirMaterializeFilter &want)
{
	if (!_types_materialized) {
		_mat_filter = want;
		_types_materialized = true;
		materialize_pass();
		return _restored;
	}
	if (!_mat_filter.active)
		return _restored;	// whole container already built
	bool grew = false;
	std::vector<uint32_t> added;	// asked records the installed filter lacks
	if (!want.active) {
		_mat_filter.active = false;
		_mat_filter.exact = false;
		grew = true;
	} else {
		for (std::set<madc::dis::istring>::const_iterator it =
		     want.extra_names.begin(); it != want.extra_names.end(); ++it) {
			if (_mat_filter.extra_names.count(*it))
				continue;
			if (_mat_filter.exact
			    || name_verdict(it->c_str(), NULL, _mat_filter) < 0)
				grew = true;
			_mat_filter.extra_names.insert(*it);
		}
		// exact -> closure is a widening (unindexed records join).
		if (_mat_filter.exact && !want.exact) {
			_mat_filter.exact = false;
			grew = true;
		}
		for (std::set<uint32_t>::const_iterator it = want.bound_units.begin();
		     it != want.bound_units.end(); ++it)
			if (_mat_filter.bound_units.insert(*it).second)
				grew = true;
		// A newly bound name can admit a candidate an earlier generation
		// judged outside the closure: every list again.
		for (std::set<uint32_t>::const_iterator it = want.unseeded_units.begin();
		     it != want.unseeded_units.end(); ++it)
			if (_mat_filter.unseeded_units.insert(*it).second)
				grew = true;
		_mat_filter.system_units.insert(want.system_units.begin(),
						want.system_units.end());
		// Withheld functions join with their unit (a new bound unit is a
		// full generation of its own, which reads them).
		_mat_filter.withheld.insert(want.withheld.begin(), want.withheld.end());
		for (std::set<uint32_t>::const_iterator it = want.records.begin();
		     it != want.records.end(); ++it)
			if (_mat_filter.records.insert(*it).second)
				added.push_back(*it);
	}
	if (grew)
		materialize_pass();	// every asked record rides the full visit
	else if (!added.empty())
		materialize_pass(&added);
	return _restored;
}

// One recorded parameter back onto the function a DK_FUNC record rebuilds, as
// it was parsed (LOADED == parsed): its type, its const flag
// (FuncDef::const_params, which a copy constructor's selection reads) and its
// C++ spelling, index-aligned with the parameters. Every arm that rebuilds a
// FuncDef from a paramrec run pushes through here.
static void restore_param(FuncDef *fd, DataDef *pd,
			  const madc::dis::paramrec &pr,
			  const madc::dis::FrozenDefArena &a)
{
	fd->parameters.push_back(pd);
	fd->const_params.push_back((pr.flags & madc::dis::PF_CONST_PARAM) != 0);
	const char *sp = pr.cpp_spelling_id ? a.c_str(pr.cpp_spelling_id) : NULL;
	fd->param_cpp_spellings.push_back(sp ? sp : "");
}

// A callable's SIGNATURE (see the declaration).
FuncDef *CirFrozenForest::restore_signature(uint32_t func_tid) const
{
	const madc::dis::FrozenDefArena &a = _arena;
	madc::dis::defrec fr;
	if (!func_tid || !madc::dis::arena_id_is_project(func_tid)
	    || !a.get_def_at(func_tid, fr) || fr.kind != madc::dis::DK_FUNC)
		return NULL;
	DataDef *ret = swizzle(fr.ref0);
	if (!ret)
		return NULL;
	std::vector<DataDef *> ps(fr.params_count);
	std::vector<madc::dis::paramrec> prs(fr.params_count);
	for (uint32_t p = 0; p < fr.params_count; ++p)
		if (!a.get_payload(fr.params_begin, p, prs[p])
		    || !(ps[p] = swizzle(prs[p].type_id)))
			return NULL;
	FuncDef *tfd = new FuncDef(*ret);
	_mat_storage.push_back(tfd);
	for (uint32_t p = 0; p < fr.params_count; ++p)
		restore_param(tfd, ps[p], prs[p], a);
	tfd->is_varargs = (fr.flags & madc::dis::DF_IS_VARARGS) != 0;
	tfd->is_void_params = (fr.flags & madc::dis::DF_IS_VOID_PARAMS) != 0;
	tfd->is_const_method = (fr.flags & madc::dis::DF_IS_CONST_METHOD) != 0;
	tfd->declaration_only = true;
	if (fr.name_id)
		if (const char *fn = a.c_str(fr.name_id))
			tfd->name = fn;
	return tfd;
}

void CirFrozenForest::materialize_pass(const std::vector<uint32_t> *added)
{
	ForestWorkFrame _fw(_work_secs, _work_depth);
	double _t0 = forest_now();
	++_stat_mat_generations;

	// A type-less freeze binds an empty arena view: every aggregate pass
	// below no-ops (empty slot buckets) and only pinned-typed globals
	// restore.
	const madc::dis::FrozenDefArena &a = _arena;

	// Pass 0 — what this generation VISITS (pack slices design §6). Under a
	// closure filter with the pack's seed index: the seed lists of the
	// bound units and of the units declaring a name the filter asks for,
	// plus the always list and the records the filter asks for directly —
	// and, once admission settles, the dependents of every admitted class.
	// A generation whose only growth is asked records (`added`) visits
	// those records alone. Otherwise every record, one slot scan (no
	// filter, a container without seeds, or an asked-for name no unit
	// declares).
	// Candidate lists ascend (slot / record order == the whole walk's), so
	// every pass below restores in the order the whole walk did.
	std::vector<uint32_t> agg_ids;		// candidate aggregate TIDS
	std::vector<uint32_t> typedef_slots, enum_slots, ns_slots, free_func_slots;
	std::vector<size_t> tmpl_idx, glob_idx;	// template / global record indices
	const ForestRecordableBits &rbits = recordable_bits();
	const ForestSeeds &sd = seeds();
	bool seeded = _mat_filter.active && !sd.empty()
		    && sd.lists() == unit_count() + 1;
	// Records alone need the seed index (it places their dependents);
	// without one, every record as above.
	const bool records_only = added && seeded;
	// The seed-parity probe (MADC_SEED_PARITY_PROBE=1, printed at the end of
	// the pass) labels every restored record with how this generation reached
	// it: DEMANDED — asked for by record (CirMaterializeFilter::records, a
	// dialect lookup's demand); LISTED in a visited seed list (a bound unit's —
	// every entity its decl index, its parse or its instantiations made — or
	// the always list); or CARRIED (a reference pull, a function-local class
	// admitted by structure, an admitted owner's dependent). Nothing else may
	// restore.
	static const char *par_probe = ::getenv("MADC_SEED_PARITY_PROBE");
	const bool par_on = par_probe && par_probe[0] == '1';
	std::set<uint32_t> par_listed_slots, par_carried_slots;
	std::set<size_t> par_listed_tmpl, par_carried_tmpl, par_listed_glob, par_carried_glob;
	auto par_carry_tid = [&](uint32_t tid) {
		if (par_on && madc::dis::arena_id_is_project(tid))
			par_carried_slots.insert(madc::dis::arena_slot_of(tid));
	};
	auto par_label = [&](const std::set<uint32_t> &asked, const std::set<uint32_t> &listed,
			     const std::set<uint32_t> &carried, uint32_t key) -> char {
		return asked.count(key) ? 'd' : listed.count(key) ? 'l'
		     : carried.count(key) ? 'c' : '?';
	};
	auto par_label_idx = [&](const std::set<size_t> &asked, const std::set<size_t> &listed,
				 const std::set<size_t> &carried, size_t key) -> char {
		return asked.count(key) ? 'd' : listed.count(key) ? 'l'
		     : carried.count(key) ? 'c' : '?';
	};
	std::set<uint32_t> seed_lists;
	if (seeded && !records_only) {
		for (std::set<uint32_t>::const_iterator it =
		     _mat_filter.bound_units.begin();
		     it != _mat_filter.bound_units.end(); ++it)
			if (*it < unit_count())
				seed_lists.insert(*it);
		for (std::set<madc::dis::istring>::const_iterator it =
		     _mat_filter.extra_names.begin();
		     seeded && it != _mat_filter.extra_names.end(); ++it) {
			uint32_t n = 0;
			const uint32_t *run = decl_names().units_of(it->c_str(), n);
			if (!n)
				seeded = false;	// no unit to visit it from
			// An unseeded unit's list stays unvisited: what restores
			// of it is asked by record.
			if (_mat_filter.unseeded_units.empty())
				seed_lists.insert(run, run + n);
			else
				for (uint32_t k = 0; k < n; ++k)
					if (_mat_filter.bound_units.count(run[k])
					    || !_mat_filter.unseeded_units.count(run[k]))
						seed_lists.insert(run[k]);
		}
		seed_lists.insert(sd.always());
	}
	// The records asked for directly: visited (when this generation visits
	// lists, not the whole walk) and KEPT whatever their verdict — every
	// verdict gate below consults the asked_* sets.
	std::vector<uint32_t> slots;
	std::set<uint32_t> asked_slots;
	std::set<size_t> asked_tmpl, asked_glob;
	auto ask_record = [&](uint32_t rid, bool visit) {
		const uint32_t idx = cir_seed_item_index(rid);
		switch (cir_seed_item_kind(rid)) {
		case CIR_SEED_DEP_ARENA:
			asked_slots.insert(idx);
			if (visit)
				slots.push_back(idx);
			break;
		case CIR_SEED_DEP_TEMPLATE:
			asked_tmpl.insert(idx);
			if (visit)
				tmpl_idx.push_back(idx);
			break;
		case CIR_SEED_DEP_GLOBAL:
			asked_glob.insert(idx);
			if (visit)
				glob_idx.push_back(idx);
			break;
		default:
			break;
		}
	};
	if (records_only)
		for (size_t i = 0; i < added->size(); ++i)
			ask_record((*added)[i], true);
	else
		for (std::set<uint32_t>::const_iterator it = _mat_filter.records.begin();
		     it != _mat_filter.records.end(); ++it)
			ask_record(*it, seeded);
	// The verdict gates' asked test. No record asked (every bind but a
	// dialect demand): one size read per candidate, never a lookup.
	auto asked_slot = [&](uint32_t s) -> bool {
		return !asked_slots.empty() && asked_slots.count(s);
	};
	auto asked_template = [&](size_t i) -> bool {
		return !asked_tmpl.empty() && asked_tmpl.count(i);
	};
	auto asked_global = [&](size_t i) -> bool {
		return !asked_glob.empty() && asked_glob.count(i);
	};
	// A withheld record no record asks (the dialect's, restored as a lookup
	// demands it): never a candidate — a type still restores when an
	// admitted record's chain pulls it. No record withheld (every bind but
	// the dialect's): one size read per probe.
	if (_withheld_sorted.size() != _mat_filter.withheld.size())
		_withheld_sorted.assign(_mat_filter.withheld.begin(), _mat_filter.withheld.end());
	auto withheld_record = [&](uint32_t rid) -> bool {
		return !_withheld_sorted.empty()
		    && std::binary_search(_withheld_sorted.begin(), _withheld_sorted.end(), rid);
	};
	auto withheld_slot = [&](uint32_t s) -> bool {
		return withheld_record((CIR_SEED_DEP_ARENA << 30) | s) && !asked_slot(s);
	};
	auto withheld_template = [&](size_t i) -> bool {
		return withheld_record((CIR_SEED_DEP_TEMPLATE << 30) | (uint32_t)i)
		    && !asked_template(i);
	};
	auto withheld_global = [&](size_t i) -> bool {
		return withheld_record((CIR_SEED_DEP_GLOBAL << 30) | (uint32_t)i)
		    && !asked_global(i);
	};
	// A demanded record's place in its restored list (demand_restored): an
	// asked or withheld one, as it is pushed.
	const bool demands = !_withheld_sorted.empty() || !asked_slots.empty()
			  || !asked_tmpl.empty() || !asked_glob.empty();
	auto note_demand_restored = [&](uint32_t rid, size_t at) {
		if (!demands)
			return;
		const uint32_t idx = cir_seed_item_index(rid);
		bool asked = false;
		switch (cir_seed_item_kind(rid)) {
		case CIR_SEED_DEP_ARENA:    asked = asked_slot(idx); break;
		case CIR_SEED_DEP_TEMPLATE: asked = asked_template(idx); break;
		case CIR_SEED_DEP_GLOBAL:   asked = asked_global(idx); break;
		default: break;
		}
		if (asked || withheld_record(rid))
			_demand_restored[rid] = at;
	};
	// An aggregate an earlier generation built whole: its pulls were chased
	// then. A records-only generation's chase stops at it.
	auto built_whole = [&](uint32_t tid) -> bool {
		return records_only && _defs_by_tid.count(tid) && !_named_only.count(tid);
	};
	// One arena slot into its kind's list.
	auto bucket_slot = [&](uint32_t s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r))
			return;
		switch (r.kind) {
		case madc::dis::DK_STRUCT:
		case madc::dis::DK_UNION:
		case madc::dis::DK_CLASS:
			if (arena_recordable_candidate(r) && !withheld_slot(s))
				agg_ids.push_back(madc::dis::arena_id_of(s));
			break;
		case madc::dis::DK_TYPEDEF:
			if (!withheld_slot(s))
				typedef_slots.push_back(s);
			break;
		case madc::dis::DK_ENUM:
			if (!withheld_slot(s))
				enum_slots.push_back(s);
			break;
		case madc::dis::DK_NSLINK:
		case madc::dis::DK_NSBIND:
		case madc::dis::DK_NSALIAS:
		case madc::dis::DK_DEFBODY:
			ns_slots.push_back(s);
			break;
		case madc::dis::DK_FUNC:
			if ((r.flags & madc::dis::DF_IS_FREE_FUNC) && !withheld_slot(s))
				free_func_slots.push_back(s);
			break;
		default:
			break;
		}
	};
	if (seeded) {
		for (std::set<uint32_t>::const_iterator it = seed_lists.begin();
		     it != seed_lists.end(); ++it) {
			ForestSeeds::Run ar = sd.arena(*it);
			slots.insert(slots.end(), ar.p, ar.p + ar.n);
			ForestSeeds::Run tr = sd.templates(*it);
			tmpl_idx.insert(tmpl_idx.end(), tr.p, tr.p + tr.n);
			ForestSeeds::Run gr = sd.globals(*it);
			glob_idx.insert(glob_idx.end(), gr.p, gr.p + gr.n);
		}
		forest_sort_unique(slots);
		forest_sort_unique(tmpl_idx);
		forest_sort_unique(glob_idx);
		_stat_mat_visited = slots.size() + tmpl_idx.size() + glob_idx.size();
		for (size_t i = 0; i < slots.size(); ++i)
			bucket_slot(slots[i]);
		if (par_on) {
			par_listed_slots.insert(slots.begin(), slots.end());
			par_listed_tmpl.insert(tmpl_idx.begin(), tmpl_idx.end());
			par_listed_glob.insert(glob_idx.begin(), glob_idx.end());
		}
	} else {
		for (uint32_t s = 0, n = a.def_slots(); s < n; ++s)
			bucket_slot(s);
		for (size_t i = 0; i < _templates.size(); ++i)
			tmpl_idx.push_back(i);
		for (size_t i = 0; i < _globals.size(); ++i)
			glob_idx.push_back(i);
		_stat_mat_visited = a.def_slots() + tmpl_idx.size() + glob_idx.size();
	}
	// Recordability: frozen with the pack (RECORDABLE — the fixpoint the
	// freeze ran, cir_arena_recordable.h) — a set view (count) so
	// arena_chain_ok reads it like the fixpoint's set.
	struct RecordableView {
		const ForestRecordableBits *bits;
		size_t count(uint32_t tid) const { return bits->recordable(tid); }
	} recordable = { &rbits };

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
			madc::dis::memberrec mr = {};
			if (!a.get_payload(r.members_begin, m, mr)
			    || !arena_chain_ok(a, mr.type_id, tid, recordable)
			    || (mr.vbase_id && !recordable.count(mr.vbase_id))) {
				const char *mn = mr.name_id ? a.c_str(mr.name_id) : "?";
				why = std::string("member ") + (mn ? mn : "?");
				break;
			}
		}
		if (why == "?" && r.kind == madc::dis::DK_CLASS)
			for (uint32_t b = 0; b < r.bases_count; ++b) {
				madc::dis::baserec br = {};
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
	// DataDefs. Every verdict is the forest's one owner (name_verdict /
	// head_verdict) against the installed filter — the predicate
	// registration applies too. Tri-state so the free-function walk below
	// can reproduce registration's display-name refinement (which applies
	// only when the primary key is UNINDEXED). Memoized per generation.
	auto verdict_str = [&](const char *nm, const char *ns) -> int {
		return CirFrozenForest::name_verdict(nm, ns, _mat_filter);
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
	std::unordered_map<uint32_t, int> head_memo;
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
		std::unordered_map<uint32_t, int>::iterator hi =
			head_memo.find(r.canon_id);
		if (hi == head_memo.end())
			hi = head_memo.insert(std::make_pair(r.canon_id,
				head_verdict(a.c_str(r.canon_id), _mat_filter))).first;
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
	// Admitted DECLARED-ONLY (format 63): a class product the bound closure
	// only NAMES — listed where it was first named, or reached through a
	// typedef, a signature, a pointer or a reference — whose template no
	// bound unit defines (ForestSeeds::definers). A live
	// parse of that closure has it declared, never defined (<iosfwd>'s
	// stringbuf without <sstream>): it is allocated and registered, not
	// filled, and nothing it reaches is chased. A completeness-requiring
	// reach (a member by value, a base) admits it whole.
	std::set<uint32_t> named_only;
	auto names_only = [&](uint32_t tid) -> bool {
		if (!_mat_filter.active || sd.empty()
		    || !madc::dis::arena_id_is_project(tid))
			return false;
		ForestSeeds::Run dr = sd.definers(madc::dis::arena_slot_of(tid));
		if (!dr.n)
			return false;
		for (uint32_t k = 0; k < dr.n; ++k)
			if (_mat_filter.binds(dr.p[k]))
				return false;
		return true;
	};
	if (_mat_filter.active) {
		std::vector<uint32_t> work;	// admitted aggregates to expand
		std::vector<uint32_t> tq;	// type-ids whose chains to chase: the
						// referent must be complete
		std::vector<uint32_t> tq_named;	// ... where a declaration suffices
		for (size_t i = 0; i < agg_ids.size(); ++i) {
			uint32_t tid = agg_ids[i];
			if (!recordable.count(tid))
				continue;
			madc::dis::defrec r;
			if (!a.get_def_at(tid, r))
				continue;
			// R1: a function-local class is unnameable outside its
			// function — it can never be a consumer DEMAND. It must
			// not seed the chase (its members pulled the whole C++
			// instantiation universe into pure-C binds); it stays
			// admittable through reference pulls.
			if (r.flags & madc::dis::DF_CLASS_FN_LOCAL)
				continue;
			if (built_whole(tid))
				continue;
			if (asked_slot(madc::dis::arena_slot_of(tid))
			    || record_kept(r)) {
				DBG(fprintf(stderr,
					    "forest agg seed name=%s ns=%s canon=%s\n",
					    r.name_id ? a.c_str(r.name_id) : "-",
					    r.ns_id ? a.c_str(r.ns_id) : "-",
					    r.canon_id ? a.c_str(r.canon_id) : "-"));
				admitted.insert(tid);
				// Listed where it was first NAMED (its point of
				// creation) yet defined by no bound unit: declared.
				if (names_only(tid))
					named_only.insert(tid);
				else
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
		for (size_t bi = 0; bi < typedef_slots.size(); ++bi) {
			madc::dis::defrec r;
			if (!a.get_def_at(madc::dis::arena_id_of(typedef_slots[bi]), r))
				continue;
			if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
				continue;
			if (asked_slot(typedef_slots[bi])
			    || name_verdict(r.name_id, r.ns_id) >= 0) {
				DBG(fprintf(stderr,
					    "forest td seed v=%d name=%s ns=%s"
					    " flags=%x\n",
					    name_verdict(r.name_id, r.ns_id),
					    r.name_id ? a.c_str(r.name_id) : "-",
					    r.ns_id ? a.c_str(r.ns_id) : "-",
					    (unsigned)r.flags));
				tq_named.push_back(r.ref0);
			}
		}
		for (size_t bi = 0; bi < free_func_slots.size(); ++bi) {
			madc::dis::defrec r;
			if (!a.get_def_at(madc::dis::arena_id_of(free_func_slots[bi]), r))
				continue;
			if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
				continue;
			{
				// An asked record is demanded: a bound verdict.
				int v = asked_slot(free_func_slots[bi]) ? 1
				      : name_verdict(r.name_id, 0);
				if (v == 0 && r.disp_id && r.ns_id)
					v = name_verdict(r.disp_id, r.ns_id);
				if (v < 0)
					continue;
				// R1: an UNINDEXED free function that was
				// BODIED at freeze is an instantiation
				// product (an __oN overload instance, a
				// member-fn instance, a builtin shim) — not a
				// declared header surface. It must not seed
				// the reference chase: at live parse its
				// types exist only when the bound headers'
				// own machinery creates them, and seeding it
				// dragged the C++ instantiation universe into
				// pure-C binds. Its restore self-gates below
				// (types never admitted -> the swizzle lacks).
				if (v == 0 && (r.flags & madc::dis::DF_WAS_BODIED))
					continue;
				DBG(fprintf(stderr,
					    "forest fn seed v=%d name=%s disp=%s ns=%s"
					    " flags=%x\n",
					    v,
					    r.name_id ? a.c_str(r.name_id) : "-",
					    r.disp_id ? a.c_str(r.disp_id) : "-",
					    r.ns_id ? a.c_str(r.ns_id) : "-",
					    (unsigned)r.flags));
				tq_named.push_back(r.ref0);
				for (uint32_t p = 0; p < r.params_count; ++p) {
					madc::dis::paramrec pr;
					if (a.get_payload(r.params_begin, p, pr))
						tq_named.push_back(pr.type_id);
				}
			}
		}
		// A file-scope global the filter keeps chases its type.
		auto seed_global = [&](size_t i) {
			const cir_forest_global_record &g = _globals[i];
			if ((g.gflags & CIR_GLOBALF_TU_ROOT) || withheld_global(i))
				return;
			uint32_t nl = 0, sl = 0;
			const char *nm = pool_cstr(g.name_id, nl);
			const char *ns = g.ns_id ? pool_cstr(g.ns_id, sl) : NULL;
			if (asked_global(i) || verdict_str(nm, ns) >= 0)
				tq.push_back(g.type_id);
		};
		for (size_t gi = 0; gi < glob_idx.size(); ++gi)
			if (glob_idx[gi] < _globals.size())
				seed_global(glob_idx[gi]);
		for (size_t ti = 0; ti < tmpl_idx.size(); ++ti) {
			size_t i = tmpl_idx[ti];
			if (i >= _templates.size() || withheld_template(i))
				continue;
			const cir_forest_template_record &t = _templates[i];
			if ((t.flags & CIR_TMPLF_TU_ROOT) || !t.owner_type_id)
				continue;
			uint32_t kl = 0, sl = 0;
			const char *key = pool_cstr(t.key_id, kl);
			const char *ns = t.ns_id ? pool_cstr(t.ns_id, sl) : NULL;
			// R1: only a BOUND-DECLARED (+1) member template seeds
			// its owner. A derived (unindexed, 0) record is an
			// instantiation product — its rebind/member keys seeded
			// every instantiated owner (allocator<char>, …) into
			// pure-C binds, defeating the owner fence; it now rides
			// the owner's own admission (the build loop's fence).
			if (asked_template(i) || verdict_str(key, ns) > 0)
				tq.push_back(t.owner_type_id);
		}
		auto admit_agg = [&](uint32_t tid, bool need_complete) {
			if (!recordable.count(tid))
				return;
			if (admitted.count(tid)) {
				if (need_complete && named_only.erase(tid))
					work.push_back(tid);
				return;
			}
			if (built_whole(tid))
				return;
			DBG({
				madc::dis::defrec ar;
				if (a.get_def_at(tid, ar))
					fprintf(stderr,
						"forest admit pull name=%s\n",
						ar.name_id ? a.c_str(ar.name_id)
							   : "-");
			});
			admitted.insert(tid);
			par_carry_tid(tid);
			if (!need_complete && names_only(tid)) {
				named_only.insert(tid);
				return;
			}
			work.push_back(tid);
		};
		// A callable's signature (a fn-ptr's, a member-function pointer's):
		// its return and parameter types join the chase.
		auto pull_signature = [&](uint32_t func_tid) {
			madc::dis::defrec fr;
			if (!func_tid || !a.get_def_at(func_tid, fr)
			    || fr.kind != madc::dis::DK_FUNC)
				return;
			tq_named.push_back(fr.ref0);
			for (uint32_t p = 0; p < fr.params_count; ++p) {
				madc::dis::paramrec pr;
				if (a.get_payload(fr.params_begin, p, pr))
					tq_named.push_back(pr.type_id);
			}
		};
		auto drain_pulls = [&]() {
		while (!work.empty() || !tq.empty() || !tq_named.empty()) {
			if (!tq.empty() || !tq_named.empty()) {
				bool need_complete = !tq.empty();
				std::vector<uint32_t> &q = need_complete ? tq : tq_named;
				uint32_t tid = q.back();
				q.pop_back();
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
						// Through a pointer or reference a
						// declaration suffices.
						if (r.kind == madc::dis::DK_PTR
						    || r.kind == madc::dis::DK_REF)
							need_complete = false;
						tid = r.ref0;
						continue;
					}
					if (r.kind == madc::dis::DK_STRUCT
					    || r.kind == madc::dis::DK_UNION
					    || r.kind == madc::dis::DK_CLASS)
						admit_agg(tid, need_complete);
					else if (r.kind == madc::dis::DK_ENUM) {
						pulled_enums.insert(tid);
						par_carry_tid(tid);
					}
					else if (r.kind == madc::dis::DK_FPTR)
						pull_signature(r.ref0);
					else if (r.kind == madc::dis::DK_MEMBERPTR) {
						// v53: the owner (when the parse
						// resolved one) and the member type or
						// signature.
						if (r.body_unit)
							tq_named.push_back(r.body_unit);
						if (r.flags & madc::dis::DF_MEMBERPTR_FUNCTION)
							pull_signature(r.ref0);
						else
							tq_named.push_back(r.ref0);
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
					admit_agg(br.base_id, true);
			}
			for (uint32_t g = 0; g < r.anon_count; ++g) {
				madc::dis::anonrec ar;
				if (a.get_payload(r.anon_begin, g, ar))
					admit_agg(ar.sub_type_id, true);
			}
			for (uint32_t m = 0; m < r.methods_count; ++m) {
				madc::dis::methodrec mr;
				madc::dis::defrec fr;
				if (!a.get_payload(r.methods_begin, m, mr)
				    || !mr.func_id
				    || !a.get_def_at(mr.func_id, fr)
				    || fr.kind != madc::dis::DK_FUNC)
					continue;
				tq_named.push_back(fr.ref0);
				for (uint32_t p = 0; p < fr.params_count; ++p) {
					madc::dis::paramrec pr;
					if (a.get_payload(fr.params_begin, p, pr))
						tq_named.push_back(pr.type_id);
				}
			}
		}
		};	// drain_pulls
		drain_pulls();
		// R1 completion: fn-local classes admit by STRUCTURE, not by
		// name — they are unnameable outside their function, so they
		// never seed, but a bound inline body references its local
		// class (basic_string::_M_construct's Guard) and its types
		// must swizzle. Admit each fn-local class whose every member /
		// base chain already resolves inside the admitted set (true
		// exactly when its owner's world materialized), then drain the
		// pulls it opens; iterate to a fixpoint.
		//
		// Seeded: the dependents of each admitted class (pack slices
		// design §3 — a method-keyed import, deferred body or namespace-
		// scope Variable, a member template: each restores exactly when
		// its owner class does) join the candidates once the owner is
		// admitted, and a dependent global chases its type like a seeded
		// one. Same fixpoint.
		std::set<uint32_t> deps_done;
		auto add_dependents = [&]() -> bool {
			if (!seeded)
				return false;
			bool pushed = false;
			for (std::set<uint32_t>::const_iterator it = admitted.begin();
			     it != admitted.end(); ++it) {
				if (named_only.count(*it))
					continue;	// a declaration has no dependents
				if (!deps_done.insert(*it).second)
					continue;
				ForestSeeds::Run dr =
					sd.dependents(madc::dis::arena_slot_of(*it));
				for (uint32_t k = 0; k < dr.n; ++k) {
					uint32_t kind = cir_seed_item_kind(dr.p[k]);
					uint32_t idx = cir_seed_item_index(dr.p[k]);
					if (kind == CIR_SEED_DEP_ARENA) {
						bucket_slot(idx);
						if (par_on)
							par_carried_slots.insert(idx);
					} else if (kind == CIR_SEED_DEP_TEMPLATE) {
						tmpl_idx.push_back(idx);
						if (par_on)
							par_carried_tmpl.insert(idx);
					} else if (kind == CIR_SEED_DEP_GLOBAL
						 && idx < _globals.size()) {
						glob_idx.push_back(idx);
						if (par_on)
							par_carried_glob.insert(idx);
						size_t before = tq.size();
						seed_global(idx);
						pushed = pushed || tq.size() != before;
					}
				}
			}
			return pushed;
		};
		for (bool fnl_grew = true; fnl_grew; ) {
			fnl_grew = false;
			for (size_t i = 0; i < agg_ids.size(); ++i) {
				uint32_t tid = agg_ids[i];
				if (!recordable.count(tid) || admitted.count(tid))
					continue;
				madc::dis::defrec r;
				if (!a.get_def_at(tid, r)
				    || !(r.flags & madc::dis::DF_CLASS_FN_LOCAL))
					continue;
				bool ok = true;
				for (uint32_t m = 0; ok && m < r.members_count; ++m) {
					madc::dis::memberrec mr;
					if (!a.get_payload(r.members_begin, m, mr)
					    || !arena_chain_ok(a, mr.type_id, tid, admitted)
					    || (mr.vbase_id && !admitted.count(mr.vbase_id)))
						ok = false;
				}
				for (uint32_t b = 0; ok && b < r.bases_count; ++b) {
					madc::dis::baserec br;
					if (!a.get_payload(r.bases_begin, b, br)
					    || !admitted.count(br.base_id))
						ok = false;
				}
				if (!ok)
					continue;
				admitted.insert(tid);
				work.push_back(tid);
				par_carry_tid(tid);
				fnl_grew = true;
			}
			if (add_dependents())
				fnl_grew = true;
			if (fnl_grew)
				drain_pulls();
		}
		// Enums an admitted record's chain pulled restore even when no
		// candidate list carries them.
		for (std::set<uint32_t>::const_iterator it = pulled_enums.begin();
		     it != pulled_enums.end(); ++it)
			if (madc::dis::arena_id_is_project(*it))
				enum_slots.push_back(madc::dis::arena_slot_of(*it));
		if (seeded || !pulled_enums.empty()) {
			// Dependents and pulls joined after the seed lists: back
			// to the whole walk's order.
			forest_sort_unique(ns_slots);
			forest_sort_unique(typedef_slots);
			forest_sort_unique(enum_slots);
			forest_sort_unique(free_func_slots);
			forest_sort_unique(tmpl_idx);
			forest_sort_unique(glob_idx);
		}
		DBG(std::cout << "materialize filter: " << admitted.size()
			      << " of " << agg_ids.size()
			      << " candidate aggregates admitted ("
			      << pulled_enums.size() << " enums pulled, "
			      << (seeded ? seed_lists.size() : 0)
			      << " seed lists)" << std::endl);
	}

	// Pass 1: allocate a DataDef per recordable aggregate, so forward member /
	// base ids resolve in pass 2. The map persists on the forest
	// (_defs_by_tid) so post-restore consumers — lazy ClassPattern payload
	// reads — can swizzle serialized producer tids.
	std::map<uint32_t, DataDef *> &by_id = _defs_by_tid;
	// S2: this generation's fresh aggregate allocations — pass 2 fills
	// exactly these (earlier generations already filled theirs).
	std::set<uint32_t> fresh_aggs;
	// A declared-only product an earlier generation registered that this one
	// admits whole: filled in place (live completes a declared class at its
	// definition), registered once.
	std::set<uint32_t> completing;
	// Under a filter: the admitted set (seeds plus every pull, which can
	// reach past the candidate lists); ascending tids = slot order.
	std::vector<uint32_t> admitted_ids(admitted.begin(), admitted.end());
	const std::vector<uint32_t> &pass1_ids =
		_mat_filter.active ? admitted_ids : agg_ids;
	for (size_t i = 0; i < pass1_ids.size(); ++i) {
		uint32_t tid = pass1_ids[i];
		if (!recordable.count(tid))
			continue;
		if (by_id.count(tid)) {
			if (!named_only.count(tid) && _named_only.erase(tid)) {
				fresh_aggs.insert(tid);
				completing.insert(tid);
			}
			continue;	// built by an earlier generation
		}
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r))
			continue;
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		if (!nm || !*nm)
			continue;
		if (named_only.count(tid))
			_named_only.insert(tid);
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
			sdd = new DataDefCLASS(madc::dis::istring(nm), r.size,
					       DataType::dtRESERVED);
		else
			sdd = new DataDefSTRUCT(madc::dis::istring(nm), r.size);
		// A union with member functions is a CLASS with union layout
		// (libstdc++'s _Any_data); its members overlap by their recorded
		// offsets, restored verbatim like every aggregate's.
		sdd->union_layout = (r.flags & madc::dis::DF_UNION_LAYOUT) != 0;
		sdd->definition_origin = (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
		? AggregateDefinitionOrigin::TranslationUnitRoot
		: AggregateDefinitionOrigin::Included;
		sdd->forest_restored = true;	// mixed bind/live seam provenance (task #57)
		_mat_storage.push_back(sdd);
		by_id[tid] = sdd;
		fresh_aggs.insert(tid);
	}

	// Pass 1a (v21): ENUM types — leaves of the type graph, allocated before
	// the derived fixpoint so pointer/const chains over an enum resolve.
	// Size + canonical spelling load verbatim (a scoped `: size_t` enum is 8
	// bytes, not the ctor's int default).
	for (size_t bi = 0; bi < enum_slots.size(); ++bi) {
		uint32_t tid = madc::dis::arena_id_of(enum_slots[bi]);
		if (by_id.count(tid))
			continue;	// built by an earlier generation
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r) || r.kind != madc::dis::DK_ENUM)
			continue;
		// Rung 2a: an enum outside the bound closure skips unless an
		// admitted record's chain pulled it.
		if (_mat_filter.active && !pulled_enums.count(tid)
		    && !asked_slot(enum_slots[bi]) && !record_kept(r))
			continue;
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		if (!nm || !*nm)
			continue;
		DataDefENUM *edd = new DataDefENUM(madc::dis::istring(nm));
		// v48: the underlying type (ref0, a pinned primitive id) and
		// whether it was DECLARED (DF_ENUM_FIXED_BASE) restore apart —
		// promotion reads them apart ([conv.prom]/3-4); v27-v47 re-adopted
		// every recorded base as declared. The STORAGE (size + raw type)
		// restores verbatim, so the restored enum lowers to the same C
		// type the live parse emitted ([dcl.enum]p8; libc++
		// `enum class : uint8_t`, a packed enum, a 64-bit range).
		if (r.ref0)
			if (DataDef *u = swizzle(r.ref0))
			{
				edd->underlying = u;
				edd->fixed_base =
					(r.flags & madc::dis::DF_ENUM_FIXED_BASE) != 0;
			}
		// v49: defined in C, so compatible with that underlying type.
		edd->c_compatible = (r.flags & madc::dis::DF_ENUM_C_COMPATIBLE) != 0;
		if (r.size)
			edd->restore_layout(r.size, r.datatype ? r.datatype
						     : (uint32_t)edd->rawtype());
		if (r.canon_id)
			if (const char *cn = a.c_str(r.canon_id))
				edd->set_canonical_spelling(cn);
		_mat_storage.push_back(edd);
		by_id[tid] = edd;
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
	// S2: the definer map persists on the forest so a later generation's
	// import resolves a definer an earlier generation built.
	std::map<uint32_t, Variable *> &method_by_func_id = _method_by_func_id;
	struct PendingMethodImport {
		DataDefCLASS *cdd;
		uint32_t      func_id;
		madc::dis::istring   disp;
		uint32_t      mflags;
		uint32_t      rec_index;	// methodrec position — order parity
	};
	std::vector<PendingMethodImport> pending_method_imports;
	// Live ORDER parity for using-decl imports: `using _Base_type::construct;`
	// precedes the class's OWN same-name overloads in the header, so live's
	// methods vector and first-wins method_map slot hold the IMPORT first.
	// The restore stages imports for a post-pass (the definer may build
	// later), which appended them AFTER the own overloads — resolution then
	// tried the own overload first (LOADED != PARSED; the __alloc_traits
	// custom-pointer construct shadowing the imported allocator_traits one —
	// the v0.68 release-lane undefined `__alloc_traits...__construct`).
	// Track each restored method's methodrec index so the post-pass can
	// re-insert the import at its recorded position.
	std::map<DataDefCLASS *, std::vector<uint32_t> > method_rec_indices;
	std::map<std::pair<DataDefCLASS *, madc::dis::istring>, uint32_t>
		method_disp_first_rec;

	// Pass 2: fill each aggregate VERBATIM (offsets / counts / access /
	// origin / bitfields as stored, no finalize, no re-derivation) — then
	// class extras + methods under the v6 selection rules.
	// This generation's fresh allocations, ascending (earlier generations
	// already filled theirs).
	// The consumer-visible surface of a filled (or declared-only) aggregate:
	// a CirRestoredType, once per record.
	auto register_aggregate = [&](uint32_t tid, const madc::dis::defrec &r,
				      DataDefSTRUCT *sdd) {
		if (completing.count(tid))
			return;		// registered by the generation that declared it
		if (r.flags & madc::dis::DF_IS_ANONYMOUS)
			return;		// nameless sub-aggregate: never surfaced standalone
		if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
			return;		// v24: the program's own type — the forest holds
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
		rt.record     = (CIR_SEED_DEP_ARENA << 30) | madc::dis::arena_slot_of(tid);
		note_demand_restored(rt.record, _restored.size());
		_restored.push_back(rt);
		if (par_on)
			_par_via.types.push_back(par_label(asked_slots, par_listed_slots, par_carried_slots,
							   madc::dis::arena_slot_of(tid)));
	};
	static const char *named_only_probe = ::getenv("MADC_NAMED_ONLY_PROBE");
	for (std::set<uint32_t>::const_iterator fi = fresh_aggs.begin();
	     fi != fresh_aggs.end(); ++fi) {
		uint32_t tid = *fi;
		std::map<uint32_t, DataDef *>::iterator ai = by_id.find(tid);
		if (ai == by_id.end())
			continue;
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(ai->second);
		if (!sdd)
			continue;
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r))
			continue;
		if (named_only.count(tid)) {
			// Declared, never defined: the incomplete class a live parse
			// of this closure holds — its identity (name, canonical
			// template-id spelling), no layout, no members.
			if (named_only_probe && *named_only_probe == '1')
				fprintf(stderr, "NAMEDONLY %s\n",
					r.canon_id ? a.c_str(r.canon_id) : a.c_str(r.name_id));
			sdd->size = 0;
			sdd->is_complete = false;
			if (r.canon_id)
				if (const char *cs = a.c_str(r.canon_id))
					sdd->set_canonical_spelling(cs);
			if (DataDefCLASS *cdd = dynamic_cast<DataDefCLASS *>(sdd))
				cdd->from_system_header =
					(r.flags & madc::dis::DF_FROM_SYSTEM_HDR) != 0;
			sdd->type_id = 0;
			register_aggregate(tid, r, sdd);
			continue;
		}
		bool ok = true;
		madc::dis::istring fill_why;
		for (uint32_t m = 0; m < r.members_count; ++m) {
			madc::dis::memberrec mr;
			if (!a.get_payload(r.members_begin, m, mr)) {
				ok = false;
				fill_why = "member payload #" + std::to_string(m);
				break;
			}
			DataDef *mdd = swizzle(mr.type_id);
			// String id zero is both the arena's empty-string sentinel and the
			// correct C spelling of an unnamed bit-field.  Admit that exact
			// structural case; a non-bit-field member still requires a name.
			const char *mnm = mr.name_id ? a.c_str(mr.name_id)
				: ((mr.bf_flags & 1u) ? "" : NULL);
			if (!mdd || !mnm) {
				ok = false;
				fill_why = std::string("member ")
					 + (mr.name_id && a.c_str(mr.name_id)
					    ? a.c_str(mr.name_id) : "?")
					 + (mdd ? " (no name)" : " (type chain)");
				break;
			}
			sdd->members.push_back(memberpair_t(madc::dis::istring(mnm), mdd));
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
			// v32: virtual-base provenance, swizzled like every class
			// cross-ref. A dangling id means the closure failed — drop
			// the aggregate rather than silently degrade to static.
			if (mr.vbase_id) {
				DataDefCLASS *vbc = dynamic_cast<DataDefCLASS *>(
					swizzle(mr.vbase_id));
				if (!vbc) {
					ok = false;
					fill_why = std::string("member ")
						 + (mnm && *mnm ? mnm : "?") + " vbase";
					break;
				}
				sdd->member_vbase[m] = vbc;
			}
		}
		if (!ok) {
			// An ADMITTED record that cannot fill is an INVISIBLE loss: the
			// consumer gets neither a definition nor a live rescue, so the
			// class materializes as nothing at all (task #64 — std::ios_base
			// vanished from a bound darwin forest exactly here, and the only
			// symptom was `struct has no member __vptr` a whole layer later).
			// Name it, the fill-side twin of the closure diagnostic above.
			DBG(std::cout << "materialize fill: DROPPED "
			    << (r.name_id && a.c_str(r.name_id) ? a.c_str(r.name_id) : "?")
			    << " (" << fill_why << ")" << std::endl);
			continue;		// defensive: the closure should have dropped it
		}
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
				swizzle(ar.sub_type_id));
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
					swizzle(vr.class_id));
				if (vbc)
					cdd->vbase_offset[vbc] = vr.offset;
			}
			for (uint32_t vg = 0; vg < r.vgroup_count; ++vg) {
				madc::dis::vgrouprec gr;
				if (!a.get_payload(r.vgroup_begin, vg, gr))
					break;
				DataDefCLASS::VtableGroup g;
				g.owner = dynamic_cast<DataDefCLASS *>(
					swizzle(gr.owner_id));
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
			// v42: the flat vtable_slots list and the virtual_methods
			// name set — the parse-time state the groups were derived
			// from. Pass 1.45/1.8 gate the deleting dtor on
			// vtable_slot("~$deleting"), `delete p` dispatches on it,
			// and a consumer-side derived class detects an override
			// through is_virtual_method(): all four answered "not
			// virtual" for every restored class before this run.
			cdd->vtable_slots.clear();
			for (uint32_t sl = 0; sl < r.vslot_count; ++sl) {
				uint32_t nid = 0;
				if (!a.get_word(r.vslot_begin, sl, nid))
					break;
				const char *sn = a.c_str(nid);
				if (sn)
					cdd->vtable_slots.push_back(sn);
			}
			for (uint32_t vm = 0; vm < r.vmeth_count; ++vm) {
				uint32_t nid = 0;
				if (!a.get_word(r.vmeth_begin, vm, nid))
					break;
				const char *vn = a.c_str(nid);
				if (vn)
					cdd->virtual_methods[vn] = true;
			}
			// v43: the friendship grants — the access check's name
			// state (a hoisted hidden-friend body reads the private
			// member it was befriended for).
			cdd->friend_function_names.clear();
			for (uint32_t ff = 0; ff < r.friendfn_count; ++ff) {
				uint32_t nid = 0;
				if (!a.get_word(r.friendfn_begin, ff, nid))
					break;
				const char *fn = a.c_str(nid);
				if (fn)
					cdd->friend_function_names.push_back(fn);
			}
			cdd->friend_class_names.clear();
			for (uint32_t fc = 0; fc < r.friendcls_count; ++fc) {
				uint32_t nid = 0;
				if (!a.get_word(r.friendcls_begin, fc, nid))
					break;
				const char *cn = a.c_str(nid);
				if (cn)
					cdd->friend_class_names.push_back(cn);
			}
			// The layout-time secondary owners list is not in the
			// record; the groups it produced are. Refill it so the
			// struct emitter names every secondary vptr field the
			// restored constructors stamp (LOADED == parsed).
			cdd->secondary_vptr_owners_from_groups();
			bool bok = true;
			for (uint32_t b = 0; b < r.bases_count; ++b) {
				madc::dis::baserec br;
				if (!a.get_payload(r.bases_begin, b, br)) {
					bok = false;
					fill_why = "base payload #" + std::to_string(b);
					break;
				}
				DataDefCLASS *bc = dynamic_cast<DataDefCLASS *>(
					swizzle(br.base_id));
				if (!bc) {
					madc::dis::defrec bd;
					fill_why = std::string("base ")
						 + (a.get_def_at(br.base_id, bd) && bd.name_id
						    && a.c_str(bd.name_id)
						    ? a.c_str(bd.name_id) : "?");
					bok = false;
					break;
				}
				BaseSpec bs;
				bs.base       = bc;
				bs.offset     = br.offset;
				bs.is_virtual = (br.flags & madc::dis::BSF_VIRTUAL) != 0;
				bs.is_primary = (br.flags & madc::dis::BSF_PRIMARY) != 0;
				bs.access     = br.flags >> madc::dis::BSF_ACCESS_SHIFT;
				cdd->bases.push_back(bs);
			}
			if (!bok) {
				DBG(std::cout << "materialize fill: DROPPED "
				    << (r.name_id && a.c_str(r.name_id)
					? a.c_str(r.name_id) : "?")
				    << " (" << fill_why << ")" << std::endl);
				continue;
			}
			cdd->base_class = cdd->bases.empty() ? NULL : cdd->bases[0].base;

			// Methods: the arena run holds EVERY parsed method; apply the
			// v6 save-side selection here so the restored surface matches
			// (symbol-less non-dtor specials, display-less plain methods,
			// and unresolvable signatures all lack). MEMBER-TEMPLATE
			// records restore VERBATIM: the decl-only placeholders at
			// their saved __oN ranks (live registers each pattern as one —
			// register_skipped_class_template_function; the flush attaches
			// the CIR_TMPLK_MEMBER record and the pattern fields thaw at
			// first content read — madc_thaw_member_template) and the
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
						pi.cdd       = cdd;
						pi.func_id   = md.func_id;
						pi.disp      = dispkey ? dispkey : "";
						pi.mflags    = md.flags;
						pi.rec_index = mi;
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
				DataDef *ret = swizzle(fr.ref0);
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
					// hidden __this — excluded from mangling, but the
					// const and spelling arrays stay index-aligned
					// with parameters (parseFunction parity).
					fd->const_params.push_back(false);
					fd->param_cpp_spellings.push_back(madc::dis::istring());
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
					DataDef *pd = swizzle(pr.type_id);
					if (!pd) { pok = false; break; }
					// The C++ param spelling rides the paramrec exactly
					// as on the free-function arm below: without it a
					// restored method's signature is NOT the parsed
					// state, and a consumer-side Itanium re-mangle
					// (bind_external_class_symbols) fabricates a
					// parameter-less symbol (C1Ev) for a ctor whose
					// pack-side binding stayed empty.
					restore_param(fd, pd, pr, a);
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
				madc::dis::istring dispname =
					dispkey ? madc::dis::istring(dispkey) : madc::dis::istring();
				fd->method_display_name = dispname;	// v6 parity: the KEY rides here
				if (fr.emit_symbol_id)
					if (const char *es = a.c_str(fr.emit_symbol_id))
						fd->emit_symbol = es;
				fd->is_const_method =
					(fr.flags & madc::dis::DF_IS_CONST_METHOD) != 0;
				fd->is_volatile_method =
					(fr.flags & madc::dis::DF_IS_VOLATILE_METHOD) != 0;
				fd->pure_virtual =
					(fr.flags & madc::dis::DF_PURE_VIRTUAL) != 0;
				fd->defaulted_or_deleted =
					(fr.flags & madc::dis::DF_FUNC_DEFAULTED_OR_DELETED) != 0;
				fd->is_deleted =
					(fr.flags & madc::dis::DF_FUNC_IS_DELETED) != 0;
				fd->is_explicit =
					(md.flags & madc::dis::MF_EXPLICIT) != 0;
				fd->noexcept_spec =
					(fr.flags & madc::dis::DF_NOEXCEPT_TRUE) ? FuncDef::NxTrue
					: (fr.flags & madc::dis::DF_NOEXCEPT_UNKNOWN) ? FuncDef::NxUnknown
					: FuncDef::NxNone;
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
					fd->forest_body_src  = this;	// B3: the body's forest
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
				    && madc::dis::istring(mnm) != cdd->name + "__" + dispname)
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
				    && madc::dis::istring(mnm) != cdd->name + "__" + cdd->name)
					fd->local_emit_name = mnm;
				Variable *mv = new Variable(madc::dis::istring(mnm ? mnm : ""),
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
						     ? swizzle(ar.type_id)
						     : NULL;
					if (!pn || !*pn || !ptp)
						continue;
					Variable *pv = new Variable(madc::dis::istring(pn),
								    *ptp, 1, NULL, false);
					pv->flags |= vfPARAM | vfLOCAL;
					_mat_vars.push_back(pv);
					mm->parameters.push_back(pv);
				}
				_mat_vars.push_back(mv);
				cdd->methods.push_back(mv);
				method_rec_indices[cdd].push_back(mi);
				method_by_func_id[md.func_id] = mv;
				if (!dispname.empty()
				    && cdd->method_map.find(dispname)
				       == cdd->method_map.end())
					method_disp_first_rec[std::make_pair(
						cdd, dispname)] = mi;
				if (!dispname.empty())
					cdd->method_map[dispname] = mv;
				if (is_ctor) {
					cdd->ctors.push_back(mv);
					// Env-gated probe (MADC_COPY_PROBE=<substr>):
					// the methodrec-arm ctor push — which restore
					// path populates a class's ctor set.
					static const char *cpp_ =
						::getenv("MADC_COPY_PROBE");
					if (cpp_ && *cpp_ && cdd->name.find(cpp_)
							     != madc::dis::istring::npos)
						fprintf(stderr, "[ctor-push] matrec"
							" cls=%s var=%s fd=%p len='%s'\n",
							cdd->name.c_str(),
							mv->name.c_str(), (void *)fd,
							fd->local_emit_name.c_str()); // allowed-exception: debug print, not symbol build
				}
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
				DataDef *ad = swizzle(ar.type_id);
				if (!an || !*an || !ad)
					continue;
				// Env-gated probe (MADC_MTI_PROBE_CLASS=<substr>):
				// every restored class-scope type alias whose OWNER
				// matches — the wrong-alias-target diagnostic.
				{
					static const char *mtp =
						::getenv("MADC_MTI_PROBE_CLASS");
					if (mtp && *mtp
					    && cdd->name.find(mtp) != madc::dis::istring::npos)
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
				DataDef *ad = swizzle(ar.type_id);
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
		register_aggregate(tid, r, sdd);
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
		if (!present) {
			// Order parity: insert at the import's methodrec position
			// among this class's restored methods, exactly where the
			// live class-body parse placed it (using-decl first).
			std::vector<uint32_t> &recs = method_rec_indices[pi.cdd];
			size_t pos = 0;
			while (pos < recs.size() && recs[pos] < pi.rec_index)
				++pos;
			if (pos > pi.cdd->methods.size())
				pos = pi.cdd->methods.size();
			pi.cdd->methods.insert(pi.cdd->methods.begin() + pos, mv);
			recs.insert(recs.begin() + pos, pi.rec_index);
		}
		if (!pi.disp.empty()) {
			std::map<std::pair<DataDefCLASS *, madc::dis::istring>,
				 uint32_t>::iterator fr =
				method_disp_first_rec.find(
					std::make_pair(pi.cdd, pi.disp));
			// First-wins parity: the import owns the method_map slot
			// when its methodrec precedes every own claimant's (live:
			// the using-decl came first in the class body).
			if (pi.cdd->method_map.find(pi.disp)
			    == pi.cdd->method_map.end()
			    || (fr != method_disp_first_rec.end()
				&& pi.rec_index < fr->second)
			    || fr == method_disp_first_rec.end())
				pi.cdd->method_map[pi.disp] = mv;
			if (fr == method_disp_first_rec.end()
			    || pi.rec_index < fr->second)
				method_disp_first_rec[std::make_pair(
					pi.cdd, pi.disp)] = pi.rec_index;
		}
		if (pi.mflags & madc::dis::MF_CTOR) {
			pi.cdd->ctors.push_back(mv);
			// Env-gated probe (MADC_COPY_PROBE=<substr>): ctor-set push.
			static const char *cpp_ = ::getenv("MADC_COPY_PROBE");
			if (cpp_ && *cpp_
			    && pi.cdd->name.find(cpp_) != madc::dis::istring::npos)
				fprintf(stderr, "[ctor-push] import cls=%s var=%s\n",
					pi.cdd->name.c_str(), mv->name.c_str());
		}
	}

	// Pass 3: DK_TYPEDEF records (flat + namespaced aliases, at their
	// synthetic slots) -> (name, ns, underlying).
	//
	// Env-gated probe (MADC_ALIAS_PROBE=<substr>): report every matching
	// alias record with its namespace, target id and verdict — the arena-side
	// twin of MADC_DECLIDX_PROBE. Each `continue` below drops an alias the
	// consumer then fails to resolve with "use of undeclared identifier",
	// naming the alias and never the target, so the drop is invisible from
	// the outside; the probe is what makes it a measurement.
	static const char *aliprobe = ::getenv("MADC_ALIAS_PROBE");
	auto alias_say = [&](const madc::dis::defrec &r, const char *nm,
			     const char *verdict) {
		const char *ns = r.ns_id ? a.c_str(r.ns_id) : NULL;
		fprintf(stderr, "ALIAS %s%s%s ref0=%u %s\n", ns ? ns : "",
			ns && *ns ? "::" : "", nm, r.ref0, verdict);
	};
	for (size_t bi = 0; bi < typedef_slots.size(); ++bi) {
		uint32_t s = typedef_slots[bi];
		if (_mat_done_slots.count(s))
			continue;	// pushed by an earlier generation
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r)
		    || r.kind != madc::dis::DK_TYPEDEF)
			continue;
		const char *nm = r.name_id ? a.c_str(r.name_id) : NULL;
		const bool probed = aliprobe && *aliprobe && nm
				  && strstr(nm, aliprobe) != NULL;
		if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN) {
			if (probed)
				alias_say(r, nm, "DROPPED (tu-root fence)");
			continue;	// v24: the program's own typedef — fenced
		}
		if (!nm || !*nm)
			continue;
		DataDef *underlying = swizzle(r.ref0);
		if (!underlying) {
			if (probed)
				alias_say(r, nm, "DROPPED (target not materialized)");
			continue;
		}
		if (probed)
			alias_say(r, nm, "kept");
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
		rt.record     = (CIR_SEED_DEP_ARENA << 30) | s;
		note_demand_restored(rt.record, _restored.size());
		_restored.push_back(rt);
		if (par_on)
			_par_via.types.push_back(par_label(asked_slots, par_listed_slots, par_carried_slots, s));
		_mat_done_slots.insert(s);
	}

	// Pass 3a (v25): namespace-surface records — inline-namespace links
	// (NSLINK: the flush re-runs the live mirror) and using-declaration fn
	// imports (NSBIND: the flush rebinds namespace_map[ns][name] to the
	// restored fn's Variable).
	for (size_t bi = 0; bi < ns_slots.size(); ++bi) {
		uint32_t s = ns_slots[bi];
		if (_mat_done_slots.count(s))
			continue;	// pushed by an earlier generation
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r))
			continue;
		if (r.kind == madc::dis::DK_NSLINK) {
			CirRestoredNsLink l;
			l.parent = r.ns_id ? a.c_str(r.ns_id) : NULL;
			l.child  = r.name_id ? a.c_str(r.name_id) : NULL;
			if (l.child && *l.child) {
				_restored_nslinks.push_back(l);
				_mat_done_slots.insert(s);
			}
		} else if (r.kind == madc::dis::DK_NSBIND) {
			CirRestoredNsBind b;
			b.ns   = r.ns_id ? a.c_str(r.ns_id) : NULL;
			b.name = r.name_id ? a.c_str(r.name_id) : NULL;
			b.key  = r.disp_id ? a.c_str(r.disp_id) : NULL;
			b.ov_member =
				(r.flags & madc::dis::DF_NSBIND_OVERLOAD_MEMBER) != 0;
			if (b.ns && *b.ns && b.name && *b.name && b.key && *b.key) {
				_restored_nsbinds.push_back(b);
				if (par_on)
					_par_via.binds.push_back(par_label(asked_slots, par_listed_slots,
									   par_carried_slots, s));
				_mat_done_slots.insert(s);
			}
		} else if (r.kind == madc::dis::DK_NSALIAS) {
			// v27: a namespace alias — the flush re-adds the map entry.
			CirRestoredNsAlias na;
			na.alias  = r.ns_id ? a.c_str(r.ns_id) : NULL;
			na.target = r.name_id ? a.c_str(r.name_id) : NULL;
			if (na.alias && *na.alias && na.target && *na.target) {
				_restored_nsaliases.push_back(na);
				_mat_done_slots.insert(s);
			}
		} else if (r.kind == madc::dis::DK_DEFBODY) {
			// v26: a deferred method body — four token runs (word
			// quads: off/bytes/count/file_id) at params_begin.
			if (r.params_count != 4 || !r.name_id)
				continue;
			CirRestoredDeferredBody d;
			d.key   = a.c_str(r.name_id);
			d.owner = NULL;
			if (r.ref0) {
				DataDef *od = swizzle(r.ref0);
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
				uint32_t w[4];
				for (uint32_t k = 0; ok && k < 4; ++k)
					ok = a.get_word(off, k, w[k]);
				if (!ok)
					break;
				CirRestoredTemplateRun &run = d.runs[sx];
				run.bytes = NULL;
				run.len   = w[1];
				run.count = w[2];
				run.file  = w[3] ? a.c_str(w[3]) : NULL;
				if (run.count)
					run.bytes = a.tok_run(w[0], run.len);
			}
			if (ok) {
				_restored_defbodies.push_back(d);
				if (par_on)
					_par_via.bodies.push_back(par_label(asked_slots, par_listed_slots,
									    par_carried_slots, s));
				_mat_done_slots.insert(s);
			}
		}
	}

	// Pass 3b (v21): DK_ENUM records -> (name, ns, dd, enumerators). The
	// DataDefENUM was allocated in pass 1a; the scoped enumerator values ride
	// the record's constvalrec run and rebuild as constant Variables in the
	// tag's pseudo-namespace on the parser side.
	for (size_t bi = 0; bi < enum_slots.size(); ++bi) {
		uint32_t s = enum_slots[bi];
		uint32_t tid = madc::dis::arena_id_of(s);
		if (_mat_done_slots.count(s))
			continue;	// pushed by an earlier generation
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r) || r.kind != madc::dis::DK_ENUM)
			continue;
		if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
			continue;	// v24: the program's own enum — fenced
		if (r.flags & madc::dis::DF_ENUM_CLASS_NESTED)
			continue;	// [basic.scope.class]/1: a class-nested tag is
					// a MEMBER — live never publishes it flat or in
					// the namespace, and neither may the restore. Its
					// object is still built in pass 1a (so member /
					// param / fn-ptr-signature ids swizzle) and the
					// owner's type_aliases restore re-attaches it,
					// which IS its whole live registration.
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
		rt.record = (CIR_SEED_DEP_ARENA << 30) | s;
		note_demand_restored(rt.record, _restored.size());
		_restored.push_back(rt);
		if (par_on)
			_par_via.types.push_back(par_label(asked_slots, par_listed_slots, par_carried_slots, s));
		_mat_done_slots.insert(s);
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
	for (size_t bi = 0; bi < free_func_slots.size(); ++bi) {
		uint32_t s = free_func_slots[bi];
		if (_mat_done_slots.count(s))
			continue;	// pushed by an earlier generation
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
		// unindexed = derived entity = keep. An asked record is kept.
		if (_mat_filter.active && !asked_slot(s)) {
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
		DataDef *ret = swizzle(r.ref0);
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
			DataDef *pd = swizzle(pr.type_id);
			if (!pd) { pok = false; break; }
			// v21: the C++ param spelling rides the record — the
			// Itanium mangle of a declaration-only ns function
			// (storage_alias_name at flush) reads it.
			restore_param(fd, pd, pr, a);
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
		fd->defaulted_or_deleted =
			(r.flags & madc::dis::DF_FUNC_DEFAULTED_OR_DELETED) != 0;
		fd->is_deleted = (r.flags & madc::dis::DF_FUNC_IS_DELETED) != 0;
		fd->is_void_params   = (r.flags & madc::dis::DF_IS_VOID_PARAMS) != 0;
		fd->c_linkage        = (r.flags & madc::dis::DF_FUNC_C_LINKAGE) != 0;
		fd->noexcept_spec    = (r.flags & madc::dis::DF_NOEXCEPT_TRUE) ? FuncDef::NxTrue
				     : (r.flags & madc::dis::DF_NOEXCEPT_UNKNOWN) ? FuncDef::NxUnknown
				     : FuncDef::NxNone;
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
		fd->template_return_deduce_forwarding =
			(r.flags & madc::dis::DF_TRET_FWD) != 0;
		fd->template_return_ref =
			!(r.flags & madc::dis::DF_TRET_REF) ? RefKind::None
			: (r.flags & madc::dis::DF_TRET_RREF) ? RefKind::Rvalue
			: RefKind::Lvalue;
		// v46: the overload-set DECLARATION IDENTITY — the parameter
		// spelling and an instantiation product's template-argument
		// identity spellings — so the restored member ranks exactly as
		// the live declaration did (the explicit-template-argument
		// prefix match binds it; the ambiguity verdict sees it).
		if (r.ovl_spelling_id)
			if (const char *os = a.c_str(r.ovl_spelling_id))
				fd->overload_spelling = os;
		for (uint32_t t = 0; t < r.ovl_targ_count; ++t) {
			uint32_t nid = 0;
			if (!a.get_word(r.ovl_targ_begin, t, nid))
				break;
			const char *tn = a.c_str(nid);
			fd->overload_template_args.push_back(tn ? tn : "");
		}
		if (has_body) {
			fd->has_forest_body  = true;
			fd->forest_body_unit = r.body_unit;
			fd->forest_body_idx  = r.body_idx;
			fd->forest_body_src  = this;	// B3: the body's forest
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
		rf.record = (CIR_SEED_DEP_ARENA << 30) | s;
		note_demand_restored(rf.record, _restored_funcs.size());
		// v26 piece (a): the fn's NAMED parameters (aliasrec run) — the
		// flush fills Method::parameters so a deferred body's re-parse
		// resolves its parameter names.
		for (uint32_t ai = 0; ai < r.alias_count; ++ai) {
			madc::dis::aliasrec ar;
			if (!a.get_payload(r.alias_begin, ai, ar) || !ar.name_id)
				continue;
			const char *pn = a.c_str(ar.name_id);
			DataDef *ptp = ar.type_id
				     ? swizzle(ar.type_id) : NULL;
			if (!pn || !*pn || !ptp)
				continue;
			rf.mparams.push_back(std::make_pair(pn, ptp));
		}
		_restored_funcs.push_back(rf);
		if (par_on)
			_par_via.funcs.push_back(par_label(asked_slots, par_listed_slots, par_carried_slots, s));
		_mat_done_slots.insert(s);
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
	for (size_t gi = 0; gi < glob_idx.size(); ++gi) {
		size_t i = glob_idx[gi];
		if (i >= _globals.size() || _mat_done_globals.count(i))
			continue;		// pushed by an earlier generation
		const cir_forest_global_record &g = _globals[i];
		if (g.gflags & CIR_GLOBALF_TU_ROOT)
			continue;		// v24: the program's own global — fenced
		if (withheld_global(i))
			continue;		// the dialect's, restored as demanded
		uint32_t nlen = 0;
		const char *nm = pool_cstr(g.name_id, nlen);
		DataDef *ty = swizzle(g.type_id);
		if (!nm || !ty)
			continue;		// unresolved type -> bind cleanly lacks it
		CirRestoredGlobal rg;
		rg.name       = nm;
		rg.type       = ty;
		rg.flags      = g.flags;
		rg.gflags     = g.gflags;
		uint32_t nslen = 0;
		rg.ns         = g.ns_id ? pool_cstr(g.ns_id, nslen) : NULL;	// v22
		uint32_t allen = 0;
		rg.alias      = g.alias_id ? pool_cstr(g.alias_id, allen) : NULL;	// v39
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
		// v66: the frozen storage declaration + init group — words
		// [def_unit, def_idx, n, n (unit, idx) pairs] at init_value.
		rg.def_unit = rg.def_idx = 0;
		if (g.gflags & CIR_GLOBALF_FROZEN_DEF) {
			const uint32_t at = (uint32_t)g.init_value;
			uint32_t n = 0;
			bool locs_ok = a.get_word(at, 0, rg.def_unit)
				    && a.get_word(at, 1, rg.def_idx)
				    && a.get_word(at, 2, n);
			for (uint32_t k = 0; locs_ok && k < n; ++k) {
				uint32_t iu = 0, ix = 0;
				locs_ok = a.get_word(at, 3 + 2 * k, iu)
				       && a.get_word(at, 4 + 2 * k, ix);
				if (locs_ok)
					rg.init_locs.push_back(std::make_pair(iu, ix));
			}
			if (!locs_ok) {
				fprintf(stderr, "madc: forest global %s: frozen"
					" locations out of range\n", nm);
				continue;
			}
		}
		rg.record = (CIR_SEED_DEP_GLOBAL << 30) | (uint32_t)i;
		note_demand_restored(rg.record, _restored_globals.size());
		_restored_globals.push_back(rg);
		if (par_on)
			_par_via.globals.push_back(par_label_idx(asked_glob, par_listed_glob,
								       par_carried_glob, i));
		_mat_done_globals.insert(i);
	}

	// v20 (widening slice 2): build the restored TEMPLATE-NAME view — names
	// resolved against the container pool, the owner class swizzled through
	// the same by_id, each token run exposed as a span into the loaded
	// TEMPLATE_TOKENS bytes. Token materialization happens on the parser side
	// (forest_restore_decls), where the active spelling pool + intern_file
	// live. A record whose owner class did not restore cleanly LACKS (a
	// member-template pattern without its class cannot instantiate anyway).
	{
		for (size_t ti = 0; ti < tmpl_idx.size(); ++ti) {
			size_t i = tmpl_idx[ti];
			if (i >= _templates.size() || _mat_done_templates.count(i))
				continue;	// pushed by an earlier generation
			if (withheld_template(i))
				continue;	// the dialect's, restored as demanded
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
			// Owner fence FIRST (R1): a member-template pattern
			// without its restored class cannot instantiate, and
			// judging it before any payload touch keeps the
			// segment decode demand-driven — every derived
			// (unindexed) member-template record of an
			// unmaterialized instantiated class drops here.
			DataDefCLASS *oc = NULL;
			if (t.owner_type_id) {
				oc = dynamic_cast<DataDefCLASS *>(
					swizzle(t.owner_type_id));
				if (!oc)
					continue;	// owner did not restore -> cleanly lack
			}
			// Rung-2a parity for template state (R1): the SAME
			// verdict the admitted-set seeding applies (key under
			// its ns, unindexed = derived = keep) gates the
			// record — and the payload/token segments decode only
			// at the first surviving record, so a TU whose bound
			// closure declares no templates (trivial C) never
			// pays their zstd cost. An asked record is kept.
			if (_mat_filter.active && !asked_template(i)) {
				uint32_t sl = 0;
				const char *tns = t.ns_id ? pool_cstr(t.ns_id, sl)
							  : NULL;
				int tv = verdict_str(key, tns);
				DBG(fprintf(stderr,
					    "forest template verdict %d key=%s ns=%s"
					    " kind=%u owner=%u\n",
					    tv, key, tns ? tns : "-",
					    (unsigned)t.kind,
					    (unsigned)t.owner_type_id));
				if (tv < 0)
					continue;
			}
			// slice B1: IDENTITY only — no payload touch. The raw
			// offsets ride the record; params / run table / pattern
			// slice decode in hydrate_restored_template() on first
			// demand (and the payload/token SEGMENTS stay cold until
			// the first hydrate of any record).
			CirRestoredTemplate rt;
			rt.kind  = t.kind;
			rt.key   = key;
			rt.name  = t.name_id ? pool_str(t.name_id) : NULL;
			rt.ns    = t.ns_id ? pool_str(t.ns_id) : NULL;
			rt.extra = t.extra_id ? pool_str(t.extra_id) : NULL;
			rt.owner = oc;	// swizzled by the owner fence above (NULL = ns-scope)
			rt.flags = t.flags;
			rt.pattern = NULL;
			rt.pattern_words = 0;
			rt.pattern_reason = t.pattern_reason;
			rt.rec_param_begin   = t.param_begin;
			rt.rec_param_count   = t.param_count;
			rt.rec_run_begin     = t.run_begin;
			rt.rec_spec_count    = t.spec_count;
			rt.rec_pattern_begin = t.pattern_begin;
			rt.rec_pattern_words = t.pattern_words;
			rt.record = (CIR_SEED_DEP_TEMPLATE << 30) | (uint32_t)i;
			// One slot per frozen record, reserved at the first push: an
			// append never moves a record the parser points into.
			if (_restored_templates.capacity() < _templates.size())
				_restored_templates.reserve(_templates.size());
			note_demand_restored(rt.record, _restored_templates.size());
			_restored_templates.push_back(rt);
			if (par_on)
				_par_via.tmpls.push_back(par_label_idx(asked_tmpl, par_listed_tmpl,
									      par_carried_tmpl, i));
			_mat_done_templates.insert(i);
		}
	}

	// The seed-parity probe (scripts/check-pack-seed-parity.sh): after each
	// generation, every record restored so far with the verdict registration
	// gives it under the filter as it now stands (the gate reads the LAST
	// block, so a record restored early and one restored by a later, wider
	// generation are judged alike) and how it was reached (_par_via, above):
	// ` demanded`, ` listed`, ` carried`, or ` unreached`. A record a dialect
	// fragment's decl index names carries its role ahead of that: ` anchor` —
	// a global with storage, which the bind restores
	// (CirDeclRecordFacts::anchor) — or ` dialect`, which restores only as
	// demanded or carried (the gate fails a ` dialect listed` line).
	if (par_on) {
		// The generation's forest by role (B2: a dialect compile binds the
		// image beside the system pack; the gate keeps each forest's last
		// block, so one forest's generation never hides the other's).
		const cir_forest_role_row *par_role = cir_forest_role_info(_role);
		fprintf(stderr, "SEEDPAR-GEN %s\n", par_role ? par_role->name : "role?");
		std::map<uint32_t, bool> par_dialect;	// record -> an anchor
		std::vector<cir_forest_decl_entry> di;
		for (uint32_t u = 0; u < unit_count(); ++u) {
			const char *un = unit_name(u);
			if (!un || !embedded_dialect_fragment_p(madc::dis::istring(un))
			    || !unit_decl_index(u, di))
				continue;
			for (size_t k = 0; k < di.size(); ++k) {
				CirDeclRecordFacts f;
				if (decl_record_facts(di[k].kind, di[k].record_id, f))
					par_dialect[di[k].record_id] = f.anchor();
			}
		}
		auto say = [&](const char *kind, const char *ns, const char *nm, int v,
			       uint32_t record, const std::vector<char> &via, size_t i) {
			const char c = i < via.size() ? via[i] : '?';
			std::map<uint32_t, bool>::const_iterator d = par_dialect.find(record);
			fprintf(stderr, "SEEDPAR %s %s%s%s v=%d%s %s\n", kind,
				ns && *ns ? ns : "", ns && *ns ? "::" : "",
				nm ? nm : "?", v,
				d == par_dialect.end() ? "" : d->second ? " anchor" : " dialect",
				c == 'd' ? "demanded" : c == 'l' ? "listed"
				: c == 'c' ? "carried" : "unreached");
		};
		for (size_t i = 0; i < _restored.size(); ++i) {
			const CirRestoredType &t = _restored[i];
			int v = verdict_str(t.name, t.ns);
			if (t.ns && *t.ns && t.kind != CIR_TYPEK_TYPEDEF)
				v = std::max(v, verdict_str(t.name, NULL));
			if (t.dd && !t.dd->canonical_cpp_spelling().empty()
			    && head_verdict(t.dd->canonical_cpp_spelling().c_str(),
					    _mat_filter) < 0)
				v = -1;
			// A template-id product is placed by the units requiring
			// it, not by its template's name (design §4): its own kind.
			bool product = t.dd && t.dd->canonical_cpp_spelling().find('<')
					      != madc::dis::istring::npos;
			say(t.kind == CIR_TYPEK_TYPEDEF ? "typedef"
			    : t.kind == CIR_TYPEK_ENUM ? "enum"
			    : product ? "product" : "tag", t.ns, t.name, v, t.record,
			    _par_via.types, i);
		}
		for (size_t i = 0; i < _restored_funcs.size(); ++i) {
			const CirRestoredFunc &f = _restored_funcs[i];
			int v = verdict_str(f.name, NULL);
			if (v == 0 && f.fd && !f.fd->function_display_name.empty()
			    && !f.fd->namespace_name.empty())
				v = verdict_str(f.fd->function_display_name.c_str(),
						f.fd->namespace_name.c_str());
			say(f.fd && f.fd->is_specialization_product() ? "instance" : "func",
			    NULL, f.name, v, f.record, _par_via.funcs, i);
		}
		for (size_t i = 0; i < _restored_globals.size(); ++i)
			say("global", _restored_globals[i].ns, _restored_globals[i].name,
			    verdict_str(_restored_globals[i].name, _restored_globals[i].ns),
			    _restored_globals[i].record, _par_via.globals, i);
		for (size_t i = 0; i < _restored_templates.size(); ++i)
			say("template", _restored_templates[i].ns, _restored_templates[i].key,
			    verdict_str(_restored_templates[i].key, _restored_templates[i].ns),
			    _restored_templates[i].record, _par_via.tmpls, i);
		for (size_t i = 0; i < _restored_nsbinds.size(); ++i)
			say("nsbind", _restored_nsbinds[i].ns, _restored_nsbinds[i].key,
			    verdict_str(_restored_nsbinds[i].key, NULL), CIR_DECL_RECORD_NONE,
			    _par_via.binds, i);
		for (size_t i = 0; i < _restored_defbodies.size(); ++i)
			say("defbody", NULL, _restored_defbodies[i].key,
			    verdict_str(_restored_defbodies[i].key, NULL), CIR_DECL_RECORD_NONE,
			    _par_via.bodies, i);
	}

	_stat_mat_secs += forest_now() - _t0;
}

// Typeid -> arena name, derived per query (R1): the arena is id-addressed
// by project slot, so a def's arena id IS the freeze-time typeid — an O(1)
// record read replaces the old eager whole-arena map built at open.
const char *CirFrozenForest::type_name_for(uint32_t type_id) const
{
	madc::dis::defrec r;
	if (!_arena.get_def_at(type_id, r) || !r.name_id)
		return NULL;
	return _arena.c_str(r.name_id);
}

// v20 extern-decl index: one symbol -> (unit, idx) per producer top-level
// extern. v65: a ForestDeclNames table bound in place on the FIRST query (R1)
// and binary-searched — never a decode of every name through the string pool.
// A malformed table logs loud and stays empty (misses resolve like an
// unindexed symbol).
bool CirFrozenForest::extern_loc_for(const madc::dis::istring &sym,
				     uint32_t &unit, uint32_t &idx)
{
	if (!_extern_index_bound) {
		_extern_index_bound = true;
		ForestWorkFrame _fw(_work_secs, _work_depth);
		size_t len = 0;
		const madc::dis::snapshot_segment *xs =
			_reader.find(CIR_FOREST_SEG_EXTERN_LOCS);
		const uint8_t *p = xs && xs->kind == SNAP_KIND_CIR_EXTERN_LOCS
				 ? forest_segment_bytes(_reader, *xs, len) : NULL;
		if (xs && !_extern_names.bind(p, len))
			fprintf(stderr, "madc: forest extern index corrupt\n");
	}
	uint32_t n = 0;
	const uint32_t *run = _extern_names.units_of(sym.c_str(), n);
	if (n != 2)
		return false;
	unit = run[0];
	idx  = run[1];
	return true;
}

const char *CirFrozenForest::restored_template_string(uint32_t id) const
{
	if (!id)
		return NULL;
	uint32_t len = 0;
	return pool_cstr(id, len);
}

// R1: bind the TEMPLATE_PAYLOAD + TEMPLATE_TOKENS segments on first demand
// (memoized) — from materialize's first filter-surviving template record,
// or a late ClassPattern token-run read. SPANS into the mapped image or the
// process-level decoded cache (forest_segment_bytes): one decode per
// process, zero per-forest copies. Segment payloads are 16-aligned in the
// image and the cache buffers are heap-allocated, so the uint32 view is
// aligned on both paths. A bind failure logs loud and leaves the pair
// empty: every run read then cleanly lacks.
bool CirFrozenForest::ensure_template_payload() const
{
	if (_template_payload_loaded)
		return true;
	_template_payload_loaded = true;
	ForestWorkFrame _fw(_work_secs, _work_depth);
	if (const madc::dis::snapshot_segment *tp =
		_reader.find(CIR_FOREST_SEG_TEMPLATE_PAYLOAD)) {
		size_t len = 0;
		const uint8_t *p = NULL;
		const madc::dis::lazy_blocks *lz = NULL;
		if (tp->kind != SNAP_KIND_CIR_TEMPLATE_PAYLOAD
		    || (tp->raw_size
			&& !(p = forest_segment_bytes(_reader, *tp, len, &lz)))
		    || len % sizeof(uint32_t)) {
			fprintf(stderr, "madc: forest template payload corrupt\n");
			return false;
		}
		_template_payload = (const uint32_t *)p;
		_template_payload_words = len / sizeof(uint32_t);
		_lz_template_payload = lz;
		note_lazy_span(CIR_FOREST_SEG_TEMPLATE_PAYLOAD, lz);
	}
	if (const madc::dis::snapshot_segment *tt =
		_reader.find(CIR_FOREST_SEG_TEMPLATE_TOKENS)) {
		size_t len = 0;
		const uint8_t *p = NULL;
		const madc::dis::lazy_blocks *lz = NULL;
		if (tt->kind != SNAP_KIND_CIR_TEMPLATE_TOKENS
		    || (tt->raw_size
			&& !(p = forest_segment_bytes(_reader, *tt, len, &lz)))) {
			_template_tokens = NULL;
			_template_tokens_len = 0;
			fprintf(stderr, "madc: forest template tokens corrupt\n");
			return false;
		}
		_template_tokens = p;
		_template_tokens_len = len;
		_lz_template_tokens = lz;
		note_lazy_span(CIR_FOREST_SEG_TEMPLATE_TOKENS, lz);
	}
	return true;
}

// The template payload words [off, off + words) / token bytes [off, off +
// len), made readable (their blocks decoded on first touch); false when out
// of range — the read then cleanly lacks.
bool CirFrozenForest::template_payload_span(size_t off, size_t words) const
{
	return off <= _template_payload_words
	    && words <= _template_payload_words - off
	    && madc::dis::lazy_ensure(_lz_template_payload,
				      off * sizeof(uint32_t),
				      words * sizeof(uint32_t));
}

bool CirFrozenForest::template_token_span(size_t off, size_t len) const
{
	return off <= _template_tokens_len && len <= _template_tokens_len - off
	    && madc::dis::lazy_ensure(_lz_template_tokens, off, len);
}

// slice B1 (task #25): decode ONE restored-template record's payload-backed
// fields — params, the positional token-run table (body / constraint /
// per-param defaults / per-slot spec patterns), and the ClassPattern payload
// slice — memoized per record. The identity fields were filled by the
// materialize walk; the payload/token segments decode at the FIRST hydrate
// (ensure_template_payload), so a TU that instantiates nothing never pays.
bool CirFrozenForest::hydrate_restored_template(CirRestoredTemplate &rt) const
{
	if (rt.hydrated)
		return !rt.hydrate_failed;
	rt.hydrated = true;
	rt.hydrate_failed = true;
	ForestWorkFrame _fw(_work_secs, _work_depth);
	if (!ensure_template_payload())
		return false;
	auto run_at = [&](uint32_t word_off) -> CirRestoredTemplateRun {
		CirRestoredTemplateRun rr;
		rr.bytes = NULL;
		rr.len = 0;
		rr.count = 0;
		rr.file = NULL;
		cir_forest_token_run tr;
		if (!template_payload_span(word_off,
					   madc::dis::pod_words<cir_forest_token_run>())
		    || !madc::dis::pod_read(_template_payload,
					    _template_payload_words, word_off, tr))
			return rr;
		if (tr.tok_count
		    && template_token_span(tr.tok_off, tr.tok_bytes)) {
			rr.bytes = _template_tokens + tr.tok_off;
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
	if (rt.rec_pattern_words) {
		size_t begin = rt.rec_pattern_begin;
		size_t count = rt.rec_pattern_words;
		if (!template_payload_span(begin, count))
			return false;
		rt.pattern = _template_payload + begin;
		rt.pattern_words = rt.rec_pattern_words;
	}
	for (uint32_t p = 0; p < rt.rec_param_count; ++p) {
		cir_forest_template_param pr;
		if (!template_payload_span(rt.rec_param_begin + p * pw, pw)
		    || !madc::dis::pod_read(_template_payload,
					    _template_payload_words,
					    rt.rec_param_begin + p * pw, pr))
			return false;
		uint32_t plen = 0;
		const char *pn = pool_cstr(pr.name_id, plen);
		if (!pn)
			return false;
		rt.params.push_back(std::make_pair(pn, pr.pflags));
	}
	// Positional run table: body, constraint, per-param defaults,
	// per-slot spec patterns.
	uint32_t ro = rt.rec_run_begin;
	rt.body       = run_at(ro); ro += (uint32_t)rw;
	rt.constraint = run_at(ro); ro += (uint32_t)rw;
	for (uint32_t p = 0; p < rt.rec_param_count; ++p) {
		rt.defaults.push_back(run_at(ro));
		ro += (uint32_t)rw;
	}
	for (uint32_t sp = 0; sp < rt.rec_spec_count; ++sp) {
		rt.spec.push_back(run_at(ro));
		ro += (uint32_t)rw;
	}
	rt.hydrate_failed = false;
	return true;
}

CirRestoredTemplateRun CirFrozenForest::restored_template_run(
	const cir_forest_token_run &run) const
{
	ensure_template_payload();
	CirRestoredTemplateRun restored;
	restored.bytes = NULL;
	restored.len = 0;
	restored.count = 0;
	restored.file = NULL;
	if (run.tok_count && template_token_span(run.tok_off, run.tok_bytes)) {
		restored.bytes = _template_tokens + run.tok_off;
		restored.len = run.tok_bytes;
		restored.count = run.tok_count;
	}
	if (run.file_id) {
		uint32_t len = 0;
		restored.file = pool_cstr(run.file_id, len);
	}
	return restored;
}

size_t CirFrozenForest::units_loaded() const
{
	size_t n = 0;
	for (size_t u = 0; u < _segs.size(); ++u)
		if (_segs[u])
			++n;
	return n;
}

void CirFrozenForest::node_record_counts(unsigned long long &loaded,
					 unsigned long long &materialized,
					 std::string *rows) const
{
	loaded = materialized = 0;
	for (size_t u = 0; u < _segs.size(); ++u)
		if (_segs[u]) {
			size_t n = _segs[u]->record_count(), m = _segs[u]->materialized_count();
			loaded += n;
			materialized += m;
			if (rows) {
				size_t bd = 0, bt = 0;
				_segs[u]->block_counts(bd, bt);
				char buf[80];
				snprintf(buf, sizeof(buf), "%s%zu:%zu/%zu[%zu/%zu]",
					 rows->empty() ? "" : " ", u, m, n, bd, bt);
				*rows += buf;
			}
		}
}

// The load-on-demand step: bind one unit's four payload segments in place and
// register the segment. Format 61 stores them BLOCKED, so binding decodes
// nothing: a record, child entry, connector or position decodes its block on
// first read, and its bounds (child range, connector target, position) are
// checked there (CirFrozenSegment::resolve) — a corrupt container fails at
// that read, never as a wild one. Here: the directory's record, connector and
// position counts against the segments' sizes.
CirFrozenSegment *CirFrozenForest::unit_segment(uint32_t unit)
{
	if (unit >= _units.size())
		return NULL;
	if (_segs[unit])
		return _segs[unit];
	ForestWorkFrame _fw(_work_secs, _work_depth);
	double _t0 = forest_now();

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
	if (recs->raw_size % sizeof(cir_frozen_record)
	    || kids->raw_size % sizeof(uint32_t)
	    || conn->raw_size % sizeof(uint64_t)
	    || poss->raw_size % sizeof(cir_frozen_pos)) {
		fprintf(stderr, "madc: forest unit %u payload corrupt\n", unit);
		return NULL;
	}
	CirFrozenSegment::Table t[4];
	const madc::dis::snapshot_segment *segs[4] = { recs, kids, conn, poss };
	const size_t elem[4] = { sizeof(cir_frozen_record), sizeof(uint32_t),
				 sizeof(uint64_t), sizeof(cir_frozen_pos) };
	for (int k = 0; k < 4; ++k) {
		size_t len = 0;
		t[k].p = forest_segment_bytes(_reader, *segs[k], len, &t[k].lz);
		if (!t[k].p && len) {
			fprintf(stderr, "madc: forest unit %u payload corrupt\n", unit);
			return NULL;
		}
		t[k].n = len / elem[k];
	}
	if (t[0].n != _units[unit].record_count
	    || t[2].n != _units[unit].connector_count
	    || t[3].n != t[0].n) {
		fprintf(stderr, "madc: forest unit %u failed validation\n", unit);
		return NULL;
	}

	DBG(fprintf(stderr, "forest: loading unit %u (%zu records)\n",
		    unit, t[0].n));
	_segs[unit] = new CirFrozenSegment(unit, t[0], t[1], t[2], t[3], this, _c2m);
	_stat_unitload_secs += forest_now() - _t0;
	_stat_unitload_count += 1;
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
		    || cx->tsubst_pack_expand != cy->tsubst_pack_expand
		    || cx->tsubst_arg_uncoerced != cy->tsubst_arg_uncoerced)
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
