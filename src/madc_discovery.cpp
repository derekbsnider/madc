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
    if ( !record.is_object() )
	record = value::make_object();
    std::map<std::string, value> &f = record.object();
    f["key"] = value(key);
    f["pid"] = value(sys.pid);
    f["started"] = value((int64_t)std::time(0));
    f["version"] = value(sys.version);
    f["host"] = value(sys.hostname);
    _path = path;
    _file->replace(jsonrpc::encode(record) + "\n");
    return claim_result::owner;
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

} // namespace discovery
} // namespace madc
