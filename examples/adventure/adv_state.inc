// ---- state: the world session, the actor, and the hub access layer ----
// ECS over the madc hub: per-entity state (object state/found/seq,
// location abbrev, containment) lives in world bags via ui::; the hot
// per-turn scalars below are script globals for the single session —
// the save slice (A10) banks them into the world-state entity. MUD
// seam: PLAYER is an actor entity, not a semantic singleton — systems
// take an actor id.

long W = 0;			// hub session handle
long PLAYER = 0;		// the player actor entity
long STATE_E = 0;		// world-state singleton entity
long LOC = 0;			// current location entity
long NEWLOC = 0;		// destination the travel system chose
long OLDLOC = 0, OLDLC2 = 0;	// BACK targets (oldlc2 = pre-death safe spot)
long TURNS = 0, LIMIT = 330, ABBNUM = 5, DETAIL = 0;
long CLOCK1 = 30, CLOCK2 = 50, DFLAG = 0, TALLY = 0;
long IWEST = 0, IGO = 0, FOOBAR = 0, KNFLOC = 0, HOLDING = 0;
bool CLOSNG = false, CLOSED = false, PANIC = false, WZDARK = false;
bool LMWARN = false;		// lamp-dim warning given
bool NOVICE = false, SEENBIGWORDS = false, CLSHNT = false;
long NUMDIE = 0, NDEATHS = 3;	// deaths so far / lives (obituary count)
long TRNLUZ = 0, SAVED = 0;	// turn/save score penalties
long BONUS = 0;			// closing bonus: 0 none 1 splat 2 defeat 3 win
long DKILL = 0;			// dwarves killed (A7)
long SEQ_NEXT = 0;		// listing-order stamp for the next drop
var ZZWORD;			// the reservoir magic word (rng_seed builds it)
var VOC_M, VOC_A, VOC_O;	// vocabulary: word -> [ids], by class
var NOACT;			// message-only verbs: name -> true
var FIXEDSIDES;			// objname -> {loc, seq}: the LIVE second
				// placement of two-placed objects
var ENT;			// entity-id memo: name -> id
var OBJ_NAMES;			// object names in objnum order
var OBJ_INDEX;			// object name -> objnum (1-based)
var HINT_ORDER;			// padded-number key -> hint name
var HINT_LC;			// hint name -> turn counter at hinted locs
var HINT_USED;			// hint name -> true once taken
var CMD;			// the current command bag
var OLDOBJ;			// previous command's object (clear_command)

// Listing-order key: zero-padded (999999999 - seq), so ascending map
// key order IS descending placement order (the atloc-prepend law).
string pad_seq_key(long seq)
{
	return std::format("{:09}", 999999999 - seq);
}

// Memoized entity lookup (the world's find is linear; 255 entities).
long E(const char *name)
{
	if ( !php::array_key_exists(name, ENT) )
		ENT[name] = ui::entity_by_name(W, name);
	return ENT[name].as_integer();
}

long bag_int(long e, const char *key, long dflt)
{
	var v;
	ui::get(v, W, e, key);
	if ( v.is_null() )
		return dflt;
	return v.as_integer();
}

bool bag_flag(long e, const char *key)
{
	var v;
	ui::get(v, W, e, key);
	if ( v.is_null() )
		return false;
	return v.as_boolean();
}

// ---- location conditions (the YAML flags bag) --------------------------

bool loccond(long l, const char *flag)
{
	if ( !l )
		return false;
	var c;
	ui::get(c, W, l, "conditions");
	if ( c.is_null() )
		return false;
	var f = c[flag];
	if ( f.is_null() )
		return false;
	return f.as_boolean();
}

bool outside_loc(long l) { return loccond(l, "ABOVE") || loccond(l, "FOREST"); }
bool inside_loc(long l)  { return !outside_loc(l) || l == E("LOC_BUILDING"); }

// FORCED(loc) == the first travel rule has no verbs (the HERE marker).
bool forced_loc(long l)
{
	if ( !l )
		return false;
	var tr;
	ui::get(tr, W, l, "travel");
	if ( tr.is_null() )
		return false;
	for ( value r : tr )
	{
		var vb = r["verbs"];
		return vb.empty();
	}
	return false;
}

// ---- object state -------------------------------------------------------

long obj_state(long o)            { return bag_int(o, "state", 0); }
void obj_set_state(long o, long s) { ui::set(W, o, "state", s); }
bool obj_found(long o)            { return bag_flag(o, "found"); }
bool obj_treasure(long o)         { return bag_flag(o, "treasure"); }

// Stashed = the reference's negative prop: the description stays
// suppressed until the object is picked up and put down again.
bool obj_stashed(long o)             { return bag_flag(o, "stashed"); }
void obj_set_stashed(long o, bool s) { ui::set(W, o, "stashed", s); }

bool toting(long o)  { return o != 0 && ui::location(W, o) == PLAYER; }

// The LIVE location of an object's fixed (second) side; 0 when it has
// none or it is off the map.
long fixed_side_loc(const char *name)
{
	if ( !php::array_key_exists(name, FIXEDSIDES) )
		return 0;
	var side = FIXEDSIDES[name];
	var l = side["loc"];
	if ( l.empty() )
		return 0;
	return E(l.c_str());
}

// Move an object's fixed side (the reference's move(obj+NOBJECTS, loc)):
// re-stamps its listing seq; dest 0 removes it from the map.
void move_fixed_side(const char *name, long dest)
{
	if ( dest )
	{
		var dn;
		ui::name_of(dn, W, dest);
		FIXEDSIDES[name]["loc"] = dn;
	}
	else
		FIXEDSIDES[name]["loc"] = "";
	FIXEDSIDES[name]["name"] = name;	// a NEW entry needs its key
	FIXEDSIDES[name]["seq"] = SEQ_NEXT;
	SEQ_NEXT++;
}

bool at_obj(long o)
{
	if ( !o )
		return false;
	if ( ui::location(W, o) == LOC )
		return true;
	var nm;
	ui::name_of(nm, W, o);
	return fixed_side_loc(nm.c_str()) == LOC;
}
bool here_obj(long o) { return at_obj(o) || toting(o); }

// The index of a named state in the object's states array (labels from
// the YAML; a numeric spelling converts directly).
long state_index(long o, value &label)
{
	if ( php::ctype_digit(label) )
		return php::intval(label);
	var st;
	ui::get(st, W, o, "states");
	return st.index(label);
}

long state_index_c(long o, const char *label)
{
	var l = label;
	return state_index(o, l);
}

// Move an object and stamp the listing-order law: a fresh drop lists
// FIRST (the reference's atloc prepend), so it takes the next seq and
// listings read seq DESCENDING.
void move_object(long o, long dest)
{
	ui::move(W, o, dest);
	ui::set(W, o, "seq", SEQ_NEXT);
	SEQ_NEXT++;
}

// Carry/drop with the holding-count law (the reference's carry/drop:
// the bird is weightless so cage+bird counting stays right).
void carry_object(long o, const char *name)
{
	if ( toting(o) )
		return;
	if ( strcmp(name, "BIRD") != 0 )
		HOLDING++;
	move_object(o, PLAYER);
}

void drop_object(long o, const char *name, long dest)
{
	if ( toting(o) && strcmp(name, "BIRD") != 0 )
		HOLDING--;
	move_object(o, dest);
}

void destroy_object(long o, const char *name)
{
	drop_object(o, name, 0);
}

// The reference's put(): move somewhere AND stash.
void put_object(long o, const char *name, long dest, long pval)
{
	drop_object(o, name, dest);
	obj_set_state(o, pval);
	obj_set_stashed(o, true);
}

// Is the object fixed in place? A runtime `fixed` bag prop (0 = freed,
// nonzero = fixed) overrides the static shape (immovable one-placed or
// two-placed).
bool obj_is_fixed(long o)
{
	var f;
	ui::get(f, W, o, "fixed");
	if ( !f.is_null() )
		return f.as_integer() != 0;
	if ( bag_flag(o, "immovable") )
		return true;
	var fx;
	ui::get(fx, W, o, "fixed_at");
	return !fx.is_null();
}

void obj_set_fixed(long o, long fixedness)
{
	ui::set(W, o, "fixed", fixedness);
}

// The STATIC original placement (converter `place` prop; 0 = nowhere).
long static_place(long o)
{
	var p;
	ui::get(p, W, o, "place");
	if ( p.is_null() )
		return 0;
	return E(p.c_str());
}

// The STATIC original fixed-side location (converter `fixed_at`).
long static_fixed(long o)
{
	var p;
	ui::get(p, W, o, "fixed_at");
	if ( p.is_null() )
		return 0;
	return E(p.c_str());
}

// The liquid in the bottle: "WATER", "OIL", or "" — by bottle state.
void liquid_in_bottle(value &out)
{
	out = "";
	long b = E("BOTTLE");
	long st = obj_state(b);
	if ( st == state_index_c(b, "WATER_BOTTLE") )
		out = "WATER";
	else if ( st == state_index_c(b, "OIL_BOTTLE") )
		out = "OIL";
}

// Count of the listing chain (placed objects + live fixed sides) at a
// location; `only` = the single entry's name when the count is 1.
long objects_at_loc(value &only, long at)
{
	only = "";
	long n = 0;
	if ( !at )
		return 0;
	var nm_loc;
	ui::name_of(nm_loc, W, at);
	for ( value side : FIXEDSIDES )
	{
		var l = side["loc"];
		if ( l == nm_loc )
		{
			var snm = side["name"];
			only = snm;
			n++;
		}
	}
	var held;
	ui::contents(held, W, at);
	for ( value h : held )
	{
		long o = E(h.c_str());
		var sq;
		ui::get(sq, W, o, "seq");
		if ( sq.is_null() )
			continue;
		only = h;
		n++;
	}
	if ( n != 1 )
		only = "";
	return n;
}

// Juggle: re-front both sides of an object in the listing order (the
// reference's pick-up-and-put-down).
void juggle_object(long o, const char *name)
{
	move_object(o, ui::location(W, o));
	if ( php::array_key_exists(name, FIXEDSIDES) )
		move_fixed_side(name, fixed_side_loc(name));
}

// The i'th dwarf actor entity (1..6; 6 = the pirate).
long dwarf_ent(long i)
{
	string dn = std::format("dwarf{}", i);
	return E(dn.c_str());
}

// First dwarf at a location (0 = none / dwarves not active; the pirate
// never counts). Dead-dwarf bookkeeping lands with A7.
long atdwrf(long l)
{
	if ( DFLAG < 2 )
		return 0;
	for ( long di = 1; di <= 5; di++ )
		if ( ui::location(W, dwarf_ent(di)) == l )
			return di;
	return 0;
}

bool is_dark()
{
	if ( loccond(LOC, "LIT") )
		return false;
	long lamp = E("LAMP");
	if ( obj_state(lamp) != 0 && here_obj(lamp) )
		return false;
	return true;
}

// The liquid at a location: "WATER", "OIL", or "" (dry).
void liq_loc(value &out, long l)
{
	out = "";
	if ( loccond(l, "FLUID") )
	{
		if ( loccond(l, "OILY") )
			out = "OIL";
		else
			out = "WATER";
	}
}

// ---- boot + init --------------------------------------------------------

bool world_boot()
{
	ifstream probe;
	probe.open("examples/adventure/adventure.world");
	if ( probe.good() )
	{
		probe.close();
		W = ui::world_open("examples/adventure/adventure.world");
	}
	else
		W = ui::world_open("adventure.world");
	return W != 0;
}

void init_game()
{
	long tables = E("adv-tables");
	var sq;
	ui::get(sq, W, tables, "seq_next");
	SEQ_NEXT = sq.as_integer();
	ui::get(OBJ_NAMES, W, tables, "objects");
	var na;
	ui::get(na, W, tables, "noaction");
	for ( value n : na )
		NOACT[n.c_str()] = true;
	long ve = E("adv-vocabulary");
	ui::get(VOC_M, W, ve, "motions");
	ui::get(VOC_A, W, ve, "actions");
	ui::get(VOC_O, W, ve, "objwords");

	// Runtime entities ride the same world as the dungeon (one save).
	PLAYER = ui::create(W, "player");
	STATE_E = ui::create(W, "world-state");
	LOC = E("LOC_START");
	NEWLOC = LOC;
	OLDLOC = 0;
	OLDLC2 = 0;
	ui::move(W, PLAYER, LOC);

	// Dwarves (actors; the 6th is the pirate) at their start rooms.
	var dl;
	ui::get(dl, W, tables, "dwarflocs");
	long di = 0;
	for ( value d : dl )
	{
		di++;
		string dn = std::format("dwarf{}", di);
		long dw = ui::create(W, dn.c_str());
		ui::set(W, dw, "is_pirate", di == 6);
		ui::set(W, dw, "seen", false);
		ui::move(W, dw, E(d.c_str()));
	}

	// Treasure bookkeeping, objnum index, and the LIVE fixed-side map.
	// Treasures with an inventory line start NOTFOUND; everything else
	// is found.
	long tally = 0;
	long objnum = 0;
	for ( value on : OBJ_NAMES )
	{
		objnum++;
		OBJ_INDEX[on.c_str()] = objnum;
		long o = E(on.c_str());
		if ( !o )
			continue;
		bool found = true;
		if ( obj_treasure(o) )
		{
			tally++;
			var inv;
			ui::get(inv, W, o, "inventory");
			if ( !inv.is_null() )
				found = false;
		}
		ui::set(W, o, "found", found);
		var fx;
		ui::get(fx, W, o, "fixed_at");
		if ( !fx.is_null() )
		{
			var sf;
			ui::get(sf, W, o, "seq_fixed");
			if ( !sf.is_null() )
			{
				FIXEDSIDES[on.c_str()]["name"] = on;
				FIXEDSIDES[on.c_str()]["loc"] = fx;
				FIXEDSIDES[on.c_str()]["seq"]
					= sf.as_integer();
			}
		}
	}
	TALLY = tally;

	// Hints in NUMBER order (converter hints_by_number — the checkhints
	// scan order); lives from the obituary count.
	ui::get(HINT_ORDER, W, tables, "hints_by_number");
	for ( value hn : HINT_ORDER )
		HINT_LC[hn.c_str()] = (long)0;
	var tbl;
	ui::get(tbl, W, tables, "obituaries");
	NDEATHS = tbl.count();
}
