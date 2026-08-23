// The shared adventure-pilot driver (Track 7 Phase 1, S4b) — included by
// testadventure*.mad; each test's .argv fixture picks the world file and
// the optional session role key. Deliberately dumb two-word command loop:
// parsing sophistication is not what the pilot tests. Meta-commands
// (look/inspect/save/turns/quit) are projection selection and session
// control — not world mutations — so they bypass the verb table by
// design; everything that CHANGES the world goes through ui::act.
#include <cstdio>
#include <cstring>
#include <ns_ui>

int main(int argc, char **argv)
{
    if ( argc < 2 )
    {
	printf("usage: adventure <world-file> [role-key]\n");
	return 1;
    }
    int64_t w = ui::world_open(argv[1]);
    if ( w == 0 )
	return 1;
    if ( argc > 2 )
	ui::session_grant(w, argv[2]);
    int64_t hero = ui::entity_by_name(w, "hero");
    value out;
    ui::render_look(out, w, hero);
    printf("%s", out.c_str());

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
	    ui::render_look(out, w, hero);
	    printf("%s", out.c_str());
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
	    printf("Turn %lld.\n", ui::turn_count(w));
	    continue;
	}
	int64_t before = ui::location(w, hero);
	ui::act(out, w, hero, line, rest);
	if ( out.c_str()[0] == 0 )
	    printf("I don't know how to %s.\n", line);
	else
	    printf("%s\n", out.c_str());
	if ( ui::location(w, hero) != before )
	{
	    ui::render_look(out, w, hero);
	    printf("%s", out.c_str());
	}
    }
    ui::world_close(w);
    return 0;
}
