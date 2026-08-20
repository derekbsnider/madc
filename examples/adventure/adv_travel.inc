// ---- the travel interpreter (the reference's playermove) ---------------
// Ordered rule bags per location: {verbs:[...], cond, action}. A rule
// with no verbs is the forced (HERE) marker and matches any motion.
// Condition failure skips past every consecutive rule with the same
// (cond, action) signature — the reference's traveleq law.

bool rule_matches(value &r, value &motion)
{
	var vb = r["verbs"];
	if ( vb.count() == 0 )
		return true;
	for ( value v : vb )
		if ( v == motion )
			return true;
	return false;
}

void rule_sig(value &out, value &r)
{
	out = "";
	var c = r["cond"];
	if ( !c.is_null() )
	{
		for ( value x : c )
		{
			out += x.c_str();
			out += ",";
		}
	}
	out += "|";
	var a = r["action"];
	if ( !a.is_null() )
	{
		for ( value x : a )
		{
			out += x.c_str();
			out += ",";
		}
	}
}

// Travel-rule condition forms: null | [pct N] | [carry OBJ] |
// [with OBJ] | [not OBJ STATE].
bool cond_pass(value &r)
{
	var c = r["cond"];
	if ( c.is_null() )
		return true;
	var kind = c[0].c_str();
	if ( kind == "pct" )
		return pct(php::intval(c[1].c_str()));
	var objname = c[1].c_str();
	long o = E(objname.c_str());
	if ( kind == "carry" )
		return toting(o);
	if ( kind == "with" )
		return toting(o) || at_obj(o);
	if ( kind == "not" )
	{
		var st = c[2].c_str();
		return obj_state(o) != state_index(o, st);
	}
	return true;
}

void bad_motion(value &motion)
{
	if ( motion == "EAST" || motion == "WEST" || motion == "SOUTH"
	  || motion == "NORTH" || motion == "NE" || motion == "NW"
	  || motion == "SW" || motion == "SE" || motion == "UP"
	  || motion == "DOWN" )
		rspeak("BAD_DIRECTION");
	else if ( motion == "FORWARD" || motion == "LEFT"
	       || motion == "RIGHT" )
		rspeak("UNSURE_FACING");
	else if ( motion == "OUTSIDE" || motion == "INSIDE" )
		rspeak("NO_INOUT_HERE");
	else if ( motion == "XYZZY" || motion == "PLUGH" )
		rspeak("NOTHING_HAPPENS");
	else if ( motion == "CRAWL" )
		rspeak("WHICH_WAY");
	else
		rspeak("CANT_APPLY");
}

void travel_apply(value &motion)
{
	var tr;
	ui::get(tr, W, LOC, "travel");
	long phase = 0;		// 0 = searching, 1 = evaluating, 2 = skipping
	var skip_sig;
	for ( value r : tr )
	{
		if ( phase == 0 )
		{
			if ( !rule_matches(r, motion) )
				continue;
			phase = 1;
		}
		else if ( phase == 2 )
		{
			var sig;
			rule_sig(sig, r);
			if ( sig == skip_sig )
				continue;
			phase = 1;
		}
		if ( !cond_pass(r) )
		{
			rule_sig(skip_sig, r);
			phase = 2;
			continue;
		}
		var act = r["action"];
		var kind = act[0].c_str();
		if ( kind == "goto" )
		{
			var dest = act[1].c_str();
			NEWLOC = E(dest.c_str());
			return;
		}
		if ( kind == "speak" )
		{
			var mn = act[1].c_str();
			rspeak(mn.c_str());
			NEWLOC = LOC;
			return;
		}
		// Special travel (plover passage / transport, troll
		// bridge) — slice A8. Loud so a fragment reaching one is
		// visibly out of scope, never silently wrong.
		fprintf(stderr, "advent: special travel unimplemented (A8)\n");
		NEWLOC = LOC;
		return;
	}
	if ( phase == 0 )
	{
		bad_motion(motion);
		return;
	}
	fprintf(stderr, "advent: conditional travel group with no tail\n");
	exit(3);
}

// Destination of a location's FIRST rule when it is a goto (the BACK
// scan's forced-passthrough probe).
long first_goto_dest(long l)
{
	var tr;
	ui::get(tr, W, l, "travel");
	if ( tr.is_null() )
		return 0;
	for ( value r : tr )
	{
		var act = r["action"];
		var kind = act[0].c_str();
		if ( kind == "goto" )
		{
			var dest = act[1].c_str();
			return E(dest.c_str());
		}
		return 0;
	}
	return 0;
}

void playermove(value &motion)
{
	NEWLOC = LOC;
	if ( motion == "NUL" )
		return;
	if ( motion == "BACK" )
	{
		long tgt = OLDLOC;
		if ( forced_loc(tgt) )
			tgt = OLDLC2;
		OLDLC2 = OLDLOC;
		OLDLOC = LOC;
		if ( loccond(LOC, "NOBACK") )
		{
			rspeak("TWIST_TURN");
			return;
		}
		if ( tgt == LOC )
		{
			rspeak("FORGOT_PATH");
			return;
		}
		// First rule going straight to tgt wins; otherwise the LAST
		// rule whose forced destination's first hop lands on tgt.
		var via;
		via = "";
		bool via_found = false;
		var thru;
		thru = "";
		bool thru_found = false;
		var tr;
		ui::get(tr, W, LOC, "travel");
		for ( value r : tr )
		{
			var act = r["action"];
			var kind = act[0].c_str();
			if ( kind != "goto" )
				continue;
			var dest = act[1].c_str();
			long d = E(dest.c_str());
			var vb = r["verbs"];
			if ( d == tgt )
			{
				if ( vb.count() == 0 )
					via = "__FORCED";
				else
					via = vb[0].c_str();
				via_found = true;
				break;
			}
			if ( forced_loc(d) && first_goto_dest(d) == tgt )
			{
				if ( vb.count() == 0 )
					thru = "__FORCED";
				else
					thru = vb[0].c_str();
				thru_found = true;
			}
		}
		if ( !via_found && thru_found )
		{
			via = thru;
			via_found = true;
		}
		if ( !via_found )
		{
			rspeak("NOT_CONNECTED");
			return;
		}
		travel_apply(via);
		return;
	}
	if ( motion == "LOOK" )
	{
		// Pretend it is not dark so staring into the gloom is safe.
		if ( DETAIL < 3 )
			rspeak("NO_MORE_DETAIL");
		DETAIL = DETAIL + 1;
		WZDARK = false;
		ui::set(W, LOC, "abbrev", (long)0);
		return;
	}
	if ( motion == "CAVE" )
	{
		if ( outside_loc(LOC) && LOC != E("LOC_GRATE") )
			rspeak("FOLLOW_STREAM");
		else
			rspeak("NEED_DETAIL");
		return;
	}
	OLDLC2 = OLDLOC;
	OLDLOC = LOC;
	travel_apply(motion);
}
