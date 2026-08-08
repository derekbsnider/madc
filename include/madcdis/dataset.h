#ifndef __LIBMADCDIS_DATASET_H
#define __LIBMADCDIS_DATASET_H 1

#include "libmadc/datasource.h"
#include "madcdis/cursor.h"
#include "madcdis/driver.h"
#include "libmadc/error.h"
#include "madcdis/mapper.h"
#include "madcdis/query.h"
#include "libmadc/value.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace madc {

namespace detail {

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
	typename std::map<std::string, typename MappingSpec<T>::text_limit>::const_iterator lim =
	    spec.text_limits().find(field.name);
	if ( lim != spec.text_limits().end() )
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

inline int compare_query_values(const value &lhs, const value &rhs)
{
    if ( lhs.is_integer() || rhs.is_integer() )
    {
	int64_t a = lhs.as_integer();
	int64_t b = rhs.as_integer();
	if ( a > b )
	    return 1;
	if ( a == b )
	    return 0;
	return -1;
    }
    if ( lhs.is_real() || rhs.is_real() )
    {
	double a = lhs.as_real();
	double b = rhs.as_real();
	if ( a > b )
	    return 1;
	if ( a == b )
	    return 0;
	return -1;
    }
    const std::string &a = lhs.as_string();
    const std::string &b = rhs.as_string();
    if ( a > b )
	return 1;
    if ( a == b )
	return 0;
    return -1;
}

inline bool match_like_pattern_from(const std::string &input,
				    std::size_t input_pos,
				    const std::string &pattern,
				    std::size_t pattern_pos)
{
    while ( pattern_pos < pattern.size() )
    {
	char p = pattern[pattern_pos];
	if ( p == '%' )
	{
	    while ( pattern_pos < pattern.size() && pattern[pattern_pos] == '%' )
		++pattern_pos;
	    if ( pattern_pos == pattern.size() )
		return true;
	    for ( std::size_t i = input_pos; i <= input.size(); ++i )
	    {
		if ( match_like_pattern_from(input, i, pattern, pattern_pos) )
		    return true;
	    }
	    return false;
	}
	if ( input_pos >= input.size() )
	    return false;
	if ( p == '_' )
	{
	    ++input_pos;
	    ++pattern_pos;
	    continue;
	}
	if ( input[input_pos] != p )
	    return false;
	++input_pos;
	++pattern_pos;
    }
    return input_pos == input.size();
}

inline bool record_matches_query(const value &logical, const Query &query_spec)
{
    if ( query_spec.has_where_equality() )
    {
	if ( !logical.is_object() )
	    return false;
	std::map<std::string, value>::const_iterator it =
	    logical.as_object().find(query_spec.where_field());
	if ( it == logical.as_object().end() || it->second != query_spec.where_value() )
	    return false;
    }
    if ( query_spec.has_where_inequality() )
    {
	if ( !logical.is_object() )
	    return false;
	std::map<std::string, value>::const_iterator it =
	    logical.as_object().find(query_spec.where_ne_field());
	if ( it == logical.as_object().end() || it->second == query_spec.where_ne_value() )
	    return false;
    }
    if ( query_spec.has_where_in() )
    {
	if ( !logical.is_object() )
	    return false;
	std::map<std::string, value>::const_iterator it =
	    logical.as_object().find(query_spec.where_in_field());
	if ( it == logical.as_object().end() )
	    return false;
	bool matched = false;
	for ( std::size_t i = 0; i < query_spec.where_in_values().size(); ++i )
	{
	    if ( it->second == query_spec.where_in_values()[i] )
	    {
		matched = true;
		break;
	    }
	}
	if ( !matched )
	    return false;
    }
    if ( query_spec.has_where_not_in() )
    {
	if ( !logical.is_object() )
	    return false;
	std::map<std::string, value>::const_iterator it =
	    logical.as_object().find(query_spec.where_not_in_field());
	if ( it == logical.as_object().end() )
	    return false;
	for ( std::size_t i = 0; i < query_spec.where_not_in_values().size(); ++i )
	{
	    if ( it->second == query_spec.where_not_in_values()[i] )
		return false;
	}
    }
    if ( query_spec.has_where_like() )
    {
	if ( !logical.is_object() )
	    return false;
	std::map<std::string, value>::const_iterator it =
	    logical.as_object().find(query_spec.where_like_field());
	if ( it == logical.as_object().end() )
	    return false;
	if ( !it->second.is_string() || !query_spec.where_like_value().is_string() )
	    return false;
	if ( !match_like_pattern_from(it->second.as_string(), 0,
				      query_spec.where_like_value().as_string(), 0) )
	    return false;
    }
    if ( query_spec.has_lower_bound() )
    {
	if ( !logical.is_object() )
	    return false;
	std::map<std::string, value>::const_iterator it =
	    logical.as_object().find(query_spec.lower_bound_field());
	if ( it == logical.as_object().end() )
	    return false;
	int compared = compare_query_values(it->second, query_spec.lower_bound_value());
	if ( compared < 0 || (!query_spec.lower_bound_inclusive() && compared == 0) )
	    return false;
    }
    if ( query_spec.has_upper_bound() )
    {
	if ( !logical.is_object() )
	    return false;
	std::map<std::string, value>::const_iterator it =
	    logical.as_object().find(query_spec.upper_bound_field());
	if ( it == logical.as_object().end() )
	    return false;
	int compared = compare_query_values(it->second, query_spec.upper_bound_value());
	if ( compared > 0 || (!query_spec.upper_bound_inclusive() && compared == 0) )
	    return false;
    }
    return true;
}

inline value project_logical_record(const value &logical, const Query &query_spec)
{
    if ( query_spec.selected_fields().empty() || !logical.is_object() )
	return logical;

    value projected = value::make_object();
    for ( std::size_t i = 0; i < query_spec.selected_fields().size(); ++i )
    {
	const std::string &field = query_spec.selected_fields()[i];
	std::map<std::string, value>::const_iterator it = logical.as_object().find(field);
	if ( it != logical.as_object().end() )
	    projected.object()[field] = it->second;
    }
    return projected;
}

template <typename T>
class LogicalCursor : public Cursor<value>, public ErrorAwareCursor<value> {
public:
    LogicalCursor(std::unique_ptr<Cursor<value> > upstream,
		  const MappingSpec<T> &mapping)
	: upstream_(std::move(upstream)), mapping_(mapping), apply_query_(false),
	  emitted_(0), closed_(false)
    {}

    LogicalCursor(std::unique_ptr<Cursor<value> > upstream,
		  const MappingSpec<T> &mapping,
		  const Query &query)
	: upstream_(std::move(upstream)), mapping_(mapping), query_(query),
	  apply_query_(true), emitted_(0), closed_(false)
    {}

    ~LogicalCursor() override { close(); }

    bool next(value &out) override
    {
	return next_status(out, nullptr) == CursorStatus::item;
    }

    CursorStatus next_status(value &out, error *err) override
    {
	if ( closed_ || !upstream_.get() )
	    return CursorStatus::end;
	if ( apply_query_ && query_.has_limit() && emitted_ >= query_.row_limit() )
	    return CursorStatus::end;

	value storage;
	for ( ;; )
	{
	    CursorStatus status = cursor_next(*upstream_, storage, err);
	    if ( status != CursorStatus::item )
		return status;
	    value logical = storage_to_logical_record<T>(storage, mapping_);
	    if ( apply_query_ && !record_matches_query(logical, query_) )
		continue;
	    out = apply_query_ ? project_logical_record(logical, query_) : logical;
	    ++emitted_;
	    return CursorStatus::item;
	}
    }

    void close() override
    {
	if ( closed_ )
	    return;
	if ( upstream_.get() )
	    upstream_->close();
	closed_ = true;
    }

private:
    std::unique_ptr<Cursor<value> > upstream_;
    MappingSpec<T> mapping_;
    Query query_;
    bool apply_query_;
    std::size_t emitted_;
    bool closed_;
};

template <typename T>
class DecodingCursor : public Cursor<T>, public ErrorAwareCursor<T> {
public:
    DecodingCursor(std::unique_ptr<Cursor<value> > upstream,
		   std::shared_ptr<DataMapper<T> > mapper)
	: upstream_(std::move(upstream)), mapper_(mapper), closed_(false)
    {}

    ~DecodingCursor() override { close(); }

    bool next(T &out) override
    {
	return next_status(out, nullptr) == CursorStatus::item;
    }

    CursorStatus next_status(T &out, error *err) override
    {
	if ( closed_ || !upstream_.get() )
	    return CursorStatus::end;
	value logical;
	CursorStatus status = cursor_next(*upstream_, logical, err);
	if ( status == CursorStatus::item )
	    out = mapper_->decode(logical);
	return status;
    }

    void close() override
    {
	if ( closed_ )
	    return;
	if ( upstream_.get() )
	    upstream_->close();
	closed_ = true;
    }

private:
    std::unique_ptr<Cursor<value> > upstream_;
    std::shared_ptr<DataMapper<T> > mapper_;
    bool closed_;
};

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

    bool update_by_locator(const RecordLocator &locator,
			   const T &input,
			   error *err = nullptr)
    {
	if ( !open(err) )
	    return false;
	value logical = _mapper->encode(input);
	value storage = detail::logical_to_storage_record<T>(logical, _mapping);
	if ( !_driver->update_record_by_locator(locator, storage, err) )
	    return false;
	invalidate_snapshot();
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

    bool get_field_from_row(const T &row,
			    const std::string &field,
			    value &out,
			    error *err = nullptr) const
    {
	value logical = _mapper->encode(row);
	if ( !logical.is_object() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet get_field_from_row failed: record is not an object");
	    return false;
	}

	std::map<std::string, value>::const_iterator it = logical.as_object().find(field);
	if ( it == logical.as_object().end() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet get_field_from_row failed: field `" + field + "` not found");
	    return false;
	}

	out = it->second;
	return true;
    }

    bool row_matches_query(const T &row,
			   const Query &query_spec,
			   error *err = nullptr) const
    {
	value logical = _mapper->encode(row);
	if ( !logical.is_object() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet row_matches_query failed: record is not an object");
	    return false;
	}
	return detail::record_matches_query(logical, query_spec);
    }

    bool project_row(const T &row,
		     const Query &query_spec,
		     value &out,
		     error *err = nullptr) const
    {
	value logical = _mapper->encode(row);
	if ( !logical.is_object() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet project_row failed: record is not an object");
	    return false;
	}
	out = detail::project_logical_record(logical, query_spec);
	return true;
    }

    bool get_key_from_row(const T &row,
			  value &out,
			  error *err = nullptr) const
    {
	out = record_key(row);
	if ( out.type() == value::kind::null )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet get_key_from_row failed: primary key is unavailable");
	    return false;
	}
	return true;
    }

    std::unique_ptr<Cursor<T>> scan(error *err = nullptr) const
    {
	const_cast<DataSet<T> *>(this)->open(err);
	if ( !is_open() )
	    return std::unique_ptr<Cursor<T>>();
	std::unique_ptr<Cursor<value> > storage = scan_driver_cursor(*_driver, err);
	if ( !storage.get() )
	    return std::unique_ptr<Cursor<T> >();
	std::unique_ptr<Cursor<value> > logical(
	    new detail::LogicalCursor<T>(std::move(storage), _mapping));
	return std::unique_ptr<Cursor<T> >(
	    new detail::DecodingCursor<T>(std::move(logical), _mapper));
    }

    std::unique_ptr<Cursor<T>> query(const Query &query_spec,
				     error *err = nullptr) const
    {
	const_cast<DataSet<T> *>(this)->open(err);
	if ( !is_open() )
	    return std::unique_ptr<Cursor<T>>();

	if ( query_spec.query_kind() != Query::kind::builder )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet query failed: only builder queries are supported");
	    return std::unique_ptr<Cursor<T>>();
	}
	if ( query_spec.predicate_match_mode() != Query::match_mode::all )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet query failed: non-default logical composition is not implemented yet");
	    return std::unique_ptr<Cursor<T>>();
	}
	if ( !query_spec.selected_fields().empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet query failed: typed projection is not implemented yet");
	    return std::unique_ptr<Cursor<T>>();
	}
	if ( !query_spec.dataset_name().empty()
	  && query_spec.dataset_name() != _name
	  && query_spec.dataset_name() != _storage_schema.name() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet query failed: query targets dataset `" + query_spec.dataset_name()
			     + "` but this DataSet is `" + _name + "`");
	    return std::unique_ptr<Cursor<T>>();
	}

	Query storage_query = translate_query_for_storage(query_spec);
	if ( _driver->can_execute(storage_query) )
	{
	    error pushdown_err;
	    std::unique_ptr<Cursor<value> > storage =
		query_driver_cursor(*_driver, storage_query, &pushdown_err);
	    if ( storage.get() )
	    {
		std::unique_ptr<Cursor<value> > logical(
		    new detail::LogicalCursor<T>(std::move(storage), _mapping));
		return std::unique_ptr<Cursor<T> >(
		    new detail::DecodingCursor<T>(std::move(logical), _mapper));
	    }
	}

	return execute_query_locally(query_spec, err);
    }

    std::unique_ptr<Cursor<value>> query_raw(const Query &query_spec,
					     error *err = nullptr) const
    {
	const_cast<DataSet<T> *>(this)->open(err);
	if ( !is_open() )
	    return std::unique_ptr<Cursor<value> >();

	if ( query_spec.query_kind() != Query::kind::builder )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet query_raw failed: only builder queries are supported");
	    return std::unique_ptr<Cursor<value> >();
	}
	if ( query_spec.predicate_match_mode() != Query::match_mode::all )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet query_raw failed: non-default logical composition is not implemented yet");
	    return std::unique_ptr<Cursor<value> >();
	}
	if ( !query_targets_this_dataset(query_spec) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "DataSet query_raw failed: query targets dataset `" + query_spec.dataset_name()
			     + "` but this DataSet is `" + _name + "`");
	    return std::unique_ptr<Cursor<value> >();
	}

	Query storage_query = translate_query_for_storage(query_spec);
	if ( _driver->can_execute(storage_query) )
	{
	    error pushdown_err;
	    std::unique_ptr<Cursor<value> > storage =
		query_driver_cursor(*_driver, storage_query, &pushdown_err);
	    if ( storage.get() )
		return std::unique_ptr<Cursor<value> >(
		    new detail::LogicalCursor<T>(std::move(storage), _mapping));
	}

	return execute_query_locally_raw(query_spec, err);
    }

    std::unique_ptr<Cursor<T>> where_eq(const std::string &field,
					const value &match,
					error *err = nullptr) const
    {
	QueryBuilder builder = madc::query();
	builder.from(_name).where_eq(field, match);
	return query(builder.build(), err);
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
    bool query_targets_this_dataset(const Query &query_spec) const
    {
	return query_spec.dataset_name().empty()
	    || query_spec.dataset_name() == _name
	    || query_spec.dataset_name() == _storage_schema.name();
    }

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

    Query translate_query_for_storage(const Query &logical_query) const
    {
	Query storage_query = logical_query;
	if ( !storage_query.dataset_name().empty() )
	    storage_query.set_dataset_name(_storage_schema.name());
	if ( storage_query.has_where_equality() )
	{
	    storage_query.set_where_equality(
		detail::storage_name_for_logical<T>(_mapping, storage_query.where_field()),
		storage_query.where_value());
	}
	if ( storage_query.has_where_inequality() )
	{
	    storage_query.set_where_inequality(
		detail::storage_name_for_logical<T>(_mapping, storage_query.where_ne_field()),
		storage_query.where_ne_value());
	}
	if ( storage_query.has_where_in() )
	{
	    storage_query.set_where_in(
		detail::storage_name_for_logical<T>(_mapping, storage_query.where_in_field()),
		storage_query.where_in_values());
	}
	if ( storage_query.has_where_not_in() )
	{
	    storage_query.set_where_not_in(
		detail::storage_name_for_logical<T>(_mapping, storage_query.where_not_in_field()),
		storage_query.where_not_in_values());
	}
	if ( storage_query.has_where_like() )
	{
	    storage_query.set_where_like(
		detail::storage_name_for_logical<T>(_mapping, storage_query.where_like_field()),
		storage_query.where_like_value());
	}
	if ( storage_query.has_lower_bound() )
	{
	    storage_query.set_lower_bound(
		detail::storage_name_for_logical<T>(_mapping, storage_query.lower_bound_field()),
		storage_query.lower_bound_value(),
		storage_query.lower_bound_inclusive());
	}
	if ( storage_query.has_upper_bound() )
	{
	    storage_query.set_upper_bound(
		detail::storage_name_for_logical<T>(_mapping, storage_query.upper_bound_field()),
		storage_query.upper_bound_value(),
		storage_query.upper_bound_inclusive());
	}
	if ( !storage_query.selected_fields().empty() )
	{
	    std::vector<std::string> storage_fields;
	    for ( std::size_t i = 0; i < storage_query.selected_fields().size(); ++i )
	    {
		storage_fields.push_back(
		    detail::storage_name_for_logical<T>(_mapping, storage_query.selected_fields()[i]));
	    }
	    storage_query.set_selected_fields(storage_fields);
	}
	return storage_query;
    }

    std::unique_ptr<Cursor<T>> execute_query_locally(const Query &query_spec,
						     error *err) const
    {
	std::unique_ptr<Cursor<value> > storage = scan_driver_cursor(*_driver, err);
	if ( !storage.get() )
	    return std::unique_ptr<Cursor<T>>();
	std::unique_ptr<Cursor<value> > logical(
	    new detail::LogicalCursor<T>(std::move(storage), _mapping, query_spec));
	return std::unique_ptr<Cursor<T> >(
	    new detail::DecodingCursor<T>(std::move(logical), _mapper));
    }

    std::unique_ptr<Cursor<value>> execute_query_locally_raw(const Query &query_spec,
							     error *err) const
    {
	std::unique_ptr<Cursor<value> > storage = scan_driver_cursor(*_driver, err);
	if ( !storage.get() )
	    return std::unique_ptr<Cursor<value> >();
	return std::unique_ptr<Cursor<value> >(
	    new detail::LogicalCursor<T>(std::move(storage), _mapping, query_spec));
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

#endif // __LIBMADCDIS_DATASET_H
