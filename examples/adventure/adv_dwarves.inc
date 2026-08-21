// ---- dwarves + the pirate (the reference's dwarfmove/spotted_by_pirate) --
// Actors dwarf1..dwarf6 (6 = the pirate). Activity ratchets through
// DFLAG: 0 = asleep until the player is DEEP, 1 = armed (the axe-throw
// encounter), 2+ = full swing. Every randrange call below is RNG
// parity — order and count mirror the reference exactly.

// The pirate spotted the player (always "handled" — he never counts as
// a threatening dwarf). Treasure snarfing and the chest drop.
void pirate_spots()
{
	long chest = E("CHEST");
	long pirate = dwarf_ent(6);
	long chloc = E("LOC_MAZEEND12");
	if ( LOC == chloc || obj_found(chest) )
		return;
	long snarfed = 0;
	bool movechest = false;
	bool robplayer = false;
	long pyr_plac = static_place(E("PYRAMID"));
	long eme_plac = static_place(E("EMERALD"));
	for ( value on : OBJ_NAMES )
	{
		long o = E(on.c_str());
		if ( !o || !obj_treasure(o) )
			continue;
		// The pirate won't take the pyramid from the plover room
		// or the dark room (too easy).
		if ( on == "PYRAMID" && (LOC == pyr_plac || LOC == eme_plac) )
			continue;
		if ( toting(o) || here_obj(o) )
			snarfed++;
		if ( toting(o) )
		{
			movechest = true;
			robplayer = true;
		}
	}
	// Force chest placement before the player finds the last treasure.
	long lamp = E("LAMP");
	if ( TALLY == 1 && snarfed == 0 && ui::location(W, chest) == 0
	  && here_obj(lamp)
	  && obj_state(lamp) == state_index_c(lamp, "LAMP_BRIGHT") )
	{
		rspeak("PIRATE_SPOTTED");
		movechest = true;
	}
	// Chest moves BEFORE the robbery so it lists last at the maze loc.
	if ( movechest )
	{
		move_object(chest, chloc);
		move_object(E("MESSAG"), E("LOC_DEADEND13"));
		ui::move(W, pirate, chloc);
		ui::set(W, pirate, "oldloc", chloc);
		ui::set(W, pirate, "seen", false);
	}
	else
	{
		if ( bag_int(pirate, "oldloc", 0) != ui::location(W, pirate)
		  && pct(20) )
			rspeak("PIRATE_RUSTLES");
	}
	if ( robplayer )
	{
		rspeak("PIRATE_POUNCES");
		for ( value on : OBJ_NAMES )
		{
			long o = E(on.c_str());
			if ( !o || !obj_treasure(o) )
				continue;
			if ( on == "PYRAMID"
			  && (LOC == pyr_plac || LOC == eme_plac) )
				continue;
			if ( at_obj(o) && !obj_is_fixed(o) )
				carry_object(o, on.c_str());
			if ( toting(o) )
				drop_object(o, on.c_str(), chloc);
		}
	}
}

// Move the dwarves; false = the player dies (a knife stuck).
bool dwarfmove()
{
	if ( LOC == 0 || forced_loc(LOC) || loccond(NEWLOC, "NOARRR") )
		return true;
	if ( DFLAG == 0 )
	{
		if ( loccond(LOC, "DEEP") )
			DFLAG = 1;
		return true;
	}
	// The first encounter: kill 0-2 dwarves, dodge onto the axe.
	if ( DFLAG == 1 )
	{
		if ( !loccond(LOC, "DEEP")
		  || (pct(95) && (!loccond(LOC, "NOBACK") || pct(85))) )
			return true;
		DFLAG = 2;
		long k = 1;
		while ( k <= 2 )
		{
			long j = 1 + randrange(5);
			if ( pct(50) )
				ui::move(W, dwarf_ent(j), 0);
			k++;
		}
		k = 1;
		while ( k <= 5 )
		{
			long dw = dwarf_ent(k);
			if ( ui::location(W, dw) == LOC )
				ui::move(W, dw, E("LOC_NUGGET"));
			ui::set(W, dw, "oldloc", ui::location(W, dw));
			k++;
		}
		rspeak("DWARF_RAN");
		drop_object(E("AXE"), "AXE", LOC);
		return true;
	}
	// Full swing: every dwarf walks (random DEEP neighbor, no
	// backtracking unless cornered); a dwarf who has seen the player
	// sticks with him; unmoved dwarves attack.
	long dtotal = 0;
	long attack = 0;
	long stick = 0;
	long i = 1;
	while ( i <= 6 )
	{
		long dw = dwarf_ent(i);
		long dloc = ui::location(W, dw);
		if ( dloc == 0 )
		{
			i++;
			continue;
		}
		long doldloc = bag_int(dw, "oldloc", 0);
		// Candidate destinations, one per travel rule (the
		// reference's per-verb entries collapse on the
		// consecutive-duplicate check), conditions ignored.
		var tk;
		long j = 0;
		long prev = 0;
		var tr;
		ui::get(tr, W, dloc, "travel");
		if ( !tr.is_null() )
		{
			for ( value r : tr )
			{
				var act = r["action"];
				if ( act[0] != "goto" )
					continue;
				long d = E(act[1].c_str());
				if ( !loccond(d, "DEEP") )
					continue;
				if ( d == doldloc )
					continue;
				if ( j > 0 && d == prev )
					continue;
				if ( d == dloc )
					continue;
				if ( forced_loc(d) )
					continue;
				if ( i == 6 && loccond(d, "NOARRR") )
					continue;
				var nod = r["nodwarves"];
				if ( !nod.is_null() && nod.as_boolean() )
					continue;
				string tkkey = to_string(10 + j);
				tk[tkkey.c_str()] = d;
				prev = d;
				j++;
			}
		}
		// Fallback is the old location; excluded when there is any
		// real candidate. The pick consumes exactly one randrange.
		long newdloc = doldloc;
		if ( j >= 1 )
		{
			long pick = randrange(j);
			long ii = 0;
			for ( value t : tk )
			{
				if ( ii == pick )
					newdloc = t.as_integer();
				ii++;
			}
		}
		else
			randrange(1);
		ui::set(W, dw, "oldloc", dloc);
		ui::move(W, dw, newdloc);
		bool seen = (bag_flag(dw, "seen") && loccond(LOC, "DEEP"))
			 || newdloc == LOC || dloc == LOC;
		ui::set(W, dw, "seen", seen);
		if ( !seen )
		{
			i++;
			continue;
		}
		ui::move(W, dw, LOC);
		if ( i == 6 )
		{
			pirate_spots();
			i++;
			continue;
		}
		// A threatening little dwarf is in the room with him!
		dtotal++;
		if ( bag_int(dw, "oldloc", 0) == LOC )
		{
			attack++;
			if ( KNFLOC >= 0 )
				KNFLOC = LOC;
			if ( randrange(1000) < 95 * (DFLAG - 2) )
				stick++;
		}
		i++;
	}
	if ( dtotal == 0 )
		return true;
	if ( dtotal == 1 )
		rspeak_d("DWARF_SINGLE", dtotal);
	else
		rspeak_d("DWARF_PACK", dtotal);
	if ( attack == 0 )
		return true;
	if ( DFLAG == 2 )
		DFLAG = 3;
	if ( attack > 1 )
	{
		rspeak_d("THROWN_KNIVES", attack);
		if ( stick > 1 )
			rspeak_d("MULTIPLE_HITS", stick);
		else if ( stick == 1 )
			rspeak_d("ONE_HIT", stick);
		else
			rspeak_d("NONE_HIT", stick);
	}
	else
	{
		rspeak("KNIFE_THROWN");
		if ( stick != 0 )
			rspeak("GETS_YOU");
		else
			rspeak("MISSES_YOU");
	}
	if ( stick == 0 )
		return true;
	OLDLC2 = LOC;
	return false;
}
