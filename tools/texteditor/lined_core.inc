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

// Where a program's DATA directory `sub` lives (the one owner: the line
// editor's verbs and checks here, madcide's profiles, plugins and headers
// through their callers). An INSTALLED program's data is its install's: the
// running binary's own directory (madc::compiler_path) anchors the install
// layouts, <exedir>/../share/<product>/<sub> (the Linux/mac package layout),
// then <exedir>/<sub> (the Windows zip: data beside the exe), and they come
// FIRST. The source tree keeps the data beside the file that owns it
// (`src_dir`, the caller's php::dirname(__FILE__)), the answer when the
// running binary has no install layout (madc run from its build tree). It
// is never consulted first: a library compiled from the tree root spells
// __FILE__ relative (tools/madcide/...), so an installed program started
// inside a madc checkout would read that tree's data instead of its own.
// Nothing found keeps the source-tree spelling, so a report names a real
// place to fix.
void resolve_data_dir(var &dir, const char *product, const char *src_dir,
		      const char *sub)
{
    const char *self = madc::compiler_path();
    if ( strlen(self) > 0 )		// "" = the platform probe failed
    {
	var exedir = php::dirname(self);
	var share = format("{}/../share/{}/{}", exedir, product, sub);
	if ( php::file_exists(share) )
	{
	    dir = share;
	    return;
	}
	var beside = format("{}/{}", exedir, sub);
	if ( php::file_exists(beside) )
	{
	    dir = beside;
	    return;
	}
    }
    dir = format("{}/{}", src_dir, sub);
}

// False (with the reason on stderr) when a verb or check body cannot load:
// save and quit ARE verbs, so an editor without them could neither save nor
// quit, and a caller must refuse to start rather than run one. The bodies
// ship inside madcide's package (share/madcide, or beside madcide.exe), the
// one product that carries this editor.
bool bind_lineed_verbs(long w)
{
    var vdir;
    resolve_data_dir(vdir, "madcide", php::dirname(__FILE__), "verbs");
    var cdir;
    resolve_data_dir(cdir, "madcide", php::dirname(__FILE__), "checks");
    // THE subject-document rule (verbs/_subject.madv) is prepended to
    // every body at bind: `doc` = the actor's active document, else the
    // world's one "document". One owner; bodies never look it up.
    var pro;
    var ppath = format("{}/_subject.madv", vdir);
    if ( !php::file_get_contents(pro, ppath) )
    {
	println(stderr, "cannot load the editor's verbs ({}): without them it cannot save or quit — check the install layout", ppath);
	return false;
    }
    for ( int k = 0; k < 9; k = k + 1 )
    {
	var src;
	var vpath = format("{}/{}.madv", vdir, LINEED_FILES[k]);
	if ( !php::file_get_contents(src, vpath) )
	{
	    println(stderr, "cannot load the editor's verb body {}", vpath);
	    return false;
	}
	var full = format("{}\n{}", pro, src);
	ui::bind_verb(w, LINEED_VERBS[k], full.c_str());
    }
    var chk;
    var cpath = format("{}/editable.madv", cdir);
    if ( !php::file_get_contents(chk, cpath) )
    {
	println(stderr, "cannot load the editor's check body {}", cpath);
	return false;
    }
    var fullchk = format("{}\n{}", pro, chk);
    for ( int k = 0; k < 5; k = k + 1 )
	ui::bind_check(w, LINEED_EDIT_VERBS[k], fullchk.c_str());
    return true;
}

// A document's PATH and its KIND move together (client-server design §2.1:
// file extension -> kind is an INPUT-boundary table, converted ONCE when
// the file opens or is renamed — madc::file_kind_of_path, the engine's one
// table; the code rides the document as "kind" and is never re-derived
// from the name at use). ONE owner for the pair — every open, new-file and
// save-as site calls it; a view buffer is not a document and keeps only a
// display path.
void doc_set_path(long w, long doc, const char *path)
{
    ui::set(w, doc, "path", path);
    ui::set(w, doc, "kind", madc::file_kind_of_path(path));
}

// The document's kind (madc::fk*); madc::fkUNKNOWN for a document no open
// site stamped.
long doc_kind(long w, long doc)
{
    var k;
    ui::get(k, w, doc, "kind");
    if ( k.is_null() )
	return madc::fkUNKNOWN;
    return k.as_integer();
}

// An UNTITLED document has no path: the buffer a launch without a file opens
// (Thonny's <untitled>, JOE's Unnamed). Its text is real and it lexes and
// parses; a save asks for a name first.
bool doc_untitled(long w, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    return p.is_null() || strlen(p.c_str()) == 0;
}

// The name a document goes by: its path, or `untitled` for a document that
// has none. The unit name a parse, a lens and a REPL run give it, the status
// line's %n and the tab's title; where a FILE is meant, the path is read.
void doc_name(var &out, long w, long doc)
{
    if ( doc_untitled(w, doc) )
    {
	out = "untitled";
	return;
    }
    ui::get(out, w, doc, "path");
}

// THE document entity: `text` under `path`, unmodified — every document
// either editor holds is minted here. No file is read: a loaded file's
// text arrives from setup_document; a new file's and a view buffer's
// ([repl], [build], [terminal]) text is empty, and a view's name is a
// display path, never looked up on disk (a file named `[build]` in the
// working directory is not the build pane). An empty `path` mints the
// UNTITLED document: no path, and the madc dialect's kind, the language a
// madc editor's unnamed buffer is written in (Save As stamps the real one).
long new_document(long w, const char *path, const char *text)
{
    long doc = ui::create(w, "document");
    ui::text_load(w, doc, text);
    long dialect = madc::fkMADC;	// a long: ui::set's overloads take int64_t or double
    if ( strlen(path) > 0 )
	doc_set_path(w, doc, path);
    else
	ui::set(w, doc, "kind", dialect);
    ui::set(w, doc, "modified", false);
    return doc;
}

// Create the document entity and load its file: the setup BOTH editors
// share (lined's line mode and vised's TUI mode edit the same document
// shape). 0 = the file could not be read.
long setup_document(long w, const char *path, bool ro)
{
    var seed;
    if ( !php::file_get_contents(seed, path) )
	return 0;		// read first: a refused read leaves no
				// orphan document in the world
    long doc = new_document(w, path, seed.c_str());
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
    if ( !bind_lineed_verbs(w) )
    {
	ui::world_close(w);
	return;
    }

    var out;
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
