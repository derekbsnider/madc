// ---- the frame loop (the reference's main / do_move / do_command) ------
// The turn IS the frame: apply the pending move, describe, read an
// intent, dispatch. Loop levels mirror the reference's state machine:
// word_loop 1 = executed (move next), 2 = read another command,
// 3 = re-describe without moving (GO_TOP).

// Apply the move the travel system chose. False = the player died.
bool do_move_step()
{
	if ( outside_loc(NEWLOC) && NEWLOC != 0 && CLOSNG )
	{
		rspeak("EXIT_CLOSED");
		NEWLOC = LOC;
		if ( !PANIC )
			CLOCK2 = 15;
		PANIC = true;
	}
	// A dwarf blocking the way (it came from where we are going).
	if ( NEWLOC != LOC && !forced_loc(LOC) && !loccond(LOC, "NOARRR") )
	{
		long di = 1;
		while ( di <= 5 )
		{
			var dn = "dwarf";
			dn += to_string(di).c_str();
			long dw = E(dn.c_str());
			if ( bag_flag(dw, "seen")
			  && bag_int(dw, "oldloc", 0) == NEWLOC )
			{
				NEWLOC = LOC;
				rspeak("DWARF_BLOCK");
				break;
			}
			di = di + 1;
		}
	}
	LOC = NEWLOC;
	ui::move(W, PLAYER, LOC);
	if ( !dwarfmove() )
		croak();
	if ( LOC == 0 )
		croak();
	if ( !forced_loc(LOC) && is_dark() && WZDARK && pct(35) )
	{
		rspeak("PIT_FALL");
		OLDLC2 = LOC;
		croak();
		return false;
	}
	return true;
}

// Read one command into CMD (two-word gate, empty-line reread).
bool read_command()
{
	for (;;)
	{
		var line;
		if ( !get_input(line) )
			return false;
		if ( word_count(line) > 2 )
		{
			rspeak("TWO_WORDS");
			continue;
		}
		if ( line.length() > 0 )
		{
			tokenize_command(line);
			return true;
		}
	}
}

// The enter-water tease-outs and word rewrites (the reference's
// preprocess_command). False = command consumed, read another.
bool preprocess_command()
{
	long t0 = CMD["type0"].as_integer();
	long t1 = CMD["type1"].as_integer();
	var id0 = CMD["id0"];
	var id1 = CMD["id1"];
	if ( t0 == T_MOTION && id0 == "ENTER"
	  && ((t1 == T_MOTION && id1 == "STREAM")
	   || (t1 == T_OBJECT && id1 == "WATER")) )
	{
		var lq;
		liq_loc(lq, LOC);
		if ( lq == "WATER" )
			rspeak("FEET_WET");
		else
			rspeak("WHERE_QUERY");
		return false;
	}
	if ( t0 == T_OBJECT )
	{
		if ( t1 == T_ACTION )
		{
			// object-verb arrives; process as verb-object.
			var sr = CMD["raw0"];
			var st = CMD["type0"];
			var si = CMD["id0"];
			CMD["raw0"] = CMD["raw1"];
			CMD["type0"] = CMD["type1"];
			CMD["id0"] = CMD["id1"];
			CMD["raw1"] = sr;
			CMD["type1"] = st;
			CMD["id1"] = si;
			t0 = CMD["type0"].as_integer();
			t1 = CMD["type1"].as_integer();
			id0 = CMD["id0"];
			id1 = CMD["id1"];
		}
		if ( t0 == T_OBJECT && id0 == "GRATE" )
		{
			CMD["type0"] = T_MOTION;
			if ( LOC == E("LOC_START") || LOC == E("LOC_VALLEY")
			  || LOC == E("LOC_SLIT") )
				CMD["id0"] = "DEPRESSION";
			else if ( LOC == E("LOC_COBBLE")
			       || LOC == E("LOC_DEBRIS")
			       || LOC == E("LOC_AWKWARD")
			       || LOC == E("LOC_BIRDCHAMBER")
			       || LOC == E("LOC_PITTOP") )
				CMD["id0"] = "ENTRANCE";
		}
		if ( (id0 == "WATER" || id0 == "OIL")
		  && (id1 == "PLANT" || id1 == "DOOR") )
		{
			if ( at_obj(E(id1.c_str())) )
			{
				CMD["raw1"] = CMD["raw0"];
				CMD["id1"] = CMD["id0"];
				CMD["type1"] = T_OBJECT;
				CMD["raw0"] = "pour";
				CMD["id0"] = "POUR";
				CMD["type0"] = T_ACTION;
			}
		}
		if ( id0 == "CAGE" && id1 == "BIRD" && here_obj(E("CAGE"))
		  && here_obj(E("BIRD")) )
		{
			CMD["id0"] = "CARRY";
			CMD["type0"] = T_ACTION;
		}
	}
	return true;
}

// The per-turn bookkeeping half of the reference's closecheck: turn
// thresholds penalize and announce. The cave-closing clock machinery
// (clock1/clock2, tally==0 deep states) is slice A9.
void turn_bookkeeping()
{
	var ths;
	ui::get(ths, W, E("adv-tables"), "turn_thresholds");
	for ( value th : ths )
	{
		var t = th["threshold"];
		if ( TURNS == t.as_integer() + 1 )
		{
			var pl = th["point_loss"];
			TRNLUZ = TRNLUZ + pl.as_integer();
			var m = th["message"];
			speak_msg(m);
		}
	}
}

// Process the tokenized words: 1 = executed, 2 = read another command,
// 3 = re-describe without moving.
long word_loop()
{
	for (;;)
	{
		long t0 = CMD["type0"].as_integer();
		var raw0 = CMD["raw0"];
		if ( t0 == T_NOTFOUND )
		{
			sspeak_s("DONT_KNOW", raw0);
			clear_command();
			return 2;
		}
		if ( t0 == T_NONE )
		{
			clear_command();
			return 2;
		}
		// Shortcut hints (the west/go counters).
		var low0 = php::strtolower(raw0);
		if ( low0 == "west" )
		{
			IWEST = IWEST + 1;
			if ( IWEST == 10 )
				rspeak("W_IS_WEST");
		}
		var raw1 = CMD["raw1"];
		long t1 = CMD["type1"].as_integer();
		if ( low0 == "go" && raw1.length() > 0 && t1 != T_NUMERIC )
		{
			IGO = IGO + 1;
			if ( IGO == 10 )
				rspeak("GO_UNNEEDED");
		}
		if ( t0 == T_MOTION )
		{
			var m = CMD["id0"];
			playermove(m);
			return 1;
		}
		long part = P_UNKNOWN;
		if ( t0 == T_OBJECT )
		{
			part = P_UNKNOWN;
			CMD["obj"] = CMD["id0"];
		}
		else if ( t0 == T_ACTION )
		{
			CMD["verb"] = CMD["id0"];
			if ( t1 == T_NUMERIC )
				part = P_TRANSITIVE;
			else
				part = P_INTRANSITIVE;
		}
		else
		{
			// A bare number: the reference says don't-know.
			sspeak_s("DONT_KNOW", raw0);
			clear_command();
			return 2;
		}
		long g = act_dispatch(part);
		if ( g == G_TERMINATE )
			return 1;
		if ( g == G_MOVE )
		{
			var nul = "NUL";
			playermove(nul);
			return 1;
		}
		if ( g == G_WORD2 )
		{
			shift_words();
			continue;
		}
		if ( g == G_UNKNOWN )
		{
			var cap = php::ucfirst(raw0);
			sspeak_s("DO_WHAT", cap);
			CMD["obj"] = "";
			return 2;
		}
		if ( g == G_CHECKHINT )
			return 2;
		if ( g == G_CLEAROBJ )
		{
			clear_command();
			return 2;
		}
		if ( g == G_TOP )
			return 3;
		fprintf(stderr, "advent: unhandled phase code %ld\n", g);
		exit(3);
	}
}

// One described turn: false only at EOF.
bool do_command_once()
{
	for (;;)
	{
		describe_location();
		if ( forced_loc(LOC) )
		{
			var fm = "__FORCED";
			playermove(fm);
			return true;
		}
		listobjects();
		// The reference clears the command at each re-describe (so a
		// GO_TOP verb's object never leaks into the next command);
		// verb/object persistence across re-INPUT (rc 2) is preserved.
		clear_command();
		for (;;)
		{
			// (closing-time stash checks land with A9)
			WZDARK = is_dark();
			if ( KNFLOC > 0 && KNFLOC != LOC )
				KNFLOC = 0;
			checkhints();
			if ( !read_command() )
				return false;
			if ( FOOBAR > 0 )
				FOOBAR = -FOOBAR;
			else
				FOOBAR = 0;
			TURNS = TURNS + 1;
			if ( !preprocess_command() )
				continue;
			turn_bookkeeping();
			long rc = word_loop();
			if ( rc == 1 )
				return true;
			if ( rc == 3 )
				break;
			// rc == 2: read another command
		}
	}
}
