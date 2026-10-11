//////////////////////////////////////////////////////////////////////////
//									//
// madc_discovery.cpp — the advertisement directory, a key's one owner	//
// and the scan (contract in include/madc_discovery.h).			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "madc_discovery.h"
#include "madc_posix_io.h"	// held_file, host_path_*: the record's hold and place
#include "madc_documents.h"	// document_read_file: a record's text
#include "madc_jsonrpc.h"	// decode / encode: a record's JSON
#include "libmadc/sysinfo.h"	// sys.pid / version / hostname: the stamp

namespace madc {
namespace discovery {

namespace {

std::string env_of(const char *name)
{
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

} // namespace

std::string session_dir()
{
    const std::string over = env_of("MADC_SESSION_DIR");
    if ( !over.empty() )
	return over;
    const std::string xdg = env_of("XDG_STATE_HOME");
    if ( !xdg.empty() )
	return detail::host_path_join(xdg, "madc/sessions");
    std::string home;
    if ( detail::host_path_expand_home("~/.local/state/madc/sessions", home)
	 && home != "~/.local/state/madc/sessions" )
	return home;
    return std::string();
}

std::string slug(const std::string &canonical)
{
    std::string out = canonical;
    std::replace(out.begin(), out.end(), '/', '_');
    std::replace(out.begin(), out.end(), '\\', '_');
    std::replace(out.begin(), out.end(), ':', '_');
    std::replace(out.begin(), out.end(), ' ', '_');
    if ( out.size() > 1 && out[0] == '_' )
	out.erase(0, 1);
    return out;
}

claim::claim() : _file(new detail::held_file())
{
}

claim::~claim()
{
    _file->release(true);		// the record goes with the hold
}

claim_result claim::take(const std::string &key, value record, value &holder)
{
    holder = value();
    const std::string dir = session_dir();
    if ( dir.empty() )
	return claim_result::unadvertised;
    const std::string path = detail::host_path_join(dir, slug(key) + ".json");
    detail::host_path_make_parents(path);
    if ( !_file->open_held(path) )
    {
	if ( !detail::host_file_held(path) )
	    return claim_result::unadvertised;	// could not open it: no state dir
	std::string text;
	holder = value::make_object();		// still writing its record: {}
	value rec;
	if ( document_read_file(path, text) && jsonrpc::decode(text, rec) && rec.is_object() )
	    holder = rec;
	return claim_result::held_elsewhere;
    }
    std::map<std::string, value> stamp;
    stamp["key"] = value(key);
    stamp["pid"] = value(sys.pid);
    stamp["started"] = value((int64_t)std::time(0));
    stamp["version"] = value(sys.version);
    stamp["host"] = value(sys.hostname);
    _stamp = value::make_object(stamp);
    _path = path;
    update(record);
    return claim_result::owner;
}

bool claim::update(value record)
{
    if ( _path.empty() )
	return false;
    if ( !record.is_object() )
	record = value::make_object();
    for ( const auto &kv : _stamp.as_object() )
	record.object()[kv.first] = kv.second;
    return _file->replace(jsonrpc::encode(record) + "\n");
}

void claim::release()
{
    _file->release(true);
    _path.clear();
    _stamp = value();
}

std::vector<value> scan()
{
    std::vector<value> out;
    const std::string dir = session_dir();
    if ( dir.empty() )
	return out;
    std::vector<std::string> files;
    detail::glob_paths(detail::host_path_join(dir, "*.json"), files);
    std::sort(files.begin(), files.end());
    for ( const std::string &f : files )
    {
	std::string text;
	value rec;
	if ( !detail::host_file_held(f) || !document_read_file(f, text)
	     || !jsonrpc::decode(text, rec) || !rec.is_object() )
	    continue;
	rec.object()["file"] = value(f);
	out.push_back(rec);
    }
    return out;
}

value find(const std::string &file)
{
    if ( file.empty() )
	return value();
    const std::string want = detail::canonical_path_for_compare(file);
    value covering;
    for ( const value &rec : scan() )
    {
	const value *docs = jsonrpc::field(rec, "documents");
	if ( docs && docs->is_array() )
	    for ( const value &d : docs->as_array() )
		if ( d.is_string() && detail::canonical_path_for_compare(d.as_string()) == want )
		    return rec;			// an exact holder wins outright
	const std::string root = jsonrpc::text_field(rec, "root");
	if ( covering.is_null() && !root.empty()
	     && detail::host_path_within(detail::canonical_path_for_compare(root), want) )
	    covering = rec;
    }
    return covering;
}

} // namespace discovery

session_claim::session_claim() : _claim(new discovery::claim())
{
}

session_claim::~session_claim()
{
    delete _claim;
}

int64_t session_claim::take(value &holder, const char *key, value &record)
{
    return (int64_t)_claim->take(key ? key : "", record, holder);
}

bool session_claim::update(value &record)
{
    return _claim->update(record);
}

void session_claim::release()
{
    _claim->release();
}

value &session_claim::path(value &out)
{
    out = value(_claim->path());
    return out;
}

value &session_dir(value &out)
{
    out = value(discovery::session_dir());
    return out;
}

value &session_scan(value &out)
{
    out = value::make_array(discovery::scan());
    return out;
}

value &session_find(value &out, const char *file)
{
    out = discovery::find(file ? file : "");
    return out;
}

} // namespace madc
