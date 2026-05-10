#ifndef __LIBMADC_DATASOURCE_H
#define __LIBMADC_DATASOURCE_H 1

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
	ipc
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
	generic_ipc
    };

    DataSource() {}
    explicit DataSource(const std::string &uri) { assign(uri); }
    explicit DataSource(const char *uri) { assign(uri ? std::string(uri) : std::string()); }

    const std::string &uri() const { return _uri; }
    const std::string &scheme() const { return _scheme; }
    const std::string &authority() const { return _authority; }
    const std::string &path() const { return _path; }
    const std::string &location() const { return _location; }
    domain source_domain() const { return classify_scheme(_scheme); }
    family source_family() const { return classify_family(_scheme); }

    bool empty() const { return _uri.empty(); }
    bool is_storage() const { return source_domain() == domain::storage; }
    bool is_service() const { return source_domain() == domain::service; }
    bool is_ipc() const { return source_domain() == domain::ipc; }
    bool is_file_like() const { return is_file_like_scheme(_scheme); }
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
	return is_file_like()
	    || _scheme == "unix"
	    || _scheme == "pipe"
	    || _scheme == "shm";
    }
    bool is_remote() const { return !empty() && !is_local(); }

    static bool has_scheme(const std::string &uri)
    {
	std::size_t pos = uri.find("://");
	return pos != std::string::npos && pos > 0;
    }

private:
    static bool is_file_like_scheme(const std::string &scheme)
    {
	return scheme == "file"
	    || scheme == "dsv"
	    || scheme == "flr"
	    || scheme == "vlr"
	    || scheme == "sqlite"
	    || scheme == "bdb"
	    || scheme == "gdbm"
	    || scheme == "qdbm";
    }

    static family classify_family(const std::string &scheme)
    {
	if ( scheme == "file" )
	    return family::file;
	if ( scheme == "dsv"
	  || scheme == "flr"
	  || scheme == "vlr" )
	    return family::record_file;
	if ( scheme == "sqlite"
	  || scheme == "mysql"
	  || scheme == "pgsql"
	  || scheme == "postgres"
	  || scheme == "postgresql" )
	    return family::relational_database;
	if ( scheme == "bdb"
	  || scheme == "gdbm"
	  || scheme == "qdbm"
	  || scheme == "redis" )
	    return family::keyed_database;
	if ( scheme == "falkordb" )
	    return family::graph_database;
	if ( scheme == "http"
	  || scheme == "https"
	  || scheme == "ftp"
	  || scheme == "rest"
	  || scheme == "mcp" )
	    return family::service_api;
	if ( scheme == "unix" )
	    return family::unix_socket;
	if ( scheme == "pipe" )
	    return family::pipe;
	if ( scheme == "shm" )
	    return family::shared_memory;
	if ( scheme == "ipc" )
	    return family::generic_ipc;
	return family::unknown;
    }

    static domain classify_scheme(const std::string &scheme)
    {
	if ( is_file_like_scheme(scheme)
	  || scheme == "redis"
	  || scheme == "falkordb"
	  || scheme == "mysql"
	  || scheme == "pgsql"
	  || scheme == "postgres"
	  || scheme == "postgresql" )
	    return domain::storage;
	if ( scheme == "http"
	  || scheme == "https"
	  || scheme == "ftp"
	  || scheme == "rest"
	  || scheme == "mcp" )
	    return domain::service;
	if ( scheme == "unix"
	  || scheme == "pipe"
	  || scheme == "shm"
	  || scheme == "ipc" )
	    return domain::ipc;
	return domain::unknown;
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

	if ( is_file_like_scheme(_scheme)
	  || _scheme == "unix"
	  || _scheme == "pipe"
	  || _scheme == "shm" )
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
