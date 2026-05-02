#ifndef __LIBMADC_DATASOURCE_H
#define __LIBMADC_DATASOURCE_H 1

#include <cstring>
#include <string>

namespace madc {

class DataSource
{
public:
    DataSource() {}
    explicit DataSource(const std::string &uri) { assign(uri); }
    explicit DataSource(const char *uri) { assign(uri ? std::string(uri) : std::string()); }

    const std::string &uri() const { return _uri; }
    const std::string &scheme() const { return _scheme; }
    const std::string &authority() const { return _authority; }
    const std::string &path() const { return _path; }
    const std::string &location() const { return _location; }

    bool empty() const { return _uri.empty(); }
    bool is_local() const
    {
	return _scheme == "file"
	    || _scheme == "dsv"
	    || _scheme == "flr"
	    || _scheme == "vlr"
	    || _scheme == "sqlite"
	    || _scheme == "bdb"
	    || _scheme == "gdbm"
	    || _scheme == "qdbm";
    }
    bool is_remote() const { return !empty() && !is_local(); }

    static bool has_scheme(const std::string &uri)
    {
	std::size_t pos = uri.find("://");
	return pos != std::string::npos && pos > 0;
    }

private:
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

	if ( _scheme == "file"
	  || _scheme == "dsv"
	  || _scheme == "flr"
	  || _scheme == "vlr"
	  || _scheme == "sqlite"
	  || _scheme == "bdb"
	  || _scheme == "gdbm"
	  || _scheme == "qdbm" )
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
