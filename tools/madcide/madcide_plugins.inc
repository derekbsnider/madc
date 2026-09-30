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

// The plugin API version this madcide serves. A manifest's "api" names the
// version it targets.
long plugin_api_served()
{
    return 1;
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
