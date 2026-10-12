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
#include <string>
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
    // The FAMILY a kind belongs to (fkC / fkCPP / fkMADC — the range head);
    // fkUNKNOWN outside the compiler families.
    int64_t file_kind_family(int64_t kind);
    // The standard a kind answers when none was named: itself when it
    // names one, the family's default (C17, C++17, madc) for a family
    // head, 0 (the engine's default) outside the families. The ONE table
    // for "which standard does this file get" (split arc 1A.1 / 1A.4).
    int64_t file_kind_default_standard(int64_t kind);

    // ---- editing rules by kind (ex core design decision 12; src/
    // file_kind_editing.cpp). The ex core's > < and :set defaults read
    // them; madcide's dialect copies (tools/madcide/madcide_lang.inc) move
    // onto them in split arc 1D (item 18) and are deleted there.
    //
    // C's syntax lineage: the compiler families and the languages that took
    // C's braces and line comment.
    bool file_kind_c_syntax(int64_t kind);
    // The line-comment opener: "//" for C's lineage, "#" for the shell's
    // family, ";" for INI, "--" for Lua; "" when the kind has none known.
    const char *file_kind_line_comment(int64_t kind);
    // The characters that, ending a line, open one more indent level ("{"
    // for C's lineage, ":" for Python) and that, first on an indented line,
    // close one ("}"); "" for none.
    const char *file_kind_block_open(int64_t kind);
    const char *file_kind_block_close(int64_t kind);
    // One indent level when a file shows none: four spaces for Python (PEP
    // 8), two for YAML (which refuses tabs), a tab for the rest (a
    // Makefile's recipe lines need one).
    const char *file_kind_indent_default(int64_t kind);
    // The indent unit `text` shows (VS Code's detectIndentation, in small):
    // over its first 1000 lines, a tab when as many indented lines start
    // with a tab as with spaces; else the spaces the indentation most often
    // steps by between neighbouring non-blank lines (2..8 — a one-space step
    // is alignment, a C comment's ` *`), four when no step shows. "" when no
    // line is indented: the kind decides (file_kind_indent_default).
    std::string indent_detect(const std::string &text);
}

#endif // __MADC_FILE_KINDS_H
