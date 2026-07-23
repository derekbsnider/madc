#ifndef __LIBMADCDAT_SOURCE_ADAPTER_H
#define __LIBMADCDAT_SOURCE_ADAPTER_H 1

#include "libmadc/datasource.h"
#include "libmadc/error.h"
#include "madcdis/schema.h"
#include "libmadc/value.h"

#include <cstddef>
#include <string>
#include <vector>

namespace madc {

struct SourceLocator
{
    enum class kind
    {
	none,
	byte_range,
	line_range,
	key_path
    };

    kind locator_kind = kind::none;
    std::size_t byte_offset = 0;
    std::size_t byte_length = 0;
    std::size_t line_start = 0;
    std::size_t line_count = 0;
    std::string path;

    static SourceLocator none()
    {
	return SourceLocator();
    }

    static SourceLocator at_byte_range(std::size_t offset, std::size_t length)
    {
	SourceLocator out;
	out.locator_kind = kind::byte_range;
	out.byte_offset = offset;
	out.byte_length = length;
	return out;
    }

    static SourceLocator at_line_range(std::size_t start, std::size_t count)
    {
	SourceLocator out;
	out.locator_kind = kind::line_range;
	out.line_start = start;
	out.line_count = count;
	return out;
    }

    static SourceLocator at_key_path(const std::string &key_path)
    {
	SourceLocator out;
	out.locator_kind = kind::key_path;
	out.path = key_path;
	return out;
    }

    bool valid() const
    {
	return locator_kind != kind::none;
    }
};

class ExtractedRecordType
{
public:
    ExtractedRecordType()
	: _nested(false), _repeatable(true)
    {}

    explicit ExtractedRecordType(const std::string &type_name)
	: _name(type_name), _nested(false), _repeatable(true)
    {}

    const std::string &name() const { return _name; }
    const SchemaInfo &schema() const { return _schema; }
    bool nested() const { return _nested; }
    bool repeatable() const { return _repeatable; }

    ExtractedRecordType &name(const std::string &type_name)
    {
	_name = type_name;
	return *this;
    }

    ExtractedRecordType &schema(const SchemaInfo &type_schema)
    {
	_schema = type_schema;
	return *this;
    }

    ExtractedRecordType &nested(bool value = true)
    {
	_nested = value;
	return *this;
    }

    ExtractedRecordType &repeatable(bool value = true)
    {
	_repeatable = value;
	return *this;
    }

private:
    std::string _name;
    SchemaInfo _schema;
    bool _nested;
    bool _repeatable;
};

struct ExtractedRecord
{
    std::string type_name;
    value record;
    SourceLocator locator;
};

class SourceAdapter
{
public:
    virtual ~SourceAdapter() {}

    virtual const char *name() const = 0;
    virtual bool can_read(const DataSource &source) const = 0;
    virtual bool discover_types(const DataSource &source,
				std::vector<ExtractedRecordType> &out,
				error *err = nullptr) const = 0;
    virtual bool extract(const DataSource &source,
			 const std::string &type_name,
			 std::vector<ExtractedRecord> &out,
			 error *err = nullptr) const = 0;
};

} // namespace madc

#endif // __LIBMADCDAT_SOURCE_ADAPTER_H
