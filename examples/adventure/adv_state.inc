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
bool NOVICE = false, SEENBIGWORDS = false;
long SEQ_NEXT = 0;		// listing-order stamp for the next drop
var ZZWORD;			// the reservoir magic word (rng_seed builds it)
var VOC_M, VOC_A, VOC_O;	// vocabulary: word -> [ids], by class
var NOACT;			// message-only verbs: name -> true
var FIXEDAT;			// locname -> { seqkey: objname } fixed sides
var ENT;			// entity-id memo: name -> id
var OBJ_NAMES;			// object names in objnum order
var CMD;			// the current command bag

// Listing-order key: zero-padded (999999999 - seq), so ascending map
// key order IS descending placement order (the atloc-prepend law).
string pad_seq_key(long seq)
{
	string s = to_string(999999999 - seq);
	string z = "";
	long i = s.length();
	while ( i < 9 )
	{
		z += "0";
		i = i + 1;
	}
	z += s;
	return z;
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
		return vb.count() == 0;
	}
	return false;
}

// ---- object state -------------------------------------------------------

long obj_state(long o)            { return bag_int(o, "state", 0); }
void obj_set_state(long o, long s) { ui::set(W, o, "state", s); }
bool obj_found(long o)            { return bag_flag(o, "found"); }
bool obj_treasure(long o)         { return bag_flag(o, "treasure"); }

bool toting(long o)  { return o != 0 && ui::location(W, o) == PLAYER; }
bool at_obj(long o)
{
	if ( !o )
		return false;
	if ( ui::location(W, o) == LOC )
		return true;
	var fx;
	ui::get(fx, W, o, "fixed_at");
	if ( fx.is_null() )
		return false;
	return E(fx.c_str()) == LOC;
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
	if ( !st.is_null() )
	{
		long i = 0;
		for ( value s : st )
		{
			if ( s == label )
				return i;
			i = i + 1;
		}
	}
	return -1;
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
	SEQ_NEXT = SEQ_NEXT + 1;
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
		di = di + 1;
		var dn = "dwarf";
		dn += to_string(di).c_str();
		long dw = ui::create(W, dn.c_str());
		ui::set(W, dw, "is_pirate", di == 6);
		ui::set(W, dw, "seen", false);
		ui::move(W, dw, E(d.c_str()));
	}

	// Treasure bookkeeping + the fixed-side listing index. Treasures
	// with an inventory line start NOTFOUND; everything else is found.
	long tally = 0;
	for ( value on : OBJ_NAMES )
	{
		long o = E(on.c_str());
		if ( !o )
			continue;
		bool found = true;
		if ( obj_treasure(o) )
		{
			tally = tally + 1;
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
				string key = pad_seq_key(sf.as_integer());
				FIXEDAT[fx.c_str()][key.c_str()] = on.c_str();
			}
		}
	}
	TALLY = tally;
}
