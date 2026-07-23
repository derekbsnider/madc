#include "madcdis/driver.h"
#include "madcdis/query.h"

#ifdef HAVE_GDBM

#include <gdbm.h>

#include <cstdlib>
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

std::string encode_key_value(const value &input, const SchemaField &field)
{
    if ( field_is_boolean(field) )
	return std::string(1, input.as_boolean() ? '\1' : '\0');

    if ( field_is_character(field) )
    {
	const std::string &text = input.as_string();
	return std::string(1, text.empty() ? '\0' : text[0]);
    }

    if ( field_is_text(field) )
	return input.as_string();

    if ( field_is_integer(field) )
    {
	std::string out;
	std::size_t width = field.byte_size ? field.byte_size : sizeof(int64_t);
	if ( width == 8 )
	{
	    int64_t raw = input.as_integer();
	    out.assign(reinterpret_cast<const char *>(&raw), sizeof(raw));
	    return out;
	}
	if ( width == 4 )
	{
	    int32_t raw = static_cast<int32_t>(input.as_integer());
	    out.assign(reinterpret_cast<const char *>(&raw), sizeof(raw));
	    return out;
	}
	if ( width == 2 )
	{
	    int16_t raw = static_cast<int16_t>(input.as_integer());
	    out.assign(reinterpret_cast<const char *>(&raw), sizeof(raw));
	    return out;
	}
	if ( width == 1 )
	{
	    int8_t raw = static_cast<int8_t>(input.as_integer());
	    out.assign(reinterpret_cast<const char *>(&raw), sizeof(raw));
	    return out;
	}
    }

    throw std::runtime_error("gdbm key field `" + field.name + "` uses unsupported key type");
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

class GdbmDriver : public DataDriver
{
public:
    GdbmDriver()
	: _db(nullptr), _opened(false), _key_field(nullptr)
    {}

    ~GdbmDriver()
    {
	close();
    }

    const char *name() const { return "gdbm"; }
    const char *scheme() const { return "gdbm"; }

    DriverCapabilities capabilities() const
    {
	DriverCapabilities caps;
	caps.read = true;
	caps.write = true;
	caps.scan = true;
	caps.point_lookup = true;
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
			     "gdbm driver requires a logical or variable-record schema with one supported key field");
	    return false;
	}
	return true;
    }

    bool open(const DataSource &source, error *err = nullptr)
    {
	if ( source.scheme() != "gdbm" )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "gdbm driver cannot open scheme `" + source.scheme() + "`");
	    return false;
	}

	close();
	_path = source.path();
	_db = gdbm_open(const_cast<char *>(_path.c_str()),
			0,
			GDBM_WRCREAT,
			0600,
			nullptr);
	if ( !_db )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "failed to open gdbm path: " + _path);
	    return false;
	}

	_opened = true;
	return true;
    }

    void close()
    {
	if ( _db )
	    gdbm_close(_db);
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
	    && !query.has_where_inequality()
	    && !query.has_where_in()
	    && !query.has_where_not_in()
	    && !query.has_where_like()
	    && query.has_where_equality()
	    && _key_field
	    && query.where_field() == _key_field->name;
    }

    bool insert_record(const value &record, error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;

	datum key;
	if ( !extract_key(record, key, err) )
	    return false;

	std::vector<char> payload;
	if ( !encode_record(record, payload, err) )
	    return false;

	datum val;
	val.dptr = payload.empty() ? const_cast<char *>("") : &payload[0];
	val.dsize = static_cast<int>(payload.size());

	int rc = gdbm_store(_db, key, val, GDBM_INSERT);
	release_datum(key);
	if ( rc != 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "gdbm insert failed: key already exists or write was rejected");
	    return false;
	}
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

	datum key;
	if ( !encode_lookup_key(key_value, key, err) )
	    return false;

	const std::map<std::string, value> &obj = record.as_object();
	std::map<std::string, value>::const_iterator it = obj.find(_key_field->name);
	if ( it == obj.end() || it->second != key_value )
	{
	    release_datum(key);
	    return fail(err, "gdbm update failed: record key does not match lookup key");
	}

	if ( !gdbm_exists(_db, key) )
	{
	    release_datum(key);
	    return fail(err, "gdbm update failed: key not found");
	}

	std::vector<char> payload;
	if ( !encode_record(record, payload, err) )
	{
	    release_datum(key);
	    return false;
	}

	datum val;
	val.dptr = payload.empty() ? const_cast<char *>("") : &payload[0];
	val.dsize = static_cast<int>(payload.size());

	int rc = gdbm_store(_db, key, val, GDBM_REPLACE);
	release_datum(key);
	if ( rc != 0 )
	    return fail(err, "gdbm update failed: replace was rejected");
	return true;
    }

    bool erase_record(const std::string &key_field,
		      const value &key_value,
		      error *err = nullptr)
    {
	(void)key_field;
	datum key;
	if ( !encode_lookup_key(key_value, key, err) )
	    return false;
	int rc = gdbm_delete(_db, key);
	release_datum(key);
	if ( rc != 0 )
	    return fail(err, "gdbm erase failed: key not found");
	return true;
    }

    bool restore_record(const std::string &key_field,
			const value &key_value,
			error *err = nullptr)
    {
	(void)key_field;
	(void)key_value;
	return fail(err, "gdbm restore is unsupported without a tombstone sidecar");
    }

    bool compact_records(error *err = nullptr)
    {
	return fail(err, "gdbm compact is unsupported");
    }

    bool get_record(const std::string &key_field,
			    const value &key_value,
			    value &out,
			    error *err = nullptr) const
    {
	(void)key_field;
	datum key;
	if ( !encode_lookup_key(key_value, key, err) )
	    return false;
	datum val = gdbm_fetch(_db, key);
	release_datum(key);
	if ( !val.dptr )
	    return false;

	std::vector<char> payload(val.dptr, val.dptr + val.dsize);
	std::free(val.dptr);
	return decode_record(payload, out, err);
    }

    bool execute_query(const Query &query,
		       std::vector<value> &out,
		       error *err = nullptr) const
    {
	out.clear();
	if ( !can_execute(query) )
	    return fail(err, "gdbm query execution only supports primary-key equality filters");
	value record;
	if ( !get_record(_key_field->name, query.where_value(), record, err) )
	    return true;
	out.push_back(project_record(record, query));
	return true;
    }

    bool scan_records(std::vector<value> &out,
		      error *err = nullptr) const
    {
	out.clear();
	datum key = gdbm_firstkey(_db);
	while ( key.dptr )
	{
	    datum next = gdbm_nextkey(_db, key);
	    datum val = gdbm_fetch(_db, key);
	    if ( !val.dptr )
	    {
		release_datum(key);
		release_datum(next);
		return fail(err, "gdbm scan failed: fetch after firstkey/nextkey failed");
	    }

	    std::vector<char> payload(val.dptr, val.dptr + val.dsize);
	    std::free(val.dptr);

	    value record;
	    if ( !decode_record(payload, record, err) )
	    {
		release_datum(key);
		release_datum(next);
		return false;
	    }
	    out.push_back(record);

	    release_datum(key);
	    key = next;
	}
	return true;
    }

private:
    static void release_datum(datum &d)
    {
	if ( d.dptr )
	    std::free(d.dptr);
	d.dptr = nullptr;
	d.dsize = 0;
    }

    bool ensure_record_shape(const value &record, error *err) const
    {
	if ( !record.is_object() )
	    return fail(err, "gdbm record must be an object");
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( record.as_object().count(_schema.fields()[i].name) == 0 )
		return fail(err, "gdbm record is missing field `" + _schema.fields()[i].name + "`");
	}
	return true;
    }

    bool extract_key(const value &record, datum &key_out, error *err) const
    {
	if ( !_key_field )
	    return fail(err, "gdbm schema has no bound key field");
	const std::map<std::string, value> &obj = record.as_object();
	std::map<std::string, value>::const_iterator it = obj.find(_key_field->name);
	if ( it == obj.end() )
	    return fail(err, "gdbm record is missing primary key field `" + _key_field->name + "`");
	return encode_lookup_key(it->second, key_out, err);
    }

    bool encode_lookup_key(const value &input, datum &out, error *err) const
    {
	try
	{
	    std::string encoded = encode_key_value(input, *_key_field);
	    out.dsize = static_cast<int>(encoded.size());
	    out.dptr = static_cast<char *>(std::malloc(encoded.size() ? encoded.size() : 1));
	    if ( !out.dptr )
		return fail(err, "gdbm key allocation failed");
	    if ( encoded.size() )
		std::memcpy(out.dptr, encoded.data(), encoded.size());
	    return true;
	}
	catch ( const std::exception &e )
	{
	    return fail(err, e.what());
	}
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
		    return fail(err, "gdbm text field `" + field.name + "` exceeds 32-bit length limit");
		uint32_t len = static_cast<uint32_t>(text.size());
		append_pod(out, len);
		if ( len )
		    out.insert(out.end(), text.begin(), text.end());
		continue;
	    }

	    return fail(err, "gdbm field `" + field.name + "` uses unsupported type `" + field.type_name + "`");
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
		    return fail(err, "gdbm payload truncated in bool field");
		out.object()[field.name] = value(read_pod<uint8_t>(payload, offset) != 0);
		offset += sizeof(uint8_t);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 8 )
	    {
		if ( offset + 8 > payload.size() )
		    return fail(err, "gdbm payload truncated in int64 field");
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
		    return fail(err, "gdbm payload truncated in int32 field");
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
		    return fail(err, "gdbm payload truncated in int16 field");
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
		    return fail(err, "gdbm payload truncated in int8 field");
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
		    return fail(err, "gdbm payload truncated in float field");
		out.object()[field.name] = value(static_cast<double>(read_pod<float>(payload, offset)));
		offset += 4;
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 8 )
	    {
		if ( offset + 8 > payload.size() )
		    return fail(err, "gdbm payload truncated in double field");
		out.object()[field.name] = value(read_pod<double>(payload, offset));
		offset += 8;
		continue;
	    }
	    if ( field_is_character(field) )
	    {
		if ( offset + 1 > payload.size() )
		    return fail(err, "gdbm payload truncated in char field");
		out.object()[field.name] = value(std::string(1, read_pod<char>(payload, offset)));
		offset += 1;
		continue;
	    }
	    if ( field_is_text(field) )
	    {
		if ( offset + sizeof(uint32_t) > payload.size() )
		    return fail(err, "gdbm payload truncated before text length");
		uint32_t len = read_pod<uint32_t>(payload, offset);
		offset += sizeof(uint32_t);
		if ( offset + len > payload.size() )
		    return fail(err, "gdbm payload truncated inside text field");
		out.object()[field.name] = value(std::string(&payload[offset], &payload[offset] + len));
		offset += len;
		continue;
	    }

	    return fail(err, "gdbm payload contains unsupported field type");
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
    GDBM_FILE _db;
    bool _opened;
    const SchemaField *_key_field;
};

class GdbmDriverFactory : public DataDriverRegistry::Factory
{
public:
    std::unique_ptr<DataDriver> create() const
    {
	return std::unique_ptr<DataDriver>(new GdbmDriver());
    }
};

} // namespace

void register_gdbm_storage_driver(DataDriverRegistry &registry)
{
    registry.register_factory("gdbm",
			      std::unique_ptr<DataDriverRegistry::Factory>(new GdbmDriverFactory()));
}

} // namespace madc

#endif // HAVE_GDBM
