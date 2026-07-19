#include "madcdis/driver.h"
#include "madcdis/query.h"

#ifdef HAVE_BDB

#include <db.h>

#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace madc {

namespace {

template <typename T>
void append_pod(std::vector<char> &buffer, const T &input)
{
    std::size_t old = buffer.size();
    buffer.resize(old + sizeof(T));
    std::memcpy(&buffer[old], &input, sizeof(T));
}

template <typename T>
T read_pod(const std::vector<char> &buffer, std::size_t offset)
{
    T out;
    std::memcpy(&out, &buffer[offset], sizeof(T));
    return out;
}

bool field_is_integer(const SchemaField &field)
{
    return field.resolved_kind() == SchemaField::kind::integer;
}

bool field_is_real(const SchemaField &field)
{
    return field.resolved_kind() == SchemaField::kind::real;
}

bool field_is_boolean(const SchemaField &field)
{
    return field.resolved_kind() == SchemaField::kind::boolean;
}

bool field_is_character(const SchemaField &field)
{
    return field.resolved_kind() == SchemaField::kind::character;
}

bool field_is_text(const SchemaField &field)
{
    return field.resolved_kind() == SchemaField::kind::text
	|| field.resolved_kind() == SchemaField::kind::object;
}

bool is_supported_key_field(const SchemaField &field)
{
    return field_is_integer(field)
	|| field_is_boolean(field)
	|| field_is_character(field)
	|| field_is_text(field);
}

int compare_query_values(const value &lhs, const value &rhs)
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

value project_record(const value &record, const Query &query)
{
    if ( query.selected_fields().empty() || !record.is_object() )
	return record;

    value projected = value::make_object();
    for ( std::size_t i = 0; i < query.selected_fields().size(); ++i )
    {
	std::map<std::string, value>::const_iterator it =
	    record.as_object().find(query.selected_fields()[i]);
	if ( it != record.as_object().end() )
	    projected.object()[query.selected_fields()[i]] = it->second;
    }
    return projected;
}

void append_be(std::string &out, uint64_t value, std::size_t width)
{
    for ( std::size_t i = 0; i < width; ++i )
    {
	std::size_t shift = (width - 1 - i) * 8;
	out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

std::string encode_key_value(const value &input, const SchemaField &field)
{
    std::string out;

    if ( field_is_boolean(field) )
    {
	out.push_back(input.as_boolean() ? '\1' : '\0');
	return out;
    }

    if ( field_is_character(field) )
    {
	const std::string &text = input.as_string();
	out.push_back(text.empty() ? '\0' : text[0]);
	return out;
    }

    if ( field_is_text(field) )
	return input.as_string();

    if ( field_is_integer(field) )
    {
	std::size_t width = field.byte_size ? field.byte_size : sizeof(int64_t);
	if ( width == 8 )
	{
	    uint64_t raw = static_cast<uint64_t>(input.as_integer());
	    if ( field.is_signed )
		raw ^= 0x8000000000000000ULL;
	    append_be(out, raw, 8);
	    return out;
	}
	if ( width == 4 )
	{
	    uint32_t raw = static_cast<uint32_t>(input.as_integer());
	    if ( field.is_signed )
		raw ^= 0x80000000U;
	    append_be(out, raw, 4);
	    return out;
	}
	if ( width == 2 )
	{
	    uint16_t raw = static_cast<uint16_t>(input.as_integer());
	    if ( field.is_signed )
		raw ^= 0x8000U;
	    append_be(out, raw, 2);
	    return out;
	}
	if ( width == 1 )
	{
	    uint8_t raw = static_cast<uint8_t>(input.as_integer());
	    if ( field.is_signed )
		raw ^= 0x80U;
	    out.push_back(static_cast<char>(raw));
	    return out;
	}
    }

    throw std::runtime_error("bdb key field `" + field.name + "` uses unsupported key type");
}

class BdbDriver : public DataDriver
{
public:
    BdbDriver()
	: _db(nullptr), _opened(false), _key_field(nullptr)
    {}

    ~BdbDriver()
    {
	close();
    }

    const char *name() const { return "bdb"; }
    const char *scheme() const { return "bdb"; }

    DriverCapabilities capabilities() const
    {
	DriverCapabilities caps;
	caps.read = true;
	caps.write = true;
	caps.scan = true;
	caps.point_lookup = true;
	caps.range_lookup = true;
	caps.filter_pushdown = true;
	caps.limit_pushdown = true;
	return caps;
    }

    bool bind_schema(const SchemaInfo &schema, error *err = nullptr)
    {
	_schema = schema;
	_key_field = nullptr;
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( _schema.fields()[i].key )
	    {
		_key_field = &_schema.fields()[i];
		break;
	    }
	}
	if ( !can_bind_schema(schema) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "bdb driver requires a logical or variable-record schema with one supported key field");
	    return false;
	}
	return true;
    }

    bool open(const DataSource &source, error *err = nullptr)
    {
	if ( source.scheme() != "bdb" )
	    return fail(err, "bdb driver cannot open scheme `" + source.scheme() + "`");

	close();
	_path = source.path();

	int rc = db_create(&_db, nullptr, 0);
	if ( rc != 0 || !_db )
	    return fail(err, "failed to create Berkeley DB handle");

	rc = _db->open(_db,
		      nullptr,
		      _path.c_str(),
		      nullptr,
		      DB_BTREE,
		      DB_CREATE,
		      0600);
	if ( rc != 0 )
	{
	    close();
	    return fail(err, "failed to open bdb path: " + _path);
	}

	_opened = true;
	return true;
    }

    void close()
    {
	if ( _db )
	    _db->close(_db, 0);
	_db = nullptr;
	_path.clear();
	_opened = false;
    }

    bool is_open() const { return _opened && _db != nullptr; }

    bool can_bind_schema(const SchemaInfo &schema) const
    {
	if ( schema.fields().empty() )
	    return false;
	if ( schema.record_layout() == SchemaInfo::layout_mode::fixed_record )
	    return false;

	std::size_t key_count = 0;
	for ( std::size_t i = 0; i < schema.fields().size(); ++i )
	{
	    const SchemaField &field = schema.fields()[i];
	    if ( field.field_shape == SchemaField::shape::array
	      || field.field_shape == SchemaField::shape::bytes
	      || field.field_shape == SchemaField::shape::dynamic )
		return false;
	    if ( field.needs_scalar_width() && field.byte_size == 0 )
		return false;
	    if ( field.resolved_kind() == SchemaField::kind::unknown )
		return false;
	    if ( field.key )
	    {
		++key_count;
		if ( !is_supported_key_field(field) )
		    return false;
	    }
	}
	return key_count == 1;
    }

    bool can_execute(const Query &query) const
    {
	return query.query_kind() == Query::kind::builder
	    && _key_field
	    && !query.has_where_inequality()
	    && !query.has_where_in()
	    && !query.has_where_not_in()
	    && !query.has_where_like()
	    && ((!query.has_where_equality())
		|| query.where_field() == _key_field->name)
	    && ((!query.has_lower_bound())
		|| query.lower_bound_field() == _key_field->name)
	    && ((!query.has_upper_bound())
		|| query.upper_bound_field() == _key_field->name)
	    && (query.has_where_equality() || query.has_lower_bound() || query.has_upper_bound());
    }

    bool insert_record(const value &record, error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;

	DBT key, data;
	if ( !build_key_dbt(record, key, err) )
	    return false;
	if ( !build_data_dbt(record, data, err) )
	    return false;

	int rc = _db->put(_db, nullptr, &key, &data, DB_NOOVERWRITE);
	if ( rc == DB_KEYEXIST )
	    return fail(err, "bdb insert failed: key already exists");
	if ( rc != 0 )
	    return fail(err, "bdb insert failed");
	return true;
    }

    bool update_record(const std::string &key_field,
		       const value &key_value,
		       const value &record,
		       error *err = nullptr)
    {
	(void)key_field;
	if ( !ensure_record_shape(record, err) )
	    return false;

	const std::map<std::string, value> &obj = record.as_object();
	std::map<std::string, value>::const_iterator it = obj.find(_key_field->name);
	if ( it == obj.end() || it->second != key_value )
	    return fail(err, "bdb update failed: record key does not match lookup key");

	DBT key;
	if ( !build_lookup_key_dbt(key_value, key, err) )
	    return false;
	if ( !exists(key) )
	    return fail(err, "bdb update failed: key not found");

	DBT data;
	if ( !build_data_dbt(record, data, err) )
	    return false;

	int rc = _db->put(_db, nullptr, &key, &data, 0);
	if ( rc != 0 )
	    return fail(err, "bdb update failed");
	return true;
    }

    bool erase_record(const std::string &key_field,
		      const value &key_value,
		      error *err = nullptr)
    {
	(void)key_field;
	DBT key;
	if ( !build_lookup_key_dbt(key_value, key, err) )
	    return false;
	int rc = _db->del(_db, nullptr, &key, 0);
	if ( rc == DB_NOTFOUND )
	    return fail(err, "bdb erase failed: key not found");
	if ( rc != 0 )
	    return fail(err, "bdb erase failed");
	return true;
    }

    bool restore_record(const std::string &key_field,
			const value &key_value,
			error *err = nullptr)
    {
	(void)key_field;
	(void)key_value;
	return fail(err, "bdb restore is unsupported without a tombstone sidecar");
    }

    bool compact_records(error *err = nullptr)
    {
	return fail(err, "bdb compact is unsupported");
    }

    bool get_record(const std::string &key_field,
		    const value &key_value,
		    value &out,
		    error *err = nullptr) const
    {
	(void)key_field;
	DBT key, data;
	if ( !build_lookup_key_dbt(key_value, key, err) )
	    return false;
	std::memset(&data, 0, sizeof(data));
	data.flags = DB_DBT_MALLOC;
	int rc = _db->get(_db, nullptr, &key, &data, 0);
	if ( rc == DB_NOTFOUND )
	    return false;
	if ( rc != 0 )
	    return fail(err, "bdb get failed");

	std::vector<char> payload(static_cast<char *>(data.data),
				  static_cast<char *>(data.data) + data.size);
	std::free(data.data);
	return decode_record(payload, out, err);
    }

    bool execute_query(const Query &query,
		       std::vector<value> &out,
		       error *err = nullptr) const
    {
	out.clear();
	if ( !can_execute(query) )
	    return fail(err, "bdb query execution only supports primary-key equality and key-range filters");
	if ( query.has_where_equality() )
	{
	    value record;
	    if ( !get_record(_key_field->name, query.where_value(), record, err) )
		return true;
	    out.push_back(project_record(record, query));
	    return true;
	}

	DBT key, data;
	std::memset(&key, 0, sizeof(key));
	if ( query.has_lower_bound() )
	{
	    if ( !build_lookup_key_dbt(query.lower_bound_value(), key, err) )
		return false;
	}

	DBC *cursor = nullptr;
	int rc = _db->cursor(_db, nullptr, &cursor, 0);
	if ( rc != 0 )
	    return fail(err, "bdb range query failed: could not open cursor");

	std::memset(&data, 0, sizeof(data));
	data.flags = DB_DBT_MALLOC;
	rc = cursor->get(cursor, &key, &data, query.has_lower_bound() ? DB_SET_RANGE : DB_FIRST);
	for ( ; rc == 0; rc = cursor->get(cursor, &key, &data, DB_NEXT) )
	{
	    std::vector<char> payload(static_cast<char *>(data.data),
				      static_cast<char *>(data.data) + data.size);
	    std::free(data.data);
	    data.data = nullptr;

	    value record;
	    if ( !decode_record(payload, record, err) )
	    {
		cursor->close(cursor);
		return false;
	    }
	    std::map<std::string, value>::const_iterator key_it = record.as_object().find(_key_field->name);
	    if ( key_it == record.as_object().end() )
	    {
		cursor->close(cursor);
		return fail(err, "bdb range query failed: decoded record is missing key field");
	    }
	    if ( query.has_lower_bound() )
	    {
		int cmp = compare_query_values(key_it->second, query.lower_bound_value());
		if ( cmp < 0 || (!query.lower_bound_inclusive() && cmp == 0) )
		{
		    std::memset(&data, 0, sizeof(data));
		    data.flags = DB_DBT_MALLOC;
		    continue;
		}
	    }
	    if ( query.has_upper_bound() )
	    {
		int cmp = compare_query_values(key_it->second, query.upper_bound_value());
		if ( cmp > 0 || (!query.upper_bound_inclusive() && cmp == 0) )
		    break;
	    }
	    out.push_back(project_record(record, query));
	    if ( query.has_limit() && out.size() >= query.row_limit() )
		break;

	    std::memset(&data, 0, sizeof(data));
	    data.flags = DB_DBT_MALLOC;
	}

	cursor->close(cursor);
	if ( rc != 0 && rc != DB_NOTFOUND )
	    return fail(err, "bdb range query failed during cursor iteration");
	return true;
    }

    bool scan_records(std::vector<value> &out,
		      error *err = nullptr) const
    {
	out.clear();
	DBC *cursor = nullptr;
	int rc = _db->cursor(_db, nullptr, &cursor, 0);
	if ( rc != 0 )
	    return fail(err, "bdb scan failed: could not open cursor");

	DBT key, data;
	std::memset(&key, 0, sizeof(key));
	std::memset(&data, 0, sizeof(data));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	for ( rc = cursor->get(cursor, &key, &data, DB_FIRST);
	      rc == 0;
	      rc = cursor->get(cursor, &key, &data, DB_NEXT) )
	{
	    std::vector<char> payload(static_cast<char *>(data.data),
				      static_cast<char *>(data.data) + data.size);
	    value record;
	    if ( !decode_record(payload, record, err) )
	    {
		std::free(key.data);
		std::free(data.data);
		cursor->close(cursor);
		return false;
	    }
	    out.push_back(record);
	    std::free(key.data);
	    std::free(data.data);
	    std::memset(&key, 0, sizeof(key));
	    std::memset(&data, 0, sizeof(data));
	    key.flags = DB_DBT_MALLOC;
	    data.flags = DB_DBT_MALLOC;
	}

	cursor->close(cursor);
	if ( rc != DB_NOTFOUND )
	    return fail(err, "bdb scan failed during cursor iteration");
	return true;
    }

private:
    bool exists(DBT &key) const
    {
	int rc = _db->exists(_db, nullptr, &key, 0);
	return rc == 0;
    }

    bool ensure_record_shape(const value &record, error *err) const
    {
	if ( !record.is_object() )
	    return fail(err, "bdb record must be an object");
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( record.as_object().count(_schema.fields()[i].name) == 0 )
		return fail(err, "bdb record is missing field `" + _schema.fields()[i].name + "`");
	}
	return true;
    }

    bool build_key_dbt(const value &record, DBT &out, error *err) const
    {
	const std::map<std::string, value> &obj = record.as_object();
	std::map<std::string, value>::const_iterator it = obj.find(_key_field->name);
	if ( it == obj.end() )
	    return fail(err, "bdb record is missing primary key field `" + _key_field->name + "`");
	return build_lookup_key_dbt(it->second, out, err);
    }

    bool build_lookup_key_dbt(const value &input, DBT &out, error *err) const
    {
	try
	{
	    _key_buffer = encode_key_value(input, *_key_field);
	    std::memset(&out, 0, sizeof(out));
	    out.data = _key_buffer.empty() ? nullptr : const_cast<char *>(_key_buffer.data());
	    out.size = static_cast<u_int32_t>(_key_buffer.size());
	    return true;
	}
	catch ( const std::exception &e )
	{
	    return fail(err, e.what());
	}
    }

    bool build_data_dbt(const value &record, DBT &out, error *err) const
    {
	if ( !encode_record(record, _data_buffer, err) )
	    return false;
	std::memset(&out, 0, sizeof(out));
	out.data = _data_buffer.empty() ? nullptr : &_data_buffer[0];
	out.size = static_cast<u_int32_t>(_data_buffer.size());
	return true;
    }

    bool encode_record(const value &record, std::vector<char> &out, error *err) const
    {
	out.clear();
	const std::map<std::string, value> &obj = record.as_object();
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    const SchemaField &field = _schema.fields()[i];
	    const value &input = obj.at(field.name);

	    if ( field_is_boolean(field) )
	    {
		uint8_t b = input.as_boolean() ? 1 : 0;
		append_pod(out, b);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 8 )
	    {
		if ( field.is_signed )
		    append_pod(out, static_cast<int64_t>(input.as_integer()));
		else
		    append_pod(out, static_cast<uint64_t>(input.as_integer()));
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 4 )
	    {
		if ( field.is_signed )
		    append_pod(out, static_cast<int32_t>(input.as_integer()));
		else
		    append_pod(out, static_cast<uint32_t>(input.as_integer()));
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 2 )
	    {
		if ( field.is_signed )
		    append_pod(out, static_cast<int16_t>(input.as_integer()));
		else
		    append_pod(out, static_cast<uint16_t>(input.as_integer()));
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 1 )
	    {
		if ( field.is_signed )
		    append_pod(out, static_cast<int8_t>(input.as_integer()));
		else
		    append_pod(out, static_cast<uint8_t>(input.as_integer()));
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 4 )
	    {
		append_pod(out, static_cast<float>(input.as_real()));
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 8 )
	    {
		append_pod(out, static_cast<double>(input.as_real()));
		continue;
	    }
	    if ( field_is_character(field) )
	    {
		const std::string &text = input.as_string();
		append_pod(out, text.empty() ? '\0' : text[0]);
		continue;
	    }
	    if ( field_is_text(field) )
	    {
		const std::string &text = input.as_string();
		if ( text.size() > 0xffffffffu )
		    return fail(err, "bdb text field `" + field.name + "` exceeds 32-bit length limit");
		uint32_t len = static_cast<uint32_t>(text.size());
		append_pod(out, len);
		if ( len )
		    out.insert(out.end(), text.begin(), text.end());
		continue;
	    }

	    return fail(err, "bdb field `" + field.name + "` uses unsupported type `" + field.type_name + "`");
	}
	return true;
    }

    bool decode_record(const std::vector<char> &payload, value &out, error *err) const
    {
	out = value::make_object();
	std::size_t offset = 0;
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    const SchemaField &field = _schema.fields()[i];
	    if ( field_is_boolean(field) )
	    {
		if ( offset + sizeof(uint8_t) > payload.size() )
		    return fail(err, "bdb payload truncated in bool field");
		out.object()[field.name] = value(read_pod<uint8_t>(payload, offset) != 0);
		offset += sizeof(uint8_t);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 8 )
	    {
		if ( offset + 8 > payload.size() )
		    return fail(err, "bdb payload truncated in int64 field");
		if ( field.is_signed )
		    out.object()[field.name] = value(read_pod<int64_t>(payload, offset));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod<uint64_t>(payload, offset)));
		offset += 8;
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 4 )
	    {
		if ( offset + 4 > payload.size() )
		    return fail(err, "bdb payload truncated in int32 field");
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod<int32_t>(payload, offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod<uint32_t>(payload, offset)));
		offset += 4;
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 2 )
	    {
		if ( offset + 2 > payload.size() )
		    return fail(err, "bdb payload truncated in int16 field");
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod<int16_t>(payload, offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod<uint16_t>(payload, offset)));
		offset += 2;
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 1 )
	    {
		if ( offset + 1 > payload.size() )
		    return fail(err, "bdb payload truncated in int8 field");
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod<int8_t>(payload, offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod<uint8_t>(payload, offset)));
		offset += 1;
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 4 )
	    {
		if ( offset + 4 > payload.size() )
		    return fail(err, "bdb payload truncated in float field");
		out.object()[field.name] = value(static_cast<double>(read_pod<float>(payload, offset)));
		offset += 4;
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 8 )
	    {
		if ( offset + 8 > payload.size() )
		    return fail(err, "bdb payload truncated in double field");
		out.object()[field.name] = value(read_pod<double>(payload, offset));
		offset += 8;
		continue;
	    }
	    if ( field_is_character(field) )
	    {
		if ( offset + 1 > payload.size() )
		    return fail(err, "bdb payload truncated in char field");
		out.object()[field.name] = value(std::string(1, read_pod<char>(payload, offset)));
		offset += 1;
		continue;
	    }
	    if ( field_is_text(field) )
	    {
		if ( offset + sizeof(uint32_t) > payload.size() )
		    return fail(err, "bdb payload truncated before text length");
		uint32_t len = read_pod<uint32_t>(payload, offset);
		offset += sizeof(uint32_t);
		if ( offset + len > payload.size() )
		    return fail(err, "bdb payload truncated inside text field");
		out.object()[field.name] = value(std::string(&payload[offset], &payload[offset] + len));
		offset += len;
		continue;
	    }

	    return fail(err, "bdb payload contains unsupported field type");
	}
	return true;
    }

    bool fail(error *err, const std::string &message) const
    {
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 message);
	return false;
    }

    SchemaInfo _schema;
    std::string _path;
    DB *_db;
    bool _opened;
    const SchemaField *_key_field;
    mutable std::string _key_buffer;
    mutable std::vector<char> _data_buffer;
};

class BdbDriverFactory : public DataDriverRegistry::Factory
{
public:
    std::unique_ptr<DataDriver> create() const
    {
	return std::unique_ptr<DataDriver>(new BdbDriver());
    }
};

} // namespace

void register_bdb_storage_driver(DataDriverRegistry &registry)
{
    registry.register_factory("bdb",
			      std::unique_ptr<DataDriverRegistry::Factory>(new BdbDriverFactory()));
}

} // namespace madc

#endif // HAVE_BDB
