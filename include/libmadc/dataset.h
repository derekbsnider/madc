#ifndef __LIBMADC_DATASET_H
#define __LIBMADC_DATASET_H 1

#include "libmadc/datasource.h"
#include "libmadc/driver.h"
#include "libmadc/error.h"
#include "libmadc/mapper.h"
#include "libmadc/value.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace madc {

template <typename T>
class Cursor
{
public:
    virtual ~Cursor() {}

    virtual bool next(T &out) = 0;
    virtual void close() = 0;
};

namespace detail {

template <typename T>
class VectorCursor : public Cursor<T>
{
public:
    explicit VectorCursor(std::vector<T> rows)
	: _rows(std::move(rows)), _index(0), _closed(false)
    {}

    bool next(T &out)
    {
	if ( _closed || _index >= _rows.size() )
	    return false;
	out = _rows[_index++];
	return true;
    }

    void close()
    {
	_closed = true;
	_rows.clear();
	_index = 0;
    }

private:
    std::vector<T> _rows;
    std::size_t _index;
    bool _closed;
};

template <typename T>
std::string logical_name_for_storage(const MappingSpec<T> &spec,
				       const std::string &storage_name)
{
    const std::map<std::string, std::string> &field_names = spec.field_names();
    for ( std::map<std::string, std::string>::const_iterator it = field_names.begin();
	  it != field_names.end(); ++it )
    {
	if ( it->second == storage_name )
	    return it->first;
    }
    return storage_name;
}

template <typename T>
std::string storage_name_for_logical(const MappingSpec<T> &spec,
				       const std::string &logical_name)
{
    const std::map<std::string, std::string> &field_names = spec.field_names();
    typename std::map<std::string, std::string>::const_iterator it = field_names.find(logical_name);
    if ( it != field_names.end() )
	return it->second;
    return logical_name;
}

template <typename T>
bool field_is_excluded(const MappingSpec<T> &spec, const std::string &logical_name)
{
    const std::vector<std::string> &excluded = spec.excluded_fields();
    return std::find(excluded.begin(), excluded.end(), logical_name) != excluded.end();
}

template <typename T>
SchemaInfo map_schema_for_storage(const SchemaInfo &schema, const MappingSpec<T> &spec)
{
    std::vector<SchemaField> fields;
    for ( std::size_t i = 0; i < schema.fields().size(); ++i )
    {
	SchemaField field = schema.fields()[i];
	if ( field_is_excluded(spec, field.name) )
	    continue;
	typename std::map<std::string, typename MappingSpec<T>::string_limit>::const_iterator lim =
	    spec.string_limits().find(field.name);
	if ( lim != spec.string_limits().end() )
	{
	    field.byte_size = lim->second.max_bytes;
	    field.text_overflow =
		lim->second.policy == MappingSpec<T>::overflow_policy::truncate
		? SchemaField::overflow_policy::truncate
		: SchemaField::overflow_policy::fail;
	}
	field.key = std::find(spec.keys().begin(), spec.keys().end(), field.name) != spec.keys().end();
	field.name = storage_name_for_logical(spec, field.name);
	fields.push_back(field);
    }

    SchemaInfo out(schema.name());
    out.set_kind(schema.schema_kind());
    out.set_dataset_role(spec.dataset_role());
    out.set_packed_binary_compatible(schema.packed_binary_compatible());
    out.set_fixed_size(schema.fixed_size());
    switch ( spec.record_layout() )
    {
	case MappingSpec<T>::layout_mode::logical:
	    out.set_record_layout(SchemaInfo::layout_mode::logical);
	    break;
	case MappingSpec<T>::layout_mode::fixed_record:
	    out.set_record_layout(SchemaInfo::layout_mode::fixed_record);
	    break;
	case MappingSpec<T>::layout_mode::variable_record:
	    out.set_record_layout(SchemaInfo::layout_mode::variable_record);
	    break;
    }
    out.set_record_size(spec.record_size());
    out.set_record_overflow_policy(
	spec.record_overflow_policy() == MappingSpec<T>::overflow_policy::truncate
	? SchemaInfo::overflow_policy::truncate
	: SchemaInfo::overflow_policy::fail);
    if ( spec.ordered_records() )
    {
	out.set_ordered_key(storage_name_for_logical(spec, spec.ordered_key_field()),
			    spec.ordered_key_compare());
    }
    if ( spec.has_tombstone_file() )
	out.set_tombstone_path(spec.tombstone_path());
    if ( spec.has_dead_record_file() )
	out.set_dead_record_path(spec.dead_record_path());
    for ( std::size_t i = 0; i < fields.size(); ++i )
	out.add_field(fields[i]);
    return out;
}

template <typename T>
value logical_to_storage_record(const value &logical_record,
				  const MappingSpec<T> &spec)
{
    if ( !logical_record.is_object() )
	return logical_record;

    value storage_record = value::make_object();
    const std::map<std::string, value> &fields = logical_record.as_object();
    for ( std::map<std::string, value>::const_iterator it = fields.begin();
	  it != fields.end(); ++it )
    {
	if ( field_is_excluded(spec, it->first) )
	    continue;
	storage_record.object()[storage_name_for_logical(spec, it->first)] = it->second;
    }
    return storage_record;
}

template <typename T>
value storage_to_logical_record(const value &storage_record,
				  const MappingSpec<T> &spec)
{
    if ( !storage_record.is_object() )
	return storage_record;

    value logical_record = value::make_object();
    const std::map<std::string, value> &fields = storage_record.as_object();
    for ( std::map<std::string, value>::const_iterator it = fields.begin();
	  it != fields.end(); ++it )
    {
	logical_record.object()[logical_name_for_storage(spec, it->first)] = it->second;
    }
    return logical_record;
}

} // namespace detail

template <typename T>
class DataSet
{
public:
    class iterator
    {
    public:
	typedef std::forward_iterator_tag iterator_category;
	typedef T value_type;
	typedef std::ptrdiff_t difference_type;
	typedef const T *pointer;
	typedef const T &reference;

	iterator()
	    : _rows(), _index(0)
	{}

	reference operator*() const
	{
	    return (*_rows)[_index];
	}

	pointer operator->() const
	{
	    return &(*_rows)[_index];
	}

	iterator &operator++()
	{
	    ++_index;
	    return *this;
	}

	iterator operator++(int)
	{
	    iterator out(*this);
	    ++(*this);
	    return out;
	}

	bool operator==(const iterator &other) const
	{
	    return _rows == other._rows && _index == other._index;
	}

	bool operator!=(const iterator &other) const
	{
	    return !(*this == other);
	}

    private:
	friend class DataSet<T>;

	iterator(std::shared_ptr<std::vector<T> > rows, std::size_t index)
	    : _rows(rows), _index(index)
	{}

	std::shared_ptr<std::vector<T> > _rows;
	std::size_t _index;
    };

    explicit DataSet(const DataSource &source)
	: _source(source), _opened(false)
    {}
    explicit DataSet(const std::string &uri)
	: _source(uri), _opened(false)
    {}
    explicit DataSet(const char *uri)
	: _source(uri), _opened(false)
    {}
    ~DataSet() {}

    DataSet &name(const std::string &logical_name)
    {
	_name = logical_name;
	return *this;
    }

    DataSet &mapping(const MappingSpec<T> &spec)
    {
	_mapping = spec;
	return *this;
    }

    DataSet &mapper(std::shared_ptr<DataMapper<T>> mapper_instance)
    {
	_mapper = mapper_instance;
	return *this;
    }

    DataSet &format(std::shared_ptr<FormatAdapter<T>> adapter)
    {
	_format = adapter;
	return *this;
    }

    DataSet &driver(std::unique_ptr<DataDriver> driver_instance)
    {
	_driver = std::move(driver_instance);
	return *this;
    }

    const std::string &name() const { return _name; }
    const DataSource &source() const { return _source; }

    bool open(error *err = nullptr)
    {
	if ( _opened )
	    return true;
	if ( !_mapper )
	    _mapper = infer_mapper<T>(_mapping);
	if ( !_mapper )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet open failed: no DataMapper<T> is configured");
	    return false;
	}

	if ( !_driver )
	    _driver = DataDriverRegistry::instance().create(_source);
	if ( !_driver )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet open failed: no DataDriver is registered for scheme `" + _source.scheme() + "`");
	    return false;
	}

	_storage_schema = detail::map_schema_for_storage<T>(_mapper->schema(), _mapping);
	if ( !_driver->can_bind_schema(_storage_schema) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet open failed: driver cannot bind schema `" + _storage_schema.name() + "`");
	    return false;
	}
	if ( !_driver->bind_schema(_storage_schema, err) )
	    return false;
	if ( !_driver->open(_source, err) )
	    return false;
	_opened = true;
	return true;
    }

    void close()
    {
	if ( _driver )
	    _driver->close();
	invalidate_snapshot();
	_opened = false;
    }

    bool is_open() const
    {
	return _opened && _driver.get() && _driver->is_open();
    }

    bool insert(const T &input, error *err = nullptr)
    {
	if ( !open(err) )
	    return false;
	value logical = _mapper->encode(input);
	value storage = detail::logical_to_storage_record<T>(logical, _mapping);
	if ( !_driver->insert_record(storage, err) )
	    return false;
	invalidate_snapshot();
	return true;
    }

    bool push_back(const T &input, error *err = nullptr)
    {
	return insert(input, err);
    }

    bool insert_with_locator(const T &input,
			     RecordLocator &locator,
			     error *err = nullptr)
    {
	if ( !open(err) )
	    return false;
	value logical = _mapper->encode(input);
	value storage = detail::logical_to_storage_record<T>(logical, _mapping);
	if ( !_driver->insert_record_with_locator(storage, locator, err) )
	    return false;
	invalidate_snapshot();
	return true;
    }

    bool update(const T &input, error *err = nullptr)
    {
	if ( !open(err) )
	    return false;
	const std::string logical_key = primary_key_field();
	if ( logical_key.empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet update failed: no key field is configured");
	    return false;
	}

	value logical = _mapper->encode(input);
	if ( !logical.is_object() || logical.as_object().count(logical_key) == 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet update failed: encoded record does not carry key field `" + logical_key + "`");
	    return false;
	}

	value key = logical.as_object().at(logical_key);
	value storage = detail::logical_to_storage_record<T>(logical, _mapping);
	if ( !_driver->update_record(storage_key_field(), key, storage, err) )
	    return false;
	invalidate_snapshot();
	return true;
    }

    bool erase(const value &key, error *err = nullptr)
    {
	if ( !open(err) )
	    return false;
	if ( storage_key_field().empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet erase failed: no key field is configured");
	    return false;
	}
	if ( !_driver->erase_record(storage_key_field(), key, err) )
	    return false;
	invalidate_snapshot();
	return true;
    }

    bool restore(const value &key, error *err = nullptr)
    {
	if ( !open(err) )
	    return false;
	if ( storage_key_field().empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet restore failed: no key field is configured");
	    return false;
	}
	if ( !_driver->restore_record(storage_key_field(), key, err) )
	    return false;
	invalidate_snapshot();
	return true;
    }

    bool compact(error *err = nullptr)
    {
	if ( !open(err) )
	    return false;
	if ( !_driver->compact_records(err) )
	    return false;
	invalidate_snapshot();
	return true;
    }

    bool erase(iterator pos, error *err = nullptr)
    {
	if ( pos == end(err) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet erase(iterator) failed: iterator is at end()");
	    return false;
	}
	return erase(record_key(*pos), err);
    }

    bool get(const value &key, T &out, error *err = nullptr) const
    {
	const_cast<DataSet<T> *>(this)->open(err);
	if ( !is_open() )
	    return false;
	if ( storage_key_field().empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet get failed: no key field is configured");
	    return false;
	}

	value storage_record;
	if ( !_driver->get_record(storage_key_field(), key, storage_record, err) )
	    return false;

	value logical = detail::storage_to_logical_record<T>(storage_record, _mapping);
	out = _mapper->decode(logical);
	return true;
    }

    bool get_by_locator(const RecordLocator &locator,
			T &out,
			error *err = nullptr) const
    {
	const_cast<DataSet<T> *>(this)->open(err);
	if ( !is_open() )
	    return false;

	value storage_record;
	if ( !_driver->get_record_by_locator(locator, storage_record, err) )
	    return false;

	value logical = detail::storage_to_logical_record<T>(storage_record, _mapping);
	out = _mapper->decode(logical);
	return true;
    }

    bool get_field(const value &key,
		   const std::string &field,
		   value &out,
		   error *err = nullptr) const
    {
	const_cast<DataSet<T> *>(this)->open(err);
	if ( !is_open() )
	    return false;
	if ( storage_key_field().empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet get_field failed: no key field is configured");
	    return false;
	}

	value storage_record;
	if ( !_driver->get_record(storage_key_field(), key, storage_record, err) )
	    return false;

	value logical = detail::storage_to_logical_record<T>(storage_record, _mapping);
	if ( !logical.is_object() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet get_field failed: record is not an object");
	    return false;
	}

	std::map<std::string, value>::const_iterator it = logical.as_object().find(field);
	if ( it == logical.as_object().end() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet get_field failed: field `" + field + "` not found");
	    return false;
	}

	out = it->second;
	return true;
    }

    std::unique_ptr<Cursor<T>> scan(error *err = nullptr) const
    {
	if ( !const_cast<DataSet<T> *>(this)->refresh_snapshot(err) )
	    return std::unique_ptr<Cursor<T>>();
	return std::unique_ptr<Cursor<T> >(new detail::VectorCursor<T>(*_iteration_rows));
    }

    std::unique_ptr<Cursor<T>> where_eq(const std::string &field,
					const value &match,
					error *err = nullptr) const
    {
	const_cast<DataSet<T> *>(this)->open(err);
	if ( !is_open() )
	    return std::unique_ptr<Cursor<T>>();

	std::vector<value> rows;
	if ( !_driver->scan_records(rows, err) )
	    return std::unique_ptr<Cursor<T>>();

	const std::string storage_field = detail::storage_name_for_logical<T>(_mapping, field);
	std::vector<T> decoded;
	for ( std::size_t i = 0; i < rows.size(); ++i )
	{
	    if ( !rows[i].is_object() )
		continue;
	    const std::map<std::string, value> &record = rows[i].as_object();
	    std::map<std::string, value>::const_iterator it = record.find(storage_field);
	    if ( it == record.end() || it->second != match )
		continue;
	    value logical = detail::storage_to_logical_record<T>(rows[i], _mapping);
	    decoded.push_back(_mapper->decode(logical));
	}

	return std::unique_ptr<Cursor<T>>(new detail::VectorCursor<T>(std::move(decoded)));
    }

    std::size_t size(error *err = nullptr) const
    {
	if ( !const_cast<DataSet<T> *>(this)->refresh_snapshot(err) )
	    return 0;
	return _iteration_rows->size();
    }

    std::size_t count(error *err = nullptr) const
    {
	return size(err);
    }

    std::size_t count(const std::string &field,
		      const value &match,
		      error *err = nullptr) const
    {
	std::unique_ptr<Cursor<T> > cur = where_eq(field, match, err);
	if ( !cur.get() )
	    return 0;
	std::size_t n = 0;
	T row;
	while ( cur->next(row) )
	    ++n;
	cur->close();
	return n;
    }

    bool empty(error *err = nullptr) const
    {
	return size(err) == 0;
    }

    iterator begin(error *err = nullptr) const
    {
	if ( !const_cast<DataSet<T> *>(this)->refresh_snapshot(err) )
	    return iterator();
	return iterator(_iteration_rows, 0);
    }

    iterator end(error *err = nullptr) const
    {
	if ( !_iteration_rows.get() )
	{
	    if ( !const_cast<DataSet<T> *>(this)->refresh_snapshot(err) )
		return iterator();
	}
	return iterator(_iteration_rows, _iteration_rows->size());
    }

    iterator find(const value &key, error *err = nullptr) const
    {
	if ( !const_cast<DataSet<T> *>(this)->refresh_snapshot(err) )
	    return iterator();
	for ( std::size_t i = 0; i < _iteration_rows->size(); ++i )
	{
	    if ( record_key((*_iteration_rows)[i]) == key )
		return iterator(_iteration_rows, i);
	}
	return end(err);
    }

    bool current(const iterator &it, T &out, error *err = nullptr) const
    {
	(void)err;
	if ( it == end() )
	    return false;
	out = *it;
	return true;
    }

    bool contains(const value &key, error *err = nullptr) const
    {
	T out;
	return get(key, out, err);
    }

private:
    void invalidate_snapshot() const
    {
	_iteration_rows.reset();
    }

    bool refresh_snapshot(error *err) const
    {
	const_cast<DataSet<T> *>(this)->open(err);
	if ( !is_open() )
	    return false;

	std::vector<value> rows;
	if ( !_driver->scan_records(rows, err) )
	    return false;

	std::shared_ptr<std::vector<T> > decoded(new std::vector<T>());
	decoded->reserve(rows.size());
	for ( std::size_t i = 0; i < rows.size(); ++i )
	{
	    value logical = detail::storage_to_logical_record<T>(rows[i], _mapping);
	    decoded->push_back(_mapper->decode(logical));
	}
	_iteration_rows = decoded;
	return true;
    }

    value record_key(const T &input) const
    {
	std::string logical_key = primary_key_field();
	if ( logical_key.empty() )
	    return value();
	value logical = _mapper->encode(input);
	if ( !logical.is_object() )
	    return value();
	std::map<std::string, value>::const_iterator it = logical.as_object().find(logical_key);
	if ( it == logical.as_object().end() )
	    return value();
	return it->second;
    }

    std::string primary_key_field() const
    {
	if ( !_mapping.keys().empty() )
	    return _mapping.keys()[0];
	for ( std::size_t i = 0; i < _storage_schema.fields().size(); ++i )
	{
	    const std::string logical =
		detail::logical_name_for_storage<T>(_mapping, _storage_schema.fields()[i].name);
	    if ( _storage_schema.fields()[i].key )
		return logical;
	}
	return std::string();
    }

    std::string storage_key_field() const
    {
	std::string logical = primary_key_field();
	if ( logical.empty() )
	    return std::string();
	return detail::storage_name_for_logical<T>(_mapping, logical);
    }

    std::string _name;
    DataSource _source;
    MappingSpec<T> _mapping;
    std::shared_ptr<DataMapper<T>> _mapper;
    std::shared_ptr<FormatAdapter<T>> _format;
    std::unique_ptr<DataDriver> _driver;
    SchemaInfo _storage_schema;
    mutable std::shared_ptr<std::vector<T> > _iteration_rows;
    bool _opened;
};

template <typename T>
DataSet<T> bind(const DataSource &source)
{
    return DataSet<T>(source);
}

template <typename T>
DataSet<T> bind(const std::string &uri)
{
    return DataSet<T>(uri);
}

} // namespace madc

#endif // __LIBMADC_DATASET_H
