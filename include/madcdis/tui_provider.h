#ifndef __MADCDIS_TUI_PROVIDER_H
#define __MADCDIS_TUI_PROVIDER_H 1

// madcdis/tui_provider.h — the level-1 renderer's TARGET seam (Track 7.2
// R5): the byte-moving half behind the tui_model. A target owns exactly
// the physical terminal — mode switching, escape emission, key reads —
// and none of the intelligence (layout, focus, key semantics, diffing
// live in tui_model.h, dependency-free).
//
// PROVIDER MODEL (design, Decisions Incorporated item 4): level 0 is
// internal and dependency-free; level-1+ renderers are providers behind
// this registry — the madcdat storage-driver pattern applied to
// renderers. The built-in target is the hand-rolled VT100/xterm one
// (src/ui_term.cpp; zero dependencies, so it ships everywhere POSIX);
// richer targets (a Windows Console one, a vendored library) register
// beside it without touching the model, the session surface, or any
// application.
//
// THREAD-SAFETY CONTRACT: a target instance is confined to the thread
// that opened it; the registry is populated once before first use (the
// same single-threaded session contract as ns_ui's session table).

#include <cstring>
#include <vector>

#include "madcdis/tui_model.h"

namespace madc {
namespace hub {

class tui_target
{
public:
    virtual ~tui_target() {}
    // Enter grid mode (raw keys, addressable cells); report the surface
    // size. False = this target cannot serve here (reason on stderr).
    virtual bool open(size_t &rows, size_t &cols) = 0;
    // Leave grid mode, restoring the terminal exactly as found.
    virtual void close() = 0;
    // Present `next`, given the previously painted grid (the diff basis);
    // position and show/hide the cursor per the grid's cursor fields.
    virtual void paint(const tui_grid &prev, const tui_grid &next) = 0;
    // Block for input and deliver the batch of immediately-available
    // keys (batching is what printable-run coalescing rides, §7.5). A
    // size change arrives as a tui_key::resize in the batch. False = the
    // input source ended.
    virtual bool read_keys(std::vector<tui_keyev> &out) = 0;
    // The current surface size.
    virtual void size(size_t &rows, size_t &cols) = 0;
};

typedef tui_target *(*tui_target_factory)();

struct tui_target_entry
{
    const char	      *name;
    tui_target_factory factory;
};

inline std::vector<tui_target_entry> &tui_targets()
{
    static std::vector<tui_target_entry> targets;
    return targets;
}

inline void register_tui_target(const char *name, tui_target_factory factory)
{
    tui_target_entry e;
    e.name = name;
    e.factory = factory;
    tui_targets().push_back(e);
}

// Instantiate a target by name; null/empty = the first registered (the
// default). Null when none is registered or the name is unknown.
inline tui_target *create_tui_target(const char *name)
{
    std::vector<tui_target_entry> &t = tui_targets();
    for ( size_t i = 0; i < t.size(); ++i )
	if ( !name || !*name || strcmp(name, t[i].name) == 0 )
	    return t[i].factory();
    return (tui_target *)0;
}

// Populate the registry with the targets this build carries (defined in
// src/ui_term.cpp; idempotent). The consumer calls it before the first
// create — the madcdat register_optional_storage_drivers shape.
void register_builtin_tui_targets();

} // namespace hub
} // namespace madc

#endif // __MADCDIS_TUI_PROVIDER_H
