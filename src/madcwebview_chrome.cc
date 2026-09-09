// madcwebview_chrome.cc — madc's native chrome around the webview (API in
// madcwebview_chrome.h): the menu bar over the window (madcide GUI chrome
// S2) and the platform's file dialogs (S4), on all three platforms the
// library builds for.
//
//   GTK4:  a GtkPopoverMenuBar fed by a GMenu model, its items bound to a
//          GSimpleActionGroup inserted on the window under the `menu.`
//          prefix; the webview widget is re-parented into a vertical box
//          under the bar (upstream sets the widget as the window's child
//          and never touches it again). Dialogs: GtkFileDialog (4.10+).
//   Cocoa: the application's main menu (NSMenu on NSApp — the bar lives at
//          the top of the screen, the window is untouched); each item
//          targets one runtime-registered object whose action reads the
//          item's tag back into the command id. The first submenu is the
//          application menu macOS names after the process; its Quit is the
//          window's own close (performClose:), the close button's path.
//          Dialogs: NSOpenPanel / NSSavePanel as a sheet on the window.
//   Win32: an HMENU bar set on the window (SetMenu; the client area shrinks
//          and the frame change re-lays the webview's child), WM_COMMAND
//          read through a comctl32 subclass of upstream's window procedure.
//          Dialogs: IFileOpenDialog / IFileSaveDialog (COM), shown from a
//          message the subclass posts to itself so the call returns first.
//
// Every menu call rebuilds the model from scratch — the engine sends the
// whole menu whenever any item changed, so there is no per-item state to
// keep in step. The key spelling is read ONCE, by describe_key below; each
// platform maps the kind it names onto its own vocabulary and never reads
// the spelling itself.
#define WEBVIEW_HEADER 1
#include "webview/webview.h"
#include "madcwebview_chrome.h"

#include <map>
#include <string>
#include <vector>

// ---- the one reading of a madc key spelling ------------------------------
// Which single key the spelling names — a control letter ("^s"), a control
// punctuation ("^_" "^^" "^]" "^\"), a named key ("pgdn"), a printable —
// or no single key at all: a chord (a space in the spelling) or an unknown
// form. Only a CONTROL key becomes a platform accelerator: a bare named key
// or printable bound as a menu accelerator would fire the command on every
// such keystroke typed into the page, so those show beside the title like a
// chord does (the platform never binds them; the page still delivers the
// key to the engine, which resolves it exactly as the terminal would).
namespace {

struct key_desc {
	enum kind_t { none, ctrl_letter, ctrl_punct, named, printable };
	kind_t kind;
	char ch;		// ctrl_letter: the lower-case letter; ctrl_punct / printable: the char
	std::string name;	// named: the spelling ("pgdn")
	key_desc() : kind(none), ch(0) {}
};

const char *const named_keys[] = {
	"enter", "tab", "backspace", "esc", "up", "down", "left", "right",
	"home", "end", "pgup", "pgdn", "del", "ins", "space",
};

key_desc describe_key(const std::string &key)
{
	key_desc d;
	if (key.empty() || key.find(' ') != std::string::npos)
		return d;
	for (size_t i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); i++)
		if (key == named_keys[i]) {
			d.kind = key_desc::named;
			d.name = key;
			return d;
		}
	if (key.size() == 2 && key[0] == '^') {
		char c = key[1];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
			d.kind = key_desc::ctrl_letter;
			d.ch = (char)(c | 0x20);
		} else if (c == '_' || c == '^' || c == ']' || c == '\\') {
			d.kind = key_desc::ctrl_punct;
			d.ch = c;
		}
		return d;
	}
	if (key.size() == 1) {
		d.kind = key_desc::printable;
		d.ch = key[0];
	}
	return d;
}

bool is_accelerator(const key_desc &d)
{
	return d.kind == key_desc::ctrl_letter || d.kind == key_desc::ctrl_punct;
}

// A key shown in a label: JOE's own spelling, letters upper-cased ("^k d"
// -> "^K D").
std::string chord_label(const std::string &key)
{
	std::string out;
	for (size_t i = 0; i < key.size(); i++) {
		char c = key[i];
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		out += c;
	}
	return out;
}

// The label an item carries where the platform has no display-only
// accelerator column (GTK, Cocoa): the title, and after it the key the
// platform does not bind — a chord, a bare key.
std::string item_label(const char *title, const std::string &key, const key_desc &d)
{
	std::string label = title;
	if (!key.empty() && !is_accelerator(d))
		label += "   " + chord_label(key);
	return label;
}

} // namespace

#if !defined(_WIN32) && !defined(__APPLE__)

// ======================================================================
// GTK4 / WebKitGTK
// ======================================================================

#include <gtk/gtk.h>

namespace {

struct submenu {
	std::string title;
	GMenu *menu;		// the submenu model
	GMenu *section;		// the section items are appended to
};

struct menu_state {
	GtkWidget *win;		// the webview's window (native handle)
	GtkWidget *view;	// the webview widget (native handle)
	GtkWidget *box;		// our vertical box: bar over view
	GtkWidget *bar;		// the GtkPopoverMenuBar
	GSimpleActionGroup *group;	// the LIVE action group ("menu.")
	GMenu *root;		// the model being BUILT (begin .. end)
	GSimpleActionGroup *building;	// the group being built
	std::vector<submenu> menus;	// first-appearance order
	madcwebview_menu_action_fn cb;
	void *arg;
	menu_state() : win(0), view(0), box(0), bar(0), group(0), root(0),
		       building(0), cb(0), arg(0) {}
};

std::map<webview_t, menu_state> &states()
{
	static std::map<webview_t, menu_state> s;
	return s;
}

struct action_ctx {
	menu_state *st;
	std::string id;
};

void on_activate(GSimpleAction *, GVariant *, gpointer data)
{
	action_ctx *c = static_cast<action_ctx *>(data);
	if (c->st->cb)
		c->st->cb(c->id.c_str(), c->st->arg);
}

void free_ctx(gpointer data, GClosure *)
{
	delete static_cast<action_ctx *>(data);
}

// The GTK accelerator for a control key ("^s" -> "<Control>s"); "" for
// anything the platform does not bind (see describe_key).
std::string accel_of(const key_desc &d)
{
	switch (d.kind) {
	case key_desc::ctrl_letter:
		return std::string("<Control>") + d.ch;
	case key_desc::ctrl_punct: {
		static const struct { char c; const char *gtk; } punct[] = {
			{ '_', "<Control>underscore" }, { '^', "<Control>asciicircum" },
			{ ']', "<Control>bracketright" }, { '\\', "<Control>backslash" },
		};
		for (size_t i = 0; i < sizeof(punct) / sizeof(punct[0]); i++)
			if (d.ch == punct[i].c)
				return punct[i].gtk;
		return std::string();
	}
	default:
		return std::string();
	}
}

submenu *find_menu(menu_state &st, const char *title, bool create)
{
	for (size_t i = 0; i < st.menus.size(); i++)
		if (st.menus[i].title == title)
			return &st.menus[i];
	if (!create)
		return 0;
	submenu m;
	m.title = title;
	m.menu = g_menu_new();
	m.section = g_menu_new();
	g_menu_append_section(m.menu, NULL, G_MENU_MODEL(m.section));
	g_object_unref(m.section);	// the submenu holds it
	g_menu_append_submenu(st.root, title, G_MENU_MODEL(m.menu));
	g_object_unref(m.menu);		// the root holds it
	st.menus.push_back(m);
	return &st.menus.back();
}

menu_state *state_of(webview_t w)
{
	if (!w)
		return 0;
	menu_state &st = states()[w];
	if (!st.win) {
		st.win = (GtkWidget *)webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
		st.view = (GtkWidget *)webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET);
	}
	if (!st.win || !st.view)
		return 0;
	return &st;
}

} // namespace

extern "C" {

WEBVIEW_API int madcwebview_menu_begin(webview_t w)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	if (st->root)
		g_object_unref(st->root);
	if (st->building)
		g_object_unref(st->building);
	st->root = g_menu_new();
	st->building = g_simple_action_group_new();
	st->menus.clear();
	return 0;
}

WEBVIEW_API int madcwebview_menu_add(webview_t w, const char *menu, const char *id,
				     const char *title, const char *key, int enabled)
{
	menu_state *st = state_of(w);
	if (!st || !st->root || !menu || !id || !*id || !title)
		return 1;
	submenu *m = find_menu(*st, menu, true);
	const std::string k = key ? key : "";
	const key_desc d = describe_key(k);
	const std::string accel = accel_of(d);
	const std::string label = item_label(title, k, d);
	const std::string detailed = std::string("menu.") + id;
	GMenuItem *item = g_menu_item_new(label.c_str(), detailed.c_str());
	if (!accel.empty())
		g_menu_item_set_attribute(item, "accel", "s", accel.c_str());
	g_menu_append_item(m->section, item);
	g_object_unref(item);
	GSimpleAction *act = g_simple_action_new(id, NULL);
	g_simple_action_set_enabled(act, enabled != 0);
	action_ctx *ctx = new action_ctx;
	ctx->st = st;
	ctx->id = id;
	g_signal_connect_data(act, "activate", G_CALLBACK(on_activate), ctx,
			      free_ctx, (GConnectFlags)0);
	g_action_map_add_action(G_ACTION_MAP(st->building), G_ACTION(act));
	g_object_unref(act);
	return 0;
}

WEBVIEW_API int madcwebview_menu_separator(webview_t w, const char *menu)
{
	menu_state *st = state_of(w);
	if (!st || !st->root || !menu)
		return 1;
	submenu *m = find_menu(*st, menu, true);
	m->section = g_menu_new();
	g_menu_append_section(m->menu, NULL, G_MENU_MODEL(m->section));
	g_object_unref(m->section);
	return 0;
}

WEBVIEW_API int madcwebview_menu_end(webview_t w)
{
	menu_state *st = state_of(w);
	if (!st || !st->root)
		return 1;
	if (!st->box) {
		// First menu: re-parent the webview widget under the bar.
		g_object_ref(st->view);
		gtk_window_set_child(GTK_WINDOW(st->win), NULL);
		st->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		st->bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(st->root));
		gtk_box_append(GTK_BOX(st->box), st->bar);
		gtk_widget_set_hexpand(st->view, TRUE);
		gtk_widget_set_vexpand(st->view, TRUE);
		gtk_box_append(GTK_BOX(st->box), st->view);
		g_object_unref(st->view);
		gtk_window_set_child(GTK_WINDOW(st->win), st->box);
	} else {
		gtk_popover_menu_bar_set_menu_model(GTK_POPOVER_MENU_BAR(st->bar),
						    G_MENU_MODEL(st->root));
	}
	gtk_widget_insert_action_group(st->win, "menu", G_ACTION_GROUP(st->building));
	if (st->group)
		g_object_unref(st->group);
	st->group = st->building;
	st->building = 0;
	g_object_unref(st->root);	// the bar holds the model
	st->root = 0;
	st->menus.clear();
	return 0;
}

WEBVIEW_API int madcwebview_menu_on_action(webview_t w, madcwebview_menu_action_fn cb,
					   void *arg)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	st->cb = cb;
	st->arg = arg;
	return 0;
}

WEBVIEW_API int madcwebview_menu_activate(webview_t w, const char *id)
{
	menu_state *st = state_of(w);
	if (!st || !st->group || !id)
		return 1;
	if (!g_action_group_has_action(G_ACTION_GROUP(st->group), id))
		return 1;
	g_action_group_activate_action(G_ACTION_GROUP(st->group), id, NULL);
	return 0;
}

} // extern "C" — the menu API

// ---- file dialogs (S4): GtkFileDialog (GTK 4.10+), asynchronous ----------
// The dialog runs inside the platform loop the host is already in
// (webview_run); its completion callback resolves the GFile to a path and
// hands it to the caller's callback — "" when the user cancelled or the
// platform reported an error. The context is freed after the one call.

#if GTK_CHECK_VERSION(4, 10, 0)
namespace {

struct dialog_ctx {
	madcwebview_dialog_fn cb;
	void *arg;
	bool save;
};

void on_dialog_done(GObject *source, GAsyncResult *result, gpointer data)
{
	dialog_ctx *c = static_cast<dialog_ctx *>(data);
	GError *err = 0;
	GFile *f = c->save
		? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &err)
		: gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &err);
	char *path = f ? g_file_get_path(f) : 0;
	if (c->cb)
		c->cb(path ? path : "", c->arg);
	if (path)
		g_free(path);
	if (f)
		g_object_unref(f);
	if (err)
		g_error_free(err);
	delete c;
}

int dialog_run(webview_t w, const char *title, const char *initial,
	       madcwebview_dialog_fn cb, void *arg, bool save)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	GtkFileDialog *d = gtk_file_dialog_new();
	if (title && *title)
		gtk_file_dialog_set_title(d, title);
	if (initial && *initial) {
		GFile *g = g_file_new_for_path(initial);
		if (g_file_query_file_type(g, G_FILE_QUERY_INFO_NONE, NULL)
		    == G_FILE_TYPE_DIRECTORY)
			gtk_file_dialog_set_initial_folder(d, g);
		else
			gtk_file_dialog_set_initial_file(d, g);
		g_object_unref(g);
	}
	dialog_ctx *c = new dialog_ctx;
	c->cb = cb;
	c->arg = arg;
	c->save = save;
	if (save)
		gtk_file_dialog_save(d, GTK_WINDOW(st->win), NULL, on_dialog_done, c);
	else
		gtk_file_dialog_open(d, GTK_WINDOW(st->win), NULL, on_dialog_done, c);
	g_object_unref(d);	// the operation holds its own reference
	return 0;
}

} // namespace

extern "C" {

WEBVIEW_API int madcwebview_dialog_open(webview_t w, const char *title,
					const char *initial,
					madcwebview_dialog_fn cb, void *arg)
{
	return dialog_run(w, title, initial, cb, arg, false);
}

WEBVIEW_API int madcwebview_dialog_save(webview_t w, const char *title,
					const char *initial,
					madcwebview_dialog_fn cb, void *arg)
{
	return dialog_run(w, title, initial, cb, arg, true);
}

} // extern "C"
#else
extern "C" {
WEBVIEW_API int madcwebview_dialog_open(webview_t, const char *, const char *,
					madcwebview_dialog_fn, void *) { return 1; }
WEBVIEW_API int madcwebview_dialog_save(webview_t, const char *, const char *,
					madcwebview_dialog_fn, void *) { return 1; }
}
#endif

// The one-shot UI-thread timer (the window's bounded wait): a GLib timeout
// source on the main context the webview's loop runs; fires once.
namespace {
struct tick_ctx {
	madcwebview_tick_fn cb;
	void *arg;
};
gboolean tick_fire(gpointer data)
{
	tick_ctx *c = static_cast<tick_ctx *>(data);
	c->cb(c->arg);
	delete c;
	return G_SOURCE_REMOVE;
}
} // namespace

extern "C" {
WEBVIEW_API int madcwebview_tick(webview_t w, unsigned ms, madcwebview_tick_fn cb,
				 void *arg)
{
	if (!w || !cb)
		return 1;
	tick_ctx *c = new tick_ctx;
	c->cb = cb;
	c->arg = arg;
	g_timeout_add(ms, tick_fire, c);
	return 0;
}
} // extern "C"

#elif defined(__APPLE__)

// ======================================================================
// Cocoa / WKWebView — C++ over the Objective-C runtime, as the library
// itself is (no Objective-C source; -fblocks for the panel's completion).
// ======================================================================

#include <objc/objc-runtime.h>
#include <objc/NSObjCRuntime.h>
#include <dispatch/dispatch.h>	// the tick: dispatch_after on the main queue

namespace {

// objc_msgSend cast to the exact signature of the message.
template <typename R, typename... A>
R send(id self, SEL s, A... args)
{
	return reinterpret_cast<R (*)(id, SEL, A...)>(objc_msgSend)(self, s, args...);
}

id klass(const char *n) { return (id)objc_getClass(n); }
SEL sel(const char *n) { return sel_registerName(n); }
id nsstr(const char *s)
{
	return send<id>(klass("NSString"), sel("stringWithUTF8String:"), s ? s : "");
}
id nsstr(const std::string &s) { return nsstr(s.c_str()); }

// An autorelease pool around a call that creates autoreleased objects.
struct pool {
	id p;
	pool() : p(send<id>(klass("NSAutoreleasePool"), sel("new"))) {}
	~pool() { send<void>(p, sel("drain")); }
};

const unsigned long modifier_control = 1UL << 18;	// NSEventModifierFlagControl
const long modal_response_ok = 1;			// NSModalResponseOK
const char assoc_key = 0;				// the associated-object key (its address)

struct menu_state {
	webview_t w;
	id win;			// the NSWindow (native handle)
	id target;		// the one action target (a MadcMenuTarget)
	id root;		// the main menu being BUILT (begin .. end)
	std::vector<std::pair<std::string, id> > menus;	// title -> submenu, being built
	std::vector<std::string> building;	// item tag -> command id, being built
	std::map<std::string, id> items_building;	// command id -> NSMenuItem, being built
	std::vector<std::string> ids;		// the LIVE tag table
	std::map<std::string, id> items;	// the LIVE items (activate)
	madcwebview_menu_action_fn cb;
	void *arg;
	menu_state() : w(0), win(0), target(0), root(0), cb(0), arg(0) {}
};

std::map<webview_t, menu_state> &states()
{
	static std::map<webview_t, menu_state> s;
	return s;
}

// The target's one action: the chosen item's tag names the command.
void on_menu_action(id self, SEL, id item)
{
	menu_state *st = (menu_state *)objc_getAssociatedObject(self, &assoc_key);
	if (!st)
		return;
	long tag = send<long>(item, sel("tag"));
	if (st->cb && tag >= 0 && (size_t)tag < st->ids.size())
		st->cb(st->ids[tag].c_str(), st->arg);
}

Class target_class()
{
	static Class c = 0;
	if (!c) {
		c = objc_lookUpClass("MadcMenuTarget");
		if (!c) {
			c = objc_allocateClassPair((Class)klass("NSObject"), "MadcMenuTarget", 0);
			class_addMethod(c, sel("menuAction:"), (IMP)on_menu_action, "v@:@");
			objc_registerClassPair(c);
		}
	}
	return c;
}

menu_state *state_of(webview_t w)
{
	if (!w)
		return 0;
	menu_state &st = states()[w];
	if (!st.win) {
		st.w = w;
		st.win = (id)webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
	}
	if (!st.win)
		return 0;
	if (!st.target) {
		st.target = send<id>((id)target_class(), sel("new"));
		objc_setAssociatedObject(st.target, &assoc_key, (id)&st, OBJC_ASSOCIATION_ASSIGN);
	}
	return &st;
}

// A submenu item on the root holding a menu of that title.
id add_submenu(menu_state &st, const char *title, id menu)
{
	id holder = send<id>(send<id>(klass("NSMenuItem"), sel("alloc")),
			     sel("initWithTitle:action:keyEquivalent:"),
			     nsstr(title), (SEL)0, nsstr(""));
	send<void>(holder, sel("setSubmenu:"), menu);
	send<void>(st.root, sel("addItem:"), holder);
	send<void>(holder, sel("release"));	// the root holds it
	send<void>(menu, sel("release"));	// the holder holds it
	return menu;
}

id find_menu(menu_state &st, const char *title)
{
	for (size_t i = 0; i < st.menus.size(); i++)
		if (st.menus[i].first == title)
			return st.menus[i].second;
	id m = send<id>(send<id>(klass("NSMenu"), sel("alloc")), sel("initWithTitle:"), nsstr(title));
	send<void>(m, sel("setAutoenablesItems:"), (BOOL)NO);	// `enabled` is the engine's fact
	add_submenu(st, title, m);
	st.menus.push_back(std::make_pair(std::string(title), m));
	return m;
}

// The application menu: macOS takes the FIRST submenu as it and names it
// after the process. Its Quit is the window's own close — the close
// button's path — so the session sees the window closing exactly as it
// does today (never NSApp terminate:, which would end the process under
// the editor's feet).
void add_app_menu(menu_state &st)
{
	id app = send<id>(send<id>(klass("NSMenu"), sel("alloc")), sel("initWithTitle:"), nsstr(""));
	id name = send<id>(send<id>(klass("NSProcessInfo"), sel("processInfo")), sel("processName"));
	id qtitle = send<id>(nsstr("Quit "), sel("stringByAppendingString:"), name);
	id quit = send<id>(send<id>(klass("NSMenuItem"), sel("alloc")),
			   sel("initWithTitle:action:keyEquivalent:"),
			   qtitle, sel("performClose:"), nsstr("q"));
	send<void>(quit, sel("setTarget:"), st.win);
	send<void>(app, sel("addItem:"), quit);
	send<void>(quit, sel("release"));
	add_submenu(st, "", app);
}

} // namespace

extern "C" {

WEBVIEW_API int madcwebview_menu_begin(webview_t w)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	pool arp;
	if (st->root)
		send<void>(st->root, sel("release"));
	st->root = send<id>(send<id>(klass("NSMenu"), sel("alloc")), sel("initWithTitle:"), nsstr(""));
	st->menus.clear();
	st->building.clear();
	st->items_building.clear();
	add_app_menu(*st);
	return 0;
}

WEBVIEW_API int madcwebview_menu_add(webview_t w, const char *menu, const char *cmd,
				     const char *title, const char *key, int enabled)
{
	menu_state *st = state_of(w);
	if (!st || !st->root || !menu || !cmd || !*cmd || !title)
		return 1;
	pool arp;
	id m = find_menu(*st, menu);
	const std::string k = key ? key : "";
	const key_desc d = describe_key(k);
	std::string keq;
	if (is_accelerator(d))
		keq = std::string(1, d.ch);
	id item = send<id>(send<id>(klass("NSMenuItem"), sel("alloc")),
			   sel("initWithTitle:action:keyEquivalent:"),
			   nsstr(item_label(title, k, d)), sel("menuAction:"), nsstr(keq));
	if (!keq.empty())
		send<void>(item, sel("setKeyEquivalentModifierMask:"), modifier_control);
	send<void>(item, sel("setTarget:"), st->target);
	send<void>(item, sel("setTag:"), (long)st->building.size());
	send<void>(item, sel("setEnabled:"), (BOOL)(enabled != 0));
	st->building.push_back(cmd);
	st->items_building[cmd] = item;
	send<void>(m, sel("addItem:"), item);
	send<void>(item, sel("release"));	// the menu holds it
	return 0;
}

WEBVIEW_API int madcwebview_menu_separator(webview_t w, const char *menu)
{
	menu_state *st = state_of(w);
	if (!st || !st->root || !menu)
		return 1;
	pool arp;
	id m = find_menu(*st, menu);
	send<void>(m, sel("addItem:"), send<id>(klass("NSMenuItem"), sel("separatorItem")));
	return 0;
}

WEBVIEW_API int madcwebview_menu_end(webview_t w)
{
	menu_state *st = state_of(w);
	if (!st || !st->root)
		return 1;
	pool arp;
	id app = send<id>(klass("NSApplication"), sel("sharedApplication"));
	send<void>(app, sel("setMainMenu:"), st->root);
	send<void>(st->root, sel("release"));	// the application holds it
	st->root = 0;
	st->ids.swap(st->building);
	st->items.swap(st->items_building);
	st->building.clear();
	st->items_building.clear();
	st->menus.clear();
	return 0;
}

WEBVIEW_API int madcwebview_menu_on_action(webview_t w, madcwebview_menu_action_fn cb,
					   void *arg)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	st->cb = cb;
	st->arg = arg;
	return 0;
}

WEBVIEW_API int madcwebview_menu_activate(webview_t w, const char *cmd)
{
	menu_state *st = state_of(w);
	if (!st || !cmd)
		return 1;
	std::map<std::string, id>::iterator it = st->items.find(cmd);
	if (it == st->items.end())
		return 1;
	pool arp;
	id item = it->second;
	if (!send<BOOL>(item, sel("isEnabled")))
		return 0;	// a known item, disabled: nothing fires (GTK's answer)
	id m = send<id>(item, sel("menu"));
	long idx = send<long>(m, sel("indexOfItem:"), item);
	send<void>(m, sel("performActionForItemAtIndex:"), idx);
	return 0;
}

} // extern "C" — the menu API

// ---- file dialogs (S4): NSOpenPanel / NSSavePanel, a sheet on the window --
// Asynchronous like GTK's: the call returns once the sheet is up; the
// completion block runs later on the main run loop (the one webview_run is
// in), resolves the URL to a path and hands it to the caller's callback —
// "" when the user cancelled. The panel is retained until then.
namespace {

struct dialog_ctx {
	madcwebview_dialog_fn cb;
	void *arg;
	id panel;
};

int dialog_run(webview_t w, const char *title, const char *initial,
	       madcwebview_dialog_fn cb, void *arg, bool save)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	pool arp;
	id panel = save ? send<id>(klass("NSSavePanel"), sel("savePanel"))
			: send<id>(klass("NSOpenPanel"), sel("openPanel"));
	if (!panel)
		return 1;
	send<void>(panel, sel("retain"));
	if (!save) {
		send<void>(panel, sel("setCanChooseFiles:"), (BOOL)YES);
		send<void>(panel, sel("setCanChooseDirectories:"), (BOOL)NO);
		send<void>(panel, sel("setAllowsMultipleSelection:"), (BOOL)NO);
	}
	if (title && *title) {
		send<void>(panel, sel("setTitle:"), nsstr(title));
		send<void>(panel, sel("setMessage:"), nsstr(title));	// a sheet shows the message, not the title
	}
	if (initial && *initial) {
		id fm = send<id>(klass("NSFileManager"), sel("defaultManager"));
		BOOL isdir = NO;
		BOOL exists = send<BOOL>(fm, sel("fileExistsAtPath:isDirectory:"), nsstr(initial), &isdir);
		std::string dir = initial, name;
		if (!exists || !isdir) {
			size_t cut = dir.find_last_of('/');
			name = cut == std::string::npos ? dir : dir.substr(cut + 1);
			dir = cut == std::string::npos ? std::string() : dir.substr(0, cut);
		}
		if (!dir.empty())
			send<void>(panel, sel("setDirectoryURL:"),
				   send<id>(klass("NSURL"), sel("fileURLWithPath:isDirectory:"),
					    nsstr(dir), (BOOL)YES));
		if (save && !name.empty())
			send<void>(panel, sel("setNameFieldStringValue:"), nsstr(name));
	}
	dialog_ctx *c = new dialog_ctx;
	c->cb = cb;
	c->arg = arg;
	c->panel = panel;
	void (^done)(long) = ^(long response) {
		pool arp2;
		std::string path;
		if (response == modal_response_ok) {
			id url = send<id>(c->panel, sel("URL"));
			id p = url ? send<id>(url, sel("path")) : (id)0;
			const char *s = p ? send<const char *>(p, sel("UTF8String")) : 0;
			if (s)
				path = s;
		}
		if (c->cb)
			c->cb(path.c_str(), c->arg);
		send<void>(c->panel, sel("release"));
		delete c;
	};
	send<void>(panel, sel("beginSheetModalForWindow:completionHandler:"), st->win, done);
	return 0;
}

} // namespace

extern "C" {

WEBVIEW_API int madcwebview_dialog_open(webview_t w, const char *title,
					const char *initial,
					madcwebview_dialog_fn cb, void *arg)
{
	return dialog_run(w, title, initial, cb, arg, false);
}

WEBVIEW_API int madcwebview_dialog_save(webview_t w, const char *title,
					const char *initial,
					madcwebview_dialog_fn cb, void *arg)
{
	return dialog_run(w, title, initial, cb, arg, true);
}

} // extern "C"


// The one-shot UI-thread timer (the window's bounded wait): dispatch_after on
// the main queue, which the run loop the webview runs drains; fires once.
extern "C" {
WEBVIEW_API int madcwebview_tick(webview_t w, unsigned ms, madcwebview_tick_fn cb,
				 void *arg)
{
	if (!w || !cb)
		return 1;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * 1000000LL),
		       dispatch_get_main_queue(), ^{ cb(arg); });
	return 0;
}
} // extern "C"

#else

// ======================================================================
// Win32 / WebView2
// ======================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

namespace {

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), 0, 0);
	if (n <= 0)
		return std::wstring();
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
	return out;
}

std::string narrow(const wchar_t *ws)
{
	if (!ws || !*ws)
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, 0, 0, 0, 0);
	if (n <= 1)
		return std::string();
	std::vector<char> buf((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws, -1, &buf[0], n, 0, 0);
	return std::string(&buf[0]);
}

struct menu_state {
	webview_t w;
	HWND win;		// the top-level window (native handle)
	HMENU bar;		// the LIVE bar (SetMenu'd)
	HMENU root;		// the bar being BUILT (begin .. end)
	std::vector<std::pair<std::string, HMENU> > menus;	// title -> popup, being built
	std::vector<std::string> building;	// command number - 1 -> id, being built
	std::vector<std::string> ids;		// the LIVE table
	bool subclassed;
	madcwebview_menu_action_fn cb;
	void *arg;
	madcwebview_tick_fn tick_cb;	// the armed one-shot tick (WM_TIMER)
	void *tick_arg;
	menu_state() : w(0), win(0), bar(0), root(0), subclassed(false), cb(0), arg(0),
		       tick_cb(0), tick_arg(0) {}
};

std::map<webview_t, menu_state> &states()
{
	static std::map<webview_t, menu_state> s;
	return s;
}

const UINT_PTR subclass_id = 0x6d616463;	// 'madc'
const UINT WM_MADC_DIALOG = WM_APP + 0x11;	// lp = a dialog_ctx to run
const UINT_PTR tick_timer_id = 0x7469636b;	// 'tick' — the one-shot tick's SetTimer id

struct dialog_ctx {
	HWND win;
	std::wstring title;
	std::wstring initial;
	madcwebview_dialog_fn cb;
	void *arg;
	bool save;
};

void run_dialog(dialog_ctx *c);

// The subclass of upstream's window procedure: a menu selection arrives
// as WM_COMMAND on the top-level window; everything else passes on.
LRESULT CALLBACK chrome_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
	menu_state *st = reinterpret_cast<menu_state *>(ref);
	switch (msg) {
	case WM_COMMAND:
		if (HIWORD(wp) == 0 && lp == 0) {	// a menu item, not a control
			UINT cmd = LOWORD(wp);
			if (cmd >= 1 && cmd <= st->ids.size()) {
				if (st->cb)
					st->cb(st->ids[cmd - 1].c_str(), st->arg);
				return 0;
			}
		}
		break;
	case WM_MADC_DIALOG:
		run_dialog(reinterpret_cast<dialog_ctx *>(lp));
		return 0;
	case WM_TIMER:
		if (wp == tick_timer_id) {	// the one-shot tick: fire once
			KillTimer(hwnd, tick_timer_id);
			madcwebview_tick_fn cb = st->tick_cb;
			void *arg = st->tick_arg;
			st->tick_cb = 0;
			st->tick_arg = 0;
			if (cb)
				cb(arg);
			return 0;
		}
		break;
	case WM_NCDESTROY: {
		LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
		RemoveWindowSubclass(hwnd, chrome_proc, subclass_id);
		states().erase(st->w);	// the window is gone; so is its chrome
		return r;
	}
	default:
		break;
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

menu_state *state_of(webview_t w)
{
	if (!w)
		return 0;
	menu_state &st = states()[w];
	if (!st.win) {
		st.w = w;
		st.win = (HWND)webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW);
	}
	if (!st.win)
		return 0;
	if (!st.subclassed)
		st.subclassed = SetWindowSubclass(st.win, chrome_proc, subclass_id,
						  reinterpret_cast<DWORD_PTR>(&st)) != 0;
	if (!st.subclassed)
		return 0;
	return &st;
}

HMENU find_menu(menu_state &st, const char *title)
{
	for (size_t i = 0; i < st.menus.size(); i++)
		if (st.menus[i].first == title)
			return st.menus[i].second;
	HMENU m = CreatePopupMenu();
	AppendMenuW(st.root, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(m),
		    widen(title).c_str());
	st.menus.push_back(std::make_pair(std::string(title), m));
	return m;
}

// The accelerator column (the text after the tab in an item's label —
// display only; the key itself reaches the engine through the page): a
// control key in Windows' spelling, a bare key in Windows' name, a chord
// in JOE's own spelling.
std::wstring accel_text(const std::string &key, const key_desc &d)
{
	switch (d.kind) {
	case key_desc::ctrl_letter:
		return std::wstring(L"Ctrl+") + (wchar_t)(d.ch - 'a' + 'A');
	case key_desc::ctrl_punct:
		return std::wstring(L"Ctrl+") + (wchar_t)d.ch;
	case key_desc::named: {
		static const struct { const char *madc; const wchar_t *win; } names[] = {
			{ "enter", L"Enter" }, { "tab", L"Tab" }, { "backspace", L"Backspace" },
			{ "esc", L"Esc" }, { "up", L"Up" }, { "down", L"Down" },
			{ "left", L"Left" }, { "right", L"Right" }, { "home", L"Home" },
			{ "end", L"End" }, { "pgup", L"PgUp" }, { "pgdn", L"PgDn" },
			{ "del", L"Del" }, { "ins", L"Ins" }, { "space", L"Space" },
		};
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (d.name == names[i].madc)
				return names[i].win;
		return widen(chord_label(key));
	}
	case key_desc::printable:
		return widen(chord_label(key));
	default:
		return widen(chord_label(key));
	}
}

} // namespace

extern "C" {

WEBVIEW_API int madcwebview_menu_begin(webview_t w)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	if (st->root)
		DestroyMenu(st->root);
	st->root = CreateMenu();
	if (!st->root)
		return 1;
	st->menus.clear();
	st->building.clear();
	return 0;
}

WEBVIEW_API int madcwebview_menu_add(webview_t w, const char *menu, const char *id,
				     const char *title, const char *key, int enabled)
{
	menu_state *st = state_of(w);
	if (!st || !st->root || !menu || !id || !*id || !title)
		return 1;
	HMENU m = find_menu(*st, menu);
	const std::string k = key ? key : "";
	const key_desc d = describe_key(k);
	std::wstring label = widen(title);
	if (!k.empty())
		label += L"\t" + accel_text(k, d);
	st->building.push_back(id);
	AppendMenuW(m, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED),
		    (UINT_PTR)st->building.size(), label.c_str());
	return 0;
}

WEBVIEW_API int madcwebview_menu_separator(webview_t w, const char *menu)
{
	menu_state *st = state_of(w);
	if (!st || !st->root || !menu)
		return 1;
	AppendMenuW(find_menu(*st, menu), MF_SEPARATOR, 0, 0);
	return 0;
}

WEBVIEW_API int madcwebview_menu_end(webview_t w)
{
	menu_state *st = state_of(w);
	if (!st || !st->root)
		return 1;
	HMENU old = st->bar;
	if (!SetMenu(st->win, st->root)) {
		DestroyMenu(st->root);
		st->root = 0;
		st->menus.clear();
		st->building.clear();
		return 1;
	}
	st->bar = st->root;
	st->root = 0;
	if (old)
		DestroyMenu(old);	// SetMenu does not destroy the bar it replaces
	st->ids.swap(st->building);
	st->building.clear();
	st->menus.clear();
	DrawMenuBar(st->win);
	// The bar took client area: the frame change re-lays the window, and
	// the explicit WM_SIZE re-fits upstream's widget (it re-reads the
	// client rect and moves the WebView2 bounds with it).
	SetWindowPos(st->win, 0, 0, 0, 0, 0,
		     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	RECT rc;
	if (GetClientRect(st->win, &rc))
		SendMessageW(st->win, WM_SIZE, SIZE_RESTORED,
			     MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
	return 0;
}

WEBVIEW_API int madcwebview_menu_on_action(webview_t w, madcwebview_menu_action_fn cb,
					   void *arg)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	st->cb = cb;
	st->arg = arg;
	return 0;
}

WEBVIEW_API int madcwebview_menu_activate(webview_t w, const char *id)
{
	menu_state *st = state_of(w);
	if (!st || !st->bar || !id)
		return 1;
	UINT cmd = 0;
	for (size_t i = 0; i < st->ids.size(); i++)
		if (st->ids[i] == id) {
			cmd = (UINT)(i + 1);
			break;
		}
	if (!cmd)
		return 1;
	UINT state = GetMenuState(st->bar, cmd, MF_BYCOMMAND);
	if (state != (UINT)-1 && (state & MF_GRAYED))
		return 0;	// a known item, disabled: nothing fires (GTK's answer)
	SendMessageW(st->win, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
	return 0;
}

} // extern "C" — the menu API

// ---- file dialogs (S4): IFileOpenDialog / IFileSaveDialog ----------------
// Show() runs a modal loop of its own, so the request is POSTED to the
// window and served from the message loop the host is already in
// (webview_run): the call returns 0 once the request is queued, the dialog
// runs later on the UI thread, and the callback fires when it closes —
// "" when the user cancelled. COM is the apartment upstream initialized.
namespace {

void run_dialog(dialog_ctx *c)
{
	std::string path;
	IFileDialog *dlg = 0;
	HRESULT hr = CoCreateInstance(c->save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, 0,
				      CLSCTX_INPROC_SERVER,
				      c->save ? IID_IFileSaveDialog : IID_IFileOpenDialog,
				      reinterpret_cast<void **>(&dlg));
	if (SUCCEEDED(hr) && dlg) {
		DWORD opts = 0;
		if (SUCCEEDED(dlg->GetOptions(&opts)))
			dlg->SetOptions(opts | FOS_FORCEFILESYSTEM
					| (c->save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
		if (!c->title.empty())
			dlg->SetTitle(c->title.c_str());
		if (!c->initial.empty()) {
			std::wstring dir = c->initial, name;
			DWORD attr = GetFileAttributesW(dir.c_str());
			if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
				size_t cut = dir.find_last_of(L"\\/");
				name = cut == std::wstring::npos ? dir : dir.substr(cut + 1);
				dir = cut == std::wstring::npos ? std::wstring() : dir.substr(0, cut);
			}
			if (!dir.empty()) {
				IShellItem *folder = 0;
				if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), 0, IID_IShellItem,
									  reinterpret_cast<void **>(&folder)))
				    && folder) {
					dlg->SetFolder(folder);
					folder->Release();
				}
			}
			if (c->save && !name.empty())
				dlg->SetFileName(name.c_str());
		}
		hr = dlg->Show(c->win);
		if (SUCCEEDED(hr)) {
			IShellItem *item = 0;
			if (SUCCEEDED(dlg->GetResult(&item)) && item) {
				PWSTR ws = 0;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &ws)) && ws) {
					path = narrow(ws);
					CoTaskMemFree(ws);
				}
				item->Release();
			}
		}
		dlg->Release();
	}
	if (c->cb)
		c->cb(path.c_str(), c->arg);
	delete c;
}

int dialog_run(webview_t w, const char *title, const char *initial,
	       madcwebview_dialog_fn cb, void *arg, bool save)
{
	menu_state *st = state_of(w);
	if (!st)
		return 1;
	dialog_ctx *c = new dialog_ctx;
	c->win = st->win;
	c->title = widen(title ? title : "");
	c->initial = widen(initial ? initial : "");
	c->cb = cb;
	c->arg = arg;
	c->save = save;
	if (!PostMessageW(st->win, WM_MADC_DIALOG, 0, reinterpret_cast<LPARAM>(c))) {
		delete c;
		return 1;
	}
	return 0;
}

} // namespace

extern "C" {

WEBVIEW_API int madcwebview_dialog_open(webview_t w, const char *title,
					const char *initial,
					madcwebview_dialog_fn cb, void *arg)
{
	return dialog_run(w, title, initial, cb, arg, false);
}

WEBVIEW_API int madcwebview_dialog_save(webview_t w, const char *title,
					const char *initial,
					madcwebview_dialog_fn cb, void *arg)
{
	return dialog_run(w, title, initial, cb, arg, true);
}


// The one-shot UI-thread timer (the window's bounded wait): SetTimer on the
// top-level window; the chrome subclass serves WM_TIMER and fires once.
WEBVIEW_API int madcwebview_tick(webview_t w, unsigned ms, madcwebview_tick_fn cb,
				 void *arg)
{
	menu_state *st = state_of(w);
	if (!st || !cb)
		return 1;
	st->tick_cb = cb;
	st->tick_arg = arg;
	if (!SetTimer(st->win, tick_timer_id, ms ? ms : 1, NULL)) {
		st->tick_cb = 0;
		st->tick_arg = 0;
		return 1;
	}
	return 0;
}

} // extern "C"

#endif
