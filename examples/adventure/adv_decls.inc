// ---- the project's shared surface -----------------------------------
// One declaration module, included by every translation unit. ALL game
// state lives in the Game object (the player rides inside it as a
// Player member) — a single `extern Game G;` is the only extern in the
// program. Cross-TU function prototypes follow, grouped by the module
// that DEFINES them; module-private helpers and state are not declared
// here. The build is examples/adventure/advent.cc.json (madc
// --project): the manifest lists the TUs, MIR links them, advent.mad's
// main runs.

// Terminate modes (score_game / terminate_game).
enum { M_QUIT = 0, M_ENDGAME = 1, M_SCOREGAME = 2 };

// Word classes (vocabulary).
enum { T_NONE = 0, T_MOTION = 1, T_OBJECT = 2, T_ACTION = 3,
       T_NUMERIC = 4, T_NOTFOUND = 5 };

// Verb-dispatch results (act_dispatch).
enum { G_TERMINATE = 1, G_MOVE = 2, G_WORD2 = 3, G_UNKNOWN = 4,
       G_CHECKHINT = 5, G_CLEAROBJ = 6, G_TOP = 7, G_DWARFWAKE = 8 };

// Command parts (act_dispatch).
enum { P_UNKNOWN = 0, P_INTRANSITIVE = 1, P_TRANSITIVE = 2 };

// Player-relative flag bits (Player::flags).
enum {
	PF_WZDARK       = 1,	// it was dark here last turn (as he saw it)
	PF_LMWARN       = 2,	// lamp-dim warning given
	PF_NOVICE       = 4,	// asked for instructions (shorter lamp)
	PF_SEENBIGWORDS = 8,	// has seen the giant-room big words
	PF_CLSHNT       = 16,	// took the closing hint (score penalty)
	PF_BLOODED      = 32	// drank the dragon's blood (listen changes)
};

// Game-relative flag bits (Game::flags).
enum {
	GF_CLOSING = 1,		// the cave is closing
	GF_CLOSED  = 2,		// the cave is closed
	GF_PANIC   = 4		// closing panic clock started
};

// The player: one actor's state (MUD seams — the player is an actor
// entity in the hub, never a singleton).
class Player
{
    public:
	long ent;		// the player actor entity in the hub
	long loc;		// current location entity
	long newloc;		// destination the travel system chose
	long oldloc, oldlc2;	// BACK targets (oldlc2 = pre-death safe spot)
	long holding;		// objects carried
	long detail;		// LOOK detail nag counter
	long iwest, igo;	// WEST / GO nag counters
	long foobar;		// FEE FIE FOE FOO progress
	long knfloc;		// where the dwarf knife landed
	long numdie;		// deaths so far
	long flags;		// PF_* bits
	var cmd;		// the current command bag
	var oldobj;		// previous command's object (clear_command)
	bool has(long f);
	void set(long f);
	void clear(long f);
	void put(long f, bool on);
};

// The game: everything truly global to one running game.
class Game
{
    public:
	long w;			// hub session handle
	long turns;
	long limit = 330;	// lamp turns left
	long abbnum = 5;	// full location description every N visits
	long tally;		// treasures not yet seen
	long clock1 = 30, clock2 = 50;	// cave-closing clocks
	long dflag;		// dwarf activation stage
	long dkill;		// dwarves killed
	long bonus;		// closing bonus: 0 none 1 splat 2 defeat 3 win
	long trnluz, saved;	// turn/save score penalties
	long ndeaths = 3;	// lives (obituary count)
	long flags;		// GF_* bits
	var zzword;		// the reservoir magic word (rng_seed builds it)
	var voc_m, voc_a, voc_o;// vocabulary: word -> [ids], by class
	var noact;		// message-only verbs: name -> true
	var fixedsides;		// objname -> {loc, seq}: the LIVE second
				// placement of two-placed objects
	var obj_names;		// object names in objnum order
	var obj_index;		// object name -> objnum (1-based)
	var hint_order;		// padded-number key -> hint name
	var hint_lc;		// hint name -> turn counter at hinted locs
	var hint_used;		// hint name -> true once taken
	Player player;		// the player rides inside the game object
	bool has(long f);
	void set(long f);
	void clear(long f);
	void put(long f, bool on);
};

// The one extern in the program.
extern Game G;
// ---- adv_state.mad ----
void listing_insert(var &byseq, long seq, const char *name);
long E(const char *name);
long bag_int(long e, const char *key, long dflt);
bool bag_flag(long e, const char *key);
bool loccond(long l, const char *flag);
bool outside_loc(long l);
bool inside_loc(long l);
bool forced_loc(long l);
bool obj_stashed(long o);
void obj_set_stashed(long o, bool s);
long obj_state(long o);
void obj_set_state(long o, long s);
bool obj_state_equals(long o, long pval);
bool obj_found(long o);
bool obj_treasure(long o);
bool toting(long o);
long fixed_side_loc(const char *name);
void move_fixed_side(const char *name, long dest);
bool at_obj(long o);
bool here_obj(long o);
long state_index(long o, var &label);
long state_index_c(long o, const char *label);
void move_object(long o, long dest);
void carry_object(long o, const char *name);
void drop_object(long o, const char *name, long dest);
void destroy_object(long o, const char *name);
void put_object(long o, const char *name, long dest, long pval);
bool obj_is_fixed(long o);
void obj_set_fixed(long o, long fixedness);
long static_place(long o);
long static_fixed(long o);
void liquid_in_bottle(var &out);
long objects_at_loc(var &only, long at);
void juggle_object(long o, const char *name);
long dwarf_ent(long i);
long atdwrf(long l);
bool is_dark();
void liq_loc(var &out, long l);
bool world_boot();
void init_game();

// ---- adv_rng.mad ----
long randrange(long range);
bool pct(long n);
void rng_seed(long seedval);

// ---- adv_io.mad ----
void render_msg(var &out, var &text, var &dargs, var &sval);
void speak_rendered(var &text);
void speak_msg(var &text);
void speak_msg_noblank(var &text);
void speak_msg_d(var &text, long d);
void msg_of(var &out, const char *name);
void rspeak(const char *name);
void rspeak_d(const char *name, long d);
void rspeak_d3(const char *name, long a, long b, long c);
void sspeak_s(const char *name, var &s);
bool get_input(var &out);
void first_word(var &out, var &line);
bool silent_ask();
bool ask_yes_no(const char *qname, const char *yname, const char *nname);

// ---- adv_vocab.mad ----
long word_count(var &line);
void tokenize_command(var &line);
void shift_words();
void clear_command();

// ---- adv_describe.mad ----
void pspeak_prop(long o, const char *prop, long kk, bool blank);
void pspeak_look(long o, long kk);
void pspeak_touch(long o);
void state_change(long o, long kk);
void state_change_c(long o, const char *label);
void describe_location();
void listobjects();

// ---- adv_score.mad ----
long score_game(long mode);
void terminate_game(long mode);
void croak();
void checkhints();

// ---- adv_dwarves.mad ----
bool dwarfmove();

// ---- adv_travel.mad ----
void playermove(var &motion);

// ---- adv_actions.mad ----
long quip(const char *msg);
long act_dispatch(long part);

// ---- adv_loop.mad ----
bool do_move_step();
bool do_command_once();
