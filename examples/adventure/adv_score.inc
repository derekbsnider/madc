// ---- scoring, wrap-up, death, hints (score.c + main.c pieces) -----------

long M_QUIT = 0;	// terminate modes
long M_ENDGAME = 1;
long M_SCOREGAME = 2;

// The scoring algorithm, verbatim from the reference (430 total).
// Returns the score; also computes the possible maximum.
long MXSCOR = 0;

long score_game(long mode)
{
	long sc = 0;
	MXSCOR = 0;
	long chest_num = OBJ_INDEX["CHEST"].as_integer();
	long objnum = 0;
	for ( value on : OBJ_NAMES )
	{
		objnum++;
		long o = E(on.c_str());
		if ( !o || !obj_treasure(o) )
			continue;
		var inv;
		ui::get(inv, W, o, "inventory");
		if ( inv.is_null() )
			continue;
		long k = 12;
		if ( objnum == chest_num )
			k = 14;
		else if ( objnum > chest_num )
			k = 16;
		if ( obj_found(o) )
			sc = sc + 2;
		// The building bonus needs prop EXACTLY 0 (the reference's
		// STATE_FOUND) — a broken vase or cavity-set gem earns none.
		if ( ui::location(W, o) == E("LOC_BUILDING")
		  && obj_found(o) && obj_state(o) == 0 )
			sc = sc + k - 2;
		MXSCOR = MXSCOR + k;
	}
	sc = sc + (NDEATHS - NUMDIE) * 10;
	MXSCOR = MXSCOR + NDEATHS * 10;
	if ( mode == M_ENDGAME )
		sc = sc + 4;
	MXSCOR = MXSCOR + 4;
	if ( DFLAG != 0 )
		sc = sc + 25;
	MXSCOR = MXSCOR + 25;
	if ( CLOSNG )
		sc = sc + 25;
	MXSCOR = MXSCOR + 25;
	if ( CLOSED )
	{
		if ( BONUS == 0 )
			sc = sc + 10;
		if ( BONUS == 1 )
			sc = sc + 25;
		if ( BONUS == 2 )
			sc = sc + 30;
		if ( BONUS == 3 )
			sc = sc + 45;
	}
	MXSCOR = MXSCOR + 45;
	if ( ui::location(W, E("MAGAZINE")) == E("LOC_WITTSEND") )
		sc++;
	MXSCOR++;
	sc = sc + 2;
	MXSCOR = MXSCOR + 2;

	// Hint / novice / oyster-clue / turn / save penalties.
	for ( value hn : HINT_ORDER )
	{
		if ( php::array_key_exists(hn.c_str(), HINT_USED ) )
		{
			var hd;
			ui::get(hd, W, E("adv-hints"), hn.c_str());
			sc = sc - hd["penalty"].as_integer();
		}
	}
	if ( NOVICE )
		sc = sc - 5;
	if ( CLSHNT )
		sc = sc - 10;
	sc = sc - TRNLUZ - SAVED;

	if ( mode == M_SCOREGAME )
		rspeak_d3("GARNERED_POINTS", sc, MXSCOR, TURNS);
	return sc;
}

void terminate_game(long mode)
{
	long points = score_game(mode);
	if ( points + TRNLUZ + 1 >= MXSCOR && TRNLUZ != 0 )
		rspeak("TOOK_LONG");
	if ( points + SAVED + 1 >= MXSCOR && SAVED != 0 )
		rspeak("WITHOUT_SUSPENDS");
	rspeak_d3("TOTAL_SCORE", points, MXSCOR, TURNS);
	var classes;
	ui::get(classes, W, E("adv-tables"), "classes");
	long ncls = classes.count();
	long i = 0;
	for ( value cls : classes )
	{
		i++;
		if ( i == 1 )
			continue;	// the reference starts at index 1
		var th = cls["threshold"];
		if ( th.as_integer() >= points )
		{
			var m = cls["message"];
			speak_msg(m);
			if ( i < ncls )
			{
				long nxt = th.as_integer() + 1 - points;
				rspeak_d("NEXT_HIGHER", nxt);
			}
			else
				rspeak("NO_HIGHER");
			exit(0);
		}
	}
	rspeak("OFF_SCALE");
	exit(0);
}

// The name at a 1-based objnum (counter walk; reverse drops use it).
void obj_name_at(value &out, long idx)
{
	out = "";
	long i = 0;
	for ( value on : OBJ_NAMES )
	{
		i++;
		if ( i == idx )
		{
			out = on;
			return;
		}
	}
}

// Death: obituary prompt, then either the end or reincarnation at the
// building (liquids gone, lamp off, belongings left where he died —
// dropped in REVERSE objnum order, the lamp accessible above ground).
void croak()
{
	var obits;
	ui::get(obits, W, E("adv-tables"), "obituaries");
	var q, yr;
	long i = 0;
	for ( value ob : obits )
	{
		if ( i == NUMDIE )
		{
			q = ob["query"];
			yr = ob["yes_response"];
		}
		i++;
	}
	NUMDIE++;
	if ( CLOSNG )
	{
		rspeak("DEATH_CLOSING");
		terminate_game(M_ENDGAME);
	}
	bool again = false;
	{
		// yes_or_no over already-fetched texts.
		var nm;
		msg_of(nm, "OK_MAN");
		for (;;)
		{
			speak_msg(q);
			var reply;
			if ( !get_input(reply) )
				exit(0);
			var w;
			first_word(w, reply);
			var lw = php::strtolower(w);
			if ( lw.empty() )
			{
				rspeak("PLEASE_ANSWER");
				continue;
			}
			var h = lw.substr(0, 1);
			if ( h == "y" )
			{
				speak_msg(yr);
				again = true;
				break;
			}
			if ( h == "n" )
			{
				speak_msg(nm);
				break;
			}
			rspeak("PLEASE_ANSWER");
		}
	}
	if ( !again || NUMDIE == NDEATHS )
		terminate_game(M_ENDGAME);
	ui::move(W, E("WATER"), 0);
	ui::move(W, E("OIL"), 0);
	long lamp = E("LAMP");
	if ( toting(lamp) )
		obj_set_state(lamp, state_index_c(lamp, "LAMP_DARK"));
	long n = OBJ_NAMES.count();
	long j = n;
	while ( j >= 1 )
	{
		var nm2;
		obj_name_at(nm2, j);
		long o = E(nm2.c_str());
		if ( o && toting(o) )
		{
			if ( nm2 == "LAMP" )
				drop_object(o, "LAMP", E("LOC_START"));
			else
				drop_object(o, nm2.c_str(), OLDLC2);
		}
		j--;
	}
	LOC = E("LOC_BUILDING");
	OLDLOC = LOC;
	NEWLOC = LOC;
	ui::move(W, PLAYER, LOC);
}

// Hint machinery (main.c checkhints): per-hint turn counters at hinted
// locations, per-hint eligibility gates, the two-step prompt.
void checkhints()
{
	var lochints;
	ui::get(lochints, W, LOC, "hints");
	if ( lochints.empty() )
		return;
	for ( value hn : HINT_ORDER )
	{
		if ( php::array_key_exists(hn.c_str(), HINT_USED) )
			continue;
		// Not a hint of THIS location? counter resets to -1.
		bool here = false;
		for ( value lh : lochints )
			if ( lh == hn )
				here = true;
		if ( !here )
			HINT_LC[hn.c_str()] = -1;
		HINT_LC[hn.c_str()] = HINT_LC[hn.c_str()].as_integer() + 1;
		var hd;
		ui::get(hd, W, E("adv-hints"), hn.c_str());
		if ( HINT_LC[hn.c_str()].as_integer()
		     < hd["turns"].as_integer() )
			continue;
		// Eligibility per hint (the reference's switch).
		bool offer = false;
		if ( hn == "CAVE" )
		{
			long grate = E("GRATE");
			offer = obj_state(grate)
				== state_index_c(grate, "GRATE_CLOSED")
			     && !here_obj(E("KEYS"));
			if ( !offer )
			{
				HINT_LC[hn.c_str()] = 0;
				continue;
			}
		}
		else if ( hn == "BIRD" )
		{
			var ob = OLDOBJ;
			offer = ui::location(W, E("BIRD")) == LOC
			     && toting(E("ROD")) && ob == "BIRD";
			if ( !offer )
				continue;
		}
		else if ( hn == "SNAKE" )
		{
			offer = here_obj(E("SNAKE")) && !here_obj(E("BIRD"));
			if ( !offer )
			{
				HINT_LC[hn.c_str()] = 0;
				continue;
			}
		}
		else if ( hn == "MAZE" || hn == "WOODS" )
		{
			var only;
			bool bare = objects_at_loc(only, LOC) == 0
				 && objects_at_loc(only, OLDLOC) == 0
				 && objects_at_loc(only, OLDLC2) == 0;
			if ( hn == "MAZE" )
				offer = bare && HOLDING > 1;
			else
				offer = bare;
			if ( !offer )
			{
				if ( hn == "MAZE" )
					HINT_LC[hn.c_str()] = 0;
				continue;
			}
		}
		else if ( hn == "DARK" )
		{
			offer = obj_found(E("EMERALD"))
			     && !obj_found(E("PYRAMID"));
			if ( !offer )
			{
				HINT_LC[hn.c_str()] = 0;
				continue;
			}
		}
		else if ( hn == "WITT" )
			offer = true;
		else if ( hn == "CLIFF" )
		{
			offer = DFLAG == 0;
			if ( !offer )
			{
				HINT_LC[hn.c_str()] = 0;
				continue;
			}
		}
		else if ( hn == "OGRE" )
		{
			long i = atdwrf(LOC);
			if ( i < 0 )
			{
				HINT_LC[hn.c_str()] = 0;
				continue;
			}
			offer = here_obj(E("OGRE")) && i == 0;
			if ( !offer )
				continue;
		}
		else if ( hn == "JADE" )
		{
			offer = TALLY == 1 && !obj_found(E("JADE"));
			if ( !offer )
			{
				HINT_LC[hn.c_str()] = 0;
				continue;
			}
		}
		else
			offer = true;
		// The offer: question, cost, then the hint itself.
		HINT_LC[hn.c_str()] = 0;
		var q = hd["question"];
		var qh;
		qh = q.c_str();
		var none;
		msg_of(none, "NO_MESSAGE");
		var okm;
		msg_of(okm, "OK_MAN");
		bool wants = false;
		for (;;)
		{
			speak_msg(qh);
			var reply;
			if ( !get_input(reply) )
				exit(0);
			var w;
			first_word(w, reply);
			var lw = php::strtolower(w);
			if ( lw.empty() )
			{
				rspeak("PLEASE_ANSWER");
				continue;
			}
			var h = lw.substr(0, 1);
			if ( h == "y" )
			{
				wants = true;
				break;
			}
			if ( h == "n" )
			{
				speak_msg(okm);
				break;
			}
			rspeak("PLEASE_ANSWER");
		}
		if ( !wants )
			return;
		long penalty = hd["penalty"].as_integer();
		rspeak_d("HINT_COST", penalty);
		var hq;
		msg_of(hq, "WANT_HINT");
		var htext = hd["hint"];
		var ht;
		ht = htext.c_str();
		bool took = false;
		for (;;)
		{
			speak_msg(hq);
			var reply;
			if ( !get_input(reply) )
				exit(0);
			var w;
			first_word(w, reply);
			var lw = php::strtolower(w);
			if ( lw.empty() )
			{
				rspeak("PLEASE_ANSWER");
				continue;
			}
			var h = lw.substr(0, 1);
			if ( h == "y" )
			{
				speak_msg(ht);
				took = true;
				break;
			}
			if ( h == "n" )
			{
				speak_msg(okm);
				break;
			}
			rspeak("PLEASE_ANSWER");
		}
		if ( took )
		{
			HINT_USED[hn.c_str()] = true;
			if ( LIMIT > 30 )
				LIMIT = LIMIT + 30 * penalty;
		}
	}
}
