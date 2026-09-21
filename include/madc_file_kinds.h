// madc_file_kinds.h — the file-kind vocabulary's BOUNDARY converters (the
// engine side). The enum text is include/madc/bits/file_kinds, shared with
// the dialect; THIS is where a kind meets text, and the only place: ONE name
// table and ONE extension table (src/file_kinds.cpp), the standards' names
// coming from the one `--std=` table (Program::standard_canonical_name) so a
// spelling is never re-typed. Text is legal only at an INPUT boundary (a
// command line, a manifest, a layout file, a file name) and converts ONCE
// there; a misspelling converts to fkUNKNOWN — the caller's refusal (rule:
// .claude/rules/enum-over-strings.md). The dialect declares the same three
// functions in <ns_madc> and binds them mangled-direct.
//
// Thread contract: constant tables; pure functions.
#ifndef __MADC_FILE_KINDS_H
#define __MADC_FILE_KINDS_H 1

#include <cstdint>
#include "madc/bits/file_kinds"

namespace madc {
    // The canonical spelling of a kind: a standard's `--std=` name ("c11",
    // "c++17", "madc"), a family's ("c", "c++"), the IR's ("mc11"), a text /
    // other-language / binary kind's; "" for fkUNKNOWN or a value the tables
    // do not carry. int64_t so a dialect caller hands it a bag integer.
    const char *file_kind_name(int64_t kind);
    // The input converter: the kind a spelling names; fkUNKNOWN = an unknown
    // word (`--std=`'s aliases — "c90", "cpp11", "c" for c11 — are not
    // file-kind names: here "c" is the C FAMILY).
    int64_t file_kind_of(const char *name);
    // The kind a file NAME's extension answers (case-insensitive; a few
    // extensionless basenames — Makefile — are kinds too): the FAMILY for
    // C/C++ (fkC / fkCPP — the standard within a family comes from the
    // manifest or --std=, never from the extension); fkUNKNOWN for an
    // extension no table names (the editor still edits it as text).
    int64_t file_kind_of_path(const char *path);
}

#endif // __MADC_FILE_KINDS_H
