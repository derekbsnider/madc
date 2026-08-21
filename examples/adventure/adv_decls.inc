// ---- the project's shared surface -----------------------------------
// One declaration module, included by every translation unit: the
// cross-TU globals (extern) and function prototypes, grouped by the
// module that DEFINES them. Module-private helpers and state are not
// declared here — the narrowest contract that links.
// The build is examples/adventure/advent.cc.json (madc --project);
// the manifest lists the TUs, MIR links them, advent.mad's main runs.

// ---- adv_state.mad ----
extern long W;
extern long PLAYER;
extern long LOC;
extern long NEWLOC;
extern long OLDLOC;
extern long OLDLC2;
extern long TURNS;
extern long LIMIT;
extern long ABBNUM;
extern long DETAIL;
extern long CLOCK1;
extern long CLOCK2;
extern long DFLAG;
extern long TALLY;
extern long IWEST;
extern long IGO;
extern long FOOBAR;
extern long KNFLOC;
extern long HOLDING;
extern bool CLOSNG;
extern bool CLOSED;
extern bool PANIC;
extern bool WZDARK;
extern bool LMWARN;
extern bool NOVICE;
extern bool SEENBIGWORDS;
extern bool CLSHNT;
extern long NUMDIE;
extern long NDEATHS;
extern long TRNLUZ;
extern long SAVED;
extern long BONUS;
extern long DKILL;
extern var ZZWORD;
extern var VOC_M;
extern var VOC_A;
extern var VOC_O;
extern var NOACT;
extern var FIXEDSIDES;
extern var OBJ_NAMES;
extern var OBJ_INDEX;
extern var HINT_ORDER;
extern var HINT_LC;
extern var HINT_USED;
extern var CMD;
extern var OLDOBJ;
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
extern long T_NONE;
extern long T_MOTION;
extern long T_OBJECT;
extern long T_ACTION;
extern long T_NUMERIC;
extern long T_NOTFOUND;
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
extern long M_QUIT;
extern long M_ENDGAME;
extern long M_SCOREGAME;
long score_game(long mode);
void terminate_game(long mode);
void croak();
void checkhints();

// ---- adv_dwarves.mad ----
bool dwarfmove();

// ---- adv_travel.mad ----
void playermove(var &motion);

// ---- adv_actions.mad ----
extern long G_TERMINATE;
extern long G_MOVE;
extern long G_WORD2;
extern long G_UNKNOWN;
extern long G_CHECKHINT;
extern long G_CLEAROBJ;
extern long G_TOP;
extern long G_DWARFWAKE;
extern long P_UNKNOWN;
extern long P_INTRANSITIVE;
extern long P_TRANSITIVE;
long quip(const char *msg);
long act_dispatch(long part);

// ---- adv_loop.mad ----
bool do_move_step();
bool do_command_once();
