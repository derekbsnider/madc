// The line-mode text editor's core (Track 7.2 R4; design doc §7.7): the
// verb binder and the session loop. Every command is madc SOURCE through
// the one action registry (the script-entity binding kind — the engine
// ships none of this, Rule #7); every edit is a range operation over the
// document's piece-table component, composed from line spans. The driver
// only splits words and phrases the unknown-verb status — vocabulary and
// the §7.3 rules (read-only refusals, modified-quit confirmation) live in
// the verb bodies under verbs/.
//
// Commands: p [N [M]] · c N TEXT · i N TEXT · a TEXT · d N [M] · f TEXT ·
// w · q · q!

const char *LINEED_VERBS[9] = { "p", "c", "i", "a", "d", "f", "w", "q", "q!" };
const char *LINEED_FILES[9] = { "p", "c", "i", "a", "d", "f", "w", "q", "qbang" };

void bind_lineed_verbs(long w)
{
    for ( int k = 0; k < 9; k = k + 1 )
    {
	value src;
	if ( !php::file_get_contents(src,
		format("examples/texteditor/verbs/{}.madv", LINEED_FILES[k])) )
	{
	    println(stderr, "cannot load verb body {}", LINEED_FILES[k]);
	    continue;
	}
	ui::bind_verb(w, LINEED_VERBS[k], src.c_str());
    }
}

void run_session(const char *path, bool ro)
{
    long w = ui::world_new();
    long doc = ui::create(w, "document");
    value seed;
    if ( !php::file_get_contents(seed, path) )
    {
	printf("cannot read %s\n", path);
	return;
    }
    ui::text_load(w, doc, seed.c_str());
    ui::set(w, doc, "path", path);
    ui::set(w, doc, "modified", false);
    if ( ro )
	ui::set(w, doc, "read_only", true);
    long es = ui::create(w, "editor-state");
    bind_lineed_verbs(w);

    value out;
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
	ui::act(out, w, es, line, rest);
	if ( out.c_str()[0] == 0 )
	    printf("? %s\n", line);
	else
	    printf("%s\n", out.c_str());
	var qd;
	ui::get(qd, w, es, "quit");
	if ( !qd.is_null() && qd.as_boolean() )
	    break;
    }
    ui::world_close(w);
}
