// madcide_plugins.inc — plugins, Stage A: BUNDLES (docs/plans/2026-09-30-
// madcide-plugins.md §5.1-5.2; plan §41.11a step 1). A plugin is a directory
// holding its manifest, <name>/<name>.plugin (JSON, read without running
// anything), and the data files it contributes, in their existing formats.
// A BUNDLE is a plugin with no code: it names the keys, layout, menu, theme
// and status a session loads, and its settings' defaults. `default` is the
// bundle in use when nothing selects another, and its words fill every word
// another bundle leaves unset.
//
// One owner each:
//   madcide_config_dir    the user's configuration directory (settings.json,
//                         plugins/)
//   plugin_find           the search path, first found by name wins: the
//                         user's plugins/, then the shipped plugins/
//   plugin_manifest_read  a manifest's words, checked once, at load
//   active_profile        which bundle: the command line's --profile, then
//                         the program's own name (program_name) when a
//                         bundle of that name exists, then settings.json's
//                         "profile", then `default`
//   bundle_select         the active bundle onto the bag
//   bundle_word           the name a bundle gives a data kind, else the
//                         default bundle's (base_word)
//   bundle_data_path      where that data file lives: the bundle's own
//                         directory, then the shared profiles/
//   setting_get           a setting: settings.json, then the active bundle's
//                         default, then the default bundle's
//
// Bag state (per client, set at init_view_es): bundle (the name in use),
// bundledir (its directory; "" for the built-in default), bundleman (its
// manifest), bundlebase (the default bundle's manifest), usersettings (the
// parsed settings.json).
//
// Thread contract: read at session open, on the session's thread; the
// manifests and settings on the bag are read-only afterwards (plugin design
// §7).

#include "include/madcide/plugin_api"	// the API version, the handler contracts

// Where madcide's configuration lives, from its inputs (the environment's
// values; NULL or "" = unset): MADCIDE_CONFIG_DIR (madcide's own override —
// the test suite points it at a fixture, or nowhere), then
// $XDG_CONFIG_HOME/madcide, then %APPDATA%/madcide on Windows, then
// ~/.config/madcide. "" = none (no home): no settings and no user plugins,
// never an error.
void config_dir_of(var &out, const char *over, const char *xdg,
		   const char *appdata, const char *home, bool win)
{
    out = "";
    if ( over != 0 && strlen(over) > 0 )
    {
	out = over;
	return;
    }
    if ( xdg != 0 && strlen(xdg) > 0 )
    {
	out = format("{}/madcide", xdg);
	return;
    }
    if ( win && appdata != 0 && strlen(appdata) > 0 )
    {
	out = format("{}/madcide", appdata);
	return;
    }
    if ( home != 0 && strlen(home) > 0 )
	out = format("{}/.config/madcide", home);
}

void madcide_config_dir(var &out)
{
    bool win = strcmp(madc::sys.platform, "win32") == 0;
    config_dir_of(out, getenv("MADCIDE_CONFIG_DIR"), getenv("XDG_CONFIG_HOME"),
		  getenv("APPDATA"), getenv("HOME"), win);
}

// A JSON OBJECT from a file (a manifest, settings.json). False = it cannot
// be read, is not JSON, or is not an object; `why` says which.
bool json_object_read(var &out, var &why, var &path)
{
    var text;
    if ( !php::file_get_contents(text, path.c_str()) )
    {
	why = "cannot be read";
	return false;
    }
    if ( !js::parse(out, text) )
    {
	why = "is not valid JSON";
	return false;
    }
    if ( !out.is_object() )
    {
	why = "is not a JSON object";
	return false;
    }
    return true;
}

// A plugin's or a data file's NAME is one path component: letters, digits,
// '_', '-' and '.', not starting with '.'. It is never a path, so
// `--profile ../x` names no plugin.
bool plugin_name_ok(const char *name)
{
    long n = strlen(name);
    if ( n == 0 || name[0] == '.' )
	return false;
    for ( long i = 0; i < n; i = i + 1 )
    {
	char c = name[i];
	bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		  || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
	if ( !ok )
	    return false;
    }
    return true;
}

// The plugin search path (§5.1): the user's plugins/ first, so a user's
// bundle overrides a shipped one of the same name, then the shipped
// plugins/ (beside this file in the source tree, else the install layouts:
// resolve_data_dir, the one data-location owner).
void plugin_dirs(array &out)
{
    var cfg;
    madcide_config_dir(cfg);
    if ( strlen(cfg.c_str()) > 0 )
	out.push(format("{}/plugins", cfg));
    var shipped;
    resolve_data_dir(shipped, "madcide", php::dirname(__FILE__), "plugins");
    out.push(shipped);
}

// The directory holding plugin `name` (<dir>/<name>.plugin), the first on
// the search path. False = no directory holds it.
bool plugin_find(var &dir, const char *name0)
{
    var name = name0;		// ring discipline: own the text first
    array dirs;
    plugin_dirs(dirs);
    for ( var d : dirs )
    {
	var cand = format("{}/{}", d, name);
	var man = format("{}/{}.plugin", cand, name);
	if ( php::file_exists(man) )
	{
	    dir = cand;
	    return true;
	}
    }
    return false;
}

// The plugin API version this madcide serves (<madcide/plugin_api>'s). A
// manifest's "api" names the version it targets, and a plugin library
// records the one it was built against.
long plugin_api_served()
{
    return MADCIDE_PLUGIN_API;
}

// Plugin `name`'s manifest from `dir`, every word checked once (the input
// boundary, §5.1): each field is a plugin_field word, "name" is the
// directory's, "api" is the served version, each contribution is a
// contribution_kind word, a data kind names a file (one name, no path) and
// settings is an object. False = refused; `why` names the file and the word.
bool plugin_manifest_read(var &m, var &why, var &dir, const char *name0)
{
    var name = name0;		// ring discipline: own the text first
    var path = format("{}/{}.plugin", dir, name);
    var jwhy;
    if ( !json_object_read(m, jwhy, path) )
    {
	why = format("{} {}", path, jwhy);
	return false;
    }
    array fields;
    php::array_keys(fields, m);
    for ( var f : fields )
	if ( plugin_field_of(f) == pfNONE )
	{
	    why = format("{}: unknown field '{}'", path, f);
	    return false;
	}
    var mn;
    keyed_get(mn, m, plugin_field_name(pfNAME));
    if ( mn.is_null() || !mn.is_string() || mn != name )
    {
	why = format("{}: its \"name\" must be '{}', its directory's", path, name);
	return false;
    }
    var api;
    keyed_get(api, m, plugin_field_name(pfAPI));
    if ( api.is_null() || !api.is_integer() )
    {
	why = format("{}: no \"api\" (the plugin API version it targets)", path);
	return false;
    }
    if ( api.as_integer() != plugin_api_served() )
    {
	why = format("{}: targets plugin API {}; this madcide serves {}", path,
		     api, plugin_api_served());
	return false;
    }
    for ( long f = pfTITLE; f <= pfVERSION; f = f + 1 )
    {
	var s;
	keyed_get(s, m, plugin_field_name(f));
	if ( !s.is_null() && !s.is_string() )
	{
	    why = format("{}: \"{}\" must be text", path, plugin_field_name(f));
	    return false;
	}
    }
    var code;
    keyed_get(code, m, plugin_field_name(pfCODE));
    if ( !code.is_null() && (!code.is_string() || !plugin_name_ok(code.c_str())) )
    {
	why = format("{}: \"code\" must name the plugin's source file in its directory", path);
	return false;
    }
    var c;
    keyed_get(c, m, plugin_field_name(pfCONTRIBUTES));
    if ( c.is_null() )
	return true;
    if ( !c.is_object() && !(c.is_array() && php::count(c) == 0) )
    {
	why = format("{}: \"contributes\" must be an object", path);
	return false;
    }
    array kinds;
    php::array_keys(kinds, c);
    for ( var k : kinds )
    {
	long kind = contribution_kind_of(k);
	if ( kind == cbNONE )
	{
	    why = format("{}: unknown contribution '{}'", path, k);
	    return false;
	}
	var v;
	keyed_get(v, c, k.c_str());
	if ( contribution_is_data(kind) )
	{
	    if ( !v.is_string() || !plugin_name_ok(v.c_str()) )
	    {
		why = format("{}: \"{}\" must name a .{} file", path, k, k);
		return false;
	    }
	}
	else if ( !v.is_object() && !(v.is_array() && php::count(v) == 0) )
	{
	    why = format("{}: \"{}\" must be an object", path, k);
	    return false;
	}
    }
    return true;
}

// The built-in DEFAULT bundle: the words madcide loaded before bundles, and
// the manifest in use when no default.plugin is found or the one found is
// refused. The shipped plugins/default/default.plugin says the same
// (testmadcide_bundles pins the two to one reading).
const char *default_bundle_text()
{
    return "{ \"name\": \"default\", \"title\": \"Workbench\", \"api\": 1,"
	   " \"contributes\": { \"keys\": \"joe\", \"status\": \"joe\","
	   " \"theme\": \"default\", \"menu\": \"default\", \"layout\": \"default\" } }";
}

// The user's settings.json (§5.2), in the configuration directory, onto the
// bag ("usersettings"). A missing file is no settings. One that cannot be
// read or is not a JSON object is ignored, and the status line says so.
void user_settings_load(long w, long es)
{
    var cfg;
    madcide_config_dir(cfg);
    if ( strlen(cfg.c_str()) == 0 )
	return;
    var path = format("{}/settings.json", cfg);
    if ( !php::file_exists(path) )
	return;
    var s;
    var why;
    if ( !json_object_read(s, why, path) )
    {
	ui::set(w, es, "msg", format("{} {} — ignored.", path, why));
	return;
    }
    ui::set(w, es, "usersettings", s);
}

// Create the directory and every ancestor. php::mkdir is PHP's ONE level
// (there is no recursive flag), so walk the components; an already-existing
// level is not an error. False = it does not exist afterwards. The one
// owner: the session advertisements' directory (madcide_discover.inc) and
// the configuration directory (user_settings_save) both make theirs here.
bool dir_ensure(var &dir)
{
    const char *d = dir.c_str();
    if ( strlen(d) == 0 )
	return false;
    if ( php::file_exists(d) )
	return true;
    array parts;
    php::explode(parts, "/", d);
    long n = php::count(parts);
    // An absolute path's leading empty component IS the root; a relative one
    // grows from the working directory.
    var grown = ".";
    if ( d[0] == '/' )
	grown = "";
    for ( long i = 0; i < n; i = i + 1 )
    {
	const char *seg = php::array_get_cstr(parts, i);
	if ( strlen(seg) == 0 )
	    continue;
	var next = format("{}/{}", grown, seg);
	grown = next;
	if ( !php::file_exists(grown.c_str()) )
	    php::mkdir(grown.c_str());
    }
    return php::file_exists(d);
}

// settings.json written whole from the bag's usersettings (plan §41.11a
// step 3e), beside its reader, user_settings_load. The configuration
// directory is made when missing. False, with the reason on the status line,
// when there is none or the write fails.
// Thread contract: the session that changed a setting writes the file;
// between processes the last write wins.
bool user_settings_save(long w, long es)
{
    var cfg;
    madcide_config_dir(cfg);
    if ( strlen(cfg.c_str()) == 0 )
    {
	ui::set(w, es, "msg", "No configuration directory: the choice is not kept.");
	return false;
    }
    var path = format("{}/settings.json", cfg);
    var us;
    ui::get(us, w, es, "usersettings");
    if ( !dir_ensure(cfg)
      || php::file_put_contents(path.c_str(), format("{}\n", js::stringify(us, 2))) < 0 )
    {
	ui::set(w, es, "msg", format("{} cannot be written: the choice is not kept.", path));
	return false;
    }
    return true;
}

// The key style a bundle keeps (plan §41.11a step 3e): settings.json's
// "keys" object, by bundle name (`"keys": { "chthonic": "vscode" }`), so a
// choice made in one bundle leaves the others' keys as they ship. "" when
// the bundle has none.
void keys_choice(var &out, long w, long es)
{
    out = "";
    var us;
    ui::get(us, w, es, "usersettings");
    var ks;
    keyed_get(ks, us, "keys");
    if ( ks.is_null() || !ks.is_object() )
	return;
    var b;
    ui::get(b, w, es, "bundle");
    var v;
    keyed_get(v, ks, b.c_str());
    if ( !v.is_null() && v.is_string() )
	out = v;
}

// Keep `name` as the bundle's key style, and write settings.json.
bool keys_choice_save(long w, long es, const char *name0)
{
    var name = name0;		// ring discipline: own the text first
    var us;
    ui::get(us, w, es, "usersettings");
    var ks;
    keyed_get(ks, us, "keys");
    var b;
    ui::get(b, w, es, "bundle");
    if ( ks.is_null() || !ks.is_object() )
    {
	var fresh;
	fresh[b.c_str()] = name;
	ks = fresh;
    }
    else
	ks[b.c_str()] = name;
    if ( us.is_null() || !us.is_object() )
    {
	var top = { "keys": ks };
	us = top;
    }
    else
	us["keys"] = ks;
    ui::set(w, es, "usersettings", us);
    return user_settings_save(w, es);
}

void path_sans_ext(var &out, var &p);	// madcide_core.inc: THE {base} rule

// The name the program was invoked by: madc::sys.argv[0]'s last path
// component without its extension, so /usr/bin/chthonic, chthonic.exe and
// the script tools/madcide/madcide.mad read chthonic, chthonic and madcide.
// A symbolic or hard link is invoked under its own name.
void program_name(var &out)
{
    out = "";
    if ( php::count(madc::sys.argv) == 0 )
	return;
    var a0 = madc::sys.argv[0];
    var base = php::basename(a0);
    path_sans_ext(out, base);
}

// WHICH bundle (§5.2): the command line's --profile (`cli`, "" = not
// given), then the program's own name when a bundle of that name exists (a
// `chthonic` binary, or a link named chthonic, opens the chthonic bundle
// with no flag; owner 2026-09-30), then settings.json's "profile", then
// `default`, so nothing changes without one of them.
void active_profile(var &out, long w, long es, const char *cli)
{
    out = "default";
    if ( cli != 0 && strlen(cli) > 0 )
    {
	out = cli;
	return;
    }
    var pn;
    program_name(pn);
    var pdir;
    if ( plugin_name_ok(pn.c_str()) && plugin_find(pdir, pn.c_str()) )
    {
	out = pn;
	return;
    }
    var us;
    ui::get(us, w, es, "usersettings");
    var p;
    keyed_get(p, us, "profile");
    if ( p.is_null() )
	return;
    if ( !p.is_string() )
    {
	ui::set(w, es, "msg", "settings.json's \"profile\" is not a name — ignored.");
	return;
    }
    out = p;
}

// The default bundle onto the bag ("bundlebase"): the search path's
// default.plugin, else the built-in one. False = a default.plugin was found
// and refused (`why` says why; the built-in one is in use).
bool bundle_base_load(long w, long es, var &dir, var &why)
{
    dir = "";
    var base;
    var dd;
    bool ok = true;
    if ( plugin_find(dd, "default") )
    {
	if ( plugin_manifest_read(base, why, dd, "default") )
	    dir = dd;
	else
	    ok = false;
    }
    if ( strlen(dir.c_str()) == 0 )
	js::parse(base, default_bundle_text());
    ui::set(w, es, "bundlebase", base);
    return ok;
}

// Bundle `name` onto the bag (bundle, bundledir, bundleman, bundlebase). A
// bundle not found, or refused, leaves the default in use, and the status
// line says why. False = the bundle asked for is not the one in use.
bool bundle_select(long w, long es, const char *name0)
{
    var name = name0;		// ring discipline: own the text first
    var bdir;
    var bwhy = "";
    bool base_ok = bundle_base_load(w, es, bdir, bwhy);
    var why = "";
    var dir;
    var m;
    bool found = false;
    if ( name == "default" )
	why = "";		// the default bundle IS bundlebase
    else if ( !plugin_name_ok(name.c_str()) )
	why = format("'{}' is not a profile name", name);
    else if ( !plugin_find(dir, name.c_str()) )
	why = format("No profile '{}'", name);
    else if ( plugin_manifest_read(m, why, dir, name.c_str()) )
	found = true;
    else
	why = format("Profile '{}' refused: {}", name, why);
    if ( !base_ok )
	ui::set(w, es, "msg", format("Profile 'default' refused: {} — the built-in default is in use.", bwhy));
    if ( found )
    {
	ui::set(w, es, "bundle", name);
	ui::set(w, es, "bundledir", dir);
	ui::set(w, es, "bundleman", m);
	return true;
    }
    var base;
    ui::get(base, w, es, "bundlebase");
    ui::set(w, es, "bundle", "default");
    ui::set(w, es, "bundledir", bdir);
    ui::set(w, es, "bundleman", base);
    if ( strlen(why.c_str()) > 0 )
    {
	ui::set(w, es, "msg", format("{} — the default profile is in use.", why));
	return false;
    }
    return base_ok;
}

// One manifest's name for a data kind (`"layout": "chthonic"`); null when
// it names none.
void manifest_word(var &out, var &m, long kind)
{
    var c;
    keyed_get(c, m, plugin_field_name(pfCONTRIBUTES));
    var v;
    keyed_get(v, c, contribution_kind_name(kind));
    out = v;
}

// The default bundle's name for a data kind (the fallback word).
void base_word(var &out, long w, long es, long kind)
{
    var base;
    ui::get(base, w, es, "bundlebase");
    manifest_word(out, base, kind);
}

// The name the active bundle gives a data kind, else the default bundle's:
// the one reader of a bundle's words.
void bundle_word(var &out, long w, long es, long kind)
{
    var m;
    ui::get(m, w, es, "bundleman");
    var v;
    manifest_word(v, m, kind);
    if ( v.is_null() )
	base_word(v, w, es, kind);
    out = v;
}

// Where data file <name>.<kind> lives (the kind's word is the extension):
// the active bundle's own directory first (a bundle carries its own layout,
// menu, keys ...), then the shared profiles directory. The one path every
// data loader reads.
void bundle_data_path(var &out, long w, long es, const char *name0, long kind)
{
    var name = name0;		// ring discipline: own the text first
    var ext = contribution_kind_name(kind);
    var bdir;
    ui::get(bdir, w, es, "bundledir");
    if ( !bdir.is_null() && strlen(bdir.c_str()) > 0 )
    {
	var own = format("{}/{}.{}", bdir, name, ext);
	if ( php::file_exists(own) )
	{
	    out = own;
	    return;
	}
    }
    var dir;
    ui::get(dir, w, es, "profile_dir");
    out = format("{}/{}.{}", dir, name, ext);
}

// One manifest's default for setting `key`; null when it sets none.
void manifest_setting(var &out, var &m, const char *key)
{
    var c;
    keyed_get(c, m, plugin_field_name(pfCONTRIBUTES));
    var s;
    keyed_get(s, c, contribution_kind_name(cbSETTINGS));
    var v;
    keyed_get(v, s, key);
    out = v;
}

// A setting's value (§5.2): the user's (settings.json), else the active
// bundle's default, else the default bundle's. Null = no one sets it.
void setting_get(var &out, long w, long es, const char *key0)
{
    var key = key0;		// ring discipline: own the text first
    var us;
    ui::get(us, w, es, "usersettings");
    var v;
    keyed_get(v, us, key.c_str());
    if ( v.is_null() )
    {
	var m;
	ui::get(m, w, es, "bundleman");
	manifest_setting(v, m, key.c_str());
    }
    if ( v.is_null() )
    {
	var base;
	ui::get(base, w, es, "bundlebase");
	manifest_setting(v, base, key.c_str());
    }
    out = v;
}

// ---- contributed commands (Stage B1, step 2; §5.3, G4) ---------------------
// The registry is one world entity, `plugins` (§5.3's storage note): every
// client of the world sees the same commands. A command's handler is its
// function's ADDRESS, kept as an integer and called through cmd_handler
// (<madcide/plugin_api>) — a builtin handler's `(long)&fn`, and a library
// plugin's, registered by its activation through the API table
// (madcide_plugin_code.inc). Codes are interned from cmd_contrib_base()
// upward, in registration order.
// Thread contract: registration runs on the session's thread before the
// profiles and menus load; the rows are read-only afterwards.

// The first contributed code: above every ide_cmd enumerator.
long cmd_contrib_base()
{
    return 4096;
}

// Is a code a contributed command's (the range above every enumerator)?
bool cmd_contributed(long code)
{
    return code >= cmd_contrib_base();
}

// The world's registry entity; 0 = nothing registered yet.
long plugin_registry(long w)
{
    return ui::entity_by_name(w, "plugins");
}

// The world's registry entity, made on the first registration.
long plugin_registry_make(long w)
{
    long reg = plugin_registry(w);
    if ( reg == 0 )
	reg = ui::create(w, "plugins");
    return reg;
}

// One of the registry's lists ("commands", "views"): its rows, an empty
// array when none.
void registry_rows(var &out, long w, const char *list)
{
    var fresh = {};
    out = fresh;
    long reg = plugin_registry(w);
    if ( reg == 0 )
	return;
    var rows;
    ui::get(rows, w, reg, list);
    if ( !rows.is_null() )
	out = rows;
}

// The row of a registry list whose integer field holds a value, or null.
void registry_row(var &out, long w, const char *list, const char *field,
		  long value)
{
    var none;
    out = none;
    var rows;
    registry_rows(rows, w, list);
    for ( var r : rows )
	if ( r[field].as_integer() == value )
	{
	    out = r;
	    return;
	}
}

// The contributed commands' rows (an empty array when none).
void contributed_commands(var &out, long w)
{
    registry_rows(out, w, "commands");
}

// The contributed command a code names: its row, or null when none does.
void contributed_row(var &out, long w, long code)
{
    registry_row(out, w, "commands", "code", code);
}

// THE name <-> code table of a world: the built-in rows (cmd_table), then
// the contributed ones. Every input boundary converts through it, so a key
// profile, a menu, the -c line and the seat accept a contributed name.
void cmd_table_w(var &t, long w)
{
    cmd_table(t);
    var rows;
    contributed_commands(rows, w);
    for ( var r : rows )
	t[] = { "name": r["name"], "code": r["code"] };
}

long cmd_of_w(long w, const char *name)
{
    var t;
    cmd_table_w(t, w);
    return cmd_of_in(t, name);
}

// A code to the name a wire or a message spells, contributed codes included.
void cmd_name_w(var &out, long w, long code)
{
    if ( !cmd_contributed(code) )
    {
	cmd_name(out, code);
	return;
    }
    var r;
    contributed_row(r, w, code);
    out = "";
    if ( !r.is_null() )
	out = r["name"];
}

// Register a contributed command. The name must be a plugin name
// (plugin_name_ok) and new — neither a built-in's nor an earlier
// contribution's; the handler must be a function (cmd_handler, kept as its
// full-width address: a pointer never fits a Windows `long`). The code it is
// given; 0 = refused.
long plugin_command(long w, const char *name0, const char *title0,
		    cmd_handler handler)
{
    var name = name0;		// ring discipline: own the text first
    var title = title0;
    if ( handler == 0 || !plugin_name_ok(name.c_str()) )
	return 0;
    if ( cmd_of_w(w, name.c_str()) != cmdNONE )
	return 0;
    long reg = plugin_registry_make(w);
    var rows;
    contributed_commands(rows, w);
    long code = cmd_contrib_base() + php::count(rows);
    int64_t addr = (int64_t)handler;
    rows[] = { "name": name, "code": code, "title": title, "handler": addr };
    ui::set(w, reg, "commands", rows);
    return code;
}

// Run a contributed command's handler with the command's argument. False =
// the handler ended the session (the dispatcher's continue / quit verdict).
bool run_contributed(long w, long es, long doc, long code, const char *arg0)
{
    var arg = arg0;		// ring discipline: own the text first
    var r;
    contributed_row(r, w, code);
    if ( r.is_null() )
    {
	ui::set(w, es, "msg", format("Unknown command {}.", code));
	return true;
    }
    cmd_handler h = (cmd_handler)r["handler"].as_integer();
    return h(w, es, doc, arg.c_str());
}

// The seat tier a command needs (V6a tiers), contributed commands included:
// a contributed command's effects are its handler's, which the core cannot
// see, so it is gated as an editor-tier mutation (the fail-safe default).
bool cmd_view_row(long code);	// a contributed view's row verb (below)
long cmd_min_tier_w(long w, long code)
{
    if ( cmd_contributed(code) )
	return tierEDITOR;
    if ( cmd_view_row(code) )
	return cmd_min_tier(cmdGOTO);	// a row's activation is navigation
    return cmd_min_tier(code);
}

// ---- contributed event handlers (Stage B1, step 2; §5.3) -------------------
// A plugin subscribes a handler to a kind of the REPL pane's reply events
// (repl_event); the pane publishes each reply it handles, and every handler
// subscribed to its kind runs, in registration order, with the event
// { kind, reply } (the reply's row as the session sent it). A handler keeps a
// view's rows current or posts commands; it never holds the pane's session.
// The handler's contract is event_handler (<madcide/plugin_api>).
// Thread contract: handlers run on the session's thread, between events,
// never during composition.

// Subscribe a handler to a kind (kept as its full-width address, as a
// command's is). False = refused: a kind outside repl_event or no handler.
bool plugin_event(long w, long kind, event_handler handler)
{
    if ( handler == 0 || kind <= reNONE || kind >= reLAST )
	return false;
    long reg = plugin_registry_make(w);
    var rows;
    registry_rows(rows, w, "events");
    int64_t addr = (int64_t)handler;
    rows[] = { "kind": kind, "handler": addr };
    ui::set(w, reg, "events", rows);
    return true;
}

// Publish an event of a kind to its subscribers (none: nothing runs).
void publish_event(long w, long es, long doc, long kind, var &reply)
{
    var rows;
    registry_rows(rows, w, "events");
    var ev = { "kind": kind, "reply": reply };
    for ( var r : rows )
	if ( r["kind"].as_integer() == kind )
	{
	    event_handler h = (event_handler)r["handler"].as_integer();
	    h(w, es, doc, ev);
	}
}

// ---- contributed views (Stage B1, step 2; §5.3) -----------------------------
// A contributed view is DATA: a name (the layout profile's word), a title
// (its tab), a bag key (its rows, which the plugin's code keeps current, as
// check keeps `diags` for Problems) and the command that shows it (a tab's
// click; cmdNONE = none). Composition renders the key's rows and never calls
// plugin code. Kinds are interned from view_contrib_base() upward, above
// every ide_view enumerator. Each view's ROW verb (a choose on its rows) is
// its kind's image in [cmd_view_row_base(), cmd_contrib_base()): the code
// names the list, as Problems' rows carry cmdGOTO and Outline's cmdGOTODEF.
// A row is { content, file?, line? }: its activation goes to the line
// through goto_pane_row, the one navigation owner.
long view_contrib_base()
{
    return 128;
}

bool view_contributed(long kind)
{
    return kind >= view_contrib_base();
}

long cmd_view_row_base()
{
    return 2048;
}

// Is a code a contributed view's row verb?
bool cmd_view_row(long code)
{
    return code >= cmd_view_row_base() && code < cmd_contrib_base();
}

// The ROW verb of a list view (a choose on its rows posts it); cmdNONE = a
// kind whose rows are not a navigable list.
long view_row_cmd(long kind)
{
    if ( kind == viewPROBLEMS )
	return cmdGOTO;
    if ( kind == viewOUTLINE )
	return cmdGOTODEF;
    if ( view_contributed(kind) )
	return cmd_view_row_base() + kind - view_contrib_base();
    return cmdNONE;
}

// The list view a row verb names (view_row_cmd's inverse); viewNONE = none.
long view_of_row_cmd(long code)
{
    if ( code == cmdGOTO )
	return viewPROBLEMS;
    if ( code == cmdGOTODEF )
	return viewOUTLINE;
    if ( cmd_view_row(code) )
	return view_contrib_base() + code - cmd_view_row_base();
    return viewNONE;
}

// The contributed views' rows (an empty array when none).
void contributed_views(var &out, long w)
{
    registry_rows(out, w, "views");
}

// A layout word to its view kind: the built-in names, then the world's
// contributed views; viewNONE = neither (the layout reader's refusal).
long view_of_w(long w, var &name)
{
    long k = view_of(name);
    if ( k != viewNONE || name.is_null() )
	return k;
    var rows;
    contributed_views(rows, w);
    for ( var r : rows )
	if ( r["name"] == name )
	    return r["kind"].as_integer();
    return viewNONE;
}

// One text field of a view: a built-in's from its enum's converter, a
// contributed view's from its row ("" when the world has no such view).
void view_field_w(var &out, long w, long kind, const char *field)
{
    if ( !view_contributed(kind) )
    {
	if ( strcmp(field, "title") == 0 )
	    out = view_title(kind);
	else
	    out = view_name(kind);
	return;
    }
    var r;
    registry_row(r, w, "views", "kind", kind);
    out = "";
    if ( !r.is_null() )
	out = r[field];
}

void view_name_w(var &out, long w, long kind)
{
    view_field_w(out, w, kind, "name");
}

void view_title_w(var &out, long w, long kind)
{
    view_field_w(out, w, kind, "title");
}

// The command that shows a view (a tab's click), contributed views included.
long view_show_cmd_w(long w, long kind)
{
    if ( !view_contributed(kind) )
	return view_show_cmd(kind);
    var r;
    registry_row(r, w, "views", "kind", kind);
    return r.is_null() ? cmdNONE : r["show"].as_integer();
}

// The bag key a list view's rows live on ("" = not a list view).
void view_rows_key(var &out, long w, long kind)
{
    out = "";
    if ( kind == viewPROBLEMS )
	out = "diags";
    else if ( kind == viewOUTLINE )
	out = "outline";
    else if ( view_contributed(kind) )
    {
	var r;
	registry_row(r, w, "views", "kind", kind);
	if ( !r.is_null() )
	    out = r["key"];
    }
}

// A contributed view's bag key is the plugin's own: `plugin.name`, a plugin
// name with a dot inside it, so it never aliases a core key (none has one).
bool view_key_ok(const char *key)
{
    long n = strlen(key);
    if ( !plugin_name_ok(key) || key[n - 1] == '.' )
	return false;
    for ( long i = 1; i < n; i = i + 1 )
	if ( key[i] == '.' )
	    return true;
    return false;
}

// Register a contributed view. The name must be a plugin name and new
// (neither a built-in view's nor an earlier contribution's), the key the
// plugin's own (view_key_ok), and `show` cmdNONE or a contributed command.
// The kind it is given; viewNONE = refused.
long plugin_view(long w, const char *name0, const char *title0,
		 const char *key0, long show)
{
    var name = name0;		// ring discipline: own the text first
    var title = title0;
    var key = key0;
    if ( !plugin_name_ok(name.c_str()) || !view_key_ok(key.c_str()) )
	return viewNONE;
    if ( view_of_w(w, name) != viewNONE )
	return viewNONE;
    if ( show != cmdNONE )
    {
	var sr;
	contributed_row(sr, w, show);
	if ( sr.is_null() )
	    return viewNONE;
    }
    long reg = plugin_registry_make(w);
    var rows;
    contributed_views(rows, w);
    long kind = view_contrib_base() + php::count(rows);
    if ( view_row_cmd(kind) >= cmd_contrib_base() )
	return viewNONE;	// the row verbs' range is full
    rows[] = { "name": name, "kind": kind, "title": title, "key": key,
	       "show": show };
    ui::set(w, reg, "views", rows);
    return kind;
}
