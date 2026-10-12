// madc_diag_names.h — the diagnostics vocabularies' NAMES: the boundary text
// of <bits/diag_enums> (the enums the engine and the dialect share). ONE
// table each, in src/parser.cpp; a diagnostics row's `severity` / `phase`
// text and a rendered diagnostic's header word come from here, never re-typed.
//
// Thread contract: constant tables; pure functions.
#ifndef __MADC_DIAG_NAMES_H
#define __MADC_DIAG_NAMES_H 1

#include "madc/bits/diag_enums"

namespace madc {
    const char *diag_severity_name(diag_severity severity);
    const char *diag_phase_name(diag_phase phase);
}

#endif // __MADC_DIAG_NAMES_H
