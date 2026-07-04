/* cir_freeze.cpp — freeze/thaw a cir_node sub-DAG through the madc::dis
 * pool-snapshot container (forest Phase 2+3 / data-substrate Track B2+B3).
 * See cir_freeze.h for the contract, the forest format, and the closure
 * story.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

	// typeid -> name closure: the freezing Program's project segment (the
	// primitive segment is pinned and process-invariant; the system
	// segment gains occupants with the B4 pack pipeline).
	std::vector<uint32_t> type_names;
	if (madc_active_project_types) {
		uint32_t base = madc_active_project_types->base();
		for (uint32_t i = 0; i < (uint32_t)madc_active_project_types->size(); ++i) {
			DataDef *dd = madc_active_project_types->get(base + i);
			if (dd && !dd->name.empty()) {
				type_names.push_back(base + i);
				type_names.push_back(pool->intern(dd->name));
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
		cir_forest_dir_unit du;
		du.unit_name_id    = f.units[u].unit_name_id;
		du.record_count    = (uint32_t)f.units[u].blob.records.size();
		du.connector_count = (uint32_t)f.units[u].connectors.size();
		du.anchor_idx      = CIR_FOREST_ANCHOR_NONE;	// B4 grove entry
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
	if (!add_seg(w, CIR_FOREST_SEG_TYPE_NAMES, SNAP_KIND_CIR_TYPE_NAMES,
		     type_names.data(), type_names.size() * sizeof(uint32_t), codec))
		return false;

	for (size_t u = 0; u < f.units.size(); ++u) {
		const cir_forest_unit &fu = f.units[u];
		uint32_t base = CIR_FOREST_SEG_UNIT_BASE
			      + (uint32_t)u * CIR_FOREST_SEGS_PER_UNIT;
		if (fu.blob.records.empty())
			return false;
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
	: _c2m(NULL), _root_unit(0), _root_idx(0)
{
}

CirFrozenForest::~CirFrozenForest()
{
	for (size_t u = 0; u < _segs.size(); ++u)
		delete _segs[u];
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

	// typeid -> name closure.
	if (const madc::dis::snapshot_segment *ts =
		_reader.find(CIR_FOREST_SEG_TYPE_NAMES)) {
		std::vector<uint8_t> tn;
		if (ts->kind != SNAP_KIND_CIR_TYPE_NAMES
		    || !_reader.read_segment(*ts, tn)
		    || tn.size() % (2 * sizeof(uint32_t))) {
			fprintf(stderr, "madc: forest type-name closure corrupt\n");
			return false;
		}
		for (size_t off = 0; off + 2 * sizeof(uint32_t) <= tn.size();
		     off += 2 * sizeof(uint32_t)) {
			uint32_t tid, nid;
			memcpy(&tid, tn.data() + off, sizeof(uint32_t));
			memcpy(&nid, tn.data() + off + sizeof(uint32_t), sizeof(uint32_t));
			uint32_t slen = 0;
			if (const char *s = pool_cstr(nid, slen))
				_type_names[tid] = s;
		}
	}

	_segs.assign(hdr.unit_count, (CirFrozenSegment *)NULL);
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
