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
// The verbs that would mutate the document: gated by the ONE read-only
// availability check (checks/editable.madv) — enumeration and dispatch
// read the same rule (§7.3's affordance form).
const char *LINEED_EDIT_VERBS[5] = { "c", "i", "a", "d", "w" };

void bind_lineed_verbs(long w)
{
    // THE subject-document rule (verbs/_subject.madv) is prepended to
    // every body at bind: `doc` = the actor's active document, else the
    // world's one "document". One owner; bodies never look it up.
    value pro;
    if ( !php::file_get_contents(pro,
	    "examples/texteditor/verbs/_subject.madv") )
    {
	println(stderr, "cannot load the subject-document prologue");
	return;
    }
    for ( int k = 0; k < 9; k = k + 1 )
    {
	value src;
	if ( !php::file_get_contents(src,
		format("examples/texteditor/verbs/{}.madv", LINEED_FILES[k])) )
	{
	    println(stderr, "cannot load verb body {}", LINEED_FILES[k]);
	    continue;
	}
	var full = format("{}\n{}", pro, src);
	ui::bind_verb(w, LINEED_VERBS[k], full.c_str());
    }
    value chk;
    if ( !php::file_get_contents(chk, "examples/texteditor/checks/editable.madv") )
    {
	println(stderr, "cannot load the editable check body");
	return;
    }
    var fullchk = format("{}\n{}", pro, chk);
    for ( int k = 0; k < 5; k = k + 1 )
	ui::bind_check(w, LINEED_EDIT_VERBS[k], fullchk.c_str());
}

// Create the document entity and load its file: the setup BOTH editors
// share (lined's line mode and vised's TUI mode edit the same document
// shape). 0 = the file could not be read.
long setup_document(long w, const char *path, bool ro)
{
    long doc = ui::create(w, "document");
    value seed;
    if ( !php::file_get_contents(seed, path) )
	return 0;
    ui::text_load(w, doc, seed.c_str());
    ui::set(w, doc, "path", path);
    ui::set(w, doc, "modified", false);
    if ( ro )
	ui::set(w, doc, "read_only", true);
    return doc;
}

void run_session(const char *path, bool ro)
{
    long w = ui::world_new();
    long doc = setup_document(w, path, ro);
    if ( doc == 0 )
    {
	printf("cannot read %s\n", path);
	return;
    }
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
