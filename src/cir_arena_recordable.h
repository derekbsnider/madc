#ifndef __CIR_ARENA_RECORDABLE_H
#define __CIR_ARENA_RECORDABLE_H 1

// The arena RECORDABILITY closure — one implementation for the freeze (which
// writes it into the pack's RECORDABLE segment, docs/plans/2026-10-13-pack-
// slices-design.md §3) and the bind. An aggregate is recordable iff every
// member's type chain resolves (self allowed), every base is recordable and
// every anonymous sub-aggregate is recordable. Templated over the arena view
// (DefArena at freeze, FrozenDefArena at bind — the same accessor surface) and
// over the set the chain checks against (the recordable set, or the bind's
// admitted set).

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "cir_arena.h"

// A chain (pointer / reference / cv / array unaries over a base) resolves when
// it ends in a pinned primitive, an enum, a function or member pointer, or an
// aggregate in `in_set` (or the aggregate being judged).
template <class Arena, class Set>
static inline bool arena_chain_ok(const Arena &a, uint32_t tid, uint32_t self_id,
				  const Set &in_set)
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
			return tid == self_id || in_set.count(tid) != 0;
		case madc::dis::DK_ENUM:
			return true;	// v21: enums materialize in pass 1a (leaves)
		case madc::dis::DK_FPTR:
			return true;	// v22: fn-ptrs build on first use (the
					// target signature never gates member layout)
		case madc::dis::DK_MEMBERPTR:
			return true;	// v53: member pointers build on first use
					// (8 bytes or the {ptr, adj} pair — neither the
					// owner nor the member gates member layout)
		default:
			return false;
		}
	}
	return false;
}

// A recordability CANDIDATE: an aggregate, complete — or incomplete in the
// payload-less empty shape (v21: a synthesized recursion tail like
// _Tuple_impl<N> — live registers, inherits from, and emits it without
// completing it). Anything half-parsed WITH members/bases stays dropped.
static inline bool arena_recordable_candidate(const madc::dis::defrec &r)
{
	if (r.kind != madc::dis::DK_STRUCT && r.kind != madc::dis::DK_UNION
	    && r.kind != madc::dis::DK_CLASS)
		return false;
	return (r.flags & madc::dis::DF_IS_COMPLETE)
	    || (!r.members_count && !r.bases_count);
}

// N of a gensym'd anonymous tag "__anon_N" (0 when the name is not one) —
// the restore advances the consumer's anon-tag counter past every restored N.
static inline size_t arena_anon_tag_of(const char *nm)
{
	if (!nm || strncmp(nm, "__anon_", 7) != 0)
		return 0;
	char *end = NULL;
	unsigned long n = strtoul(nm + 7, &end, 10);
	return end && *end == '\0' ? (size_t)n : 0;
}

// GREATEST fixpoint (v22): start from ALL candidates and iteratively REMOVE any
// aggregate with an unresolvable member / base / anon group until stable. The
// old additive (least) fixpoint could not admit a POINTER CYCLE — basic_ios<char>
// holds a basic_ostream<char>* member (_M_tie) while basic_ostream derives from
// basic_ios, so neither could ever be inserted first and the whole iostream
// hierarchy dropped. A through-pointer reference only needs its target IN the
// surviving set (pass 1 allocates every survivor before the pointer builds
// over it), exactly the C incomplete-type rule; a failure still cascades to its
// dependents through the removal rounds.
template <class Arena>
static inline void arena_recordable_fixpoint(const Arena &a,
					     const std::vector<uint32_t> &agg_ids,
					     std::set<uint32_t> &recordable)
{
	recordable.insert(agg_ids.begin(), agg_ids.end());
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
				    || !arena_chain_ok(a, mr.type_id, tid, recordable)
				    || (mr.vbase_id && !recordable.count(mr.vbase_id)))
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
}

#endif // __CIR_ARENA_RECORDABLE_H
