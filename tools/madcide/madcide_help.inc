// madcide_help.inc — Help's topics (plan docs/plans/2026-10-03-chthonia-
// windows-macos.md §7d): a bundle's Markdown pages, read through the
// markdown:: namespace (the madcmark module) and listed in the Help view.
//
// A bundle contributes its help as a directory: `"help": "chthonia"` names
// chthonia.help/ beside its manifest — one Markdown file per topic, the first
// contents.md — and its commands.md describes commands, a `## NAME` heading
// per registered command and the paragraph under it. A topic, or a
// command's description, the bundle does not have is the default bundle's.
// One topic is made, not read: `menus`, every menu of the loaded menus with
// each item's key in the current key style, its command and its description.
//
// The Help view (viewHELP) lists the topic's ROWS, made by the one Markdown
// renderer (madcide_markdown.inc): a heading, a paragraph, a list item, a
// code line, a table row. A link is a row of its own whose target is a topic
// (`name.md`, `menus`) or a URL; choosing it opens the topic (helptopic) or
// says the URL. The rows are session state on the bag:
// "helprows" ({content, target}), the topic's name on "helptopic", its title
// on "helptitle".
//
// Thread contract: the session's thread; the rows are the session's.

bool show_view(long w, long es, long kind);	// madcide_core.inc, the one show owner

// A help directory: the active bundle's (base false) or the default
// bundle's; "" when that bundle contributes none.
void help_dir(var &out, long w, long es, bool base)
{
    var m;
    var dir;
    if ( base )
    {
	ui::get(m, w, es, "bundlebase");
	ui::get(dir, w, es, "bundlebasedir");
    }
    else
    {
	ui::get(m, w, es, "bundleman");
	ui::get(dir, w, es, "bundledir");
    }
    var word;
    manifest_word(word, m, cbHELP);
    out = "";
    if ( word.is_null() || dir.is_null() || strlen(dir.c_str()) == 0 )
	return;
    out = format("{}/{}.help", dir, word);
}

// A help file of one bundle (base: the default bundle's). False when it has
// none.
bool help_file_of(var &out, long w, long es, const char *name0, bool base)
{
    var name = name0;		// ring discipline: own the text first
    var dir;
    help_dir(dir, w, es, base);
    if ( strlen(dir.c_str()) == 0 )
	return false;
    var path = format("{}/{}", dir, name);
    return php::file_exists(path) && php::file_get_contents(out, path.c_str());
}

// A help file: the active bundle's, else the default bundle's.
bool help_file(var &out, long w, long es, const char *name0)
{
    var name = name0;		// ring discipline: own the text first
    return help_file_of(out, w, es, name.c_str(), false)
	|| help_file_of(out, w, es, name.c_str(), true);
}

// The command descriptions: commands.md's `## NAME` headings and the
// paragraph under each — the default bundle's, then the active bundle's over
// them, by command name. A heading that names no registered command is said
// on the status line and skipped (a misspelling is a refusal at load, never
// a description nothing shows).
void help_descriptions(var &out, long w, long es)
{
    var d;
    for ( long b = 1; b >= 0; b = b - 1 )
    {
	var text;
	if ( !help_file_of(text, w, es, "commands.md", b == 1) )
	    continue;
	var doc;
	markdown::parse(doc, text.c_str());
	var name = "";
	for ( var blk : doc["children"] )
	{
	    if ( blk["kind"] == markdown::node_kind::heading )
	    {
		name = "";
		md_inline(name, blk);
		name = php::trim(name);
		if ( cmd_of_w(w, name.c_str()) == cmdNONE )
		{
		    ui::set(w, es, "msg", format("Help: commands.md names no command '{}'.", name));
		    name = "";
		}
	    }
	    else if ( blk["kind"] == markdown::node_kind::paragraph && strlen(name.c_str()) > 0 )
	    {
		var t = "";
		md_inline(t, blk);
		d[name.c_str()] = t;
		name = "";
	    }
	}
    }
    out = d;
}

// The made topic, `menus`: every menu bar menu of the loaded menus, each
// item's title, its key in the loaded key style, its command and its
// description, as Markdown the one renderer reads.
void help_menus_text(var &out, long w, long es)
{
    var def;
    ui::get(def, w, es, "menudef");
    var table;
    ui::get(table, w, es, "keytable");
    var desc;
    help_descriptions(desc, w, es);
    var text = "# Menus and commands\n\nEvery menu and what its items do, with the key that runs each in the current key style.\n";
    if ( !def.is_null() )
	for ( var m : def )
	{
	    if ( m["place"].as_integer() != mpBAR )
		continue;
	    text = format("{}\n## {}\n\n", text, m["title"]);
	    for ( var it : m["items"] )
	    {
		if ( !it["sep"].is_null() )
		    continue;
		var line = format("- **{}**", it["title"]);
		var key;
		if ( ui::key_sequence_for(key, table, it["code"].as_integer()) )
		    line = format("{} (`{}`)", line, key);
		line = format("{} — `{}`", line, it["id"]);
		var dt;
		keyed_get(dt, desc, it["id"].c_str());
		if ( !dt.is_null() )
		    line = format("{}: {}", line, dt);
		text = format("{}{}\n", text, line);
	    }
	}
    out = text;
}

// A link's target as a topic: `name.md` (a file of the help directory) or
// `menus`; "" for anything else (a URL, a path, an anchor).
void help_target_topic(var &out, var &url)
{
    out = "";
    var u = format("{}", url);
    if ( u == "menus" )
    {
	out = u;
	return;
    }
    if ( perl::index(u.c_str(), "/") >= 0 || perl::index(u.c_str(), "#") >= 0
      || rust::ends_with(u.c_str(), ".md") == 0 )
	return;
    out = perl::substr(u, 0, strlen(u.c_str()) - 3);
}

// Open topic `name` in the Help view: its file's rows, or the made topic's.
// False, with the reason on the status line, for a topic no help directory
// has, or without the madcmark module.
bool help_open(long w, long es, const char *name0)
{
    var name = name0;		// ring discipline: own the text first
    if ( !markdown::available() )
    {
	ui::set(w, es, "msg", "Help needs the madcmark module (markdown::), which this installation does not have.");
	return false;
    }
    var text;
    if ( name == "menus" )
	help_menus_text(text, w, es);
    else
    {
	var file = format("{}.md", name);
	if ( !help_file(text, w, es, file.c_str()) )
	{
	    ui::set(w, es, "msg", format("No help topic '{}'.", name));
	    return false;
	}
    }
    var doc;
    markdown::parse(doc, text.c_str());
    var rows = {};
    if ( !(name == "contents") )
    {
	rows[] = { "content": "◂ Contents", "target": "contents.md" };
	rows[] = { "content": "" };
    }
    var blocks = doc["children"];
    md_blocks_rows(rows, blocks, "", "");
    var title = format("{}", name);
    for ( var b : doc["children"] )
	if ( b["kind"] == markdown::node_kind::heading )
	{
	    title = "";
	    md_inline(title, b);
	    break;
	}
    ui::set(w, es, "helprows", rows);
    ui::set(w, es, "helptopic", name);
    ui::set(w, es, "helptitle", title);
    show_view(w, es, viewHELP);
    return true;
}

// helptopic's argument, else the topic open now, else the contents.
bool help_topic_cmd(long w, long es, var &ev)
{
    var ta;
    keyed_get(ta, ev, "arg");
    var topic = "";
    if ( !ta.is_null() )
	topic = format("{}", ta);
    if ( strlen(topic.c_str()) == 0 )
    {
	var cur;
	ui::get(cur, w, es, "helptopic");
	topic = "contents";
	if ( !cur.is_null() && strlen(cur.c_str()) > 0 )
	    topic = cur;
    }
    return help_open(w, es, topic.c_str());
}

// A choose on a Help row (1-based `option`): a link row opens its topic, or
// says its URL; another row does nothing.
void help_choose(long w, long es, long option)
{
    var rows;
    ui::get(rows, w, es, "helprows");
    if ( rows.is_null() || option < 1 || option > php::count(rows) )
	return;
    var target;
    keyed_get(target, rows[option - 1], "target");
    if ( target.is_null() )
	return;
    var topic;
    help_target_topic(topic, target);
    if ( strlen(topic.c_str()) > 0 )
	help_open(w, es, topic.c_str());
    else
	ui::set(w, es, "msg", format("Link: {}", target));
}

// The Help view's node: the topic's rows as a list, a link row carrying
// helptopic's code (a choose posts it with the row).
void help_view_node(var &out, long w, long es, bool focus)
{
    var rows;
    ui::get(rows, w, es, "helprows");
    var items = {};
    if ( !rows.is_null() )
	for ( var r : rows )
	{
	    var it = { "role": "item", "content": r["content"] };
	    var t;
	    keyed_get(t, r, "target");
	    if ( !t.is_null() )
	    {
		var gh = { "code": cmdHELPTOPIC };
		it["hints"] = gh;
	    }
	    items[] = it;
	}
    if ( php::count(items) == 0 )
	items[] = { "role": "item", "content": "(no topic open)" };
    var title;
    ui::get(title, w, es, "helptitle");
    var cb = { "role": "choice", "label": "Help" };
    if ( !title.is_null() && strlen(title.c_str()) > 0 )
	cb["label"] = format("Help — {}", title);
    cb["children"] = items;
    var ch = { "list": 1 };
    if ( focus )
	ch["focus"] = 1;
    cb["hints"] = ch;
    out = cb;
}
