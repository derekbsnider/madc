/* madcwebview_chrome.h — madc's extension of the webview C API: the NATIVE
 * chrome around the webview — a menu bar over the window (madcide GUI chrome
 * S2) and the platform's file dialogs (S4). Built into libmadcwebview beside
 * upstream's webview.cc; scripts/gen_webview_header.py appends this text to
 * the embedded include/madc/webview.h, so the ONE module interface carries
 * upstream's API and madc's — never a hand copy.
 *
 * JSON-free by design: the host (the <ns_ui_web> fragment) walks the engine's
 * menu description and calls begin / add / separator / end; the platform
 * layer only builds widgets. `key` is a madc key spelling ("^s", "pgdn",
 * "^k d"); a single key becomes the item's accelerator, a chord is shown in
 * the item's label. A selection calls the registered action callback with
 * the item's id (on the UI thread, inside the platform's run loop).
 * activate(id) fires that callback as if the item were chosen — the test
 * seam. Return 0 = ok; nonzero = unsupported on this platform / no window /
 * unknown id. Thread contract: the UI thread only (webview's own).
 */
#ifndef MADCWEBVIEW_MENU_H
#define MADCWEBVIEW_MENU_H 1

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*madcwebview_menu_action_fn)(const char *id, void *arg);

WEBVIEW_API int madcwebview_menu_begin(webview_t w);
WEBVIEW_API int madcwebview_menu_add(webview_t w, const char *menu, const char *id,
				     const char *title, const char *key, int enabled);
WEBVIEW_API int madcwebview_menu_separator(webview_t w, const char *menu);
WEBVIEW_API int madcwebview_menu_end(webview_t w);
WEBVIEW_API int madcwebview_menu_on_action(webview_t w, madcwebview_menu_action_fn cb,
					   void *arg);
WEBVIEW_API int madcwebview_menu_activate(webview_t w, const char *id);

/* The platform's file dialogs (S4): open or save-as, over the webview's
 * window, ASYNCHRONOUS — the call returns 0 once the dialog is up and the
 * callback fires later, on the UI thread inside the platform's run loop,
 * with the chosen path ("" = cancelled). `initial` names the folder or file
 * the dialog starts at (NULL/"" = the platform's default). Nonzero = no
 * native dialog on this platform / no window. */
typedef void (*madcwebview_dialog_fn)(const char *path, void *arg);

WEBVIEW_API int madcwebview_dialog_open(webview_t w, const char *title,
					const char *initial,
					madcwebview_dialog_fn cb, void *arg);
WEBVIEW_API int madcwebview_dialog_save(webview_t w, const char *title,
					const char *initial,
					madcwebview_dialog_fn cb, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MADCWEBVIEW_MENU_H */
