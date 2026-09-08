// madcwebview_chrome.cc — madc's native chrome around the webview (API in
// madcwebview_chrome.h): the menu bar over the window (madcide GUI chrome
// S2) and the platform's file dialogs (S4). GTK4: a GtkPopoverMenuBar fed
// by a GMenu model, its items bound to a GSimpleActionGroup inserted on the
// window under the `menu.` prefix; the webview widget is re-parented into a
// vertical box under the bar (upstream sets the widget as the window's child
// and never touches it again). Every call rebuilds the model and the group
// from scratch — the engine sends the whole menu whenever any item changed,
// so there is no per-item state to keep in step. Win32 / Cocoa: not yet —
// every call answers "unsupported" and the page stays as it is.
#define WEBVIEW_HEADER 1
#include "webview/webview.h"
#include "madcwebview_chrome.h"

#if !defined(_WIN32) && !defined(__APPLE__)

#include <gtk/gtk.h>
#include <map>
#include <string>
#include <vector>

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

// A madc key spelling -> a GTK accelerator string ("^s" -> "<Control>s",
// "pgdn" -> "Page_Down"); "" when the spelling is a chord (a space) or not
// a single key GTK can show.
std::string accel_of(const std::string &key)
{
	if (key.empty() || key.find(' ') != std::string::npos)
		return std::string();
	static const struct { const char *madc, *gtk; } named[] = {
		{ "enter", "Return" }, { "tab", "Tab" }, { "backspace", "BackSpace" },
		{ "esc", "Escape" }, { "up", "Up" }, { "down", "Down" },
		{ "left", "Left" }, { "right", "Right" }, { "home", "Home" },
		{ "end", "End" }, { "pgup", "Page_Up" }, { "pgdn", "Page_Down" },
		{ "del", "Delete" }, { "ins", "Insert" }, { "space", "space" },
		{ "^_", "<Control>underscore" }, { "^^", "<Control>asciicircum" },
		{ "^]", "<Control>bracketright" }, { "^\\", "<Control>backslash" },
	};
	for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
		if (key == named[i].madc)
			return named[i].gtk;
	if (key.size() == 2 && key[0] == '^'
	    && ((key[1] >= 'a' && key[1] <= 'z') || (key[1] >= 'A' && key[1] <= 'Z')))
		return std::string("<Control>") + (char)(key[1] | 0x20);
	if (key.size() == 1)
		return key;
	return std::string();
}

// A chord shown in a label: JOE's own spelling, letters upper-cased ("^k d"
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
	const std::string accel = accel_of(k);
	std::string label = title;
	if (!k.empty() && accel.empty())
		label += "   " + chord_label(k);	// a chord: shown, not bound
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

#else /* Win32 / Cocoa: the native menu bar and dialogs are not built yet */

extern "C" {
WEBVIEW_API int madcwebview_menu_begin(webview_t) { return 1; }
WEBVIEW_API int madcwebview_menu_add(webview_t, const char *, const char *, const char *,
				     const char *, int) { return 1; }
WEBVIEW_API int madcwebview_menu_separator(webview_t, const char *) { return 1; }
WEBVIEW_API int madcwebview_menu_end(webview_t) { return 1; }
WEBVIEW_API int madcwebview_menu_on_action(webview_t, madcwebview_menu_action_fn, void *)
{ return 1; }
WEBVIEW_API int madcwebview_menu_activate(webview_t, const char *) { return 1; }
WEBVIEW_API int madcwebview_dialog_open(webview_t, const char *, const char *,
					madcwebview_dialog_fn, void *) { return 1; }
WEBVIEW_API int madcwebview_dialog_save(webview_t, const char *, const char *,
					madcwebview_dialog_fn, void *) { return 1; }
}

#endif
