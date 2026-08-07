#ifndef __LIBMADCDIS_DRIVER_H
#define __LIBMADCDIS_DRIVER_H 1

#include "libmadc/datasource.h"
#include "libmadc/error.h"
#include "madcdis/cursor.h"
#include "madcdis/schema.h"
#include "libmadc/value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace madc {

class Query;

struct DriverCapabilities
{
    bool read = true;
    bool write = true;
    bool scan = true;
    bool point_lookup = true;
    bool range_lookup = false;
    bool filter_pushdown = false;
    bool project_pushdown = false;
    bool sort_pushdown = false;
    bool limit_pushdown = false;
    bool join_pushdown = false;
    bool graph_match = false;
    bool edge_expand = false;
    bool path_search = false;
    bool transaction = false;
    bool soft_delete = false;
};

struct RecordLocator
{
    enum class kind
    {
	none,
	byte_offset
    };

    kind locator_kind = kind::none;
    uint64_t byte_offset = 0;

    static RecordLocator none()
    {
	return RecordLocator();
    }

    static RecordLocator at_byte_offset(uint64_t offset)
    {
	RecordLocator loc;
	loc.locator_kind = kind::byte_offset;
	loc.byte_offset = offset;
	return loc;
    }

    bool valid() const
    {
	return locator_kind != kind::none;
    }
};

class DataDriver
{
public:
    virtual ~DataDriver() {}

    virtual const char *name() const = 0;
    virtual const char *scheme() const = 0;
    virtual DriverCapabilities capabilities() const = 0;

    virtual bool bind_schema(const SchemaInfo &schema, error *err = nullptr) = 0;
    virtual bool open(const DataSource &source, error *err = nullptr) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual bool can_bind_schema(const SchemaInfo &schema) const = 0;
    virtual bool can_execute(const Query &query) const = 0;

    virtual bool insert_record(const value &record, error *err = nullptr) = 0;
    virtual bool insert_record_with_locator(const value &record,
					    RecordLocator &locator,
					    error *err = nullptr)
    {
	(void)record;
	locator = RecordLocator::none();
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 std::string(name()) + " does not support record locators");
	return false;
    }
    virtual bool update_record(const std::string &key_field,
			       const value &key,
			       const value &record,
			       error *err = nullptr) = 0;
    virtual bool erase_record(const std::string &key_field,
			      const value &key,
			      error *err = nullptr) = 0;
    virtual bool restore_record(const std::string &key_field,
				const value &key,
				error *err = nullptr) = 0;
    virtual bool compact_records(error *err = nullptr) = 0;
    virtual bool get_record(const std::string &key_field,
			    const value &key,
			    value &out,
			    error *err = nullptr) const = 0;
    virtual bool get_record_by_locator(const RecordLocator &locator,
				       value &out,
				       error *err = nullptr) const
    {
	(void)locator;
	(void)out;
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 std::string(name()) + " does not support record locators");
	return false;
    }
    virtual bool execute_query(const Query &query,
			       std::vector<value> &out,
			       error *err = nullptr) const
    {
	(void)query;
	out.clear();
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 std::string(name()) + " does not support query execution");
	return false;
    }
    virtual bool scan_records(std::vector<value> &out,
			      error *err = nullptr) const = 0;
};

class StreamingDataDriver
{
public:
	virtual ~StreamingDataDriver() {}

	virtual std::unique_ptr<Cursor<value> > scan_stream(error *err = nullptr) const = 0;
	virtual bool can_stream_query(const Query &query) const
	{
		(void)query;
		return false;
	}
	virtual std::unique_ptr<Cursor<value> > query_stream(const Query &query,
							       error *err = nullptr) const
	{
		(void)query;
		(void)err;
		return std::unique_ptr<Cursor<value> >();
	}
};

std::unique_ptr<Cursor<value> > scan_driver_cursor(const DataDriver &driver,
						    error *err = nullptr);
std::unique_ptr<Cursor<value> > query_driver_cursor(const DataDriver &driver,
						     const Query &query,
						     error *err = nullptr);

class DataDriverRegistry
{
public:
    class Factory
    {
    public:
	virtual ~Factory() {}
	virtual std::unique_ptr<DataDriver> create() const = 0;
    };

    static DataDriverRegistry &instance();

    void register_factory(const std::string &scheme,
			  std::unique_ptr<Factory> factory);
    bool has_factory(const std::string &scheme) const;
    std::unique_ptr<DataDriver> create(const DataSource &source) const;
    std::vector<std::string> schemes() const;

private:
    DataDriverRegistry();
    struct impl;
    std::unique_ptr<impl> _;
};

void register_optional_storage_drivers(DataDriverRegistry &registry);

} // namespace madc

#endif // __LIBMADCDIS_DRIVER_H
