// ---- action dispatch + verb systems (the reference's actions.c) --------
// A6 wave 1: the object-word analysis and take/drop/lock/unlock/light/
// extinguish/inventory/eat/drink, plus the loop-structural verbs
// (noaction class, GO's word-shift, SEED, WASTE, NOTHING). Deeper
// machinery (liquids/fill, bear/chain/troll, rug hover, listen, magic
// words, scoring) lands with A7-A9 — those paths exit LOUD, never
// silently wrong.

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

bool BLOODED = false;		// drunk the dragon's blood (A8 listen uses it)

void action_default(value &out, value &verb)
{
	ui::get(out, W, E("adv-action-defaults"), verb.c_str());
}

// speak(actions[verb].message) — the current verb's default message.
void speak_verb_default()
{
	var v = CMD["verb"];
	var dm;
	action_default(dm, v);
	speak_msg(dm);
}

void a8_wall(const char *what)
{
	fprintf(stderr, "advent: %s unimplemented (A8)\n", what);
	exit(3);
}

bool is_gemstone(value &obj)
{
	return obj == "EMERALD" || obj == "RUBY" || obj == "AMBER"
	    || obj == "SAPPH";
}

// Exactly-one-object-here test for intransitive carry: the listing
// chain (placed objects + fixed sides) must hold a single entry.
long objects_at_loc(value &only)
{
	only = "";
	long n = 0;
	var nm_loc;
	ui::name_of(nm_loc, W, LOC);
	if ( php::array_key_exists(nm_loc.c_str(), FIXEDAT) )
	{
		var fx = FIXEDAT[nm_loc.c_str()];
		for ( value f : fx )
		{
			only = f;
			n = n + 1;
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
			continue;
		only = h;
		n = n + 1;
	}
	if ( n != 1 )
		only = "";
	return n;
}

// ---- carry --------------------------------------------------------------

long h_vcarry(value &obj)
{
	if ( obj == "" )
	{
		var only;
		long n = objects_at_loc(only);
		if ( n != 1 || atdwrf(LOC) > 0 )
			return G_UNKNOWN;
		obj = only;
	}
	long o = E(obj.c_str());
	if ( toting(o) )
	{
		speak_verb_default();
		return G_CLEAROBJ;
	}
	if ( obj == "MESSAG" )
	{
		rspeak("REMOVE_MESSAGE");
		destroy_object(o, "MESSAG");
		return G_CLEAROBJ;
	}
	if ( obj_is_fixed(o) )
	{
		if ( obj == "PLANT" )
		{
			if ( obj_state(o) <= 0 )
				rspeak("DEEP_ROOTS");
			else
				rspeak("YOU_JOKING");
		}
		else if ( obj == "BEAR" )
		{
			long bear = E("BEAR");
			if ( obj_state(bear) == state_index_c(bear, "SITTING_BEAR") )
				rspeak("BEAR_CHAINED");
			else
				rspeak("YOU_JOKING");
		}
		else if ( obj == "CHAIN" )
		{
			long bear = E("BEAR");
			if ( obj_state(bear) != state_index_c(bear, "UNTAMED_BEAR") )
				rspeak("STILL_LOCKED");
			else
				rspeak("YOU_JOKING");
		}
		else if ( obj == "RUG" )
		{
			if ( obj_state(o) == state_index_c(o, "RUG_HOVER") )
				rspeak("RUG_HOVERS");
			else
				rspeak("YOU_JOKING");
		}
		else if ( obj == "URN" )
			rspeak("URN_NOBUDGE");
		else if ( obj == "CAVITY" )
			rspeak("DOUGHNUT_HOLES");
		else if ( obj == "BLOOD" )
			rspeak("FEW_DROPS");
		else if ( obj == "SIGN" )
			rspeak("HAND_PASSTHROUGH");
		else
			rspeak("YOU_JOKING");
		return G_CLEAROBJ;
	}
	if ( obj == "WATER" || obj == "OIL" )
	{
		long bottle = E("BOTTLE");
		var lb;
		liquid_in_bottle(lb);
		if ( !here_obj(bottle) || lb != obj )
		{
			if ( !toting(bottle) )
			{
				rspeak("NO_CONTAINER");
				return G_CLEAROBJ;
			}
			if ( obj_state(bottle)
			     == state_index_c(bottle, "EMPTY_BOTTLE") )
				a8_wall("fill");
			rspeak("BOTTLE_FULL");
			return G_CLEAROBJ;
		}
		obj = "BOTTLE";
		o = bottle;
	}
	if ( HOLDING >= 7 )
	{
		rspeak("CARRY_LIMIT");
		return G_CLEAROBJ;
	}
	long bird = E("BIRD");
	if ( obj == "BIRD"
	  && obj_state(bird) != state_index_c(bird, "BIRD_CAGED") )
	{
		if ( obj_state(bird)
		     == state_index_c(bird, "BIRD_FOREST_UNCAGED") )
		{
			destroy_object(bird, "BIRD");
			rspeak("BIRD_CRAP");
			return G_CLEAROBJ;
		}
		if ( !toting(E("CAGE")) )
		{
			rspeak("CANNOT_CARRY");
			return G_CLEAROBJ;
		}
		if ( toting(E("ROD")) )
		{
			rspeak("BIRD_EVADES");
			return G_CLEAROBJ;
		}
		obj_set_state(bird, state_index_c(bird, "BIRD_CAGED"));
	}
	if ( (obj == "BIRD" || obj == "CAGE")
	  && obj_state(bird) == state_index_c(bird, "BIRD_CAGED") )
	{
		// bird and cage travel together
		if ( obj == "BIRD" )
			carry_object(E("CAGE"), "CAGE");
		else
			carry_object(bird, "BIRD");
	}
	carry_object(o, obj.c_str());
	if ( obj == "BOTTLE" )
	{
		var lb2;
		liquid_in_bottle(lb2);
		if ( lb2 != "" )
			ui::move(W, E(lb2.c_str()), PLAYER);
	}
	if ( is_gemstone(obj) && !obj_found(o) )
	{
		ui::set(W, o, "found", true);
		long cav = E("CAVITY");
		obj_set_state(cav, state_index_c(cav, "CAVITY_EMPTY"));
	}
	rspeak("OK_MAN");
	return G_CLEAROBJ;
}

// ---- drop ---------------------------------------------------------------

long h_discard(value &obj)
{
	long o = E(obj.c_str());
	if ( obj == "ROD" && !toting(o) && toting(E("ROD2")) )
	{
		obj = "ROD2";
		o = E("ROD2");
	}
	if ( !toting(o) )
	{
		speak_verb_default();
		return G_CLEAROBJ;
	}
	long cav = E("CAVITY");
	if ( is_gemstone(obj) && at_obj(cav)
	  && obj_state(cav) != state_index_c(cav, "CAVITY_FULL") )
	{
		rspeak("GEM_FITS");
		obj_set_state(o, 1);	// STATE_IN_CAVITY (gems have no labels)
		obj_set_state(cav, state_index_c(cav, "CAVITY_FULL"));
		if ( here_obj(E("RUG")) )
			a8_wall("rug hover");
		drop_object(o, obj.c_str(), LOC);
		return G_CLEAROBJ;
	}
	if ( obj == "COINS" && here_obj(E("VEND")) )
	{
		destroy_object(o, "COINS");
		long bat = E("BATTERY");
		drop_object(bat, "BATTERY", LOC);
		pspeak_look(bat, state_index_c(bat, "FRESH_BATTERIES"));
		return G_CLEAROBJ;
	}
	var lb;
	liquid_in_bottle(lb);
	if ( lb == obj )
	{
		obj = "BOTTLE";
		o = E("BOTTLE");
	}
	if ( obj == "BOTTLE" && lb != "" )
		ui::move(W, E(lb.c_str()), 0);
	if ( obj == "BEAR" && at_obj(E("TROLL")) )
		a8_wall("bear vs troll");
	if ( obj == "VASE" && LOC != static_place(E("PILLOW")) )
	{
		bool whole = at_obj(E("PILLOW"));
		if ( whole )
			state_change_c(o, "VASE_WHOLE");
		else
			state_change_c(o, "VASE_DROPPED");
		if ( !whole )
			obj_set_fixed(o, -1);
		drop_object(o, "VASE", LOC);
		return G_CLEAROBJ;
	}
	long bird = E("BIRD");
	if ( obj == "CAGE"
	  && obj_state(bird) == state_index_c(bird, "BIRD_CAGED") )
		drop_object(bird, "BIRD", LOC);
	if ( obj == "BIRD" )
	{
		long dragon = E("DRAGON");
		if ( at_obj(dragon)
		  && obj_state(dragon) == state_index_c(dragon, "DRAGON_BARS") )
		{
			rspeak("BIRD_BURNT");
			destroy_object(bird, "BIRD");
			return G_CLEAROBJ;
		}
		if ( here_obj(E("SNAKE")) )
		{
			rspeak("BIRD_ATTACKS");
			if ( CLOSED )
				return G_DWARFWAKE;
			long snake = E("SNAKE");
			destroy_object(snake, "SNAKE");
			obj_set_state(snake,
				      state_index_c(snake, "SNAKE_CHASED"));
		}
		else
			rspeak("OK_MAN");
		if ( loccond(LOC, "FOREST") )
			obj_set_state(bird,
				state_index_c(bird, "BIRD_FOREST_UNCAGED"));
		else
			obj_set_state(bird,
				state_index_c(bird, "BIRD_UNCAGED"));
		drop_object(bird, "BIRD", LOC);
		return G_CLEAROBJ;
	}
	rspeak("OK_MAN");
	drop_object(o, obj.c_str(), LOC);
	return G_CLEAROBJ;
}

// ---- lock / unlock ------------------------------------------------------

long h_lock(value &verb, value &obj)
{
	if ( obj == "" )
	{
		if ( here_obj(E("CLAM")) )
			obj = "CLAM";
		if ( here_obj(E("OYSTER")) )
			obj = "OYSTER";
		if ( at_obj(E("DOOR")) )
			obj = "DOOR";
		if ( at_obj(E("GRATE")) )
			obj = "GRATE";
		if ( here_obj(E("CHAIN")) )
			obj = "CHAIN";
		if ( obj == "" )
		{
			rspeak("NOTHING_LOCKED");
			return G_CLEAROBJ;
		}
	}
	long o = E(obj.c_str());
	if ( obj == "CHAIN" )
	{
		if ( here_obj(E("KEYS")) )
			a8_wall("bear chain");
		else
			rspeak("NO_KEYS");
		return G_CLEAROBJ;
	}
	if ( obj == "GRATE" )
	{
		if ( here_obj(E("KEYS")) )
		{
			if ( CLOSNG )
			{
				rspeak("EXIT_CLOSED");
				if ( !PANIC )
					CLOCK2 = 15;
				PANIC = true;
			}
			else if ( verb == "LOCK" )
				state_change_c(o, "GRATE_CLOSED");
			else
				state_change_c(o, "GRATE_OPEN");
		}
		else
			rspeak("NO_KEYS");
		return G_CLEAROBJ;
	}
	if ( obj == "CLAM" || obj == "OYSTER" )
	{
		bool oyster = obj == "OYSTER";
		if ( verb == "LOCK" )
			rspeak("HUH_MAN");
		else if ( toting(o) )
			rspeak(oyster ? "DROP_OYSTER" : "DROP_CLAM");
		else if ( !toting(E("TRIDENT")) )
			rspeak(oyster ? "OYSTER_OPENER" : "CLAM_OPENER");
		else if ( oyster )
			rspeak("OYSTER_OPENS");
		else
		{
			destroy_object(E("CLAM"), "CLAM");
			drop_object(E("OYSTER"), "OYSTER", LOC);
			drop_object(E("PEARL"), "PEARL", E("LOC_CULDESAC"));
			rspeak("PEARL_FALLS");
		}
		return G_CLEAROBJ;
	}
	if ( obj == "DOOR" )
	{
		long door = E("DOOR");
		if ( obj_state(door) == state_index_c(door, "DOOR_UNRUSTED") )
			rspeak("OK_MAN");
		else
			rspeak("RUSTY_DOOR");
		return G_CLEAROBJ;
	}
	if ( obj == "CAGE" )
	{
		rspeak("NO_LOCK");
		return G_CLEAROBJ;
	}
	if ( obj == "KEYS" )
	{
		rspeak("CANNOT_UNLOCK");
		return G_CLEAROBJ;
	}
	speak_verb_default();
	return G_CLEAROBJ;
}

// ---- light / extinguish -------------------------------------------------

long h_light(value &obj)
{
	long lamp = E("LAMP");
	long urn = E("URN");
	if ( obj == "" )
	{
		long selects = 0;
		if ( here_obj(lamp)
		  && obj_state(lamp) == state_index_c(lamp, "LAMP_DARK")
		  && LIMIT >= 0 )
		{
			obj = "LAMP";
			selects = selects + 1;
		}
		if ( here_obj(urn)
		  && obj_state(urn) == state_index_c(urn, "URN_DARK") )
		{
			obj = "URN";
			selects = selects + 1;
		}
		if ( selects != 1 )
			return G_UNKNOWN;
	}
	if ( obj == "URN" )
	{
		if ( obj_state(urn) == state_index_c(urn, "URN_EMPTY") )
			state_change_c(urn, "URN_EMPTY");
		else
			state_change_c(urn, "URN_LIT");
		return G_CLEAROBJ;
	}
	if ( obj == "LAMP" )
	{
		if ( LIMIT < 0 )
		{
			rspeak("LAMP_OUT");
			return G_CLEAROBJ;
		}
		state_change_c(lamp, "LAMP_BRIGHT");
		if ( WZDARK )
			return G_TOP;
		return G_CLEAROBJ;
	}
	speak_verb_default();
	return G_CLEAROBJ;
}

long h_extinguish(value &obj)
{
	long lamp = E("LAMP");
	long urn = E("URN");
	if ( obj == "" )
	{
		if ( here_obj(lamp)
		  && obj_state(lamp) == state_index_c(lamp, "LAMP_BRIGHT") )
			obj = "LAMP";
		if ( here_obj(urn)
		  && obj_state(urn) == state_index_c(urn, "URN_LIT") )
			obj = "URN";
		if ( obj == "" )
			return G_UNKNOWN;
	}
	if ( obj == "URN" )
	{
		if ( obj_state(urn) != state_index_c(urn, "URN_EMPTY") )
			state_change_c(urn, "URN_DARK");
		else
			pspeak_prop(urn, "changes",
				    state_index_c(urn, "URN_DARK"), true);
		return G_CLEAROBJ;
	}
	if ( obj == "LAMP" )
	{
		state_change_c(lamp, "LAMP_DARK");
		if ( is_dark() )
			rspeak("PITCH_DARK");
		return G_CLEAROBJ;
	}
	if ( obj == "DRAGON" || obj == "VOLCANO" )
	{
		rspeak("BEYOND_POWER");
		return G_CLEAROBJ;
	}
	speak_verb_default();
	return G_CLEAROBJ;
}

// ---- inventory / eat / drink ---------------------------------------------

long h_inven()
{
	bool empty = true;
	for ( value on : OBJ_NAMES )
	{
		if ( on == "BEAR" )
			continue;
		long o = E(on.c_str());
		if ( !o || !toting(o) )
			continue;
		if ( empty )
		{
			rspeak("NOW_HOLDING");
			empty = false;
		}
		pspeak_touch(o);
	}
	if ( toting(E("BEAR")) )
		rspeak("TAME_BEAR");
	if ( empty )
		rspeak("NO_CARRY");
	return G_CLEAROBJ;
}

long h_eat(value &obj)
{
	if ( obj == "" )
	{
		if ( !here_obj(E("FOOD")) )
			return G_UNKNOWN;
		obj = "FOOD";
	}
	if ( obj == "FOOD" )
	{
		destroy_object(E("FOOD"), "FOOD");
		rspeak("THANKS_DELICIOUS");
		return G_CLEAROBJ;
	}
	if ( obj == "BIRD" || obj == "SNAKE" || obj == "CLAM"
	  || obj == "OYSTER" || obj == "DWARF" || obj == "DRAGON"
	  || obj == "TROLL" || obj == "BEAR" || obj == "OGRE" )
	{
		rspeak("LOST_APPETITE");
		return G_CLEAROBJ;
	}
	speak_verb_default();
	return G_CLEAROBJ;
}

long h_drink(value &obj)
{
	var lq;
	liq_loc(lq, LOC);
	var lb;
	liquid_in_bottle(lb);
	long bottle = E("BOTTLE");
	if ( obj == "" && lq != "WATER"
	  && (lb != "WATER" || !here_obj(bottle)) )
		return G_UNKNOWN;
	if ( obj == "BLOOD" )
	{
		destroy_object(E("BLOOD"), "BLOOD");
		state_change_c(E("DRAGON"), "DRAGON_BLOODLESS");
		BLOODED = true;
		return G_CLEAROBJ;
	}
	if ( obj != "" && obj != "WATER" )
	{
		rspeak("RIDICULOUS_ATTEMPT");
		return G_CLEAROBJ;
	}
	if ( lb == "WATER" && here_obj(bottle) )
	{
		ui::move(W, E("WATER"), 0);
		state_change_c(bottle, "EMPTY_BOTTLE");
		return G_CLEAROBJ;
	}
	speak_verb_default();
	return G_CLEAROBJ;
}

// ---- the analyser + dispatch (the reference's action()) ------------------

long act_dispatch(long part)
{
	var verb = CMD["verb"];
	var obj = CMD["obj"];

	// Message-only verbs speak their message whatever the shape.
	var w0id = CMD["id0"];
	if ( php::array_key_exists(w0id.c_str(), NOACT) )
	{
		var dm;
		action_default(dm, w0id);
		speak_msg(dm);
		return G_CLEAROBJ;
	}

	if ( part == P_UNKNOWN )
	{
		// The object-word analysis: the thing must be here, with the
		// reference's exceptions (dwarves, liquids, urn oil, plant2,
		// vanishing knife, rod2, find/inventory of absent things).
		long o = E(obj.c_str());
		var lb;
		liquid_in_bottle(lb);
		var lq;
		liq_loc(lq, LOC);
		if ( here_obj(o) )
			;
		else if ( obj == "DWARF" && atdwrf(LOC) > 0 )
			;
		else if ( !CLOSED
		       && ((lb == obj && here_obj(E("BOTTLE"))) || lq == obj) )
			;
		else if ( obj == "OIL" && here_obj(E("URN"))
		       && obj_state(E("URN"))
			  != state_index_c(E("URN"), "URN_EMPTY") )
			obj = "URN";
		else if ( obj == "PLANT" && at_obj(E("PLANT2"))
		       && obj_state(E("PLANT2")) != 0 )
			obj = "PLANT2";
		else if ( obj == "KNIFE" && KNFLOC == LOC )
		{
			KNFLOC = -1;
			rspeak("KNIVES_VANISH");
			return G_CLEAROBJ;
		}
		else if ( obj == "ROD" && here_obj(E("ROD2")) )
			obj = "ROD2";
		else if ( (verb == "FIND" || verb == "INVENTORY")
		       && CMD["raw1"].length() == 0 )
			;
		else
		{
			var raw = CMD["raw0"];
			sspeak_s("NO_SEE", raw);
			return G_CLEAROBJ;
		}
		CMD["obj"] = obj;
		if ( verb != "" )
			part = P_TRANSITIVE;
	}

	if ( part == P_INTRANSITIVE )
	{
		var raw1 = CMD["raw1"];
		if ( raw1.length() > 0 && verb != "SAY" )
			return G_WORD2;
		if ( obj == "" )
		{
			// Intransitive verbs: deduce an object or hand back.
			if ( verb == "CARRY" )
			{
				var none = "";
				return h_vcarry(none);
			}
			if ( verb == "UNLOCK" || verb == "LOCK" )
			{
				var none = "";
				return h_lock(verb, none);
			}
			if ( verb == "LIGHT" )
			{
				var none = "";
				return h_light(none);
			}
			if ( verb == "EXTINGUISH" )
			{
				var none = "";
				return h_extinguish(none);
			}
			if ( verb == "EAT" )
			{
				var none = "";
				return h_eat(none);
			}
			if ( verb == "DRINK" )
			{
				var none = "";
				return h_drink(none);
			}
			if ( verb == "INVENTORY" )
				return h_inven();
			if ( verb == "NOTHING" )
			{
				rspeak("OK_MAN");
				return G_CLEAROBJ;
			}
			if ( verb == "GO" )
			{
				speak_verb_default();
				return G_CLEAROBJ;
			}
			if ( verb == "SEED" || verb == "WASTE" )
			{
				rspeak("NUMERIC_REQUIRED");
				return G_TOP;
			}
			if ( verb == "DROP" || verb == "SAY" || verb == "WAVE"
			  || verb == "TAME" || verb == "RUB" || verb == "THROW"
			  || verb == "FIND" || verb == "FEED" || verb == "BREAK"
			  || verb == "WAKE" )
				return G_UNKNOWN;
			fprintf(stderr,
				"advent: intransitive verb %s unimplemented\n",
				verb.c_str());
			exit(3);
		}
		part = P_TRANSITIVE;
	}

	if ( part == P_TRANSITIVE )
	{
		if ( verb == "CARRY" )
			return h_vcarry(obj);
		if ( verb == "DROP" || verb == "THROW" )
		{
			// THROW routes to discard for most objects (the
			// axe/dagger combat split is A7).
			if ( verb == "THROW" )
				a8_wall("throw");
			return h_discard(obj);
		}
		if ( verb == "UNLOCK" || verb == "LOCK" )
			return h_lock(verb, obj);
		if ( verb == "LIGHT" )
			return h_light(obj);
		if ( verb == "EXTINGUISH" )
			return h_extinguish(obj);
		if ( verb == "EAT" )
			return h_eat(obj);
		if ( verb == "DRINK" )
			return h_drink(obj);
		if ( verb == "INVENTORY" || verb == "FIND" )
		{
			// find/inventory-of: A6 keeps the loud wall until the
			// find quips land.
			if ( verb == "FIND" )
			{
				fprintf(stderr,
					"advent: find unimplemented (A6+)\n");
				exit(3);
			}
			return h_inven();
		}
		if ( verb == "NOTHING" )
		{
			rspeak("OK_MAN");
			return G_CLEAROBJ;
		}
		if ( verb == "GO" || verb == "TAME" )
		{
			speak_verb_default();
			return G_CLEAROBJ;
		}
		if ( verb == "SEED" )
		{
			var raw1 = CMD["raw1"];
			long sd = php::intval(raw1);
			var dm;
			action_default(dm, verb);
			speak_msg_d(dm, sd);
			rng_seed(sd);
			TURNS = TURNS - 1;	// seed is turn-neutral
			return G_TOP;
		}
		if ( verb == "WASTE" )
		{
			var raw1 = CMD["raw1"];
			LIMIT = LIMIT - php::intval(raw1);
			var dm;
			action_default(dm, verb);
			speak_msg_d(dm, LIMIT);
			return G_TOP;
		}
		fprintf(stderr, "advent: verb %s unimplemented\n",
			verb.c_str());
		exit(3);
	}

	// part unknown with no verb: ask what to do (the reference's tail).
	var raw = CMD["raw0"];
	sspeak_s("WHAT_DO", raw);
	return G_CHECKHINT;
}
