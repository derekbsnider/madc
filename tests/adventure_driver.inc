// The shared adventure-pilot driver (Track 7 Phase 1; re-cut in Track 7.2
// R1) — included by testadventure*.mad; each test's .argv fixture picks
// the world file and the optional session role key. Deliberately dumb
// two-word command loop: parsing sophistication is not what the pilot
// tests.
//
// THE PILOT IS THE APPLICATION (Rule #7): its verbs are madc source
// (tests/adventure_verbs/*.madv) bound through ui::bind_verb; its look
// view is composed HERE from generic reads (get/contents/links). The
// engine supplies only the generic machinery. Meta-commands (look/
// inspect/save/turns/quit) are projection selection and session control —
// not world mutations — so they bypass the verb table by design;
// everything that CHANGES the world goes through ui::act.
#include <cstring>
#include "adventure_bind.inc"
#include <ns_php>

// The pilot's display convention: the authored `short` or the canonical
// name — application vocabulary, application code.
void adv_display(value &out, long w, long e)
{
    ui::get(out, w, e, "short");
    if ( out.is_null() )
	ui::name_of(out, w, e);
}

// The pilot's look view, composed from facts: title heading, description,
// co-located things, exits. A dark room without a lit light source (in
// the room or carried) shows only darkness.
void adv_look(long w, long hero)
{
    long room = ui::location(w, hero);
    if ( room == 0 )
    {
	printf("You are nowhere at all.\n");
	return;
    }
    value dark;
    ui::get(dark, w, room, "dark");
    if ( !dark.is_null() && dark.as_boolean() )
    {
	bool lit = false;
	value here;
	ui::contents(here, w, room);
	for ( value hn : here )
	{
	    value l;
	    ui::get(l, w, ui::entity_by_name(w, hn.c_str()), "lit");
	    if ( !l.is_null() && l.as_boolean() )
		lit = true;
	}
	value carried;
	ui::contents(carried, w, hero);
	for ( value cn : carried )
	{
	    value l;
	    ui::get(l, w, ui::entity_by_name(w, cn.c_str()), "lit");
	    if ( !l.is_null() && l.as_boolean() )
		lit = true;
	}
	if ( !lit )
	{
	    printf("It is pitch dark. You are likely to be eaten by a grue.\n");
	    return;
	}
    }
    value title;
    ui::get(title, w, room, "title");
    if ( title.is_null() )
	ui::name_of(title, w, room);
    printf("=== %s ===\n", title.c_str());
    value desc;
    ui::get(desc, w, room, "desc");
    if ( !desc.is_null() )
	printf("%s\n", desc.c_str());
    value here;
    ui::contents(here, w, room);
    for ( value hn : here )
    {
	long e = ui::entity_by_name(w, hn.c_str());
	if ( e == hero )
	    continue;
	value d;
	adv_display(d, w, e);
	printf("  %s is here.\n", php::ucfirst(d));
    }
    value exits;
    ui::links(exits, w, room, "exit");
    long n = exits.count();
    if ( n == 0 )
    {
	printf("Exits: none.\n");
	return;
    }
    value acc = "Exits: ";
    long i = 0;
    for ( value x : exits )
    {
	if ( i > 0 )
	{
	    if ( i + 1 == n )
		acc += " and ";
	    else
		acc += ", ";
	}
	acc += x["key"].c_str();
	i = i + 1;
    }
    printf("%s.\n", acc.c_str());
}

int main(int argc, char **argv)
{
    if ( argc < 2 )
    {
	printf("usage: adventure <world-file> [role-key]\n");
	return 1;
    }
    long w = ui::world_open(argv[1]);
    if ( w == 0 )
	return 1;
    if ( argc > 2 )
	ui::session_grant(w, argv[2]);
    bind_adventure_verbs(w);
    long hero = ui::entity_by_name(w, "hero");
    value out;
    adv_look(w, hero);

    char line[256];
    while ( fgets(line, sizeof(line), stdin) )
    {
	int len = strlen(line);
	if ( len > 0 && line[len - 1] == '\n' )
	    line[len - 1] = 0;
	if ( line[0] == 0 )
	    continue;
	printf("> %s\n", line);
	const char *rest = "";
	char *sp = strchr(line, ' ');
	if ( sp )
	{
	    *sp = 0;
	    rest = sp + 1;
	}
	if ( strcmp(line, "quit") == 0 )
	    break;
	if ( strcmp(line, "look") == 0 )
	{
	    adv_look(w, hero);
	    continue;
	}
	if ( strcmp(line, "inspect") == 0 )
	{
	    ui::render_inspect(out, w, ui::entity_by_name(w, rest));
	    printf("%s", out.c_str());
	    continue;
	}
	if ( strcmp(line, "save") == 0 )
	{
	    if ( ui::world_save(w, rest) )
		printf("Saved.\n");
	    else
		printf("Save failed.\n");
	    continue;
	}
	if ( strcmp(line, "turns") == 0 )
	{
	    value t;
	    ui::get(t, w, ui::entity_by_name(w, "world-state"), "turn");
	    printf("Turn %s.\n", t);
	    continue;
	}
	long before = ui::location(w, hero);
	ui::act(out, w, hero, line, rest);
	if ( out.c_str()[0] == 0 )
	    printf("I don't know how to %s.\n", line);
	else
	    printf("%s\n", out.c_str());
	if ( ui::location(w, hero) != before )
	    adv_look(w, hero);
    }
    ui::world_close(w);
    return 0;
}
