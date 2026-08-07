#ifndef __LIBMADC_DATASOURCE_H
#define __LIBMADC_DATASOURCE_H 1

#include <cstddef>
#include <cstring>
#include <string>

namespace madc {

class DataSource
{
public:
    enum class domain
    {
	unknown,
	storage,
	service,
	ipc,
	execution
    };

    enum class family
    {
	unknown,
	file,
	record_file,
	relational_database,
	keyed_database,
	graph_database,
	service_api,
	unix_socket,
	pipe,
	shared_memory,
	generic_ipc,
	process
    };

    DataSource() {}
    explicit DataSource(const std::string &uri) { assign(uri); }
    explicit DataSource(const char *uri) { assign(uri ? std::string(uri) : std::string()); }

    const std::string &uri() const { return _uri; }
    const std::string &scheme() const { return _scheme; }
    const std::string &authority() const { return _authority; }
    const std::string &path() const { return _path; }
    const std::string &location() const { return _location; }
    domain source_domain() const
    {
	return !_uri.empty() && _scheme.empty()
	    ? domain::storage : classify_scheme(_scheme);
    }
    family source_family() const
    {
	return !_uri.empty() && _scheme.empty()
	    ? family::file : classify_family(_scheme);
    }

    bool empty() const { return _uri.empty(); }
    bool is_storage() const { return source_domain() == domain::storage; }
    bool is_service() const { return source_domain() == domain::service; }
    bool is_ipc() const { return source_domain() == domain::ipc; }
    bool is_process() const { return source_family() == family::process; }
    bool is_file_like() const
    {
	return !_uri.empty() && _scheme.empty() ? true : is_file_like_scheme(_scheme);
    }
    bool is_plain_file() const { return source_family() == family::file; }
    bool is_record_file() const { return source_family() == family::record_file; }
    bool is_database() const
    {
	return source_family() == family::relational_database
	    || source_family() == family::keyed_database
	    || source_family() == family::graph_database;
    }
    bool is_relational_database() const { return source_family() == family::relational_database; }
    bool is_keyed_database() const { return source_family() == family::keyed_database; }
    bool is_graph_database() const { return source_family() == family::graph_database; }
    bool is_service_api() const { return source_family() == family::service_api; }
    bool is_unix_socket() const { return source_family() == family::unix_socket; }
    bool is_pipe() const { return source_family() == family::pipe; }
    bool is_shared_memory() const { return source_family() == family::shared_memory; }
    bool is_local() const
    {
	if ( !empty() && _scheme.empty() )
	    return true;
	const SchemeInfo *info = scheme_info(_scheme);
	return info && info->local;
    }
    bool is_remote() const { return !empty() && !is_local(); }

    static bool has_scheme(const std::string &uri)
    {
	std::size_t pos = uri.find("://");
	return pos != std::string::npos && pos > 0;
    }

private:
    struct SchemeInfo
    {
	const char *name;
	domain source_domain;
	family source_family;
	bool path_like;
	bool local;
    };

    static const SchemeInfo *scheme_info(const std::string &scheme)
    {
	static const SchemeInfo schemes[] = {
	    { "file", domain::storage, family::file, true, true },
	    { "dsv", domain::storage, family::record_file, true, true },
	    { "flr", domain::storage, family::record_file, true, true },
	    { "vlr", domain::storage, family::record_file, true, true },
	    { "sqlite", domain::storage, family::relational_database, true, true },
	    { "bdb", domain::storage, family::keyed_database, true, true },
	    { "gdbm", domain::storage, family::keyed_database, true, true },
	    { "qdbm", domain::storage, family::keyed_database, true, true },
	    { "mysql", domain::storage, family::relational_database, false, false },
	    { "pgsql", domain::storage, family::relational_database, false, false },
	    { "postgres", domain::storage, family::relational_database, false, false },
	    { "postgresql", domain::storage, family::relational_database, false, false },
	    { "redis", domain::storage, family::keyed_database, false, false },
	    { "falkordb", domain::storage, family::graph_database, false, false },
	    { "http", domain::service, family::service_api, false, false },
	    { "https", domain::service, family::service_api, false, false },
	    { "ftp", domain::service, family::service_api, false, false },
	    { "rest", domain::service, family::service_api, false, false },
	    { "mcp", domain::service, family::service_api, false, false },
	    { "smtp", domain::service, family::service_api, false, false },
	    { "smtps", domain::service, family::service_api, false, false },
	    { "imap", domain::service, family::service_api, false, false },
	    { "imaps", domain::service, family::service_api, false, false },
	    { "unix", domain::ipc, family::unix_socket, true, true },
	    { "pipe", domain::ipc, family::pipe, true, true },
	    { "shm", domain::ipc, family::shared_memory, true, true },
	    { "ipc", domain::ipc, family::generic_ipc, false, false },
	    { "exec", domain::execution, family::process, true, true }
	};
	for ( std::size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); ++i )
	{
	    if ( scheme == schemes[i].name )
		return &schemes[i];
	}
	return nullptr;
    }

    static bool is_file_like_scheme(const std::string &scheme)
    {
	const SchemeInfo *info = scheme_info(scheme);
	return info && info->path_like && info->source_domain == domain::storage;
    }

    static family classify_family(const std::string &scheme)
    {
	const SchemeInfo *info = scheme_info(scheme);
	return info ? info->source_family : family::unknown;
    }

    static domain classify_scheme(const std::string &scheme)
    {
	const SchemeInfo *info = scheme_info(scheme);
	return info ? info->source_domain : domain::unknown;
    }

    void assign(const std::string &uri)
    {
	_uri = uri;
	_scheme.clear();
	_authority.clear();
	_path.clear();
	_location.clear();

	if ( _uri.empty() )
	    return;

	if ( !has_scheme(_uri) )
	{
	    _path = _uri;
	    _location = _uri;
	    return;
	}

	std::size_t pos = _uri.find("://");
	_scheme = _uri.substr(0, pos);
	std::string rest = _uri.substr(pos + 3);

	const SchemeInfo *info = scheme_info(_scheme);
	if ( info && info->path_like )
	{
	    _path = rest.empty() ? std::string("/") : rest;
	    _location = _path;
	    return;
	}

	std::size_t slash = rest.find('/');
	if ( slash == std::string::npos )
	{
	    _authority = rest;
	    _path = "/";
	}
	else
	{
	    _authority = rest.substr(0, slash);
	    _path = rest.substr(slash);
	}
	_location = _authority + _path;
    }

    std::string _uri;
    std::string _scheme;
    std::string _authority;
    std::string _path;
    std::string _location;
};

} // namespace madc

#endif // __LIBMADC_DATASOURCE_H
