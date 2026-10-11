// madc_documents.cpp — a hub world's documents and their files (the contract
// is include/madc_documents.h): the disk I/O, the entity name and the kind
// stamp, written once for ui:: and the ex core alike.

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "madc_documents.h"
#include "madc_file_kinds.h"	// file_kind_of_path: the kind stamp
#include "madc_posix_io.h"	// canonical_path_for_compare: path identity;
				// host_path_type_of: a directory is refused
#include "madcdis/hub.h"
#include "madcdis/verbs.h"	// mutation_context: the one write surface

namespace madc {

namespace {

using hub::entity_id;
using hub::mutation_context;

const char *DOCUMENT_ENTITY = "document";

// A path's kind; an untitled document's is the dialect's (madcide's
// edit_lang_kind rule for untitled + unknown).
int64_t kind_of(const std::string &path)
{
    return path.empty() ? (int64_t)fkMADC : file_kind_of_path(path.c_str());
}

} // namespace

bool document_read_file(const std::string &path, std::string &text)
{
    // A directory is no file's text: an ifstream opens one on POSIX hosts
    // (the read then yields nothing), so it is refused by its type.
    if ( detail::host_path_type_of(path) == detail::host_path_type::directory )
	return false;
    std::ifstream in(path.c_str(), std::ios::binary);
    if ( !in )
	return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

bool document_write_file(const std::string &path, const std::string &text,
			 bool append)
{
    std::ofstream out(path.c_str(), append ? std::ios::binary | std::ios::app
					   : std::ios::binary | std::ios::trunc);
    if ( !out )
	return false;
    out << text;
    out.close();
    return !out.fail();
}

entity_id document_new(hub::world &w, const std::string &path)
{
    mutation_context mc(w);
    entity_id e = mc.create(DOCUMENT_ENTITY);
    mc.doc_attach(e, path, kind_of(path), std::string());
    return e;
}

entity_id document_open(hub::world &w, const std::string &path)
{
    std::string text;
    if ( path.empty() || !document_read_file(path, text) )
	return 0;
    mutation_context mc(w);
    entity_id e = mc.create(DOCUMENT_ENTITY);
    mc.doc_attach(e, path, kind_of(path), text);
    return e;
}

entity_id document_find(const hub::world &w, const std::string &path)
{
    const std::string want = path.empty()
			     ? path : detail::canonical_path_for_compare(path);
    std::vector<entity_id> all = w.docs();
    for ( size_t i = 0; i < all.size(); ++i )
    {
	const hub::document *d = w.doc_of(all[i]);
	if ( !d )
	    continue;
	if ( want.empty() ? d->path().empty()
	     : detail::canonical_path_for_compare(d->path()) == want )
	    return all[i];
    }
    return 0;
}

bool document_reload(hub::world &w, entity_id doc, const std::string &path)
{
    std::string text;
    if ( !doc || path.empty() || !w.has_doc(doc)
	 || !document_read_file(path, text) )
	return false;
    mutation_context mc(w);
    if ( !mc.text_load(doc, text) )
	return false;
    mc.doc_set_path(doc, path, kind_of(path));
    return mc.doc_mark_saved(doc);
}

bool document_save(hub::world &w, entity_id doc)
{
    const hub::document *d = w.doc_of(doc);
    const hub::text_buffer *b = w.text_of(doc);
    if ( !d || !b || d->path().empty()
	 || !document_write_file(d->path(), b->text(), false) )
	return false;
    mutation_context mc(w);
    return mc.doc_mark_saved(doc);
}

bool document_set_path(hub::world &w, entity_id doc, const std::string &path)
{
    if ( !doc )
	return false;
    mutation_context mc(w);
    return mc.doc_set_path(doc, path, kind_of(path));
}

} // namespace madc
