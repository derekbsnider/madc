// ---- action dispatch (the reference's actions.c, staged) ---------------
// A5 implements the loop-structural verbs: the noaction (message-only)
// class, GO's word-shift, and SEED (RNG parity). Verbs with a default
// message speak it; the A6 wave replaces the tail with real handlers.

long G_TERMINATE = 1;
long G_MOVE = 2;
long G_WORD2 = 3;
long G_UNKNOWN = 4;
long G_CHECKHINT = 5;
long G_CLEAROBJ = 6;
long G_TOP = 7;
long G_DWARFWAKE = 8;

long P_UNKNOWN = 0;
long P_INTRANSITIVE = 1;
long P_TRANSITIVE = 2;

void action_default(value &out, value &verb)
{
	ui::get(out, W, E("adv-action-defaults"), verb.c_str());
}

long act_dispatch(long part)
{
	var verb = CMD["id0"];

	// Message-only verbs speak their message whatever the shape.
	if ( php::array_key_exists(verb.c_str(), NOACT) )
	{
		var dm;
		action_default(dm, verb);
		speak_msg(dm);
		return G_CLEAROBJ;
	}

	if ( part == P_UNKNOWN )
	{
		// Bare object word. A6 grows the full object analysis
		// (liquids, dwarf presence, knife); the visibility gate is
		// the reference's first check.
		long o = E(verb.c_str());
		if ( !here_obj(o) )
		{
			var raw = CMD["raw0"];
			sspeak_s("NO_SEE", raw);
			return G_CLEAROBJ;
		}
		return G_UNKNOWN;
	}

	if ( part == P_INTRANSITIVE )
	{
		var raw1 = CMD["raw1"];
		if ( raw1.length() > 0 && verb != "SAY" )
			return G_WORD2;
		if ( verb == "GO" )
		{
			var dm;
			action_default(dm, verb);
			speak_msg(dm);
			return G_CLEAROBJ;
		}
		if ( verb == "NOTHING" )
		{
			rspeak("OK_MAN");
			return G_CLEAROBJ;
		}
		if ( verb == "SEED" || verb == "WASTE" )
		{
			rspeak("NUMERIC_REQUIRED");
			return G_TOP;
		}
		var dm;
		action_default(dm, verb);
		if ( dm.length() > 0 )
		{
			speak_msg(dm);
			return G_CLEAROBJ;
		}
		return G_UNKNOWN;
	}

	// Transitive (A5: word[1] is NUMERIC — the seed/waste shape).
	if ( verb == "SEED" )
	{
		var raw1 = CMD["raw1"];
		long sd = php::intval(raw1);
		var dm;
		action_default(dm, verb);
		speak_msg_d(dm, sd);
		rng_seed(sd);
		// The seed command does not consume a turn.
		TURNS = TURNS - 1;
		return G_TOP;
	}
	var dm;
	action_default(dm, verb);
	if ( dm.length() > 0 )
	{
		speak_msg(dm);
		return G_CLEAROBJ;
	}
	return G_CLEAROBJ;
}
