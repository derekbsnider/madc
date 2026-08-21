// ---- the describe/listobjects view (the transcript's room text) --------

// Speak the kk'th entry of an object's per-state text array (the
// reference's pspeak): descriptions = look, changes = change, sounds =
// hear, texts = study. A null or missing entry prints nothing.
void pspeak_prop(long o, const char *prop, long kk, bool blank)
{
	if ( kk < 0 )
		return;
	var ds;
	ui::get(ds, W, o, prop);
	if ( ds.is_null() )
		return;
	long i = 0;
	for ( value d : ds )
	{
		if ( i == kk )
		{
			if ( d.empty() )
				return;
			if ( blank )
				speak_msg(d);
			else
				speak_msg_noblank(d);
			return;
		}
		i++;
	}
}

void pspeak_look(long o, long kk)
{
	pspeak_prop(o, "descriptions", kk, true);
}

// The touch mode: the object's inventory line (no leading blank —
// inventory listings print consecutively).
void pspeak_touch(long o)
{
	var inv;
	ui::get(inv, W, o, "inventory");
	if ( !inv.is_null() && !inv.empty() )
		speak_msg_noblank(inv);
}

// state_change: set the state and speak its change message.
void state_change(long o, long kk)
{
	obj_set_state(o, kk);
	pspeak_prop(o, "changes", kk, true);
}

void state_change_c(long o, const char *label)
{
	state_change(o, state_index_c(o, label));
}

// STEPS describe by which side the player stands on (the reference's
// fixed-side check).
long steps_state(long o)
{
	var fx;
	ui::get(fx, W, o, "fixed_at");
	if ( !fx.is_null() && E(fx.c_str()) == LOC )
		return state_index_c(o, "STEPS_UP");
	return state_index_c(o, "STEPS_DOWN");
}

void describe_location()
{
	long ab = bag_int(LOC, "abbrev", 0);
	var m;
	ui::get(m, W, LOC, "short");
	if ( ab % ABBNUM == 0 || m.is_null() )
		ui::get(m, W, LOC, "long");
	if ( !forced_loc(LOC) && is_dark() )
		msg_of(m, "PITCH_DARK");
	if ( toting(E("BEAR")) )
		rspeak("TAME_BEAR");
	speak_msg(m);
	if ( LOC == E("LOC_Y2") && pct(25) && !CLOSNG )
		rspeak("SAYS_PLUGH");
}

void listobjects()
{
	if ( is_dark() )
		return;
	ui::set(W, LOC, "abbrev", bag_int(LOC, "abbrev", 0) + 1);
	// LISTING-ORDER LAW: the reference's atloc lists PREPEND on drop —
	// list by placement stamp DESCENDING. Zero-padded (999999999-seq)
	// keys make the object bag iterate in exactly that order, and the
	// LIVE fixed-side entries merge in.
	var nm_loc;
	ui::name_of(nm_loc, W, LOC);
	var byseq;
	for ( value side : FIXEDSIDES )
	{
		var l = side["loc"];
		if ( l == nm_loc )
		{
			var sq = side["seq"];
			string key = pad_seq_key(sq.as_integer());
			var snm = side["name"];
			byseq[key.c_str()] = snm.c_str();
		}
	}
	var held;
	ui::contents(held, W, LOC);
	for ( value h : held )
	{
		long o = E(h.c_str());
		var sq;
		ui::get(sq, W, o, "seq");
		if ( sq.is_null() )
			continue;	// actors and singletons are not listed
		string key = pad_seq_key(sq.as_integer());
		byseq[key.c_str()] = h.c_str();
	}
	for ( value onm : byseq )
	{
		long o = E(onm.c_str());
		if ( onm == "STEPS" && toting(E("NUGGET")) )
			continue;
		if ( !obj_found(o) || obj_stashed(o) )
		{
			if ( CLOSED )
				continue;
			ui::set(W, o, "found", true);
			obj_set_stashed(o, false);
			// Treasure first-sight specials (the reference's
			// prop hacks: full score needs prop 0 later).
			if ( onm == "RUG" )
				obj_set_state(o, state_index_c(o, "RUG_DRAGON"));
			if ( onm == "CHAIN" )
				obj_set_state(o, state_index_c(o, "CHAINING_BEAR"));
			if ( onm == "EGGS" )
				SEENBIGWORDS = true;
			TALLY--;
		}
		long kk = obj_state(o);
		if ( onm == "STEPS" )
			kk = steps_state(o);
		pspeak_look(o, kk);
	}
}
