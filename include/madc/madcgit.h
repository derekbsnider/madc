// madcgit.h — the madcgit MODULE's C interface: what `import madcgit;` (the
// <ns_git> fragment's) tokenizes and binds, one typed first-call slot per
// prototype (the module row in src/madc_modules.cpp is LAZY). A READ-ONLY view
// of a local git repository through libgit2 — the SYSTEM library, a
// dependency of the IDE's nexus and never part of madc (owner ruling
// 2026-09-15; src/modules/madcgit/madcgit.cpp is the one caller).
//
// `result` is a madc::value* (the dialect passes &out); text arguments are C
// strings. Every answer is an object, or {error: <prose>} (the graph_* refusal
// shape); madcgit_open answers 0 for a path with no repository above it.
//   madcgit_head(out, h)                      -> {sha, branch, detached}
//   madcgit_revparse(out, h, spec)            -> {sha}
//   madcgit_log(out, h, path, limit)          -> {rows: [{sha, author, email, when, summary}]}
//                                                newest first; "" = no path filter;
//                                                limit <= 0 = 100
//   madcgit_show(out, h, rev, path)           -> {text}   (the blob's bytes at rev)
//   madcgit_blame(out, h, path, line, count)  -> {rows: [{line, count, sha, author, when, summary}]}
//                                                1-based line; count 0 = to the end
//   madcgit_dirty(out, h, path)               -> {dirty: bool}
//   madcgit_blame_text(out, h, path, text, line, count) -> blame the LIVE text
//                                                against path's committed history;
//                                                an uncommitted hunk has sha ""
//   madcgit_relpath(out, h, path)             -> {path} relative to the working
//                                                tree, both sides canonicalised
// Thread contract: a handle is used only from the thread that opened it.
int64_t madcgit_open(const char *path);
bool    madcgit_close(int64_t handle);
void   *madcgit_head(void *result, int64_t handle);
void   *madcgit_revparse(void *result, int64_t handle, const char *spec);
void   *madcgit_log(void *result, int64_t handle, const char *path, int64_t limit);
void   *madcgit_show(void *result, int64_t handle, const char *rev, const char *path);
void   *madcgit_blame(void *result, int64_t handle, const char *path, int64_t line, int64_t count);
void   *madcgit_dirty(void *result, int64_t handle, const char *path);
void   *madcgit_blame_text(void *result, int64_t handle, const char *path, const char *text, int64_t line, int64_t count);
void   *madcgit_relpath(void *result, int64_t handle, const char *path);
