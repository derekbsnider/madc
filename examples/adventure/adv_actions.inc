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

void action_default(var &out, var &verb)
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

// Speak one message and finish the command — the file's most common
// exit shape.
long quip(const char *msg)
{
	rspeak(msg);
	return G_CLEAROBJ;
}

long verb_quip()
{
	speak_verb_default();
	return G_CLEAROBJ;
}

// Is x one of the SPACE-PADDED, space-delimited words? (" A B C ")
bool one_of(var &x, const char *words)
{
	return strstr(words, format(" {} ", x.c_str()).c_str()) != NULL;
}

bool is_gemstone(var &obj)
{
	return one_of(obj, " EMERALD RUBY AMBER SAPPH ");
}

// Why a fixed object won't budge — four state-conditional quips, four
// constant ones, YOU_JOKING otherwise.
const char *fixed_quip(var &obj, long o)
{
	if ( obj == "PLANT" )
		return obj_state(o) == 0 || obj_stashed(o)
		       ? "DEEP_ROOTS" : "YOU_JOKING";
	if ( obj == "BEAR" )
		return obj_state(o) == state_index_c(o, "SITTING_BEAR")
		       ? "BEAR_CHAINED" : "YOU_JOKING";
	if ( obj == "CHAIN" )
	{
		long bear = E("BEAR");
		return obj_state(bear) != state_index_c(bear, "UNTAMED_BEAR")
		       ? "STILL_LOCKED" : "YOU_JOKING";
	}
	if ( obj == "RUG" )
		return obj_state(o) == state_index_c(o, "RUG_HOVER")
		       ? "RUG_HOVERS" : "YOU_JOKING";
	if ( obj == "URN" )
		return "URN_NOBUDGE";
	if ( obj == "CAVITY" )
		return "DOUGHNUT_HOLES";
	if ( obj == "BLOOD" )
		return "FEW_DROPS";
	if ( obj == "SIGN" )
		return "HAND_PASSTHROUGH";
	return "YOU_JOKING";
}

// ---- fill (bottle / urn / vase) ------------------------------------------

long h_fill(var &obj)
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
			return quip("FILL_INVALID");
		if ( !toting(E("VASE")) )
			return quip("ARENT_CARRYING");
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
			return quip("FULL_URN");
		if ( !here_obj(bottle) )
			return quip("FILL_INVALID");
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
			return quip("FILL_INVALID");
		ui::move(W, E(lb.c_str()), 0);
		return G_CLEAROBJ;
	}
	if ( obj != "" && obj != "BOTTLE" )
		return verb_quip();
	if ( obj == "" && !here_obj(bottle) )
		return G_UNKNOWN;
	if ( here_obj(urn) && obj_state(urn)
	     != state_index_c(urn, "URN_EMPTY") )
		return quip("URN_NOPOUR");
	if ( lb != "" )
		return quip("BOTTLE_FULL");
	if ( lq == "" )
		return quip("NO_LIQUID");
	if ( lq == "OIL" )
		state_change_c(bottle, "OIL_BOTTLE");
	else
		state_change_c(bottle, "WATER_BOTTLE");
	if ( toting(bottle) )
		ui::move(W, E(lq.c_str()), PLAYER);
	return G_CLEAROBJ;
}

// ---- carry --------------------------------------------------------------

long h_vcarry(var &obj)
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
		return verb_quip();
	if ( obj == "MESSAG" )
	{
		rspeak("REMOVE_MESSAGE");
		destroy_object(o, "MESSAG");
		return G_CLEAROBJ;
	}
	if ( obj_is_fixed(o) )
		return quip(fixed_quip(obj, o));
	if ( obj == "WATER" || obj == "OIL" )
	{
		long bottle = E("BOTTLE");
		var lb;
		liquid_in_bottle(lb);
		if ( !here_obj(bottle) || lb != obj )
		{
			if ( !toting(bottle) )
				return quip("NO_CONTAINER");
			if ( obj_state(bottle)
			     == state_index_c(bottle, "EMPTY_BOTTLE") )
			{
				var bo = "BOTTLE";
				return h_fill(bo);
			}
			return quip("BOTTLE_FULL");
		}
		obj = "BOTTLE";
		o = bottle;
	}
	if ( HOLDING >= 7 )
		return quip("CARRY_LIMIT");
	long bird = E("BIRD");
	if ( obj == "BIRD"
	  && obj_state(bird) != state_index_c(bird, "BIRD_CAGED")
	  && !obj_stashed(bird) )
	{
		if ( obj_state(bird)
		     == state_index_c(bird, "BIRD_FOREST_UNCAGED") )
		{
			destroy_object(bird, "BIRD");
			return quip("BIRD_CRAP");
		}
		if ( !toting(E("CAGE")) )
			return quip("CANNOT_CARRY");
		if ( toting(E("ROD")) )
			return quip("BIRD_EVADES");
		obj_set_state(bird, state_index_c(bird, "BIRD_CAGED"));
	}
	if ( (obj == "BIRD" || obj == "CAGE")
	  && obj_state_equals(bird, state_index_c(bird, "BIRD_CAGED")) )
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
	// The reference's IS_FOUND is literally "prop == 0": a gem taken
	// out of the cavity (state 1) resets to 0 and empties the cavity.
	if ( is_gemstone(obj) && (!obj_found(o) || obj_state(o) != 0) )
	{
		ui::set(W, o, "found", true);
		obj_set_state(o, 0);
		long cav = E("CAVITY");
		obj_set_state(cav, state_index_c(cav, "CAVITY_EMPTY"));
	}
	return quip("OK_MAN");
}

// ---- drop ---------------------------------------------------------------

long h_discard(var &obj)
{
	long o = E(obj.c_str());
	if ( obj == "ROD" && !toting(o) && toting(E("ROD2")) )
	{
		obj = "ROD2";
		o = E("ROD2");
	}
	if ( !toting(o) )
		return verb_quip();
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
				// The reference's fixed-side LOCATION is also
				// the fixedness flag: hovering pins the rug.
				if ( obj_state(rug) != hover )
				{
					obj_set_state(rug, hover);
					move_fixed_side("RUG",
						static_place(E("SAPPH")));
					obj_set_fixed(rug, -1);
				}
				else
				{
					obj_set_state(rug,
						state_index_c(rug, "RUG_FLOOR"));
					move_fixed_side("RUG", 0);
					obj_set_fixed(rug, 0);
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
	{
		// The bear scares the troll away for good.
		long troll = E("TROLL");
		state_change_c(troll, "TROLL_GONE");
		move_object(troll, 0);
		move_fixed_side("TROLL", 0);
		long troll2 = E("TROLL2");
		move_object(troll2, static_place(troll));
		move_fixed_side("TROLL2", static_fixed(troll));
		juggle_object(E("CHASM"), "CHASM");
		drop_object(o, "BEAR", LOC);
		return G_CLEAROBJ;
	}
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

// The bear's chain (the reference's chain()): unlock frees the chain
// and the bear; lock only works back at the chain's original wall.
long bear_chain(var &verb)
{
	long chain = E("CHAIN");
	long bear = E("BEAR");
	long heap = state_index_c(chain, "CHAIN_HEAP");
	if ( verb != "LOCK" )
	{
		if ( obj_state(bear) == state_index_c(bear, "UNTAMED_BEAR") )
			return quip("BEAR_BLOCKS");
		if ( obj_state(chain) == heap )
			return quip("ALREADY_UNLOCKED");
		obj_set_state(chain, heap);
		obj_set_fixed(chain, 0);
		if ( obj_state(bear) == state_index_c(bear, "BEAR_DEAD") )
			obj_set_fixed(bear, -1);
		else
		{
			obj_set_state(bear,
				      state_index_c(bear, "CONTENTED_BEAR"));
			obj_set_fixed(bear, 0);
		}
		return quip("CHAIN_UNLOCKED");
	}
	if ( obj_state(chain) != heap )
		return quip("ALREADY_LOCKED");
	if ( LOC != static_place(chain) )
		return quip("NO_LOCKSITE");
	obj_set_state(chain, state_index_c(chain, "CHAIN_FIXED"));
	if ( toting(chain) )
		drop_object(chain, "CHAIN", LOC);
	obj_set_fixed(chain, -1);
	return quip("CHAIN_LOCKED");
}

long h_lock(var &verb, var &obj)
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
			return quip("NOTHING_LOCKED");
	}
	long o = E(obj.c_str());
	if ( obj == "CHAIN" )
	{
		if ( here_obj(E("KEYS")) )
			return bear_chain(verb);
		return quip("NO_KEYS");
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
			return quip("HUH_MAN");
		if ( toting(o) )
			return quip(oyster ? "DROP_OYSTER" : "DROP_CLAM");
		if ( !toting(E("TRIDENT")) )
			return quip(oyster ? "OYSTER_OPENER" : "CLAM_OPENER");
		if ( oyster )
			return quip("OYSTER_OPENS");
		destroy_object(E("CLAM"), "CLAM");
		drop_object(E("OYSTER"), "OYSTER", LOC);
		drop_object(E("PEARL"), "PEARL", E("LOC_CULDESAC"));
		return quip("PEARL_FALLS");
	}
	if ( obj == "DOOR" )
	{
		long door = E("DOOR");
		return quip(obj_state(door)
			    == state_index_c(door, "DOOR_UNRUSTED")
			    ? "OK_MAN" : "RUSTY_DOOR");
	}
	if ( obj == "CAGE" )
		return quip("NO_LOCK");
	if ( obj == "KEYS" )
		return quip("CANNOT_UNLOCK");
	return verb_quip();
}

// ---- light / extinguish -------------------------------------------------

long h_light(var &obj)
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
			selects++;
		}
		if ( here_obj(urn)
		  && obj_state(urn) == state_index_c(urn, "URN_DARK") )
		{
			obj = "URN";
			selects++;
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
			return quip("LAMP_OUT");
		state_change_c(lamp, "LAMP_BRIGHT");
		if ( WZDARK )
			return G_TOP;
		return G_CLEAROBJ;
	}
	return verb_quip();
}

long h_extinguish(var &obj)
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
		return quip("BEYOND_POWER");
	return verb_quip();
}

// ---- inventory / eat / drink ---------------------------------------------

long h_inven()
{
	bool empty = true;
	for ( var on : OBJ_NAMES )
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

long h_eat(var &obj)
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
		return quip("THANKS_DELICIOUS");
	}
	if ( one_of(obj,
		    " BIRD SNAKE CLAM OYSTER DWARF DRAGON TROLL BEAR OGRE " ) )
		return quip("LOST_APPETITE");
	return verb_quip();
}

long h_drink(var &obj)
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
		return quip("RIDICULOUS_ATTEMPT");
	if ( lb == "WATER" && here_obj(bottle) )
	{
		ui::move(W, E("WATER"), 0);
		state_change_c(bottle, "EMPTY_BOTTLE");
		return G_CLEAROBJ;
	}
	return verb_quip();
}

// ---- attack ---------------------------------------------------------------

long h_attack(var &obj)
{
	var verb = CMD["verb"];
	if ( obj == "" )
	{
		long changes = 0;
		if ( atdwrf(LOC) > 0 )
		{
			obj = "DWARF";
			changes++;
		}
		if ( here_obj(E("SNAKE")) )
		{
			obj = "SNAKE";
			changes++;
		}
		long dragon = E("DRAGON");
		if ( at_obj(dragon)
		  && obj_state(dragon) == state_index_c(dragon, "DRAGON_BARS") )
		{
			obj = "DRAGON";
			changes++;
		}
		if ( at_obj(E("TROLL")) )
		{
			obj = "TROLL";
			changes++;
		}
		if ( at_obj(E("OGRE")) )
		{
			obj = "OGRE";
			changes++;
		}
		long bear = E("BEAR");
		if ( here_obj(bear)
		  && obj_state(bear) == state_index_c(bear, "UNTAMED_BEAR") )
		{
			obj = "BEAR";
			changes++;
		}
		if ( obj == "" )
		{
			if ( here_obj(E("BIRD")) && verb != "THROW" )
			{
				obj = "BIRD";
				changes++;
			}
			if ( here_obj(E("VEND")) && verb != "THROW" )
			{
				obj = "VEND";
				changes++;
			}
			if ( here_obj(E("CLAM")) || here_obj(E("OYSTER")) )
			{
				obj = "CLAM";
				changes++;
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
			return quip("BEAR_HANDS");
		if ( st == state_index_c(bear, "BEAR_DEAD") )
			return quip("ALREADY_DEAD");
		return quip("BEAR_CONFUSED");
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
		for ( var on : OBJ_NAMES )
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
		// The dwarves' knives ricochet into the ogre.
		rspeak("KNIFE_THROWN");
		destroy_object(E("OGRE"), "OGRE");
		long dwarves = 0;
		for ( long di = 1; di <= 5; di++ )
		{
			long dw = dwarf_ent(di);
			if ( ui::location(W, dw) == LOC )
			{
				dwarves++;
				ui::move(W, dw, E("LOC_LONGWEST"));
				ui::set(W, dw, "seen", false);
			}
		}
		if ( dwarves > 1 )
			rspeak("OGRE_PANIC1");
		else
			rspeak("OGRE_PANIC2");
		return G_CLEAROBJ;
	}
	if ( obj == "" )
		return quip("NO_TARGET");
	if ( obj == "CLAM" || obj == "OYSTER" )
		return quip("SHELL_IMPERVIOUS");
	if ( obj == "SNAKE" )
		return quip("SNAKE_WARNING");
	if ( obj == "DWARF" )
	{
		if ( CLOSED )
			return G_DWARFWAKE;
		return quip("BARE_HANDS_QUERY");
	}
	if ( obj == "DRAGON" )
		return quip("ALREADY_DEAD");
	if ( obj == "TROLL" )
		return quip("ROCKY_TROLL");
	return verb_quip();
}

// ---- feed -----------------------------------------------------------------

long h_feed(var &obj)
{
	if ( obj == "BIRD" )
		return quip("BIRD_PINING");
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
		return quip("TROLL_VICES");
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
			return quip("RIDICULOUS_ATTEMPT");
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
		return verb_quip();
	}
	if ( obj == "OGRE" )
	{
		if ( here_obj(E("FOOD")) )
			rspeak("OGRE_FULL");
		else
			speak_verb_default();
		return G_CLEAROBJ;
	}
	return quip("AM_GAME");
}

// ---- throw ----------------------------------------------------------------

long throw_support(const char *msg)
{
	rspeak(msg);
	drop_object(E("AXE"), "AXE", LOC);
	return G_MOVE;
}

long h_throw(var &obj)
{
	long o = E(obj.c_str());
	if ( !toting(o) )
		return verb_quip();
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
		return quip("TROLL_SATISFIED");
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
	long dw = dwarf_ent(atdwrf(LOC));
	ui::set(W, dw, "seen", false);
	ui::move(W, dw, 0);
	DKILL++;
	if ( DKILL == 1 )
		return throw_support("DWARF_SMOKE");
	return throw_support("KILLED_DWARF");
}

// ---- wave / rub / magic ----------------------------------------------------

long h_wave(var &obj)
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
		TALLY--;
		return quip("NECKLACE_FLY");
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

long h_rub(var &obj)
{
	long urn = E("URN");
	if ( obj == "URN"
	  && obj_state(urn) == state_index_c(urn, "URN_LIT") )
	{
		destroy_object(urn, "URN");
		long amber = E("AMBER");
		drop_object(amber, "AMBER", LOC);
		obj_set_state(amber, state_index_c(amber, "AMBER_IN_ROCK"));
		TALLY--;
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
		return quip("NOTHING_HAPPENS");
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

// FEE=1 .. FUM=5, 0 for anything else (index answers -1 on a miss).
long foo_stage(var &id)
{
	var w;
	php::explode(w, " ", "FEE FIE FOE FOO FUM");
	return w.index(id) + 1;
}

long h_bigwords(var &id)
{
	long stage = foo_stage(id);
	long fb = FOOBAR;
	if ( fb < 0 )
		fb = -fb;
	if ( fb == 0 && stage >= 2 )
		return quip("NOTHING_HAPPENS");
	if ( (fb == 0 && stage == 1) || (fb + 1 == stage && stage <= 4) )
	{
		FOOBAR = stage;
		if ( stage != 4 )
			return quip("OK_MAN");
		FOOBAR = 0;
		long eggs = E("EGGS");
		long eplac = static_place(eggs);
		if ( ui::location(W, eggs) == eplac
		  || (toting(eggs) && LOC == eplac) )
			return quip("NOTHING_HAPPENS");
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
	if ( t1 == T_ACTION && foo_stage(id1) > 0 )
		return h_bigwords(id1);
	var raw1 = CMD["raw1"];
	sspeak_s("OKEY_DOKEY", raw1);
	return G_CLEAROBJ;
}

// ---- read / quit / blast / break / brief / listen --------------------------

long h_read(var &obj)
{
	if ( obj == "" )
	{
		long matches = 0;
		var resolved;
		for ( var on : OBJ_NAMES )
		{
			long oo = E(on.c_str());
			if ( !oo || !here_obj(oo) || obj_stashed(oo) )
				continue;
			var tx;
			ui::get(tx, W, oo, "texts");
			if ( tx.empty() )
				continue;
			matches++;
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
	if ( tx.empty() || !obj_found(o) )
		return verb_quip();
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
		return quip("REQUIRES_DYNAMITE");
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

long h_vbreak(var &obj)
{
	if ( obj == "MIRROR" )
	{
		if ( CLOSED )
		{
			state_change_c(E("MIRROR"), "MIRROR_BROKEN");
			return G_DWARFWAKE;
		}
		return quip("TOO_FAR");
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
	return verb_quip();
}

long h_brief()
{
	ABBNUM = 10000;
	DETAIL = 3;
	return quip("BRIEF_CONFIRM");
}

long h_listen()
{
	bool heard = false;
	var snd;
	ui::get(snd, W, LOC, "sound");
	if ( !snd.is_null() && !snd.empty() )
	{
		rspeak(snd.c_str());
		heard = true;
	}
	for ( var on : OBJ_NAMES )
	{
		long o = E(on.c_str());
		if ( !o || !here_obj(o) || !obj_found(o) || obj_stashed(o) )
			continue;
		var snds;
		ui::get(snds, W, o, "sounds");
		if ( snds.empty() )
			continue;
		long mi = obj_state(o);
		if ( on == "BIRD" && BLOODED )
			mi = mi + 3;
		var st = ZZWORD;
		// The bird sounds carry the reservoir word (%s).
		var tx;
		long i2 = 0;
		for ( var d : snds )
		{
			if ( i2 == mi )
			{
				if ( !d.is_null() && !d.empty() )
				{
					var r;
					var nod;
					render_msg(r, d, nod, st);
					speak_rendered(r);
				}
			}
			i2++;
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

long h_pour(var &obj)
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
		return verb_quip();
	if ( obj != "OIL" && obj != "WATER" )
		return quip("CANT_POUR");
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
		return quip("GROUND_WET");
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
		return quip("SHAKING_LEAVES");
	}
	long door = E("DOOR");
	if ( obj == "OIL" )
		state_change_c(door, "DOOR_UNRUSTED");
	else
		state_change_c(door, "DOOR_RUSTED");
	return G_CLEAROBJ;
}

long h_find(var &obj)
{
	long o = E(obj.c_str());
	if ( toting(o) )
		return quip("ALREADY_CARRYING");
	if ( CLOSED )
		return quip("NEEDED_NEARBY");
	var lb;
	liquid_in_bottle(lb);
	var lq;
	liq_loc(lq, LOC);
	if ( at_obj(o) || (lb == obj && at_obj(E("BOTTLE"))) || lq == obj
	  || (obj == "DWARF" && atdwrf(LOC) > 0) )
		return quip("YOU_HAVEIT");
	return verb_quip();
}

long h_fly(var &obj)
{
	long rug = E("RUG");
	if ( obj == "" )
	{
		if ( !here_obj(rug) )
			return quip("FLAP_ARMS");
		if ( obj_state(rug) != state_index_c(rug, "RUG_HOVER") )
			return quip("RUG_NOTHING2");
		obj = "RUG";
	}
	if ( obj != "RUG" )
		return verb_quip();
	if ( obj_state(rug) != state_index_c(rug, "RUG_HOVER") )
		return quip("RUG_NOTHING1");
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

long h_wake(var &obj)
{
	if ( obj != "DWARF" || !CLOSED )
		return verb_quip();
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
			return quip("KNIVES_VANISH");
		}
		else if ( obj == "ROD" && here_obj(E("ROD2")) )
			obj = "ROD2";
		else if ( (verb == "FIND" || verb == "INVENTORY")
		       && CMD["raw1"].empty() )
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
		if ( !raw1.empty() && verb != "SAY" )
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
				if ( !raw1b.empty() )
					return h_say();
				return G_UNKNOWN;
			}
			if ( verb == "NOTHING" )
				return quip("OK_MAN");
			if ( verb == "GO" )
				return verb_quip();
			if ( verb == "QUIT" )
				return h_quit();
			if ( verb == "SCORE" )
			{
				score_game(M_SCOREGAME);
				return G_CLEAROBJ;
			}
			if ( foo_stage(verb) > 0 )
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
				println(stderr,
					"advent: save/resume out of scope");
				exit(3);
			}
			if ( one_of(verb, " DROP WAVE TAME RUB THROW FIND FEED BREAK WAKE ") )
				return G_UNKNOWN;
			println(stderr,
				"advent: intransitive verb {} unimplemented",
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
		// Intransitive-only verbs used WITH an object just speak
		// their default message (the reference's transitive switch).
		if ( verb == "QUIT" || verb == "SCORE" || verb == "BRIEF"
		  || verb == "SAVE" || verb == "RESUME"
		  || foo_stage(verb) > 0 )
			return verb_quip();
		if ( verb == "BLAST" )
			return h_blast();
		if ( verb == "LISTEN" )
		{
			// The reference's transitive LISTEN only quips.
			return verb_quip();
		}
		if ( verb == "PART" )
			return h_reservoir();
		if ( verb == "NOTHING" )
			return quip("OK_MAN");
		if ( verb == "GO" || verb == "TAME" )
			return verb_quip();
		if ( verb == "SEED" )
		{
			var raw1 = CMD["raw1"];
			long sd = php::intval(raw1);
			var dm;
			action_default(dm, verb);
			speak_msg_d(dm, sd);
			rng_seed(sd);
			TURNS--;	// seed is turn-neutral
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
		println(stderr, "advent: verb {} unimplemented",
			verb.c_str());
		exit(3);
	}

	// part unknown with no verb: ask what to do (the reference's tail).
	var raw = CMD["raw0"];
	sspeak_s("WHAT_DO", raw);
	return G_CHECKHINT;
}
