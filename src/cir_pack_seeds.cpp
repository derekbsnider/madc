/* cir_pack_seeds.cpp — the pack's seed index (v58): what a bind visits and how
 * it judges it, proportional to the units the program includes.
 * docs/plans/2026-10-13-pack-slices-design.md §3-§5.
 *
 *   ForestDeclNames       DECL_NAMES: name -> declaring units (the verdicts)
 *   ForestSeeds           SEEDS: per unit, the arena slots / templates /
 *                         globals a bind of it visits; owner dependents
 *   ForestRecordableBits  RECORDABLE: the recordability fixpoint, frozen
 *   cir_forest_build_seeds  the freeze-side builder of all three
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <queue>	// madc.h uses std::queue and relies on the TU providing it
#include <string>
#include <vector>

extern thread_local bool madc_verbose;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"		// Program::PackDeclKind — a decl entry's kind (its record id)
#include "cir_freeze.h"
#include "cir_arena_recordable.h"

// --- ForestDeclNames ------------------------------------------------------

bool ForestDeclNames::bind(const uint8_t *p, size_t len)
{
	*this = ForestDeclNames();
	if (!p || len < sizeof(uint32_t))
		return false;
	uint32_t n;
	memcpy(&n, p, sizeof(n));
	const size_t head_words = 1 + 2 * ((size_t)n + 1);
	if (len < head_words * sizeof(uint32_t))
		return false;
	const uint32_t *w = (const uint32_t *)p;
	const uint32_t *off = w + 1, *uoff = w + 2 + n;
	const size_t nunits = uoff[n];
	const size_t units_at = head_words * sizeof(uint32_t);
	if (off[0] != 0 || uoff[0] != 0
	    || units_at + nunits * sizeof(uint32_t) + off[n] != len)
		return false;
	_n = n;
	_off = off;
	_uoff = uoff;
	_units = (const uint32_t *)(p + units_at);
	_nunits = (uint32_t)nunits;
	_bytes = (const char *)(p + units_at + nunits * sizeof(uint32_t));
	_nbytes = off[n];
	return true;
}

// Every probe is bounds-checked against the bound sizes (a corrupt offset
// answers "not declared", never a wild read): name_at and units_at
// (cir_freeze.h) are the two readers of one name's slot.
const uint32_t *ForestDeclNames::units_of(const char *name, uint32_t &n) const
{
	n = 0;
	if (!_n || !name)
		return NULL;
	uint32_t lo = 0, hi = _n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		// The hot probe (every name_verdict): its own reads, not name_at —
		// measured, the shared reader's extra index check costs ~107 Ir per
		// lookup across the binary search (+2.2M on cxx_stl.forest).
		uint32_t o = _off[mid];
		if (o >= _nbytes)
			return NULL;
		int c = strcmp(_bytes + o, name);
		if (c < 0)
			lo = mid + 1;
		else if (c > 0)
			hi = mid;
		else {
			uint32_t b = _uoff[mid], e = _uoff[mid + 1];
			if (b > e || e > _nunits)
				return NULL;
			n = e - b;
			return _units + b;
		}
	}
	return NULL;
}

// The run's start is the first name not below the stem (strcmp's order):
// every name the stem starts sorts at or after it, and before any name past
// it that the stem does not start.
void ForestDeclNames::for_prefix(const char *stem, uint32_t &begin, uint32_t &end) const
{
	begin = end = 0;
	if (!_n || !stem)
		return;
	uint32_t lo = 0, hi = _n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const char *nm = name_at(mid);
		if (!nm)
			return;
		if (strcmp(nm, stem) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	const size_t len = strlen(stem);
	begin = end = lo;
	for (const char *nm; (nm = name_at(end)) != NULL && !strncmp(nm, stem, len); )
		++end;
}

const uint32_t *ForestDeclNames::surface(const char *name, const char *ns, uint32_t &n) const
{
	n = 0;
	if (!name || !*name)
		return NULL;
	if (ns && *ns) {
		const uint32_t *q = units_of((std::string(ns) + "::" + name).c_str(), n);
		if (n)
			return q;
	}
	return units_of(name, n);
}

// The template a template-id spells: the name before its '<', trailing blanks
// trimmed. has_head = false when the spelling has no '<'.
static madc::dis::istring template_id_head(const char *spell, bool &has_head)
{
	const char *lt = spell ? strchr(spell, '<') : NULL;
	has_head = lt != NULL;
	if (!lt)
		return madc::dis::istring();
	size_t n = (size_t)(lt - spell);
	while (n && spell[n - 1] == ' ')
		--n;
	return madc::dis::istring(spell, n);
}

const uint32_t *ForestDeclNames::head_of(const char *canon, bool &has_head, uint32_t &n) const
{
	n = 0;
	madc::dis::istring head = template_id_head(canon, has_head);
	return has_head ? units_of(head.c_str(), n) : NULL;
}

void ForestDeclNames::build(const std::map<madc::dis::istring, std::vector<uint32_t> > &names,
			    std::vector<uint8_t> &out)
{
	// istring's operator< compares the BYTES — the order strcmp reads.
	const uint32_t n = (uint32_t)names.size();
	std::vector<uint32_t> w;
	w.push_back(n);
	uint32_t at = 0;
	for (std::map<madc::dis::istring, std::vector<uint32_t> >::const_iterator it = names.begin();
	     it != names.end(); ++it) {
		w.push_back(at);
		at += (uint32_t)it->first.size() + 1;
	}
	w.push_back(at);
	uint32_t u = 0;
	for (std::map<madc::dis::istring, std::vector<uint32_t> >::const_iterator it = names.begin();
	     it != names.end(); ++it) {
		w.push_back(u);
		u += (uint32_t)it->second.size();
	}
	w.push_back(u);
	for (std::map<madc::dis::istring, std::vector<uint32_t> >::const_iterator it = names.begin();
	     it != names.end(); ++it)
		w.insert(w.end(), it->second.begin(), it->second.end());
	out.assign((const uint8_t *)w.data(), (const uint8_t *)(w.data() + w.size()));
	for (std::map<madc::dis::istring, std::vector<uint32_t> >::const_iterator it = names.begin();
	     it != names.end(); ++it)
		out.insert(out.end(), it->first.c_str(), it->first.c_str() + it->first.size() + 1);
}

// --- ForestSeeds ----------------------------------------------------------

bool ForestSeeds::bind(const uint32_t *w, size_t words)
{
	*this = ForestSeeds();
	if (!w || words < 1)
		return false;
	const uint32_t l = w[0];
	size_t at = 1;
	if (!l || words < at + 3 * ((size_t)l + 1) + 1)
		return false;
	const uint32_t *aoff = w + at; at += l + 1;
	const uint32_t *toff = w + at; at += l + 1;
	const uint32_t *goff = w + at; at += l + 1;
	const uint32_t d = w[at++];
	if (words < at + 2 * (size_t)d)
		return false;
	const uint32_t *dep_owner = w + at; at += d;
	const uint32_t *dep_item = w + at; at += d;
	if (words < at + 1)
		return false;
	const uint32_t e = w[at++];
	if (words < at + 2 * (size_t)e)
		return false;
	const uint32_t *def_slot = w + at; at += e;
	const uint32_t *def_unit = w + at; at += e;
	if (aoff[0] || toff[0] || goff[0])
		return false;
	for (uint32_t i = 0; i < l; ++i)
		if (aoff[i] > aoff[i + 1] || toff[i] > toff[i + 1] || goff[i] > goff[i + 1])
			return false;
	const size_t nids = (size_t)aoff[l] + toff[l] + goff[l];
	if (words != at + nids)
		return false;
	_l = l;
	_aoff = aoff; _toff = toff; _goff = goff;
	_d = d; _dep_owner = dep_owner; _dep_item = dep_item;
	_e = e; _def_slot = def_slot; _def_unit = def_unit;
	_ids = w + at;
	_nids = (uint32_t)nids;
	return true;
}

ForestSeeds::Run ForestSeeds::run(const uint32_t *off, uint32_t list, uint32_t base) const
{
	Run r = { NULL, 0 };
	if (!_l || list >= _l)
		return r;
	r.p = _ids + base + off[list];
	r.n = off[list + 1] - off[list];
	return r;
}

ForestSeeds::Run ForestSeeds::dependents(uint32_t owner_slot) const
{
	Run r = { NULL, 0 };
	if (!_d)
		return r;
	const uint32_t *b = std::lower_bound(_dep_owner, _dep_owner + _d, owner_slot);
	const uint32_t *e = std::upper_bound(b, _dep_owner + _d, owner_slot);
	r.p = _dep_item + (b - _dep_owner);
	r.n = (uint32_t)(e - b);
	return r;
}

ForestSeeds::Run ForestSeeds::definers(uint32_t slot) const
{
	Run r = { NULL, 0 };
	if (!_e)
		return r;
	const uint32_t *b = std::lower_bound(_def_slot, _def_slot + _e, slot);
	const uint32_t *e = std::upper_bound(b, _def_slot + _e, slot);
	r.p = _def_unit + (b - _def_slot);
	r.n = (uint32_t)(e - b);
	return r;
}

// --- ForestRecordableBits -------------------------------------------------

bool ForestRecordableBits::bind(const uint32_t *w, size_t words)
{
	*this = ForestRecordableBits();
	if (!w || words < 2)
		return false;
	uint32_t slots = w[0];
	if (words != 2 + ((size_t)slots + 31) / 32)
		return false;
	_slots = slots;
	_max_anon = w[1];
	_bits = w + 2;
	return true;
}

// --- the freeze-side builder ----------------------------------------------

namespace {

const uint32_t NO_UNIT = 0xffffffffu;

// The seed lists under construction: list index = unit, `always` = unit_count.
struct SeedLists {
	std::vector<std::vector<uint32_t> > arena, tmpl, glob;
	std::vector<std::pair<uint32_t, uint32_t> > deps;	// (owner slot, item)
	explicit SeedLists(size_t lists) : arena(lists), tmpl(lists), glob(lists) {}
};

// Census for the freeze's summary line.
struct SeedCensus {
	size_t decided = 0, created = 0, always = 0, dependent = 0, unplaced = 0, required = 0;
};

// Add `units` (a declaring-unit run) to `into`.
void add_units(std::set<uint32_t> &into, const uint32_t *u, uint32_t n)
{
	for (uint32_t i = 0; i < n; ++i)
		into.insert(u[i]);
}

} // namespace

void cir_forest_build_seeds(cir_frozen_forest &f, const madc::dis::intern_table &pool,
			    const std::vector<uint32_t> &poi,
			    const std::vector<const char *> &poi_files,
			    std::string &summary)
{
	const uint32_t nunits = (uint32_t)f.units.size();
	const uint32_t always = nunits;
	madc::dis::DefArena &a = f.arena;

	// 1. DECL_NAMES — every unit's decl index, inverted.
	std::map<madc::dis::istring, std::vector<uint32_t> > names;
	for (uint32_t u = 0; u < nunits; ++u)
		for (const cir_forest_decl_entry &e : f.units[u].decl_index) {
			const char *nm = pool.c_str(e.name_id);
			if (nm && *nm)
				names[nm].push_back(u);
		}
	for (std::map<madc::dis::istring, std::vector<uint32_t> >::iterator it = names.begin();
	     it != names.end(); ++it) {
		std::sort(it->second.begin(), it->second.end());
		it->second.erase(std::unique(it->second.begin(), it->second.end()),
				 it->second.end());
	}
	ForestDeclNames::build(names, f.decl_names);
	ForestDeclNames dn;
	dn.bind(f.decl_names.data(), f.decl_names.size());	// the one reader, freeze side too

	std::map<madc::dis::istring, uint32_t> unit_by_name;
	for (uint32_t u = 0; u < nunits; ++u)
		unit_by_name[pool.c_str(f.units[u].unit_name_id)] = u;
	auto unit_of_file = [&](const char *file) -> uint32_t {
		if (!file || !*file)
			return NO_UNIT;
		std::map<madc::dis::istring, uint32_t>::const_iterator it = unit_by_name.find(file);
		return it == unit_by_name.end() ? NO_UNIT : it->second;
	};
	// An entity's point of creation (a token index) -> its unit. The
	// producer's own source (the root unit) is never bound by a consumer, so
	// a point there places nothing — the entity falls back to its declaring
	// file, as an unstamped one.
	auto unit_of_poi = [&](uint32_t at) -> uint32_t {
		if (at == NO_UNIT || at >= f.token_unit.size() || f.token_unit[at] == f.root_unit)
			return NO_UNIT;
		return f.token_unit[at];
	};
	// A slot's point of creation: the parse-time stamp, else the declaring
	// file of a freeze-time capture.
	auto unit_of_slot = [&](uint32_t slot) -> uint32_t {
		uint32_t u = unit_of_poi(slot < poi.size() ? poi[slot] : NO_UNIT);
		if (u != NO_UNIT)
			return u;
		return slot < poi_files.size() ? unit_of_file(poi_files[slot]) : NO_UNIT;
	};

	SeedLists L((size_t)nunits + 1);
	std::map<madc::dis::istring, SeedCensus> census;	// by kind
	const char *kind_name[] = { "none", "prim", "void", "ptr", "ref", "const", "enum",
				    "struct", "union", "class", "func", "var", "simd", "fptr",
				    "memberptr", "carray", "typedef", "nslink", "nsbind",
				    "defbody", "nsalias" };

	// Place one entity: every unit of its decided surfaces, plus — when the
	// keep predicate can hold with no relevant unit bound (`undecided`) — its
	// point of creation, else the always list.
	// Env-gated probe (MADC_SEED_PROBE=1): every entity that lands in the
	// always list, with what it was judged by — the list's size is the fixed
	// cost every bind pays, so each entry is a measurement, never a guess.
	static const char *seed_probe = ::getenv("MADC_SEED_PROBE");
	auto probe_always = [&](const char *what, const char *nm, const char *ns,
				const char *extra) {
		if (seed_probe && *seed_probe)
			fprintf(stderr, "SEED always %s name=%s ns=%s %s\n", what,
				nm ? nm : "-", ns ? ns : "-", extra ? extra : "");
	};
	const char *probe_name = NULL, *probe_nm = NULL, *probe_ns = NULL, *probe_extra = NULL;
	// Env-gated probe (MADC_SEED_PLACE_PROBE=<substr>): every seed list an
	// entity whose name holds the substring lands in.
	static const char *place_probe = ::getenv("MADC_SEED_PLACE_PROBE");
	// `used` (optional) receives the list indices the entity landed in.
	auto place = [&](std::vector<std::vector<uint32_t> > &lists, uint32_t id,
			 const std::set<uint32_t> &decided_units, bool undecided,
			 uint32_t created_unit, SeedCensus &c,
			 std::vector<uint32_t> *used = NULL) {
		auto put = [&](uint32_t l) {
			lists[l].push_back(id);
			if (used)
				used->push_back(l);
			if (place_probe && *place_probe && probe_nm && strstr(probe_nm, place_probe))
				fprintf(stderr, "SEEDPLACE %s %s -> %s\n", probe_name ? probe_name : "-",
					probe_nm, l < nunits ? pool.c_str(f.units[l].unit_name_id) : "always");
		};
		for (std::set<uint32_t>::const_iterator it = decided_units.begin();
		     it != decided_units.end(); ++it)
			if (*it < nunits)
				put(*it);
		if (!decided_units.empty())
			++c.decided;
		if (undecided) {
			if (created_unit != NO_UNIT && created_unit < nunits) {
				put(created_unit);
				++c.created;
			} else {
				put(always);
				++c.always;
				if (probe_name)
					probe_always(probe_name, probe_nm, probe_ns, probe_extra);
			}
		}
	};

	// The surfaces a named arena record is judged by — the bind's record_kept
	// (materialize) and its registration's ns_ok / flat_ok / product_permitted:
	// (name, ns) qualified-then-bare, bare name when namespaced, the head of a
	// template-id canonical spelling. Undecided = the predicate holds at an
	// empty closure (every consulted verdict 0 or -1, monotone).
	auto record_surfaces = [&](const madc::dis::defrec &r, std::set<uint32_t> &units,
				   bool with_head) -> bool {
		const char *nm = r.name_id ? a.strings.c_str(r.name_id) : NULL;
		const char *ns = r.ns_id ? a.strings.c_str(r.ns_id) : NULL;
		uint32_t n1 = 0, n2 = 0, nh = 0;
		const uint32_t *s1 = dn.surface(nm, ns, n1);
		add_units(units, s1, n1);
		bool namespaced = ns && *ns;
		if (namespaced) {
			const uint32_t *s2 = dn.units_of(nm, n2);
			add_units(units, s2, n2);
		}
		bool has_head = false;
		const uint32_t *sh = with_head
			? dn.head_of(r.canon_id ? a.strings.c_str(r.canon_id) : NULL, has_head, nh)
			: NULL;
		add_units(units, sh, nh);
		bool name_ok_empty = n1 == 0 || (namespaced && n2 == 0);
		bool head_ok_empty = !has_head || nh == 0;
		return name_ok_empty && head_ok_empty;
	};

	// 2. Arena records.
	const uint32_t nslots = a.def_slots();

	// Instantiation requirements (design §4). A product — a class product's
	// record, keyed by its registered name, or a function instance's, by its
	// instantiation identity — is restored with every unit that requires it:
	// directly (that unit's parse created or reused it), or through a product
	// whose instantiation (or a body parsed or translated for it) did. An
	// entity born inside an instantiation (its creation context) is carried
	// by that product the same way. required[s] = those units, propagated.
	// The graph is over instantiation KEYS, not slots: a requirement chain
	// may run through an instantiation with no record of its own (a member
	// template's instance), and a slot joins its key's units and its
	// creation context's.
	std::vector<madc::dis::istring> slot_key(nslots);
	std::map<madc::dis::istring, uint32_t> product_of;
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r))
			continue;
		if ((r.kind == madc::dis::DK_STRUCT || r.kind == madc::dis::DK_UNION
		     || r.kind == madc::dis::DK_CLASS) && r.name_id)
			slot_key[s] = madc::dis::istring(a.strings.c_str(r.name_id));
		else if (r.kind == madc::dis::DK_FUNC && r.ovl_spelling_id) {
			const char *os = a.strings.c_str(r.ovl_spelling_id);
			const char *at = os ? strstr(os, "\x01@") : NULL;
			if (at && at[2])
				slot_key[s] = madc::dis::istring(at + 2);
		}
		if (!slot_key[s].empty())
			product_of.insert(std::make_pair(slot_key[s], s));
	}
	typedef std::map<madc::dis::istring, std::vector<madc::dis::istring> > carry_map;
	carry_map carries;					// key -> keys it requires
	std::map<madc::dis::istring, std::set<uint32_t> > required;	// key -> requiring units
	std::vector<std::pair<madc::dis::istring, uint32_t> > req_work;	// (key, unit) newly required

	// A unit can require an instantiation only when its include closure
	// DEFINES the template: a live parse of that closure has nothing else to
	// instantiate from. The producer parsed every header in one TU, so a
	// template another root defined could be instantiated where a closure has
	// only its forward declaration (libstdc++'s __try_use_facet names every
	// standard facet: moneypunct<char> instantiated under <istream>). reach =
	// one bitset per unit, the units its edges reach (itself included).
	const size_t rwords = ((size_t)nunits + 63) / 64;
	std::vector<uint64_t> reach((size_t)nunits * rwords, 0);
	{
		std::vector<uint32_t> stack;
		for (uint32_t u = 0; u < nunits; ++u) {
			uint64_t *row = &reach[(size_t)u * rwords];
			stack.assign(1, u);
			while (!stack.empty()) {
				const uint32_t v = stack.back();
				stack.pop_back();
				if (v >= nunits || ((row[v >> 6] >> (v & 63)) & 1))
					continue;
				row[v >> 6] |= 1ull << (v & 63);
				stack.insert(stack.end(), f.units[v].edges.begin(), f.units[v].edges.end());
			}
		}
	}
	// A template's DEFINING units, by its qualified name: a class template's
	// record (namespace + bare key) or a function template's (its key is
	// already qualified), each at its first token run's file.
	std::map<madc::dis::istring, std::vector<uint32_t> > defined_in;
	for (size_t i = 0; i < f.templates.size(); ++i) {
		const cir_forest_template_record &t = f.templates[i];
		if ((t.kind != CIR_TMPLK_CLASS && t.kind != CIR_TMPLK_FN)
		    || t.run_begin + 4 > f.template_payload.size())
			continue;
		const uint32_t file_id = f.template_payload[t.run_begin + 3];
		const uint32_t du = unit_of_file(file_id ? pool.c_str(file_id) : NULL);
		const char *key = pool.c_str(t.key_id);
		if (du == NO_UNIT || !key || !*key)
			continue;
		const char *ns = t.ns_id ? pool.c_str(t.ns_id) : NULL;
		std::string q;
		if (t.kind == CIR_TMPLK_CLASS && ns && *ns)
			q.append(ns).append("::");
		q.append(key);
		defined_in[madc::dis::istring(q)].push_back(du);
	}
	// The defining units of a requirement key's template: a function
	// instance's identity spells its template before the '<'; a class
	// product's record carries its canonical template-id. NULL = unknown
	// (no filter).
	std::map<madc::dis::istring, const std::vector<uint32_t> *> def_memo;
	size_t req_dropped = 0;				// the census: requirers dropped,
	std::set<madc::dis::istring> req_unknown_keys;	// keys no defining template names
	auto template_units_of = [&](const madc::dis::istring &k) -> const std::vector<uint32_t> * {
		std::map<madc::dis::istring, const std::vector<uint32_t> *>::const_iterator mi =
			def_memo.find(k);
		if (mi != def_memo.end())
			return mi->second;
		const char *spell = k.c_str();
		std::map<madc::dis::istring, uint32_t>::const_iterator pi = product_of.find(k);
		if (!strchr(spell, '<') && pi != product_of.end()) {
			madc::dis::defrec r;
			spell = a.get_def_at(madc::dis::arena_id_of(pi->second), r) && r.canon_id
				? a.strings.c_str(r.canon_id) : NULL;
		}
		bool has_head = false;
		madc::dis::istring head = template_id_head(spell, has_head);
		const std::vector<uint32_t> *units = NULL;
		if (has_head && !head.empty()) {
			std::map<madc::dis::istring, std::vector<uint32_t> >::const_iterator di =
				defined_in.find(head);
			if (di != defined_in.end())
				units = &di->second;
		}
		def_memo[k] = units;
		return units;
	};
	auto closure_defines = [&](const madc::dis::istring &k, uint32_t u) -> bool {
		const std::vector<uint32_t> *du = template_units_of(k);
		if (!du) {
			req_unknown_keys.insert(k);
			return true;
		}
		const uint64_t *row = &reach[(size_t)u * rwords];
		for (size_t i = 0; i < du->size(); ++i)
			if ((row[(*du)[i] >> 6] >> ((*du)[i] & 63)) & 1)
				return true;
		return false;
	};
	// A key with a requirement on record keeps its (possibly emptied) entry:
	// it is placed by requirement, never by its template's name.
	auto require_unit = [&](const madc::dis::istring &k, uint32_t u) {
		if (u >= nunits)
			return;
		std::set<uint32_t> &rs = required[k];
		if (!closure_defines(k, u))
			++req_dropped;
		else if (rs.insert(u).second)
			req_work.push_back(std::make_pair(k, u));
	};
	for (size_t i = 0; i < f.requirements.size(); ++i) {
		const cir_frozen_forest::requirement &q = f.requirements[i];
		if (!q.by.empty())
			carries[q.by].push_back(q.product);
		else
			require_unit(q.product, unit_of_poi(q.poi));
	}
	while (!req_work.empty()) {
		std::pair<madc::dis::istring, uint32_t> w = req_work.back();
		req_work.pop_back();
		carry_map::const_iterator ci = carries.find(w.first);
		if (ci != carries.end())
			for (size_t k = 0; k < ci->second.size(); ++k)
				require_unit(ci->second[k], w.second);
	}
	// What a product on the requirement graph carries is on it too, even when
	// the filter left the carrier no requirer: it is required only through
	// that carrier, never by its template's name.
	{
		std::vector<madc::dis::istring> on_graph;
		for (std::map<madc::dis::istring, std::set<uint32_t> >::const_iterator it =
			required.begin(); it != required.end(); ++it)
			on_graph.push_back(it->first);
		while (!on_graph.empty()) {
			madc::dis::istring k = on_graph.back();
			on_graph.pop_back();
			carry_map::const_iterator ci = carries.find(k);
			if (ci == carries.end())
				continue;
			for (size_t j = 0; j < ci->second.size(); ++j)
				if (required.insert(std::make_pair(ci->second[j], std::set<uint32_t>())).second)
					on_graph.push_back(ci->second[j]);
		}
	}
	auto required_of_key = [&](const madc::dis::istring &k) -> const std::set<uint32_t> * {
		if (k.empty())
			return NULL;
		std::map<madc::dis::istring, std::set<uint32_t> >::const_iterator it = required.find(k);
		return it == required.end() ? NULL : &it->second;
	};
	// A slot's requiring units: its own key's (a product) and its creation
	// context's (what it was born inside). NULL = none.
	std::set<uint32_t> req_scratch;
	auto required_of = [&](uint32_t s) -> const std::set<uint32_t> * {
		if (s >= nslots)
			return NULL;
		const std::set<uint32_t> *own = required_of_key(slot_key[s]);
		const std::set<uint32_t> *ctx = s < f.slot_ctx.size()
			? required_of_key(f.slot_ctx[s]) : NULL;
		if (!ctx || ctx == own)
			return own;
		if (!own)
			return ctx;
		req_scratch = *own;
		req_scratch.insert(ctx->begin(), ctx->end());
		return &req_scratch;
	};
	// Env-gated probe (MADC_SEED_REQ_PROBE=<substr>): each product whose key
	// holds the substring — its requiring units, its creation context and
	// every requirement naming it (by a product, or at a unit).
	static const char *req_probe = ::getenv("MADC_SEED_REQ_PROBE");
	if (req_probe && *req_probe) {
		auto unit_name = [&](uint32_t u) -> const char * {
			return u < nunits ? pool.c_str(f.units[u].unit_name_id) : "-";
		};
		for (std::map<madc::dis::istring, uint32_t>::const_iterator it = product_of.begin();
		     it != product_of.end(); ++it) {
			if (!strstr(it->first.c_str(), req_probe))
				continue;
			const std::set<uint32_t> *q = required_of(it->second);
			fprintf(stderr, "SEEDREQ %s slot=%u units=%zu ctx=%s\n", it->first.c_str(),
				it->second, q ? q->size() : (size_t)0,
				it->second < f.slot_ctx.size() && !f.slot_ctx[it->second].empty()
					? f.slot_ctx[it->second].c_str() : "-");
			if (q)
				for (std::set<uint32_t>::const_iterator u = q->begin(); u != q->end(); ++u)
					fprintf(stderr, "SEEDREQ   unit %s\n", unit_name(*u));
			if (const std::vector<uint32_t> *du = template_units_of(it->first))
				for (size_t d = 0; d < du->size(); ++d)
					fprintf(stderr, "SEEDREQ   defined-in %s\n", unit_name((*du)[d]));
			for (size_t i = 0; i < f.requirements.size(); ++i)
				if (f.requirements[i].product == it->first)
					fprintf(stderr, "SEEDREQ   by %s at %s\n",
						f.requirements[i].by.empty() ? "-"
							: f.requirements[i].by.c_str(),
						f.requirements[i].by.empty()
							? unit_name(unit_of_poi(f.requirements[i].poi)) : "-");
		}
	}
	std::map<madc::dis::istring, uint32_t> free_fn_by_name;	// funcdef_map key -> slot
	std::map<uint32_t, std::vector<uint32_t> > fn_lists;	// free fn slot -> its lists
	// A class METHOD's call symbol (Variable::name, "Class__member") -> the
	// owner class's slot: an import, a namespace-scope Variable or a deferred
	// body keyed by it exists exactly when that class restores.
	std::map<madc::dis::istring, uint32_t> method_owner;
	std::vector<uint32_t> nsbind_slots, defbody_slots;
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r) || r.kind == madc::dis::DK_NONE)
			continue;
		if (r.kind == madc::dis::DK_FUNC && (r.flags & madc::dis::DF_IS_FREE_FUNC)
		    && r.name_id)
			free_fn_by_name[a.strings.c_str(r.name_id)] = s;
		if (r.kind == madc::dis::DK_STRUCT || r.kind == madc::dis::DK_UNION
		    || r.kind == madc::dis::DK_CLASS)
			for (uint32_t m = 0; m < r.methods_count; ++m) {
				madc::dis::methodrec mr;
				if (a.get_payload(r.methods_begin, m, mr) && mr.name_id)
					method_owner.insert(std::make_pair(
						madc::dis::istring(a.strings.c_str(mr.name_id)), s));
			}
	}
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r) || r.kind == madc::dis::DK_NONE)
			continue;
		SeedCensus &c = census[r.kind < sizeof(kind_name) / sizeof(kind_name[0])
				       ? kind_name[r.kind] : "?"];
		std::set<uint32_t> units;
		switch (r.kind) {
		case madc::dis::DK_STRUCT:
		case madc::dis::DK_UNION:
		case madc::dis::DK_CLASS:
		case madc::dis::DK_ENUM:
		case madc::dis::DK_TYPEDEF: {
			if (r.flags & madc::dis::DF_TU_ROOT_ORIGIN)
				break;		// the program's own: never restored
			// A function-local class is unnameable outside its function;
			// the bind admits it by STRUCTURE once its owner's world
			// materialized — visited with its point of creation (the
			// unit whose parse, or whose function's translation, made
			// it), else by every bind.
			if (r.kind != madc::dis::DK_ENUM && r.kind != madc::dis::DK_TYPEDEF
			    && (r.flags & madc::dis::DF_CLASS_FN_LOCAL)) {
				uint32_t cu = unit_of_slot(s);
				const std::set<uint32_t> *req = required_of(s);
				if (cu != NO_UNIT || req) {
					// Born inside an instantiation: its requirers, never the
					// unit the producer happened to instantiate it under.
					std::set<uint32_t> at;
					if (req)
						at = *req;
					else
						at.insert(cu);
					for (std::set<uint32_t>::const_iterator it = at.begin();
					     it != at.end(); ++it)
						L.arena[*it].push_back(s);
					++(req ? c.required : c.created);
					break;
				}
				L.arena[always].push_back(s);
				++c.always;
				probe_always("fn-local", r.name_id ? a.strings.c_str(r.name_id) : NULL,
					     NULL, NULL);
				break;
			}
			// A template-id product required by some unit is restored
			// with its requirers, never with its template's declarers; any
			// record carried by a product joins that product's units.
			const std::set<uint32_t> *req = required_of(s);
			bool product = req && r.canon_id
				&& strchr(a.strings.c_str(r.canon_id), '<');
			// A record on the requirement graph is placed by its requirers alone.
			bool undecided = record_surfaces(r, units, !product) && !product && !req;
			if (req) {
				units.insert(req->begin(), req->end());
				++c.required;
			}
			probe_name = kind_name[r.kind];
			probe_nm = r.name_id ? a.strings.c_str(r.name_id) : NULL;
			probe_ns = r.ns_id ? a.strings.c_str(r.ns_id) : NULL;
			probe_extra = r.canon_id ? a.strings.c_str(r.canon_id) : NULL;
			// A namespaced alias the freeze synthesizes (a namespace-map
			// entry naming a recorded type, no stamp of its own) exists
			// because its target was registered under that name: it is
			// created with its target.
			uint32_t created = unit_of_slot(s);
			if (created == NO_UNIT && r.kind == madc::dis::DK_TYPEDEF
			    && r.ref0 && madc::dis::arena_id_is_project(r.ref0))
				created = unit_of_slot(madc::dis::arena_slot_of(r.ref0));
			place(L.arena, s, units, undecided, created, c);
			break;
		}
		case madc::dis::DK_FUNC: {
			if (!(r.flags & madc::dis::DF_IS_FREE_FUNC)
			    || (r.flags & madc::dis::DF_TU_ROOT_ORIGIN))
				break;		// a method / signature rides its owner
			// The bind's free-function verdict: the bare key, then — when
			// that is undeclared — the display name under its namespace.
			uint32_t n1 = 0, nd = 0;
			const char *nm = r.name_id ? a.strings.c_str(r.name_id) : NULL;
			{ const uint32_t *run_ = dn.units_of(nm, n1); add_units(units, run_, n1); }
			const char *disp = r.disp_id ? a.strings.c_str(r.disp_id) : NULL;
			const char *ns = r.ns_id ? a.strings.c_str(r.ns_id) : NULL;
			// A function instance required by some unit is restored with its
			// requirers, never with its template's declarers (the display
			// name); a function carried by a product joins its units.
			const std::set<uint32_t> *req = required_of(s);
			bool instance = req && r.ovl_spelling_id
				&& strstr(a.strings.c_str(r.ovl_spelling_id), "\x01@");
			bool refines = !instance && disp && *disp && ns && *ns;
			if (refines)
				{ const uint32_t *run_ = dn.surface(disp, ns, nd); add_units(units, run_, nd); }
			bool undecided = !instance && !req && n1 == 0 && (!refines || nd == 0);
			if (req) {
				units.insert(req->begin(), req->end());
				++c.required;
			}
			probe_name = "func";
			probe_nm = nm;
			probe_ns = ns;
			probe_extra = disp;
			place(L.arena, s, units, undecided, unit_of_slot(s), c, &fn_lists[s]);
			break;
		}
		case madc::dis::DK_NSBIND:
			nsbind_slots.push_back(s);
			break;
		case madc::dis::DK_DEFBODY:
			defbody_slots.push_back(s);
			break;
		case madc::dis::DK_NSLINK:
		case madc::dis::DK_NSALIAS:
			L.arena[always].push_back(s);
			++c.always;
			break;
		default:
			break;		// derived types: built on first lookup
		}
	}
	// A using-declaration import (the flush binds it iff findVariable(key)
	// finds the function) and an ownerless deferred body ride the units that
	// can bring that function: the arena free function's own lists, the units
	// declaring its key, and — for an import — the units declaring the
	// imported qualified name. None of them: the always list (today's
	// every-bind behaviour).
	auto ride_function = [&](uint32_t s, const char *key, const char *qualified,
				 SeedCensus &c, const char *what) {
		std::map<madc::dis::istring, uint32_t>::const_iterator mo =
			key ? method_owner.find(key) : method_owner.end();
		if (mo != method_owner.end()) {
			L.deps.push_back(std::make_pair(mo->second, (CIR_SEED_DEP_ARENA << 30) | s));
			++c.dependent;
			return;
		}
		std::set<uint32_t> units;
		uint32_t n = 0;
		{ const uint32_t *run_ = dn.units_of(key, n); add_units(units, run_, n); }
		{ const uint32_t *run_ = dn.units_of(qualified, n); add_units(units, run_, n); }
		std::map<madc::dis::istring, uint32_t>::const_iterator fi =
			key ? free_fn_by_name.find(key) : free_fn_by_name.end();
		if (fi != free_fn_by_name.end()) {
			std::map<uint32_t, std::vector<uint32_t> >::const_iterator mi =
				fn_lists.find(fi->second);
			if (mi != fn_lists.end())
				units.insert(mi->second.begin(), mi->second.end());
		}
		if (units.empty()) {
			uint32_t cu = unit_of_slot(s);
			if (cu != NO_UNIT && cu < nunits) {
				L.arena[cu].push_back(s);
				++c.created;
				return;
			}
			L.arena[always].push_back(s);
			++c.always;
			probe_always(what, key, qualified, NULL);
			return;
		}
		for (std::set<uint32_t>::const_iterator it = units.begin(); it != units.end(); ++it)
			L.arena[*it].push_back(s);
		++c.decided;
	};
	for (size_t i = 0; i < nsbind_slots.size(); ++i) {
		uint32_t s = nsbind_slots[i];
		madc::dis::defrec r;
		a.get_def_at(madc::dis::arena_id_of(s), r);
		const char *nm = r.name_id ? a.strings.c_str(r.name_id) : NULL;
		const char *ns = r.ns_id ? a.strings.c_str(r.ns_id) : NULL;
		std::string q = ns && *ns && nm ? std::string(ns) + "::" + nm : std::string();
		ride_function(s, r.disp_id ? a.strings.c_str(r.disp_id) : NULL,
			      q.empty() ? NULL : q.c_str(), census["nsbind"], "nsbind");
	}
	for (size_t i = 0; i < defbody_slots.size(); ++i) {
		uint32_t s = defbody_slots[i];
		madc::dis::defrec r;
		a.get_def_at(madc::dis::arena_id_of(s), r);
		SeedCensus &c = census["defbody"];
		if (r.ref0 && madc::dis::arena_id_is_project(r.ref0)) {
			L.deps.push_back(std::make_pair(madc::dis::arena_slot_of(r.ref0),
							(CIR_SEED_DEP_ARENA << 30) | s));
			++c.dependent;
		} else
			ride_function(s, r.name_id ? a.strings.c_str(r.name_id) : NULL, NULL,
				      c, "defbody");
	}

	// The unit whose parse declared template record i (its first token run's
	// file).
	auto template_declared_in = [&](size_t i) -> uint32_t {
		const cir_forest_template_record &t = f.templates[i];
		if (t.run_begin + 4 > f.template_payload.size())
			return NO_UNIT;
		uint32_t file_id = f.template_payload[t.run_begin + 3];
		return unit_of_file(file_id ? pool.c_str(file_id) : NULL);
	};
	// The unit whose parse created global record i: its point of creation,
	// else its declaring file.
	auto global_created_in = [&](size_t i) -> uint32_t {
		uint32_t u = unit_of_poi(i < f.global_poi.size() ? f.global_poi[i] : NO_UNIT);
		if (u == NO_UNIT)
			u = unit_of_file(i < f.global_files.size() ? f.global_files[i] : NULL);
		return u;
	};

	// 3. Templates: the bind keeps one whose key verdict holds (and whose owner
	// class restored); a BOUND-declared key also seeds its owner. A member
	// template rides its owner class (a dependent); a declared key is also
	// listed under its declaring units — a namespace function template's
	// overload under the one unit declaring it.
	for (size_t i = 0; i < f.templates.size(); ++i) {
		const cir_forest_template_record &t = f.templates[i];
		SeedCensus &c = census["template"];
		if (t.flags & CIR_TMPLF_TU_ROOT)
			continue;
		const char *key = pool.c_str(t.key_id);
		const char *ns = t.ns_id ? pool.c_str(t.ns_id) : NULL;
		uint32_t nk = 0;
		std::set<uint32_t> units;
		{ const uint32_t *run_ = dn.surface(key, ns, nk); add_units(units, run_, nk); }
		if (t.owner_type_id && madc::dis::arena_id_is_project(t.owner_type_id)) {
			L.deps.push_back(std::make_pair(madc::dis::arena_slot_of(t.owner_type_id),
							(CIR_SEED_DEP_TEMPLATE << 30) | (uint32_t)i));
			++c.dependent;
			place(L.tmpl, (uint32_t)i, units, false, NO_UNIT, c);
			continue;
		}
		const uint32_t declared_in = template_declared_in(i);
		// A namespace function template is one OVERLOAD of a name many units
		// may declare overloads of (std::swap: <bits/move.h>, <sstream>,
		// <fstream>, ...): a live parse of a closure sees the overloads its
		// own units declare, so the record is listed with its declaring unit
		// alone — never with every unit declaring the name.
		if ((t.kind == CIR_TMPLK_FN || t.kind == CIR_TMPLK_FN_DECL) && declared_in != NO_UNIT) {
			units.clear();
			units.insert(declared_in);
		}
		probe_name = "template";
		probe_nm = key;
		probe_ns = ns;
		probe_extra = NULL;
		place(L.tmpl, (uint32_t)i, units, nk == 0 && units.empty(),
		      nk == 0 ? declared_in : NO_UNIT, c);
	}

	// 4. Globals: the bind keeps one whose (name, ns) verdict holds.
	for (size_t i = 0; i < f.globals.size(); ++i) {
		const cir_forest_global_record &g = f.globals[i];
		SeedCensus &c = census["global"];
		if (g.gflags & CIR_GLOBALF_TU_ROOT)
			continue;
		uint32_t n1 = 0;
		std::set<uint32_t> units;
		{ const uint32_t *run_ = dn.surface(pool.c_str(g.name_id),
					   g.ns_id ? pool.c_str(g.ns_id) : NULL, n1); add_units(units, run_, n1); }
		if (n1 == 0) {
			std::map<madc::dis::istring, uint32_t>::const_iterator mo =
				method_owner.find(pool.c_str(g.name_id));
			if (mo != method_owner.end()) {
				L.deps.push_back(std::make_pair(mo->second,
								(CIR_SEED_DEP_GLOBAL << 30) | (uint32_t)i));
				++c.dependent;
				continue;
			}
		}
		probe_name = "global";
		probe_nm = pool.c_str(g.name_id);
		probe_ns = g.ns_id ? pool.c_str(g.ns_id) : NULL;
		probe_extra = i < f.global_files.size() ? f.global_files[i] : NULL;
		const uint32_t created = global_created_in(i);
		// A global born inside an instantiation (a product's static data
		// member) is carried by that product — by its requirers alone.
		const std::set<uint32_t> *greq = i < f.global_ctx.size()
			? required_of_key(f.global_ctx[i]) : NULL;
		if (greq) {
			units.insert(greq->begin(), greq->end());
			++c.required;
		}
		place(L.glob, (uint32_t)i, units, n1 == 0 && !greq, created, c);
	}

	// 4b. Decl-entry record ids (format 64): each decl-index entry's name ->
	// the record its registration made, the column a pull-on-lookup reads. A
	// name's candidates are the records of its kind that it spells (bare, or
	// namespace-qualified); the one created in the entry's own unit wins, else
	// the lowest. A function's map key names a free function's record, else a
	// method's owner class (the method's call symbol); a variable's, a global
	// record or a using-declaration's import (DK_NSBIND); a template's, its
	// map key or a member template's own name. A record spelled by its full
	// name (`exact`) outranks a namespaced record's bare spelling: the arena
	// keeps one typedef record per registration — the flat key's and the
	// namespaced one's.
	struct rec_cand { uint32_t tagged, unit; bool exact; };
	typedef std::map<madc::dis::istring, std::vector<rec_cand> > rec_names;
	rec_names type_recs, enum_recs, fn_recs, tmpl_recs, var_recs;
	auto spell = [](rec_names &m, const char *nm, const char *ns, uint32_t tagged,
			uint32_t unit) {
		if (!nm || !*nm)
			return;
		const bool namespaced = ns && *ns;
		std::string q;
		if (namespaced)
			q.append(ns).append("::");
		const bool prefixed = namespaced && !strncmp(nm, q.c_str(), q.size());
		m[madc::dis::istring(nm)].push_back(rec_cand{tagged, unit, !namespaced || prefixed});
		if (namespaced && !prefixed)
			m[madc::dis::istring(q + nm)].push_back(rec_cand{tagged, unit, true});
	};
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r) || !r.name_id)
			continue;
		const uint32_t tagged = (CIR_SEED_DEP_ARENA << 30) | s;
		const char *nm = a.strings.c_str(r.name_id);
		const char *ns = r.ns_id ? a.strings.c_str(r.ns_id) : NULL;
		switch (r.kind) {
		case madc::dis::DK_ENUM:
			spell(enum_recs, nm, ns, tagged, unit_of_slot(s));
			spell(type_recs, nm, ns, tagged, unit_of_slot(s));
			break;
		case madc::dis::DK_STRUCT:
		case madc::dis::DK_UNION:
		case madc::dis::DK_CLASS:
		case madc::dis::DK_TYPEDEF:
		case madc::dis::DK_CARRAY:
			spell(type_recs, nm, ns, tagged, unit_of_slot(s));
			break;
		case madc::dis::DK_FUNC:
			if (r.flags & madc::dis::DF_IS_FREE_FUNC)
				spell(fn_recs, nm, NULL, tagged, unit_of_slot(s));
			break;
		case madc::dis::DK_NSBIND:	// the visible name in its importing namespace
			spell(var_recs, ns && *ns ? (std::string(ns) + "::" + nm).c_str() : nm, NULL,
			      tagged, unit_of_slot(s));
			break;
		default:
			break;
		}
	}
	for (std::map<madc::dis::istring, uint32_t>::const_iterator it = method_owner.begin();
	     it != method_owner.end(); ++it)
		spell(fn_recs, it->first.c_str(), NULL, (CIR_SEED_DEP_ARENA << 30) | it->second,
		      unit_of_slot(it->second));
	for (size_t i = 0; i < f.templates.size(); ++i) {
		const cir_forest_template_record &t = f.templates[i];
		const char *key = pool.c_str(t.key_id);
		const char *tn = t.name_id ? pool.c_str(t.name_id) : NULL;
		const char *ns = t.ns_id ? pool.c_str(t.ns_id) : NULL;
		const uint32_t tagged = (CIR_SEED_DEP_TEMPLATE << 30) | (uint32_t)i;
		spell(tmpl_recs, key, ns, tagged, template_declared_in(i));
		if (tn && key && strcmp(tn, key))
			spell(tmpl_recs, tn, ns, tagged, template_declared_in(i));
	}
	for (size_t i = 0; i < f.globals.size(); ++i) {
		const cir_forest_global_record &g = f.globals[i];
		spell(var_recs, pool.c_str(g.name_id), g.ns_id ? pool.c_str(g.ns_id) : NULL,
		      (CIR_SEED_DEP_GLOBAL << 30) | (uint32_t)i, global_created_in(i));
	}
	// A name's record: its spelling's candidates ranked by the entry's own
	// unit first, then a full-name spelling, then the lowest; a qualified
	// name some record spells only bare (a typedef or enum recorded without
	// its namespace, an inline namespace's member) through its last
	// component, among the entry's own unit's records alone. A template-id is
	// never split.
	auto resolve = [](const rec_names &m, const madc::dis::istring &nm, uint32_t u) -> uint32_t {
		uint32_t best = CIR_DECL_RECORD_NONE;
		rec_names::const_iterator it = m.find(nm);
		if (it != m.end()) {
			bool b_own = false, b_exact = false;
			for (const rec_cand &c : it->second) {
				const bool own = c.unit == u;
				if (own != b_own ? own
				    : c.exact != b_exact ? c.exact
				    : c.tagged < best) {
					best = c.tagged;
					b_own = own;
					b_exact = c.exact;
				}
			}
			return best;
		}
		const size_t cut = nm.rfind("::");
		if (cut == madc::dis::istring::npos || nm.find('<') != madc::dis::istring::npos)
			return best;
		it = m.find(madc::dis::istring(nm, cut + 2));
		if (it != m.end())
			for (const rec_cand &c : it->second)
				if (c.unit == u && c.tagged < best)
					best = c.tagged;
		return best;
	};
	size_t decl_entries = 0, decl_records = 0;
	for (uint32_t u = 0; u < nunits; ++u)
		for (cir_forest_decl_entry &e : f.units[u].decl_index) {
			++decl_entries;
			e.record_id = CIR_DECL_RECORD_NONE;
			const char *nm = pool.c_str(e.name_id);
			if (!nm || !*nm)
				continue;
			const madc::dis::istring name(nm);
			switch (e.kind) {
			case Program::pdkTypedef:
			case Program::pdkStruct:
			case Program::pdkClass:
			case Program::pdkEnum:
				e.record_id = resolve(type_recs, name, u);
				break;
			case Program::pdkFunction:
				e.record_id = resolve(fn_recs, name, u);
				break;
			case Program::pdkTemplate:
				e.record_id = resolve(tmpl_recs, name, u);
				break;
			case Program::pdkVariable: {
				e.record_id = resolve(var_recs, name, u);
				// An enumerator, registered under its enum's scope: the
				// enum's record registers it.
				const size_t cut = name.rfind("::");
				if (e.record_id == CIR_DECL_RECORD_NONE && cut != madc::dis::istring::npos)
					e.record_id = resolve(enum_recs, madc::dis::istring(name, 0, cut), u);
				break;
			}
			default:
				break;
			}
		}
	// A qualified entry is often the namespaced twin of a bare one — the same
	// declaration (one slice, one kind) registered under both keys: it takes
	// its twin's record.
	typedef std::pair<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, madc::dis::istring> >
		twin_key;	// ((slice_begin, slice_end), (kind, name))
	for (uint32_t u = 0; u < nunits; ++u) {
		std::vector<cir_forest_decl_entry> &di = f.units[u].decl_index;
		std::map<twin_key, uint32_t> twin;
		for (const cir_forest_decl_entry &e : di)
			if (e.record_id != CIR_DECL_RECORD_NONE)
				twin[twin_key(std::make_pair(e.slice_begin, e.slice_end),
					      std::make_pair(e.kind, madc::dis::istring(pool.c_str(e.name_id))))]
					= e.record_id;
		for (cir_forest_decl_entry &e : di) {
			if (e.record_id != CIR_DECL_RECORD_NONE)
				continue;
			const madc::dis::istring name(pool.c_str(e.name_id));
			const size_t cut = name.rfind("::");
			if (cut == madc::dis::istring::npos || name.find('<') != madc::dis::istring::npos)
				continue;
			std::map<twin_key, uint32_t>::const_iterator t =
				twin.find(twin_key(std::make_pair(e.slice_begin, e.slice_end),
						   std::make_pair(e.kind, madc::dis::istring(name, cut + 2))));
			if (t != twin.end())
				e.record_id = t->second;
		}
		for (const cir_forest_decl_entry &e : di)
			if (e.record_id != CIR_DECL_RECORD_NONE)
				++decl_records;
	}

	// 5. RECORDABLE — the fixpoint once, here.
	std::vector<uint32_t> agg_ids;
	size_t max_anon = 0;
	for (uint32_t s = 0; s < nslots; ++s) {
		uint32_t tid = madc::dis::arena_id_of(s);
		madc::dis::defrec r;
		if (!a.get_def_at(tid, r) || r.kind == madc::dis::DK_NONE)
			continue;
		if (r.name_id) {
			size_t n = arena_anon_tag_of(a.strings.c_str(r.name_id));
			if (n > max_anon)
				max_anon = n;
		}
		if (arena_recordable_candidate(r))
			agg_ids.push_back(tid);
	}
	std::set<uint32_t> recordable;
	arena_recordable_fixpoint(a, agg_ids, recordable);
	f.recordable.assign(2 + ((size_t)nslots + 31) / 32, 0u);
	f.recordable[0] = nslots;
	f.recordable[1] = (uint32_t)max_anon;
	for (std::set<uint32_t>::const_iterator it = recordable.begin(); it != recordable.end(); ++it) {
		uint32_t s = madc::dis::arena_slot_of(*it);
		f.recordable[2 + (s >> 5)] |= 1u << (s & 31);
	}

	// 6. SEEDS — sorted, unique runs.
	const uint32_t nl = nunits + 1;
	auto finish = [](std::vector<std::vector<uint32_t> > &lists, std::vector<uint32_t> &off,
			 std::vector<uint32_t> &ids) {
		off.assign(1, 0u);
		for (size_t l = 0; l < lists.size(); ++l) {
			std::sort(lists[l].begin(), lists[l].end());
			lists[l].erase(std::unique(lists[l].begin(), lists[l].end()), lists[l].end());
			ids.insert(ids.end(), lists[l].begin(), lists[l].end());
			off.push_back((uint32_t)(off.back() + lists[l].size()));
		}
	};
	std::vector<uint32_t> aoff, toff, goff, aids, tids, gids;
	finish(L.arena, aoff, aids);
	finish(L.tmpl, toff, tids);
	finish(L.glob, goff, gids);
	std::sort(L.deps.begin(), L.deps.end());
	L.deps.erase(std::unique(L.deps.begin(), L.deps.end()), L.deps.end());
	// A class product's DEFINERS (format 63): the units defining its template,
	// by its canonical template-id. A bind whose closure holds none of them
	// restores a product the closure only NAMES (a typedef, a signature, a
	// pointer) the way a live parse of that closure has it — declared, never
	// defined (<iosfwd>'s stringbuf without <sstream>).
	std::vector<std::pair<uint32_t, uint32_t> > defs;
	for (uint32_t s = 0; s < nslots; ++s) {
		madc::dis::defrec r;
		if (!a.get_def_at(madc::dis::arena_id_of(s), r) || !r.canon_id
		    || (r.kind != madc::dis::DK_STRUCT && r.kind != madc::dis::DK_UNION
			&& r.kind != madc::dis::DK_CLASS))
			continue;
		const std::vector<uint32_t> *du =
			template_units_of(madc::dis::istring(a.strings.c_str(r.canon_id)));
		if (du)
			for (size_t i = 0; i < du->size(); ++i)
				defs.push_back(std::make_pair(s, (*du)[i]));
	}
	std::sort(defs.begin(), defs.end());
	defs.erase(std::unique(defs.begin(), defs.end()), defs.end());
	std::vector<uint32_t> &w = f.seeds;
	w.clear();
	w.push_back(nl);
	w.insert(w.end(), aoff.begin(), aoff.end());
	w.insert(w.end(), toff.begin(), toff.end());
	w.insert(w.end(), goff.begin(), goff.end());
	w.push_back((uint32_t)L.deps.size());
	for (size_t i = 0; i < L.deps.size(); ++i)
		w.push_back(L.deps[i].first);
	for (size_t i = 0; i < L.deps.size(); ++i)
		w.push_back(L.deps[i].second);
	w.push_back((uint32_t)defs.size());
	for (size_t i = 0; i < defs.size(); ++i)
		w.push_back(defs[i].first);
	for (size_t i = 0; i < defs.size(); ++i)
		w.push_back(defs[i].second);
	w.insert(w.end(), aids.begin(), aids.end());
	w.insert(w.end(), tids.begin(), tids.end());
	w.insert(w.end(), gids.begin(), gids.end());

	// The census: per kind, entities listed by a decided name / by their point
	// of creation / always / as owner dependents / never visited / and how
	// many joined the units requiring them (instantiation requirements).
	char buf[160];
	snprintf(buf, sizeof(buf), "%u names, %zu+%zu+%zu seeds (always %u/%u/%u), %zu dependents, %zu definers, %zu recordable",
		 dn.size(), aids.size(), tids.size(), gids.size(),
		 aoff[nl] - aoff[nl - 1], toff[nl] - toff[nl - 1], goff[nl] - goff[nl - 1],
		 L.deps.size(), defs.size(), recordable.size());
	summary = buf;
	for (std::map<madc::dis::istring, SeedCensus>::const_iterator it = census.begin();
	     it != census.end(); ++it) {
		snprintf(buf, sizeof(buf), "; %s %zu/%zu/%zu/%zu/%zu/%zu", it->first.c_str(),
			 it->second.decided, it->second.created, it->second.always,
			 it->second.dependent, it->second.unplaced, it->second.required);
		summary += buf;
	}
	snprintf(buf, sizeof(buf), "; requirements %zu dropped outside the defining closure, %zu keys unfiltered",
		 req_dropped, req_unknown_keys.size());
	summary += buf;
	snprintf(buf, sizeof(buf), "; decl records %zu of %zu entries", decl_records, decl_entries);
	summary += buf;
}
