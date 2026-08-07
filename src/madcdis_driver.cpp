// Core typed-data driver adapters and registry ownership.

#include "madcdis/driver.h"
#include "madcdis/query.h"

#include <map>
#include <utility>

namespace madc {

std::unique_ptr<Cursor<value> > scan_driver_cursor(const DataDriver &driver,
						    error *err)
{
	const StreamingDataDriver *streaming =
		dynamic_cast<const StreamingDataDriver *>(&driver);
	if ( streaming )
		return streaming->scan_stream(err);

	std::vector<value> rows;
	if ( !driver.scan_records(rows, err) )
		return std::unique_ptr<Cursor<value> >();
	return std::unique_ptr<Cursor<value> >(
		new detail::VectorCursor<value>(std::move(rows)));
}

std::unique_ptr<Cursor<value> > query_driver_cursor(const DataDriver &driver,
						     const Query &query,
						     error *err)
{
	const StreamingDataDriver *streaming =
		dynamic_cast<const StreamingDataDriver *>(&driver);
	if ( streaming && streaming->can_stream_query(query) )
		return streaming->query_stream(query, err);

	std::vector<value> rows;
	if ( !driver.execute_query(query, rows, err) )
		return std::unique_ptr<Cursor<value> >();
	return std::unique_ptr<Cursor<value> >(
		new detail::VectorCursor<value>(std::move(rows)));
}

struct DataDriverRegistry::impl
{
	std::map<std::string, std::unique_ptr<Factory> > factories;
};

DataDriverRegistry::DataDriverRegistry()
	: _(new impl())
{
#ifdef HAVE_MADCDAT
	register_optional_storage_drivers(*this);
#endif
}

DataDriverRegistry &DataDriverRegistry::instance()
{
	static DataDriverRegistry registry;
	return registry;
}

void DataDriverRegistry::register_factory(const std::string &scheme,
					  std::unique_ptr<Factory> factory)
{
	_->factories[scheme] = std::move(factory);
}

bool DataDriverRegistry::has_factory(const std::string &scheme) const
{
	return _->factories.count(scheme) != 0;
}

std::unique_ptr<DataDriver> DataDriverRegistry::create(const DataSource &source) const
{
	std::map<std::string, std::unique_ptr<Factory> >::const_iterator it =
		_->factories.find(source.scheme());
	if ( it == _->factories.end() || !it->second.get() )
		return std::unique_ptr<DataDriver>();
	return it->second->create();
}

std::vector<std::string> DataDriverRegistry::schemes() const
{
	std::vector<std::string> out;
	for ( std::map<std::string, std::unique_ptr<Factory> >::const_iterator it =
		  _->factories.begin(); it != _->factories.end(); ++it )
		out.push_back(it->first);
	return out;
}

} // namespace madc
