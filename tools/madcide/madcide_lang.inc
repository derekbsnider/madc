// madcide_lang.inc — the session's LANGUAGE STANDARD (owner 2026-10-03): one
// standard per session, read by every compile of a buffer (its parse handle:
// the check, the outline, the colours, Build and Run) and by the REPL (its
// session, F5's run). `--std=` on the command line sets it, else the "std"
// setting; unset, each buffer's FILE decides: its family (<bits/file_kinds>:
// .c is C, .cpp C++, .mad madc) takes the family's row of the Language
// table, so hello.c is C17 and its REPL a C one. The `language` command
// (Run ▸ Language…) sets it, keeps it in settings.json's "std" (`file` = unset),
// re-parses every buffer and publishes seLANGUAGE, which restarts what a
// module runs under it.
//
// Thread contract: the session's thread; the standard lives on one world
// entity (`language`), shared by every client of the session.

void choice_show(long w, long es, const char *label, var &rows);	// madcide_core.inc
long active_doc(long w, long es, long fallback);			// madcide_core.inc
long refresh_diags(long w, long es, long doc);				// madcide_core.inc

// The standards the Language list offers: one per family, the one a file
// of that family takes when the session sets none (the REPL's three,
// decided 2026-09-30). A row's kind is the code everything carries; the
// spelling is the engine's (madc::file_kind_name).
void lang_rows(var &out)
{
    out = {};
    out[] = { "kind": madc::fkC17, "title": "C17" };
    out[] = { "kind": madc::fkCPP17, "title": "C++17" };
    out[] = { "kind": madc::fkMADC, "title": "madc" };
}

// The family a compiler kind belongs to (madc::fkC, fkCPP or fkMADC, the
// range heads); madc::fkUNKNOWN for a kind the compiler does not read. The
// one reader of those ranges: kind_compiler_range (madcide_layers.inc) and
// cxx_view_applies (madcide_core.inc) ask it.
long kind_family(long k)
{
    if ( k >= madc::fkC && k <= madc::fkC_LAST )
	return madc::fkC;
    if ( k >= madc::fkCPP && k <= madc::fkCPP_LAST )
	return madc::fkCPP;
    if ( k >= madc::fkMADC && k <= madc::fkMADC_LAST )
	return madc::fkMADC;
    return madc::fkUNKNOWN;
}

// A kind that names a standard: a C or a C++ standard, or the madc dialect —
// never a family head (madc::fkC is "C, standard unspecified") and never
// madc::fkMC11 (the IR's text, not a standard).
bool kind_is_standard(long k)
{
    long f = kind_family(k);
    if ( f == madc::fkMADC )
	return k == madc::fkMADC;
    return f != madc::fkUNKNOWN && k != f;
}

// A standard's spelling where it enters (the command line, the "std"
// setting, the command's argument), converted once: its kind, or
// madc::fkUNKNOWN when it names none. A leading `--std=` is madc's own
// option spelling and is accepted.
long lang_kind_of(const char *spelling)
{
    const char *s = spelling;
    if ( strncmp(s, "--std=", 6) == 0 )
	s = s + 6;
    long k = madc::file_kind_of(s);
    if ( !kind_is_standard(k) )
	return madc::fkUNKNOWN;
    return k;
}

// The world entity the standard lives on; made on first use.
long lang_world(long w)
{
    long e = ui::entity_by_name(w, "language");
    if ( e == 0 )
	e = ui::create(w, "language");
    return e;
}

// The standard the session SETS: a kind, or 0 = none (each file decides).
long lang_session(long w)
{
    return es_int(w, lang_world(w), "std", 0);
}

// The standard `doc` compiles under: the session's when it sets one; else
// its file's family's row of the Language table; 0 (the engine's default,
// madc) for a buffer of no compiler family — an untitled buffer, a text file.
long doc_standard(long w, long doc)
{
    long s = lang_session(w);
    if ( s != 0 )
	return s;
    long fam = kind_family(doc_kind(w, doc));
    if ( fam == madc::fkUNKNOWN )
	return 0;
    var rows;
    lang_rows(rows);
    for ( var r : rows )
	if ( kind_family(r["kind"].as_integer()) == fam )
	    return r["kind"].as_integer();
    return 0;
}

// A standard's title for the status line and the list: the table's, else
// the engine's spelling; `file` for none.
void lang_title(var &out, long k)
{
    out = "the file's";
    if ( k == 0 )
	return;
    out = madc::file_kind_name(k);
    var rows;
    lang_rows(rows);
    for ( var r : rows )
	if ( r["kind"].as_integer() == k )
	    out = r["title"];
}

// The session's standard at its first open (init_view_es, before seOPEN, so
// a module started by the open runs under it): the command line's (`cli`, a
// kind main converted; 0 = not given), else the "std" setting. Decided once:
// a later client of the same session (viewwindow) finds it decided. A
// setting that names no standard is ignored, and the status line says so.
void lang_open(long w, long es, long cli)
{
    long lw = lang_world(w);
    var decided;
    ui::get(decided, w, lw, "std");
    if ( !decided.is_null() )
	return;
    long k = cli;
    if ( k == 0 )
    {
	var sv;
	setting_get(sv, w, es, "std");
	if ( !sv.is_null() && sv.is_string() && strlen(sv.c_str()) > 0
	  && !(sv == "file") )
	{
	    k = lang_kind_of(sv.c_str());
	    if ( k == madc::fkUNKNOWN )
		ui::set(w, es, "msg", format("The \"std\" setting '{}' names no standard — each file decides.", sv));
	}
    }
    ui::set(w, lw, "std", k);
}

// The setting the choice is kept in (settings.json's "std"; `file` = none),
// written as Tools ▸ Key bindings writes its own.
bool lang_save(long w, long es, long k)
{
    var spelling = "file";
    if ( k != 0 )
	spelling = madc::file_kind_name(k);
    var us;
    ui::get(us, w, es, "usersettings");
    if ( us.is_null() || !us.is_object() )
    {
	var top = { "std": spelling };
	us = top;
    }
    else
	us["std"] = spelling;
    ui::set(w, es, "usersettings", us);
    return user_settings_save(w, es);
}

void doc_forget_parse(long w, long doc);	// madcide_core.inc

// The session's standard becomes `k` (0 = each file decides): every
// buffer's parse handle goes (the next need opens it under the new
// standard), the active buffer is checked again so the Problems rows and
// the colours describe the new standard, the choice is kept, and the
// modules hear seLANGUAGE (the REPL restarts its session under it).
void lang_set(long w, long es, long k)
{
    ui::set(w, lang_world(w), "std", k);
    var bufs;
    ui::get(bufs, w, es, "buffers");
    if ( !bufs.is_null() )
	for ( var row : bufs )
	    doc_forget_parse(w, row["doc"].as_integer());
    long doc = active_doc(w, es, 0);
    if ( doc != 0 )
    {
	doc_forget_parse(w, doc);
	refresh_diags(w, es, doc);
    }
    var title;
    lang_title(title, k);
    if ( lang_save(w, es, k) )
    {
	if ( k == 0 )
	    ui::set(w, es, "msg", "The language follows each file.");
	else
	    ui::set(w, es, "msg", format("The language is {}.", title));
    }
    var ev = { "std": k };
    publish_event(w, es, doc, seLANGUAGE, ev);
}

// `language` (Run ▸ Language…): with an argument — a standard's spelling
// (the seat's, -c's, a choice row's), or `file` for none — the session's
// standard becomes it; an unknown spelling is refused with the choices.
// Without one the choice list opens: the table's rows and "By file", the
// current one marked.
void lang_command(long w, long es, const char *arg0)
{
    var arg = arg0;		// ring discipline: own the text first
    var rows;
    lang_rows(rows);
    long cur = lang_session(w);
    if ( strlen(arg.c_str()) > 0 )
    {
	long k = 0;
	if ( !(arg == "file") )
	{
	    k = lang_kind_of(arg.c_str());
	    if ( k == madc::fkUNKNOWN )
	    {
		var names = "file";
		for ( var r : rows )
		    names = format("{}, {}", names, madc::file_kind_name(r["kind"].as_integer()));
		ui::set(w, es, "msg", format("Unknown language '{}' ({}, or any --std= standard).", arg, names));
		return;
	    }
	}
	if ( k == cur )
	{
	    var title;
	    lang_title(title, k);
	    ui::set(w, es, "msg", format("The language is already {}.", title));
	    return;
	}
	lang_set(w, es, k);
	return;
    }
    var crows = {};
    long fmark = 0;
    if ( cur == 0 )
	fmark = 1;
    crows[] = { "title": "By file", "code": cmdLANGUAGE, "arg": "file", "current": fmark };
    for ( var r : rows )
    {
	long k = r["kind"].as_integer();
	long mark = 0;
	if ( k == cur )
	    mark = 1;
	crows[] = { "title": r["title"], "code": cmdLANGUAGE,
		    "arg": madc::file_kind_name(k), "current": mark };
    }
    choice_show(w, es, "Language", crows);
}
