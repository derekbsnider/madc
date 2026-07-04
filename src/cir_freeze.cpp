/* cir_freeze.cpp — freeze/thaw a cir_node sub-DAG through the madc::dis
 * pool-snapshot container (forest Phase 2 / data-substrate Track B2).
 * See cir_freeze.h for the contract and the B2/B3 scope fence.
 */

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

extern thread_local bool madc_verbose;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "token_arena.h"
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
// Freeze
// ---------------------------------------------------------------------------

bool cir_freeze_subtree(cir_node *root, cir_frozen_blob &out)
{
	if (!root)
		return false;
	out.records.clear();
	out.children.clear();

	// Pass 1: discover the reachable sub-DAG, assigning dense record
	// indices at FIRST touch (share/cycle-safe — the dump-walker
	// discipline). Iterative: real trees exceed 800 deep.
	std::map<cir_node *, uint32_t> index;
	std::vector<cir_node *> order;
	std::vector<cir_node *> stack;
	index[root] = 0;
	order.push_back(root);
	stack.push_back(root);
	while (!stack.empty()) {
		cir_node *n = stack.back();
		stack.pop_back();
		for (node_t op = c2mir_node_first_op(n->as_node()); op;
		     op = c2mir_node_next_op(op)) {
			cir_node *c = CIR_NODE(op);
			if (index.insert(std::make_pair(c, (uint32_t)order.size())).second) {
				order.push_back(c);
				stack.push_back(c);
			}
		}
	}

	// Pass 2: emit one record per node + the CSR child-index pool.
	out.records.resize(order.size());
	for (size_t i = 0; i < order.size(); ++i) {
		cir_node *n = order[i];
		cir_frozen_record &r = out.records[i];
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
		case CIR_PAYLOAD_NONE: {
			r.child_base = out.children.size();
			for (node_t op = c2mir_node_first_op(n->as_node()); op;
			     op = c2mir_node_next_op(op)) {
				uint32_t ci = index[CIR_NODE(op)];
				if (ci & CIR_FROZEN_CHILD_CONNECTOR_BIT)
					return false;	// 2^31 records: out of format
				out.children.push_back(ci);
				++r.nchildren;
			}
			break;
		}
		}

		// B1 extension block — already position-independent.
		r.origin_id           = n->origin_id;
		r.datadef_id          = n->datadef_id;
		r.typedef_name_id     = n->typedef_name_id;
		r.error_msg_id        = n->error_msg_id;
		r.tsubst_pack_index   = n->tsubst_pack_index;
		r.tsubst_pack_value_id = n->tsubst_pack_value_id;
		r.tree1_origin        = n->tree1_origin;
		r.src_lang            = (uint8_t)n->src_lang;
		r.flags               = (n->synth_from_origin ? CIR_FROZEN_SYNTH_FROM_ORIGIN : 0)
				      | (n->tsubst_pack_expand ? CIR_FROZEN_PACK_EXPAND : 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Container glue
// ---------------------------------------------------------------------------

bool cir_freeze_write(const cir_frozen_blob &blob,
		      madc::dis::snapshot_writer &w, uint32_t seg_id_base,
		      PchCompression codec)
{
	if (blob.records.empty())
		return false;
	// Zero-length payloads take codec None (nothing to compress).
	PchCompression ccodec = blob.children.empty() ? PchCompression::None : codec;
	if (!w.add_segment(seg_id_base + 0, SNAP_KIND_CIR_RECORDS,
			   blob.records.data(),
			   blob.records.size() * sizeof(cir_frozen_record), codec))
		return false;
	return w.add_segment(seg_id_base + 1, SNAP_KIND_CIR_CHILDREN,
			     blob.children.empty() ? "" : (const char *)blob.children.data(),
			     blob.children.size() * sizeof(uint32_t), ccodec);
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

// ---------------------------------------------------------------------------
// Thaw: CirFrozenSegment
// ---------------------------------------------------------------------------

CirFrozenSegment::CirFrozenSegment(cir_frozen_blob &&blob, c2m_ctx_t c2m)
	: _blob(std::move(blob)), _c2m(c2m)
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
	case CIR_PAYLOAD_STR:
		if (r.str_id && TokenBase::_active_strpool) {
			const char *s = TokenBase::_active_strpool->c_str(r.str_id);
			cn->base.u.s.s = c2mir_uniq_str(_c2m, s, r.str_len);
			cn->base.u.s.len = r.str_len;
		}
		break;
	case CIR_PAYLOAD_NONE:
		break;	// children appended in phase B
	}

	cn->origin_id            = r.origin_id;
	cn->datadef_id           = r.datadef_id;
	cn->typedef_name_id      = r.typedef_name_id;
	cn->error_msg_id         = r.error_msg_id;
	cn->tsubst_pack_index    = r.tsubst_pack_index;
	cn->tsubst_pack_value_id = r.tsubst_pack_value_id;
	cn->tree1_origin         = r.tree1_origin;
	cn->src_lang             = (CirSourceLang)r.src_lang;
	cn->synth_from_origin    = (r.flags & CIR_FROZEN_SYNTH_FROM_ORIGIN) != 0;
	cn->tsubst_pack_expand   = (r.flags & CIR_FROZEN_PACK_EXPAND) != 0;
	cn->self.seg = _seg;
	cn->self.idx = idx;

	// Position is derived from the origin token (the single source of
	// truth), exactly as CirBuilder::make does for a live build.
	if (r.origin_id) {
		TokenBase *tb = madc_token_for_slot(r.origin_id);
		if (tb) {
			c2mir_pos_t pos = { tb->file, tb->line, tb->column };
			c2mir_set_node_pos(_c2m, cn->as_node(), pos);
		}
	}

	_mat[idx] = cn;
	return cn;
}

cir_node *CirFrozenSegment::node_at(uint32_t idx)
{
	if (idx >= _blob.records.size())
		return NULL;
	if (_mat[idx])
		return _mat[idx];

	// Phase A: shells for every cold record reachable from idx (iterative
	// worklist; memoized shells terminate shares and cycles).
	std::vector<uint32_t> created;
	shell(idx);
	created.push_back(idx);
	for (size_t w = 0; w < created.size(); ++w) {
		const cir_frozen_record &r = _blob.records[created[w]];
		for (uint32_t k = 0; k < r.nchildren; ++k) {
			uint32_t ci = _blob.children[r.child_base + k];
			if (!_mat[ci]) {
				shell(ci);
				created.push_back(ci);
			}
		}
	}

	// Phase B: append children for the records created THIS call only —
	// previously-materialized nodes already own their child lists. A
	// shared child appended under several parents reproduces the live
	// builder's last-appender-owns-links DLIST state (arity-1 N_SHARE
	// parents read head-only, so every parent still sees its child).
	for (size_t w = 0; w < created.size(); ++w) {
		const cir_frozen_record &r = _blob.records[created[w]];
		cir_node *parent = _mat[created[w]];
		for (uint32_t k = 0; k < r.nchildren; ++k)
			c2mir_op_append(_c2m, parent->as_node(),
					_mat[_blob.children[r.child_base + k]]->as_node());
	}

	return _mat[idx];
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
		    || cx->typedef_name_id != cy->typedef_name_id
		    || cx->error_msg_id != cy->error_msg_id
		    || cx->tsubst_pack_index != cy->tsubst_pack_index
		    || cx->tsubst_pack_value_id != cy->tsubst_pack_value_id
		    || !(cx->tree1_origin == cy->tree1_origin)
		    || cx->src_lang != cy->src_lang
		    || cx->synth_from_origin != cy->synth_from_origin
		    || cx->tsubst_pack_expand != cy->tsubst_pack_expand)
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
