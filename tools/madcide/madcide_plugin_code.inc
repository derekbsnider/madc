// madcide_plugin_code.inc — a plugin's CODE (docs/plans/2026-09-30-madcide-
// plugins.md §5.4-5.5, §8 Stage B item 2; plan §41.11a step 4): the API
// table madcide hands a plugin, the `library` transport's activation, and
// `madcide --build-plugin`, which builds a plugin's library.
//
// One owner each:
//   madcide_api           THE table (<madcide/plugin_api>): immutable, so
//                         every world and every plugin shares it
//   plugin_library_path   where a plugin's library is: <dir>/<name><suffix>
//   plugin_activate       a plugin's code activated in a world, once: its
//                         library opened with every reference bound, its API
//                         version checked, its activation run; a refusal
//                         takes back what it registered
//   plugins_activate      a session's plugins: the active bundle's code, then
//                         each plugin settings.json's "plugins" names
//   plugin_build          the plugin's source built into its library, the
//                         running madc compiling in-process with madcide's
//                         include directory (--build-plugin)
//
// The library's value-runtime references bind to the libmadc already loaded
// (the plugin design's G1 recon); everything it calls of madcide's goes
// through the table, so it binds to nothing of madcide's.
//
// Thread contract: activation runs at session open, on the session's thread,
// before the profiles and menus load; a library stays loaded for the
// process once a plugin in it is active.

// ---- the table -------------------------------------------------------------
// Each entry adapts the registry's or the session's one owner.
long ide_api_command(long w, const char *name, const char *title, cmd_handler h)
{
    return plugin_command(w, name, title, (long)h);
}

long ide_api_view(long w, const char *name, const char *title, const char *key,
	      long show)
{
    return plugin_view(w, name, title, key, show);
}

bool ide_api_event(long w, long kind, event_handler h)
{
    return plugin_event(w, kind, (long)h);
}

// A command run from a handler goes through the one dispatcher as a key
// bound to it would: its action event, `arg` its argument where the command
// reads one (action_event).
bool ide_api_run(long w, long es, long doc, long code, const char *arg)
{
    IdeSession s;
    s.attach(w, es);
    var e;
    action_event(e, code, arg);
    return s.apply_ide_event(doc, e);
}

long ide_api_command_id(long w, const char *name)
{
    return cmd_of_w(w, name);
}

void ide_api_get(var &out, long w, long scope, const char *key)
{
    ui::get(out, w, scope, key);
}

void ide_api_set(long w, long scope, const char *key, const var &value)
{
    ui::set(w, scope, key, value);
}

const ide_api madcide_api = { MADCIDE_PLUGIN_API, ide_api_command,
			      ide_api_view, ide_api_event, ide_api_run,
			      ide_api_command_id, ide_api_get, ide_api_set };

// ---- the library transport ---------------------------------------------------
// Where plugin `name`'s library is: <dir>/<name><suffix>, the target's
// shared-library suffix from the one spelling owner (madc::library_suffix).
void plugin_library_path(var &out, var &dir, const char *name)
{
    out = format("{}/{}{}", dir, name, madc::library_suffix());
}

// The registry's row counts: a mark taken before an activation.
void registry_mark(var &mark, long w)
{
    var c;
    registry_rows(c, w, "commands");
    var v;
    registry_rows(v, w, "views");
    var e;
    registry_rows(e, w, "events");
    mark = { "commands": php::count(c), "views": php::count(v),
	     "events": php::count(e) };
}

// Take back every row registered after `mark` (a refused activation leaves
// nothing behind: its codes and kinds are free again).
void registry_rollback(long w, var &mark)
{
    long reg = plugin_registry(w);
    if ( reg == 0 )
	return;
    var lists = { "commands", "views", "events" };
    for ( var l : lists )
    {
	var rows;
	registry_rows(rows, w, l.c_str());
	long keep = mark[l.c_str()].as_integer();
	var kept = {};
	for ( long i = 0; i < keep; i = i + 1 )
	    kept[] = rows[i];
	ui::set(w, reg, l.c_str(), kept);
    }
}

// Is plugin `name`'s code already active in world `w`?
bool plugin_is_active(long w, const char *name)
{
    var rows;
    registry_rows(rows, w, "active");
    for ( var r : rows )
	if ( r["name"] == name )
	    return true;
    return false;
}

// Activate plugin `name`'s code (its directory `dir`) in world `w`, once per
// world. False = refused, `why` the reason; nothing it registered remains.
bool plugin_activate(long w, var &dir, const char *name0, var &why)
{
    var name = name0;		// ring discipline: own the text first
    if ( plugin_is_active(w, name.c_str()) )
	return true;
    var lib;
    plugin_library_path(lib, dir, name.c_str());
    if ( !php::file_exists(lib) )
    {
	why = format("no library {}: build it with `madcide --build-plugin {}`",
		     lib, dir);
	return false;
    }
    var lwhy;
    long h = madc::library_open(lib.c_str(), lwhy);
    if ( h == 0 )
    {
	why = format("{} cannot be loaded: {}", lib, lwhy);
	return false;
    }
    long va = madc::library_symbol(h, "madcide_plugin_api");
    long aa = madc::library_symbol(h, "madcide_plugin_activate");
    if ( va == 0 || aa == 0 )
    {
	madc::library_close(h);
	var entry = "madcide_plugin_activate";
	if ( va == 0 )
	    entry = "madcide_plugin_api";
	why = format("{} is not a madcide plugin library (it defines no {})",
		     lib, entry);
	return false;
    }
    plugin_version_fn vf = (plugin_version_fn)va;
    long built = vf();
    if ( built != plugin_api_served() )
    {
	madc::library_close(h);
	why = format("{} was built against plugin API {}; this madcide serves {}: rebuild it with `madcide --build-plugin {}`",
		     lib, built, plugin_api_served(), dir);
	return false;
    }
    var mark;
    registry_mark(mark, w);
    plugin_activate_fn af = (plugin_activate_fn)aa;
    if ( !af(&madcide_api, w) )
    {
	registry_rollback(w, mark);
	madc::library_close(h);
	why = format("its activation refused it ({})", lib);
	return false;
    }
    long reg = plugin_registry_make(w);
    var active;
    registry_rows(active, w, "active");
    active[] = { "name": name, "library": lib, "handle": h };
    ui::set(w, reg, "active", active);
    return true;
}

// Activate one plugin found by name; a refusal goes to the status line.
void plugin_activate_named(long w, long es, var &dir, const char *name0)
{
    var name = name0;
    var why;
    if ( !plugin_activate(w, dir, name.c_str(), why) )
	ui::set(w, es, "msg", format("Plugin '{}' refused: {}", name, why));
}

// A session's plugins, activated in its world before the profiles and menus
// load, so a key profile or a menu can name their commands: the active
// bundle's code, then each plugin settings.json's "plugins" names (§5.2). A
// plugin there contributes its code; the data words stay the active
// profile's. A refusal goes to the status line, and the session opens either
// way.
void plugins_activate(long w, long es)
{
    var m;
    ui::get(m, w, es, "bundleman");
    var code;
    keyed_get(code, m, plugin_field_name(pfCODE));
    if ( !code.is_null() )
    {
	var b;
	ui::get(b, w, es, "bundle");
	var bdir;
	ui::get(bdir, w, es, "bundledir");
	plugin_activate_named(w, es, bdir, b.c_str());
    }
    var us;
    ui::get(us, w, es, "usersettings");
    var names;
    keyed_get(names, us, "plugins");
    if ( names.is_null() )
	return;
    if ( !names.is_array() )
    {
	ui::set(w, es, "msg", "settings.json: \"plugins\" must be a list of plugin names.");
	return;
    }
    for ( var n : names )
    {
	if ( !n.is_string() || !plugin_name_ok(n.c_str()) )
	{
	    ui::set(w, es, "msg", format("settings.json: \"plugins\" lists '{}', which is not a plugin name.", n));
	    continue;
	}
	var dir;
	if ( !plugin_find(dir, n.c_str()) )
	{
	    ui::set(w, es, "msg", format("No plugin '{}'.", n));
	    continue;
	}
	var pm;
	var why;
	if ( !plugin_manifest_read(pm, why, dir, n.c_str()) )
	{
	    ui::set(w, es, "msg", format("Plugin '{}' refused: {}", n, why));
	    continue;
	}
	var pc;
	keyed_get(pc, pm, plugin_field_name(pfCODE));
	if ( !pc.is_null() )
	    plugin_activate_named(w, es, dir, n.c_str());
    }
}

// ---- --build-plugin --------------------------------------------------------
// Build the plugin in `dir0` (its manifest <dir>/<name>.plugin, <name> the
// directory's last component) into its library, from the source its "code"
// names, with madcide's include directory (<madcide/plugin>) on the include
// path. False = refused: `why` says why, and `rows` carries the build's
// diagnostics rows when the compile itself refused.
bool plugin_build(var &why, var &rows, const char *dir0)
{
    var given = dir0;		// ring discipline: own the text first
    const char *gs = given.c_str();
    long n = strlen(gs);
    while ( n > 1 && (gs[n - 1] == '/' || gs[n - 1] == '\\') )
	n = n - 1;		// `dir/` names the same plugin as `dir`
    var dir = perl::substr(given, 0, n);
    var name = php::basename(dir);
    var m;
    if ( !plugin_manifest_read(m, why, dir, name.c_str()) )
	return false;
    var code;
    keyed_get(code, m, plugin_field_name(pfCODE));
    if ( code.is_null() )
    {
	why = format("{}/{}.plugin names no \"code\" to build", dir, name);
	return false;
    }
    var src = format("{}/{}", dir, code);
    var lib;
    plugin_library_path(lib, dir, name.c_str());
    var inc;
    resolve_data_dir(inc, "madcide", php::dirname(__FILE__), "include");
    var incs = { inc };
    if ( !madc::build_native(rows, src.c_str(), "shared", lib.c_str(), incs) )
    {
	why = format("{} did not build", src);
	return false;
    }
    why = lib;
    return true;
}

// The command line's --build-plugin DIR: build, then say what was built or
// why not (each diagnostics row gcc's way, file:line:column: severity:
// message). 0 = built, 1 = refused.
int run_build_plugin(const char *dir)
{
    var why;
    var rows = {};
    if ( plugin_build(why, rows, dir) )
    {
	println("madcide: built {}", why);
	return 0;
    }
    for ( var r : rows )
	println(stderr, "{}:{}:{}: {}: {}", r["file"], r["line"], r["column"],
		r["severity"], r["message"]);
    println(stderr, "madcide: --build-plugin: {}", why);
    return 1;
}
