// madcide_recent.inc — File ▸ Recent Files (plan 2026-10-03-chthonia-
// windows-macos.md §7a): the files a person opened or saved, newest first.
//
// One owner each:
//   recent_list   the list: recent.json in the configuration directory,
//                 {"files": [paths]} — canonical paths (madc::canonical_path,
//                 the identity same_file compares), at most recent_max()
//   recent_note   a file opened or saved goes first; written only for a
//                 session whose client is a person's window or terminal
//                 (the bag's "remember", IdeSession::remember — run_ide sets
//                 it; a headless run, the -c line, an api client and the
//                 test harness keep no history of the person's)
//   recent_files  `recent PATH` opens it (edit_file); bare `recent` lists the
//                 ones that still exist as a choice list
//
// Thread contract: read and written on the session's thread, the whole file
// read, changed and written back; between processes the last write wins, as
// for settings.json.

void edit_file(long w, long es, long olddoc, const char *path0);	// madcide_core.inc
void choice_show(long w, long es, const char *label, var &rows);	// madcide_core.inc

// How many files the list keeps (Thonny keeps ten).
long recent_max()
{
    return 10;
}

// recent.json's path; "" = no configuration directory (nothing is kept).
void recent_path(var &out)
{
    var cfg;
    madcide_config_dir(cfg);
    if ( strlen(cfg.c_str()) == 0 )
	out = "";
    else
	out = format("{}/recent.json", cfg);
}

// The list, newest first: empty when there is no configuration directory, no
// file yet, or a file not of this shape.
void recent_list(var &out)
{
    out = {};
    var p;
    recent_path(p);
    if ( strlen(p.c_str()) == 0 || !php::file_exists(p.c_str()) )
	return;
    var o;
    var why;
    if ( !json_object_read(o, why, p) )
	return;
    var fs;
    keyed_get(fs, o, "files");
    if ( fs.is_null() || !fs.is_array() )
	return;
    for ( var f : fs )
	if ( f.is_string() )
	    out[] = f;
}

// A file opened or saved: first in the list, once, the oldest past
// recent_max() dropped. A path that is no file (an untitled buffer, a new
// file not yet saved) is not a recent file.
void recent_note(long w, long es, const char *path0)
{
    var path = path0;		// ring discipline: own the text first
    if ( !es_flag(w, es, "remember") || strlen(path.c_str()) == 0
      || !php::file_exists(path.c_str()) || php::is_dir(path.c_str()) )
	return;
    var canon;
    madc::canonical_path(canon, path.c_str());
    var old;
    recent_list(old);
    var files = {};
    files[] = canon;
    for ( var f : old )
    {
	if ( php::count(files) >= recent_max() )
	    break;
	if ( !(f == canon) )
	    files[] = f;
    }
    var cfg;
    madcide_config_dir(cfg);
    var p;
    recent_path(p);
    if ( strlen(p.c_str()) == 0 || !dir_ensure(cfg) )
	return;
    var o = { "files": files };
    php::file_put_contents(p.c_str(), format("{}\n", js::stringify(o, 2)));
}

// A document's file, noted (its path; none = untitled).
void recent_note_doc(long w, long es, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    if ( !p.is_null() )
	recent_note(w, es, p.c_str());
}

// `recent`: with an argument (a choice row's, the seat's, -c's) that file
// opens; without one the choice list opens — the listed files that still
// exist, newest first.
void recent_files(long w, long es, long doc, const char *arg0)
{
    var arg = arg0;		// ring discipline: own the text first
    if ( strlen(arg.c_str()) > 0 )
    {
	edit_file(w, es, doc, arg.c_str());
	return;
    }
    var fs;
    recent_list(fs);
    var rows = {};
    for ( var f : fs )
	if ( php::file_exists(f.c_str()) )
	    rows[] = { "title": f, "code": cmdRECENT, "arg": f, "current": 0 };
    if ( php::count(rows) == 0 )
    {
	ui::set(w, es, "msg", "No recent files.");
	return;
    }
    choice_show(w, es, "Recent files", rows);
}
