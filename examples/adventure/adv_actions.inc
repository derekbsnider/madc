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

// ---- fill (bottle / urn / vase) ------------------------------------------

long h_fill(value &obj)
{
	long bottle = E("BOTTLE");
	long urn = E("URN");
	var lb;
	liquid_in_bottle(lb);
	var lq;
	liq_loc(lq, LOC);
	if ( obj == "VASE" )
	{
		if ( lq == "" )
		{
			rspeak("FILL_INVALID");
			return G_CLEAROBJ;
		}
		if ( !toting(E("VASE")) )
		{
			rspeak("ARENT_CARRYING");
			return G_CLEAROBJ;
		}
		rspeak("SHATTER_VASE");
		long vase = E("VASE");
		obj_set_state(vase, state_index_c(vase, "VASE_BROKEN"));
		obj_set_fixed(vase, -1);
		drop_object(vase, "VASE", LOC);
		return G_CLEAROBJ;
	}
	if ( obj == "URN" )
	{
		if ( obj_state(urn) != state_index_c(urn, "URN_EMPTY") )
		{
			rspeak("FULL_URN");
			return G_CLEAROBJ;
		}
		if ( !here_obj(bottle) )
		{
			rspeak("FILL_INVALID");
			return G_CLEAROBJ;
		}
		if ( lb == "WATER" )
		{
			obj_set_state(bottle,
				state_index_c(bottle, "EMPTY_BOTTLE"));
			rspeak("WATER_URN");
		}
		else if ( lb == "OIL" )
		{
			obj_set_state(urn, state_index_c(urn, "URN_DARK"));
			obj_set_state(bottle,
				state_index_c(bottle, "EMPTY_BOTTLE"));
			rspeak("OIL_URN");
		}
		else
		{
			rspeak("FILL_INVALID");
			return G_CLEAROBJ;
		}
		ui::move(W, E(lb.c_str()), 0);
		return G_CLEAROBJ;
	}
	if ( obj != "" && obj != "BOTTLE" )
	{
		speak_verb_default();
		return G_CLEAROBJ;
	}
	if ( obj == "" && !here_obj(bottle) )
		return G_UNKNOWN;
	if ( here_obj(urn) && obj_state(urn)
	     != state_index_c(urn, "URN_EMPTY") )
	{
		rspeak("URN_NOPOUR");
		return G_CLEAROBJ;
	}
	if ( lb != "" )
	{
		rspeak("BOTTLE_FULL");
		return G_CLEAROBJ;
	}
	if ( lq == "" )
	{
		rspeak("NO_LIQUID");
		return G_CLEAROBJ;
	}
	if ( lq == "OIL" )
		state_change_c(bottle, "OIL_BOTTLE");
	else
		state_change_c(bottle, "WATER_BOTTLE");
	if ( toting(bottle) )
		ui::move(W, E(lq.c_str()), PLAYER);
	return G_CLEAROBJ;
}

// ---- carry --------------------------------------------------------------

long h_vcarry(value &obj)
{
	if ( obj == "" )
	{
		var only;
		long n = objects_at_loc(only, LOC);
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
			{
				var bo = "BOTTLE";
				return h_fill(bo);
			}
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
		long rug = E("RUG");
		long hover = state_index_c(rug, "RUG_HOVER");
		if ( here_obj(rug)
		  && ((obj == "EMERALD" && obj_state(rug) != hover)
	           || (obj == "RUBY" && obj_state(rug) == hover)) )
		{
			if ( obj == "RUBY" )
				rspeak("RUG_SETTLES");
			else if ( toting(rug) )
				rspeak("RUG_WIGGLES");
			else
				rspeak("RUG_RISES");
			if ( !toting(rug) || obj == "RUBY" )
			{
				if ( obj_state(rug) != hover )
				{
					obj_set_state(rug, hover);
					move_fixed_side("RUG",
						static_place(E("SAPPH")));
				}
				else
				{
					obj_set_state(rug,
						state_index_c(rug, "RUG_FLOOR"));
					move_fixed_side("RUG", 0);
				}
			}
		}
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

// ---- juggle: re-front both sides of an object in the listing order ------

void juggle_object(long o, const char *name)
{
	move_object(o, ui::location(W, o));
	if ( php::array_key_exists(name, FIXEDSIDES) )
		move_fixed_side(name, fixed_side_loc(name));
}

// ---- attack ---------------------------------------------------------------

long h_attack(value &obj)
{
	var verb = CMD["verb"];
	if ( obj == "" )
	{
		long changes = 0;
		if ( atdwrf(LOC) > 0 )
		{
			obj = "DWARF";
			changes = changes + 1;
		}
		if ( here_obj(E("SNAKE")) )
		{
			obj = "SNAKE";
			changes = changes + 1;
		}
		long dragon = E("DRAGON");
		if ( at_obj(dragon)
		  && obj_state(dragon) == state_index_c(dragon, "DRAGON_BARS") )
		{
			obj = "DRAGON";
			changes = changes + 1;
		}
		if ( at_obj(E("TROLL")) )
		{
			obj = "TROLL";
			changes = changes + 1;
		}
		if ( at_obj(E("OGRE")) )
		{
			obj = "OGRE";
			changes = changes + 1;
		}
		long bear = E("BEAR");
		if ( here_obj(bear)
		  && obj_state(bear) == state_index_c(bear, "UNTAMED_BEAR") )
		{
			obj = "BEAR";
			changes = changes + 1;
		}
		if ( obj == "" )
		{
			if ( here_obj(E("BIRD")) && verb != "THROW" )
			{
				obj = "BIRD";
				changes = changes + 1;
			}
			if ( here_obj(E("VEND")) && verb != "THROW" )
			{
				obj = "VEND";
				changes = changes + 1;
			}
			if ( here_obj(E("CLAM")) || here_obj(E("OYSTER")) )
			{
				obj = "CLAM";
				changes = changes + 1;
			}
		}
		if ( changes >= 2 )
			return G_UNKNOWN;
	}
	if ( obj == "BIRD" )
	{
		if ( CLOSED )
			rspeak("UNHAPPY_BIRD");
		else
		{
			destroy_object(E("BIRD"), "BIRD");
			rspeak("BIRD_DEAD");
		}
		return G_CLEAROBJ;
	}
	if ( obj == "VEND" )
	{
		long vend = E("VEND");
		if ( obj_state(vend) == state_index_c(vend, "VEND_BLOCKS") )
			state_change_c(vend, "VEND_UNBLOCKS");
		else
			state_change_c(vend, "VEND_BLOCKS");
		return G_CLEAROBJ;
	}
	if ( obj == "BEAR" )
	{
		long bear = E("BEAR");
		long st = obj_state(bear);
		if ( st == state_index_c(bear, "UNTAMED_BEAR") )
			rspeak("BEAR_HANDS");
		else if ( st == state_index_c(bear, "BEAR_DEAD") )
			rspeak("ALREADY_DEAD");
		else
			rspeak("BEAR_CONFUSED");
		return G_CLEAROBJ;
	}
	long dragon = E("DRAGON");
	if ( obj == "DRAGON"
	  && obj_state(dragon) == state_index_c(dragon, "DRAGON_BARS") )
	{
		// Bare hands vs dragon: insist, and win.
		rspeak("BARE_HANDS_QUERY");
		if ( !silent_ask() )
		{
			var nd;
			msg_of(nd, "NASTY_DRAGON");
			speak_msg(nd);
			return G_MOVE;
		}
		state_change_c(dragon, "DRAGON_DEAD");
		long rug = E("RUG");
		obj_set_state(rug, state_index_c(rug, "RUG_FLOOR"));
		long secret5 = E("LOC_SECRET5");
		long dplac = static_place(dragon);
		long dfixd = fixed_side_loc("DRAGON");
		move_fixed_side("DRAGON", 0);
		obj_set_fixed(dragon, -1);
		move_fixed_side("RUG", 0);
		obj_set_fixed(rug, 0);	// the rug becomes takeable
		move_object(dragon, secret5);
		move_object(rug, secret5);
		drop_object(E("BLOOD"), "BLOOD", secret5);
		for ( value on : OBJ_NAMES )
		{
			long oo = E(on.c_str());
			if ( !oo )
				continue;
			long pl = ui::location(W, oo);
			if ( pl != 0 && (pl == dplac || pl == dfixd)
			  && oo != dragon && oo != rug )
				move_object(oo, secret5);
		}
		LOC = secret5;
		ui::move(W, PLAYER, LOC);
		return G_MOVE;
	}
	if ( obj == "OGRE" )
	{
		rspeak("OGRE_DODGE");
		if ( atdwrf(LOC) == 0 )
			return G_CLEAROBJ;
		a8_wall("ogre vs dwarves");
	}
	if ( obj == "" )
	{
		rspeak("NO_TARGET");
		return G_CLEAROBJ;
	}
	if ( obj == "CLAM" || obj == "OYSTER" )
	{
		rspeak("SHELL_IMPERVIOUS");
		return G_CLEAROBJ;
	}
	if ( obj == "SNAKE" )
	{
		rspeak("SNAKE_WARNING");
		return G_CLEAROBJ;
	}
	if ( obj == "DWARF" )
	{
		if ( CLOSED )
			return G_DWARFWAKE;
		rspeak("BARE_HANDS_QUERY");
		return G_CLEAROBJ;
	}
	if ( obj == "DRAGON" )
	{
		rspeak("ALREADY_DEAD");
		return G_CLEAROBJ;
	}
	if ( obj == "TROLL" )
	{
		rspeak("ROCKY_TROLL");
		return G_CLEAROBJ;
	}
	speak_verb_default();
	return G_CLEAROBJ;
}

// ---- feed -----------------------------------------------------------------

long h_feed(value &obj)
{
	if ( obj == "BIRD" )
	{
		rspeak("BIRD_PINING");
		return G_CLEAROBJ;
	}
	if ( obj == "DRAGON" )
	{
		long dragon = E("DRAGON");
		if ( obj_state(dragon)
		     != state_index_c(dragon, "DRAGON_BARS") )
			rspeak("RIDICULOUS_ATTEMPT");
		else
			rspeak("NOTHING_EDIBLE");
		return G_CLEAROBJ;
	}
	if ( obj == "SNAKE" )
	{
		if ( !CLOSED && here_obj(E("BIRD")) )
		{
			destroy_object(E("BIRD"), "BIRD");
			rspeak("BIRD_DEVOURED");
		}
		else
			rspeak("NOTHING_EDIBLE");
		return G_CLEAROBJ;
	}
	if ( obj == "TROLL" )
	{
		rspeak("TROLL_VICES");
		return G_CLEAROBJ;
	}
	if ( obj == "DWARF" )
	{
		if ( here_obj(E("FOOD")) )
		{
			DFLAG = DFLAG + 2;
			rspeak("REALLY_MAD");
		}
		else
			speak_verb_default();
		return G_CLEAROBJ;
	}
	if ( obj == "BEAR" )
	{
		long bear = E("BEAR");
		long st = obj_state(bear);
		if ( st == state_index_c(bear, "BEAR_DEAD") )
		{
			rspeak("RIDICULOUS_ATTEMPT");
			return G_CLEAROBJ;
		}
		if ( st == state_index_c(bear, "UNTAMED_BEAR") )
		{
			if ( here_obj(E("FOOD")) )
			{
				destroy_object(E("FOOD"), "FOOD");
				long axe = E("AXE");
				obj_set_fixed(axe, 0);
				obj_set_state(axe,
					state_index_c(axe, "AXE_HERE"));
				state_change_c(bear, "SITTING_BEAR");
			}
			else
				rspeak("NOTHING_EDIBLE");
			return G_CLEAROBJ;
		}
		speak_verb_default();
		return G_CLEAROBJ;
	}
	if ( obj == "OGRE" )
	{
		if ( here_obj(E("FOOD")) )
			rspeak("OGRE_FULL");
		else
			speak_verb_default();
		return G_CLEAROBJ;
	}
	rspeak("AM_GAME");
	return G_CLEAROBJ;
}

// ---- throw ----------------------------------------------------------------

long throw_support(const char *msg)
{
	rspeak(msg);
	drop_object(E("AXE"), "AXE", LOC);
	return G_MOVE;
}

long h_throw(value &obj)
{
	long o = E(obj.c_str());
	if ( !toting(o) )
	{
		speak_verb_default();
		return G_CLEAROBJ;
	}
	if ( obj_treasure(o) && at_obj(E("TROLL")) )
	{
		// Snarf a treasure for the troll.
		drop_object(o, obj.c_str(), 0);
		long troll = E("TROLL");
		move_object(troll, 0);
		move_fixed_side("TROLL", 0);
		obj_set_fixed(troll, 0);
		long troll2 = E("TROLL2");
		drop_object(troll2, "TROLL2", static_place(troll));
		move_fixed_side("TROLL2", static_fixed(troll));
		juggle_object(E("CHASM"), "CHASM");
		rspeak("TROLL_SATISFIED");
		return G_CLEAROBJ;
	}
	if ( obj == "FOOD" && here_obj(E("BEAR")) )
	{
		var bear = "BEAR";
		return h_feed(bear);
	}
	if ( obj != "AXE" )
		return h_discard(obj);
	if ( atdwrf(LOC) <= 0 )
	{
		long dragon = E("DRAGON");
		if ( at_obj(dragon)
		  && obj_state(dragon) == state_index_c(dragon, "DRAGON_BARS") )
			return throw_support("DRAGON_SCALES");
		if ( at_obj(E("TROLL")) )
			return throw_support("TROLL_RETURNS");
		if ( at_obj(E("OGRE")) )
			return throw_support("OGRE_DODGE");
		long bear = E("BEAR");
		if ( here_obj(bear)
		  && obj_state(bear) == state_index_c(bear, "UNTAMED_BEAR") )
		{
			// This'll teach him to throw the axe at the bear!
			long axe = E("AXE");
			drop_object(axe, "AXE", LOC);
			obj_set_fixed(axe, -1);
			juggle_object(bear, "BEAR");
			state_change_c(axe, "AXE_LOST");
			return G_CLEAROBJ;
		}
		CMD["obj"] = "";
		var none = "";
		return h_attack(none);
	}
	// Axe vs dwarf: the only way to kill one.
	if ( randrange(7) < DFLAG )
		return throw_support("DWARF_DODGES");
	long di = atdwrf(LOC);
	var dn = "dwarf";
	dn += to_string(di).c_str();
	long dw = E(dn.c_str());
	ui::set(W, dw, "seen", false);
	ui::move(W, dw, 0);
	DKILL = DKILL + 1;
	if ( DKILL == 1 )
		return throw_support("DWARF_SMOKE");
	return throw_support("KILLED_DWARF");
}

// ---- wave / rub / magic ----------------------------------------------------

long h_wave(value &obj)
{
	long o = E(obj.c_str());
	long bird = E("BIRD");
	long fissure = E("FISSURE");
	if ( obj != "ROD" || !toting(o)
	  || (!here_obj(bird) && (CLOSNG || !at_obj(fissure))) )
	{
		if ( !toting(o) && (obj != "ROD" || !toting(E("ROD2"))) )
			rspeak("ARENT_CARRYING");
		else
			speak_verb_default();
		return G_CLEAROBJ;
	}
	long jade = E("JADE");
	if ( obj_state(bird) == state_index_c(bird, "BIRD_UNCAGED")
	  && LOC == ui::location(W, E("STEPS")) && !obj_found(jade) )
	{
		drop_object(jade, "JADE", LOC);
		ui::set(W, jade, "found", true);
		TALLY = TALLY - 1;
		rspeak("NECKLACE_FLY");
		return G_CLEAROBJ;
	}
	bool caged = obj_state(bird) == state_index_c(bird, "BIRD_CAGED");
	if ( CLOSED )
	{
		rspeak(caged ? "CAGE_FLY" : "FREE_FLY");
		return G_DWARFWAKE;
	}
	if ( CLOSNG || !at_obj(fissure) )
	{
		rspeak(caged ? "CAGE_FLY" : "FREE_FLY");
		return G_CLEAROBJ;
	}
	if ( here_obj(bird) )
		rspeak(caged ? "CAGE_FLY" : "FREE_FLY");
	if ( obj_state(fissure) == state_index_c(fissure, "BRIDGED") )
		state_change_c(fissure, "UNBRIDGED");
	else
		state_change_c(fissure, "BRIDGED");
	return G_CLEAROBJ;
}

long h_rub(value &obj)
{
	long urn = E("URN");
	if ( obj == "URN"
	  && obj_state(urn) == state_index_c(urn, "URN_LIT") )
	{
		destroy_object(urn, "URN");
		long amber = E("AMBER");
		drop_object(amber, "AMBER", LOC);
		obj_set_state(amber, state_index_c(amber, "AMBER_IN_ROCK"));
		TALLY = TALLY - 1;
		drop_object(E("CAVITY"), "CAVITY", LOC);
		rspeak("URN_GENIES");
	}
	else if ( obj != "LAMP" )
		rspeak("PECULIAR_NOTHING");
	else
		speak_verb_default();
	return G_CLEAROBJ;
}

long h_reservoir()
{
	long reser = E("RESER");
	if ( !at_obj(reser) && LOC != E("LOC_RESBOTTOM") )
	{
		rspeak("NOTHING_HAPPENS");
		return G_CLEAROBJ;
	}
	if ( obj_state(reser) == state_index_c(reser, "WATERS_PARTED") )
		state_change_c(reser, "WATERS_UNPARTED");
	else
		state_change_c(reser, "WATERS_PARTED");
	if ( at_obj(reser) )
		return G_CLEAROBJ;
	OLDLC2 = LOC;
	NEWLOC = 0;
	rspeak("NOT_BRIGHT");
	return G_TERMINATE;
}

long foo_stage(value &id)
{
	if ( id == "FEE" )
		return 1;
	if ( id == "FIE" )
		return 2;
	if ( id == "FOE" )
		return 3;
	if ( id == "FOO" )
		return 4;
	if ( id == "FUM" )
		return 5;
	return 0;
}

long h_bigwords(value &id)
{
	long stage = foo_stage(id);
	long fb = FOOBAR;
	if ( fb < 0 )
		fb = -fb;
	if ( fb == 0 && stage >= 2 )
	{
		rspeak("NOTHING_HAPPENS");
		return G_CLEAROBJ;
	}
	if ( (fb == 0 && stage == 1) || (fb + 1 == stage && stage <= 4) )
	{
		FOOBAR = stage;
		if ( stage != 4 )
		{
			rspeak("OK_MAN");
			return G_CLEAROBJ;
		}
		FOOBAR = 0;
		long eggs = E("EGGS");
		long eplac = static_place(eggs);
		if ( ui::location(W, eggs) == eplac
		  || (toting(eggs) && LOC == eplac) )
		{
			rspeak("NOTHING_HAPPENS");
			return G_CLEAROBJ;
		}
		// Bring back the troll if the eggs were his toll.
		long troll = E("TROLL");
		if ( ui::location(W, eggs) == 0
		  && ui::location(W, troll) == 0
		  && obj_state(troll)
		     == state_index_c(troll, "TROLL_UNPAID") )
			obj_set_state(troll,
				state_index_c(troll, "TROLL_PAIDONCE"));
		if ( here_obj(eggs) )
			pspeak_look(eggs,
				state_index_c(eggs, "EGGS_VANISHED"));
		else if ( LOC == eplac )
			pspeak_look(eggs, state_index_c(eggs, "EGGS_HERE"));
		else
			pspeak_look(eggs, state_index_c(eggs, "EGGS_DONE"));
		if ( toting(eggs) )
			drop_object(eggs, "EGGS", eplac);
		else
			move_object(eggs, eplac);
		return G_CLEAROBJ;
	}
	// Started but wrong.
	if ( SEENBIGWORDS )
		rspeak("START_OVER");
	else
		rspeak("WELL_POINTLESS");
	FOOBAR = 0;
	return G_CLEAROBJ;
}

long h_say()
{
	long t1 = CMD["type1"].as_integer();
	var id1 = CMD["id1"];
	if ( t1 == T_MOTION
	  && (id1 == "XYZZY" || id1 == "PLUGH" || id1 == "PLOVER") )
		return G_WORD2;
	if ( t1 == T_ACTION && id1 == "PART" )
		return h_reservoir();
	if ( t1 == T_ACTION
	  && (id1 == "FEE" || id1 == "FIE" || id1 == "FOE"
	   || id1 == "FOO" || id1 == "FUM" || id1 == "PART") )
		return h_bigwords(id1);
	var raw1 = CMD["raw1"];
	sspeak_s("OKEY_DOKEY", raw1);
	return G_CLEAROBJ;
}

// ---- read / quit / blast / break / brief / listen --------------------------

long h_read(value &obj)
{
	if ( obj == "" )
	{
		long matches = 0;
		var resolved;
		for ( value on : OBJ_NAMES )
		{
			long oo = E(on.c_str());
			if ( !oo || !here_obj(oo) )
				continue;
			var tx;
			ui::get(tx, W, oo, "texts");
			if ( tx.is_null() || tx.count() == 0 )
				continue;
			matches = matches + 1;
			resolved = on;
		}
		if ( matches != 1 || is_dark() )
			return G_UNKNOWN;
		obj = resolved;
	}
	if ( is_dark() )
	{
		var raw = CMD["raw0"];
		sspeak_s("NO_SEE", raw);
		return G_CLEAROBJ;
	}
	long o = E(obj.c_str());
	if ( obj == "OYSTER" )
	{
		if ( !toting(o) || !CLOSED )
			rspeak("DONT_UNDERSTAND");
		else if ( !CLSHNT )
			CLSHNT = ask_yes_no("CLUE_QUERY", "WAYOUT_CLUE",
					    "OK_MAN");
		else
			pspeak_prop(o, "sounds", 1, true);
		return G_CLEAROBJ;
	}
	var tx;
	ui::get(tx, W, o, "texts");
	if ( tx.is_null() || tx.count() == 0 || !obj_found(o) )
	{
		speak_verb_default();
		return G_CLEAROBJ;
	}
	pspeak_prop(o, "texts", obj_state(o), true);
	return G_CLEAROBJ;
}

long h_quit()
{
	if ( ask_yes_no("REALLY_QUIT", "OK_MAN", "OK_MAN") )
		terminate_game(M_QUIT);
	return G_CLEAROBJ;
}

long h_blast()
{
	long rod2 = E("ROD2");
	if ( !obj_found(rod2) || !CLOSED )
	{
		rspeak("REQUIRES_DYNAMITE");
		return G_CLEAROBJ;
	}
	if ( here_obj(rod2) )
	{
		BONUS = 1;
		rspeak("SPLATTER_MESSAGE");
	}
	else if ( LOC == E("LOC_NE") )
	{
		BONUS = 2;
		rspeak("DEFEAT_MESSAGE");
	}
	else
	{
		BONUS = 3;
		rspeak("VICTORY_MESSAGE");
	}
	terminate_game(M_ENDGAME);
	return G_CLEAROBJ;
}

long h_vbreak(value &obj)
{
	if ( obj == "MIRROR" )
	{
		if ( CLOSED )
		{
			state_change_c(E("MIRROR"), "MIRROR_BROKEN");
			return G_DWARFWAKE;
		}
		rspeak("TOO_FAR");
		return G_CLEAROBJ;
	}
	long vase = E("VASE");
	if ( obj == "VASE"
	  && obj_state(vase) == state_index_c(vase, "VASE_WHOLE") )
	{
		if ( toting(vase) )
			drop_object(vase, "VASE", LOC);
		state_change_c(vase, "VASE_BROKEN");
		obj_set_fixed(vase, -1);
		return G_CLEAROBJ;
	}
	speak_verb_default();
	return G_CLEAROBJ;
}

long h_brief()
{
	ABBNUM = 10000;
	DETAIL = 3;
	rspeak("BRIEF_CONFIRM");
	return G_CLEAROBJ;
}

long h_listen()
{
	bool heard = false;
	var snd;
	ui::get(snd, W, LOC, "sound");
	if ( !snd.is_null() && snd.length() > 0 )
	{
		rspeak(snd.c_str());
		heard = true;
	}
	for ( value on : OBJ_NAMES )
	{
		long o = E(on.c_str());
		if ( !o || !here_obj(o) || !obj_found(o) )
			continue;
		var snds;
		ui::get(snds, W, o, "sounds");
		if ( snds.is_null() || snds.count() == 0 )
			continue;
		long mi = obj_state(o);
		if ( on == "BIRD" && BLOODED )
			mi = mi + 3;
		var st = ZZWORD;
		// The bird sounds carry the reservoir word (%s).
		var tx;
		long i2 = 0;
		for ( value d : snds )
		{
			if ( i2 == mi )
			{
				if ( !d.is_null() && d.length() > 0 )
				{
					var r;
					var nod;
					render_msg(r, d, nod, st);
					speak_rendered(r);
				}
			}
			i2 = i2 + 1;
		}
		if ( on == "BIRD" && mi == 5 )
			destroy_object(o, "BIRD");
		heard = true;
	}
	if ( !heard )
		rspeak("ALL_SILENT");
	return G_CLEAROBJ;
}

// ---- pour / find / fly / wake ----------------------------------------------

long h_pour(value &obj)
{
	if ( obj == "BOTTLE" || obj == "" )
	{
		var lb;
		liquid_in_bottle(lb);
		obj = lb;
		if ( obj == "" )
			return G_UNKNOWN;
	}
	long o = E(obj.c_str());
	if ( !toting(o) )
	{
		speak_verb_default();
		return G_CLEAROBJ;
	}
	if ( obj != "OIL" && obj != "WATER" )
	{
		rspeak("CANT_POUR");
		return G_CLEAROBJ;
	}
	long urn = E("URN");
	if ( here_obj(urn)
	  && obj_state(urn) == state_index_c(urn, "URN_EMPTY") )
	{
		var uo = "URN";
		return h_fill(uo);
	}
	long bottle = E("BOTTLE");
	obj_set_state(bottle, state_index_c(bottle, "EMPTY_BOTTLE"));
	ui::move(W, o, 0);
	if ( !(at_obj(E("PLANT")) || at_obj(E("DOOR"))) )
	{
		rspeak("GROUND_WET");
		return G_CLEAROBJ;
	}
	if ( !at_obj(E("DOOR")) )
	{
		if ( obj == "WATER" )
		{
			// cycle the plant through its three states
			long plant = E("PLANT");
			long ns = (obj_state(plant) + 1) % 3;
			state_change(plant, ns);
			obj_set_state(E("PLANT2"), ns);
			return G_MOVE;
		}
		rspeak("SHAKING_LEAVES");
		return G_CLEAROBJ;
	}
	long door = E("DOOR");
	if ( obj == "OIL" )
		state_change_c(door, "DOOR_UNRUSTED");
	else
		state_change_c(door, "DOOR_RUSTED");
	return G_CLEAROBJ;
}

long h_find(value &obj)
{
	long o = E(obj.c_str());
	if ( toting(o) )
	{
		rspeak("ALREADY_CARRYING");
		return G_CLEAROBJ;
	}
	if ( CLOSED )
	{
		rspeak("NEEDED_NEARBY");
		return G_CLEAROBJ;
	}
	var lb;
	liquid_in_bottle(lb);
	var lq;
	liq_loc(lq, LOC);
	if ( at_obj(o) || (lb == obj && at_obj(E("BOTTLE"))) || lq == obj
	  || (obj == "DWARF" && atdwrf(LOC) > 0) )
	{
		rspeak("YOU_HAVEIT");
		return G_CLEAROBJ;
	}
	speak_verb_default();
	return G_CLEAROBJ;
}

long h_fly(value &obj)
{
	long rug = E("RUG");
	if ( obj == "" )
	{
		if ( !here_obj(rug) )
		{
			rspeak("FLAP_ARMS");
			return G_CLEAROBJ;
		}
		if ( obj_state(rug) != state_index_c(rug, "RUG_HOVER") )
		{
			rspeak("RUG_NOTHING2");
			return G_CLEAROBJ;
		}
		obj = "RUG";
	}
	if ( obj != "RUG" )
	{
		speak_verb_default();
		return G_CLEAROBJ;
	}
	if ( obj_state(rug) != state_index_c(rug, "RUG_HOVER") )
	{
		rspeak("RUG_NOTHING1");
		return G_CLEAROBJ;
	}
	if ( LOC == E("LOC_CLIFF") )
	{
		OLDLC2 = OLDLOC;
		OLDLOC = LOC;
		NEWLOC = E("LOC_LEDGE");
		rspeak("RUG_GOES");
	}
	else if ( LOC == E("LOC_LEDGE") )
	{
		OLDLC2 = OLDLOC;
		OLDLOC = LOC;
		NEWLOC = E("LOC_CLIFF");
		rspeak("RUG_RETURNS");
	}
	else
		rspeak("NOTHING_HAPPENS");
	return G_TERMINATE;
}

long h_wake(value &obj)
{
	if ( obj != "DWARF" || !CLOSED )
	{
		speak_verb_default();
		return G_CLEAROBJ;
	}
	rspeak("PROD_DWARF");
	return G_DWARFWAKE;
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
			var none = "";
			if ( verb == "CARRY" )
				return h_vcarry(none);
			if ( verb == "UNLOCK" || verb == "LOCK" )
				return h_lock(verb, none);
			if ( verb == "LIGHT" )
				return h_light(none);
			if ( verb == "EXTINGUISH" )
				return h_extinguish(none);
			if ( verb == "EAT" )
				return h_eat(none);
			if ( verb == "DRINK" )
				return h_drink(none);
			if ( verb == "POUR" )
				return h_pour(none);
			if ( verb == "FILL" )
				return h_fill(none);
			if ( verb == "ATTACK" )
			{
				CMD["obj"] = "";
				return h_attack(none);
			}
			if ( verb == "INVENTORY" )
				return h_inven();
			if ( verb == "SAY" )
			{
				var raw1b = CMD["raw1"];
				if ( raw1b.length() > 0 )
					return h_say();
				return G_UNKNOWN;
			}
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
			if ( verb == "QUIT" )
				return h_quit();
			if ( verb == "SCORE" )
			{
				score_game(M_SCOREGAME);
				return G_CLEAROBJ;
			}
			if ( verb == "FEE" || verb == "FIE" || verb == "FOE"
			  || verb == "FOO" || verb == "FUM" )
				return h_bigwords(verb);
			if ( verb == "BRIEF" )
				return h_brief();
			if ( verb == "READ" )
				return h_read(none);
			if ( verb == "BLAST" )
				return h_blast();
			if ( verb == "FLY" )
				return h_fly(none);
			if ( verb == "LISTEN" )
				return h_listen();
			if ( verb == "PART" )
				return h_reservoir();
			if ( verb == "SEED" || verb == "WASTE" )
			{
				rspeak("NUMERIC_REQUIRED");
				return G_TOP;
			}
			if ( verb == "SAVE" || verb == "RESUME" )
			{
				// Binary save is out of scope (our save is
				// the .world round-trip); the logs using it
				// are the corpus's excluded class.
				fprintf(stderr,
					"advent: save/resume out of scope\n");
				exit(3);
			}
			if ( verb == "DROP" || verb == "WAVE" || verb == "TAME"
			  || verb == "RUB" || verb == "THROW" || verb == "FIND"
			  || verb == "FEED" || verb == "BREAK"
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
		if ( verb == "DROP" )
			return h_discard(obj);
		if ( verb == "THROW" )
			return h_throw(obj);
		if ( verb == "SAY" )
			return h_say();
		if ( verb == "UNLOCK" || verb == "LOCK" )
			return h_lock(verb, obj);
		if ( verb == "LIGHT" )
			return h_light(obj);
		if ( verb == "EXTINGUISH" )
			return h_extinguish(obj);
		if ( verb == "WAVE" )
			return h_wave(obj);
		if ( verb == "ATTACK" )
			return h_attack(obj);
		if ( verb == "POUR" )
			return h_pour(obj);
		if ( verb == "EAT" )
			return h_eat(obj);
		if ( verb == "DRINK" )
			return h_drink(obj);
		if ( verb == "RUB" )
			return h_rub(obj);
		if ( verb == "FIND" || verb == "INVENTORY" )
			return h_find(obj);
		if ( verb == "FEED" )
			return h_feed(obj);
		if ( verb == "FILL" )
			return h_fill(obj);
		if ( verb == "READ" )
			return h_read(obj);
		if ( verb == "BREAK" )
			return h_vbreak(obj);
		if ( verb == "WAKE" )
			return h_wake(obj);
		if ( verb == "FLY" )
			return h_fly(obj);
		if ( verb == "QUIT" )
			return h_quit();
		if ( verb == "SCORE" )
		{
			score_game(M_SCOREGAME);
			return G_CLEAROBJ;
		}
		if ( verb == "BLAST" )
			return h_blast();
		if ( verb == "LISTEN" )
		{
			// The reference's transitive LISTEN only quips.
			speak_verb_default();
			return G_CLEAROBJ;
		}
		if ( verb == "PART" )
			return h_reservoir();
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
