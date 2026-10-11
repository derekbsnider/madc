#ifndef __MADC_DOCUMENTS_H
#define __MADC_DOCUMENTS_H 1

// madc_documents.h — a hub world's documents and their files: the ONE owner
// of the disk I/O a document does (ex core design decision 2,
// docs/plans/2026-10-10-ex-core-design.md; split arc 1B.6). The ui::
// document publics (ui::doc_new / doc_open / doc_find / doc_load_file /
// doc_save, src/ns_ui.cpp) and the ex core's :e / :r / :w / :sav
// (src/madc_ex.cpp) read and write through these, so madcide's File menu and
// colon mode cannot drift apart.
//
// A document's identity is its path (canonical_path_for_compare, the one
// path-comparison owner); every document entity is created with the name
// "document"; a document's kind is its path's (madc::file_kind_of_path), and
// an untitled one is the dialect's until it is named.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): each call touches
// only the world passed in, on that world's thread (the hub's rule), and the
// files it names. No global state.

#include <string>

#include "madcdis/document.h"	// entity_id

namespace madc {
namespace hub { class world; }

// A file's whole text, bytes as they are. False when it cannot be read (a
// directory included).
bool document_read_file(const std::string &path, std::string &text);
// `text` written to `path`, replacing it or, `append`, after it. False when
// the file cannot be written.
bool document_write_file(const std::string &path, const std::string &text,
			 bool append);

// A new empty document named `path` (an empty path: untitled), saved.
hub::entity_id document_new(hub::world &w, const std::string &path);
// A new document holding `path`'s text, saved; 0 when it cannot be read.
hub::entity_id document_open(hub::world &w, const std::string &path);
// The document open on `path`, else 0; an empty path finds the untitled one.
hub::entity_id document_find(const hub::world &w, const std::string &path);
// The document's text replaced by `path`'s, which becomes its path; saved.
bool document_reload(hub::world &w, hub::entity_id doc, const std::string &path);
// The document written to its own path, then marked saved. False when it is
// untitled or the file cannot be written.
bool document_save(hub::world &w, hub::entity_id doc);
// The document named `path` (its kind with it): an empty path makes it
// untitled.
bool document_set_path(hub::world &w, hub::entity_id doc, const std::string &path);

} // namespace madc

#endif
