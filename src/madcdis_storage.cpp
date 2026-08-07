// Dependency-free record-file drivers owned by the madcdis core.

#include "madcdis/driver.h"
#include "madcdis/query.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <string>
#include <vector>

namespace madc {

namespace {

std::string trim_copy(const std::string &in)
{
    std::size_t start = 0;
    while ( start < in.size() && std::isspace(static_cast<unsigned char>(in[start])) )
	++start;

    std::size_t end = in.size();
    while ( end > start && std::isspace(static_cast<unsigned char>(in[end - 1])) )
	--end;

    return in.substr(start, end - start);
}

template <typename T>
void write_pod_value(std::vector<char> &buffer, std::size_t offset, const T &input)
{
    std::memcpy(&buffer[offset], &input, sizeof(T));
}

template <typename T>
T read_pod_value(const std::vector<char> &buffer, std::size_t offset)
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

bool field_uses_fixed_binary_width(const SchemaField &field)
{
    return field.needs_scalar_width()
	|| field_is_character(field)
	|| field_is_boolean(field);
}

bool fixed_record_field_requires_size(const SchemaField &field)
{
    if ( field_uses_fixed_binary_width(field) )
	return field.needs_scalar_width();
    return field_is_text(field);
}

std::size_t packed_bit_bytes(std::size_t bit_count)
{
    return (bit_count + 7u) / 8u;
}

bool read_packed_bits(const std::string &path,
		      std::size_t bit_count,
		      std::vector<bool> &out,
		      error *err,
		      const std::string &driver_name)
{
    out.assign(bit_count, false);
    std::ifstream is(path.c_str(), std::ios::binary);
    if ( !is.good() )
	return true;

    is.seekg(0, std::ios::end);
    std::streamoff bytes = is.tellg();
    is.seekg(0, std::ios::beg);
    if ( bytes < 0 )
    {
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 driver_name + " failed to inspect tombstone sidecar size");
	return false;
    }

    std::size_t expected = packed_bit_bytes(bit_count);
    if ( static_cast<std::size_t>(bytes) != expected )
    {
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 driver_name + " tombstone sidecar size does not match record count");
	return false;
    }

    std::vector<unsigned char> bytes_in(expected, 0);
    if ( expected != 0 )
    {
	is.read(reinterpret_cast<char *>(&bytes_in[0]), static_cast<std::streamsize>(bytes_in.size()));
	if ( is.gcount() != static_cast<std::streamsize>(bytes_in.size()) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     driver_name + " short read while loading tombstone sidecar");
	    return false;
	}
    }

    for ( std::size_t i = 0; i < bit_count; ++i )
	out[i] = (bytes_in[i / 8u] & static_cast<unsigned char>(1u << (i % 8u))) != 0;
    return true;
}

bool write_packed_bits(const std::string &path,
		       const std::vector<bool> &bits,
		       error *err,
		       const std::string &driver_name)
{
    std::ofstream os(path.c_str(), std::ios::binary | std::ios::trunc);
    if ( !os.good() )
    {
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 "failed to open " + driver_name + " tombstone sidecar for write: " + path);
	return false;
    }

    std::vector<unsigned char> out(packed_bit_bytes(bits.size()), 0);
    for ( std::size_t i = 0; i < bits.size(); ++i )
    {
	if ( bits[i] )
	    out[i / 8u] |= static_cast<unsigned char>(1u << (i % 8u));
    }

    if ( !out.empty() )
	os.write(reinterpret_cast<const char *>(&out[0]), static_cast<std::streamsize>(out.size()));
    return true;
}

std::string csv_escape(const std::string &in)
{
    bool needs_quotes = false;
    std::string out;
    out.reserve(in.size() + 4);
    for ( std::size_t i = 0; i < in.size(); ++i )
    {
	char ch = in[i];
	if ( ch == '"' )
	{
	    needs_quotes = true;
	    out.push_back('"');
	    out.push_back('"');
	    continue;
	}
	if ( ch == ',' || ch == '\n' || ch == '\r' )
	    needs_quotes = true;
	out.push_back(ch);
    }
    if ( !needs_quotes )
	return out;
    return '"' + out + '"';
}

std::vector<std::string> csv_split(const std::string &line)
{
    std::vector<std::string> out;
    std::string current;
    bool in_quotes = false;

    for ( std::size_t i = 0; i < line.size(); ++i )
    {
	char ch = line[i];
	if ( in_quotes )
	{
	    if ( ch == '"' )
	    {
		if ( i + 1 < line.size() && line[i + 1] == '"' )
		{
		    current.push_back('"');
		    ++i;
		}
		else
		    in_quotes = false;
	    }
	    else
		current.push_back(ch);
	    continue;
	}

	if ( ch == '"' )
	{
	    in_quotes = true;
	    continue;
	}
	if ( ch == ',' )
	{
	    out.push_back(current);
	    current.clear();
	    continue;
	}
	current.push_back(ch);
    }
    out.push_back(current);
    return out;
}

std::string value_to_text(const value &v)
{
    switch ( v.type() )
    {
	case value::kind::null:
	    return std::string();
	case value::kind::boolean:
	    return v.as_boolean() ? "1" : "0";
	case value::kind::integer:
	{
	    std::ostringstream os;
	    os << v.as_integer();
	    return os.str();
	}
	case value::kind::real:
	{
	    std::ostringstream os;
	    os << v.as_real();
	    return os.str();
	}
	case value::kind::string:
	    return v.as_string();
	case value::kind::bytes:
	case value::kind::array:
	case value::kind::object:
	case value::kind::instance:
	    throw std::runtime_error("dsv driver only supports scalar field values");
    }
    return std::string();
}

bool parse_boolean_text(const std::string &text, bool &out)
{
    std::string trimmed = trim_copy(text);
    if ( trimmed == "1" || trimmed == "true" || trimmed == "TRUE" )
    {
	out = true;
	return true;
    }
    if ( trimmed == "0" || trimmed == "false" || trimmed == "FALSE" )
    {
	out = false;
	return true;
    }
    return false;
}

bool text_to_typed_value(const std::string &text,
			 const SchemaField &field,
			 value &out,
			 std::string &why)
{
    try
    {
	if ( field_is_boolean(field) )
	{
	    bool b = false;
	    if ( !parse_boolean_text(text, b) )
	    {
		why = "invalid bool `" + text + "`";
		return false;
	    }
	    out = value(b);
	    return true;
	}
	if ( field_is_real(field) )
	{
	    out = value(std::stod(trim_copy(text)));
	    return true;
	}
	if ( field_is_integer(field) )
	{
	    out = value(static_cast<int64_t>(std::stoll(trim_copy(text))));
	    return true;
	}
	if ( field_is_text(field) || field_is_character(field) )
	{
	    out = value(text);
	    return true;
	}

	out = value(text);
	return true;
    }
    catch ( const std::exception &e )
    {
	why = e.what();
	return false;
    }
}

const SchemaField *find_schema_field(const SchemaInfo &schema,
				     const std::string &name)
{
    for ( std::size_t i = 0; i < schema.fields().size(); ++i )
    {
	if ( schema.fields()[i].name == name )
	    return &schema.fields()[i];
    }
    return nullptr;
}

bool decode_dsv_record(const std::string &line,
		       const std::vector<std::string> &header,
		       const SchemaInfo &schema,
		       value &out,
		       error *err)
{
    std::vector<std::string> cols = csv_split(line);
    if ( cols.size() != header.size() )
    {
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 "dsv row column count does not match header");
	return false;
    }

    value record = value::make_object();
    for ( std::size_t i = 0; i < header.size(); ++i )
    {
	const SchemaField *field = find_schema_field(schema, header[i]);
	if ( !field )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv header field `" + header[i]
			     + "` is not present in bound schema");
	    return false;
	}

	value typed;
	std::string why;
	if ( !text_to_typed_value(cols[i], *field, typed, why) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv parse failed for field `" + field->name
			     + "`: " + why);
	    return false;
	}
	record.object()[field->name] = typed;
    }
    out = record;
    return true;
}

class DsvCursor : public Cursor<value>, public ErrorAwareCursor<value>
{
public:
    DsvCursor(const std::string &path,
	      const std::vector<std::string> &header,
	      const SchemaInfo &schema)
	: _input(path.c_str()), _header(header), _schema(schema), _closed(false),
	  _ready(false)
    {
	std::string line;
	if ( _input.good() && std::getline(_input, line)
	  && csv_split(line) == _header )
	    _ready = true;
    }

    ~DsvCursor() override { close(); }

    bool ready() const { return _ready; }

    bool next(value &out) override
    {
	return next_status(out, nullptr) == CursorStatus::item;
    }

    CursorStatus next_status(value &out, error *err) override
    {
	if ( _closed || !_ready )
	    return CursorStatus::end;

	std::string line;
	if ( !std::getline(_input, line) )
	{
	    if ( _input.eof() )
		return CursorStatus::end;
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv stream read failed");
	    return CursorStatus::failure;
	}
	if ( !decode_dsv_record(line, _header, _schema, out, err) )
	    return CursorStatus::failure;
	return CursorStatus::item;
    }

    void close() override
    {
	if ( _closed )
	    return;
	_input.close();
	_closed = true;
    }

private:
    std::ifstream _input;
    std::vector<std::string> _header;
    SchemaInfo _schema;
    bool _closed;
    bool _ready;
};

class DsvDriver : public DataDriver, public StreamingDataDriver
{
public:
    DsvDriver()
	: _rows_loaded(false), _opened(false)
    {}

    const char *name() const { return "dsv"; }
    const char *scheme() const { return "dsv"; }

    DriverCapabilities capabilities() const
    {
	DriverCapabilities caps;
	caps.read = true;
	caps.write = true;
	caps.scan = true;
	caps.point_lookup = true;
	return caps;
    }

    bool bind_schema(const SchemaInfo &schema, error *err = nullptr)
    {
	_schema = schema;
	if ( _schema.fields().empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv driver requires at least one schema field");
	    return false;
	}
	return true;
    }

    bool open(const DataSource &source, error *err = nullptr)
    {
	if ( source.scheme() != "dsv" )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv driver cannot open scheme `" + source.scheme() + "`");
	    return false;
	}
	_path = source.path();
	_rows.clear();
	_header.clear();
	_rows_loaded = false;

	std::ifstream is(_path.c_str());
	if ( !is.good() )
	{
	    _rows_loaded = true;
	    _opened = true;
	    return true;
	}

	std::string line;
	if ( !std::getline(is, line) )
	{
	    _rows_loaded = true;
	    _opened = true;
	    return true;
	}

	_header = csv_split(line);
	if ( _header.empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv header row is empty");
	    return false;
	}

	for ( std::size_t i = 0; i < _header.size(); ++i )
	{
	    if ( !find_schema_field(_schema, _header[i]) )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "dsv header field `" + _header[i]
				 + "` is not present in bound schema");
		return false;
	    }
	}

	_opened = true;
	return true;
    }

    void close()
    {
	_rows.clear();
	_header.clear();
	_path.clear();
	_rows_loaded = false;
	_opened = false;
    }

    bool is_open() const { return _opened; }

    bool can_bind_schema(const SchemaInfo &schema) const
    {
	if ( schema.fields().empty() )
	    return false;
	for ( std::size_t i = 0; i < schema.fields().size(); ++i )
	{
	    if ( schema.fields()[i].field_shape == SchemaField::shape::array
	      || schema.fields()[i].field_shape == SchemaField::shape::bytes
	      || schema.fields()[i].field_shape == SchemaField::shape::dynamic )
		return false;
	}
	return true;
    }

    bool can_execute(const Query &query) const
    {
	return query.query_kind() == Query::kind::builder;
    }

    bool insert_record(const value &record, error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;
	if ( !ensure_rows_loaded(err) )
	    return false;
	if ( _header.empty() )
	    build_header_from_schema();
	_rows.push_back(record);
	if ( flush(err) )
	    return true;
	_rows.pop_back();
	return false;
    }

    bool update_record(const std::string &key_field,
		       const value &key,
		       const value &record,
		       error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;
	if ( !ensure_rows_loaded(err) )
	    return false;
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv update failed: key not found");
	    return false;
	}
	value previous = _rows[static_cast<std::size_t>(idx)];
	_rows[static_cast<std::size_t>(idx)] = record;
	if ( flush(err) )
	    return true;
	_rows[static_cast<std::size_t>(idx)] = previous;
	return false;
    }

    bool erase_record(const std::string &key_field,
		      const value &key,
		      error *err = nullptr)
    {
	if ( !ensure_rows_loaded(err) )
	    return false;
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv erase failed: key not found");
	    return false;
	}
	value removed = _rows[static_cast<std::size_t>(idx)];
	_rows.erase(_rows.begin() + idx);
	if ( flush(err) )
	    return true;
	_rows.insert(_rows.begin() + idx, removed);
	return false;
    }

    bool restore_record(const std::string &key_field,
			const value &key,
			error *err = nullptr)
    {
	(void)key_field;
	(void)key;
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 "dsv restore is unsupported without a tombstone sidecar");
	return false;
    }

    bool compact_records(error *err = nullptr)
    {
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 "dsv compact is unsupported");
	return false;
    }

    bool get_record(const std::string &key_field,
		    const value &key,
		    value &out,
		    error *err = nullptr) const
    {
	if ( !ensure_rows_loaded(err) )
	    return false;
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	    return false;
	out = _rows[static_cast<std::size_t>(idx)];
	return true;
    }

    bool scan_records(std::vector<value> &out,
		      error *err = nullptr) const
    {
	out.clear();
	std::unique_ptr<Cursor<value> > cursor = scan_stream(err);
	if ( !cursor.get() )
	    return false;

	value record;
	for ( ;; )
	{
	    CursorStatus status = cursor_next(*cursor, record, err);
	    if ( status == CursorStatus::end )
		break;
	    if ( status == CursorStatus::failure )
	    {
		cursor->close();
		return false;
	    }
	    out.push_back(record);
	}
	cursor->close();
	return true;
    }

    std::unique_ptr<Cursor<value> > scan_stream(error *err = nullptr) const override
    {
	if ( !_opened )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv scan requires an open driver");
	    return std::unique_ptr<Cursor<value> >();
	}
	if ( _header.empty() )
	    return std::unique_ptr<Cursor<value> >(
		new detail::VectorCursor<value>(std::vector<value>()));

	std::unique_ptr<DsvCursor> cursor(new DsvCursor(_path, _header, _schema));
	if ( !cursor->ready() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv stream could not reopen a matching header: " + _path);
	    return std::unique_ptr<Cursor<value> >();
	}
	return std::unique_ptr<Cursor<value> >(cursor.release());
    }

private:
    bool ensure_rows_loaded(error *err) const
    {
	if ( _rows_loaded )
	    return true;

	std::vector<value> rows;
	std::unique_ptr<Cursor<value> > cursor = scan_stream(err);
	if ( !cursor.get() )
	    return false;
	value record;
	for ( ;; )
	{
	    CursorStatus status = cursor_next(*cursor, record, err);
	    if ( status == CursorStatus::end )
		break;
	    if ( status == CursorStatus::failure )
	    {
		cursor->close();
		return false;
	    }
	    rows.push_back(record);
	}
	cursor->close();
	_rows.swap(rows);
	_rows_loaded = true;
	return true;
    }

    bool ensure_record_shape(const value &record, error *err) const
    {
	if ( !record.is_object() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "dsv record must be an object");
	    return false;
	}
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( record.as_object().count(_schema.fields()[i].name) == 0 )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "dsv record is missing field `" + _schema.fields()[i].name + "`");
		return false;
	    }
	}
	return true;
    }

    void build_header_from_schema()
    {
	_header.clear();
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	    _header.push_back(_schema.fields()[i].name);
    }

    int find_row_index(const std::string &key_field, const value &key) const
    {
	for ( std::size_t i = 0; i < _rows.size(); ++i )
	{
	    const std::map<std::string, value> &record = _rows[i].as_object();
	    std::map<std::string, value>::const_iterator it = record.find(key_field);
	    if ( it != record.end() && it->second == key )
		return static_cast<int>(i);
	}
	return -1;
    }

    bool flush(error *err)
    {
	std::ofstream os(_path.c_str(), std::ios::trunc);
	if ( !os.good() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "failed to open dsv path for write: " + _path);
	    return false;
	}

	for ( std::size_t i = 0; i < _header.size(); ++i )
	{
	    if ( i ) os << ',';
	    os << csv_escape(_header[i]);
	}
	os << '\n';

	for ( std::size_t row = 0; row < _rows.size(); ++row )
	{
	    const std::map<std::string, value> &record = _rows[row].as_object();
	    for ( std::size_t col = 0; col < _header.size(); ++col )
	    {
		if ( col ) os << ',';
		std::map<std::string, value>::const_iterator it = record.find(_header[col]);
		if ( it != record.end() )
		    os << csv_escape(value_to_text(it->second));
	    }
	    os << '\n';
	}
	if ( !os.good() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "failed to write dsv path: " + _path);
	    return false;
	}
	_rows_loaded = true;
	return true;
    }

    SchemaInfo _schema;
    std::string _path;
    std::vector<std::string> _header;
    mutable std::vector<value> _rows;
    mutable bool _rows_loaded;
    bool _opened;
};

class FlrDriver : public DataDriver
{
public:
    FlrDriver()
	: _opened(false)
    {}

    const char *name() const { return "flr"; }
    const char *scheme() const { return "flr"; }

    DriverCapabilities capabilities() const
    {
	DriverCapabilities caps;
	caps.read = true;
	caps.write = true;
	caps.scan = true;
	caps.point_lookup = true;
	caps.soft_delete = true;
	return caps;
    }

    bool bind_schema(const SchemaInfo &schema, error *err = nullptr)
    {
	_schema = schema;
	if ( !can_bind_schema(schema) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr driver requires fixed-record schema metadata");
	    return false;
	}
	return true;
    }

    bool open(const DataSource &source, error *err = nullptr)
    {
	if ( source.scheme() != "flr" )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr driver cannot open scheme `" + source.scheme() + "`");
	    return false;
	}

	_path = source.path();
	_rows.clear();
	_tombstones.clear();
	_tombstone_path = _schema.tombstone_path();
	_dead_record_path = _schema.dead_record_path();

	std::ifstream is(_path.c_str(), std::ios::binary);
	if ( !is.good() )
	{
	    _opened = true;
	    if ( !_tombstone_path.empty() )
		return read_packed_bits(_tombstone_path, 0, _tombstones, err, "flr");
	    return true;
	}

	is.seekg(0, std::ios::end);
	std::streamoff bytes = is.tellg();
	is.seekg(0, std::ios::beg);
	if ( bytes < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "failed to inspect flr file size");
	    return false;
	}
	if ( bytes % static_cast<std::streamoff>(_schema.record_size()) != 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr file size is not an exact multiple of record size");
	    return false;
	}

	while ( true )
	{
	    std::vector<char> record(_schema.record_size(), 0);
	    is.read(&record[0], static_cast<std::streamsize>(record.size()));
	    if ( is.gcount() == 0 )
		break;
	    if ( is.gcount() != static_cast<std::streamsize>(record.size()) )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "short read while loading flr record");
		return false;
	    }
	    value decoded;
	    if ( !decode_record(record, decoded, err) )
		return false;
	    _rows.push_back(decoded);
	}

	if ( !_tombstone_path.empty()
	  && !read_packed_bits(_tombstone_path, _rows.size(), _tombstones, err, "flr") )
	    return false;
	if ( _tombstone_path.empty() )
	    _tombstones.assign(_rows.size(), false);

	_opened = true;
	return true;
    }

    void close()
    {
	_rows.clear();
	_tombstones.clear();
	_path.clear();
	_tombstone_path.clear();
	_dead_record_path.clear();
	_opened = false;
    }

    bool is_open() const { return _opened; }

    bool can_bind_schema(const SchemaInfo &schema) const
    {
	if ( schema.record_layout() != SchemaInfo::layout_mode::fixed_record )
	    return false;
	if ( schema.record_size() == 0 || schema.fields().empty() )
	    return false;
	for ( std::size_t i = 0; i < schema.fields().size(); ++i )
	{
	    const SchemaField &field = schema.fields()[i];
	    if ( fixed_record_field_requires_size(field) && field.byte_size == 0 )
		return false;
	    if ( field.byte_size != 0
	      && field.byte_offset + field.byte_size > schema.record_size() )
		return false;
	    if ( field.field_shape == SchemaField::shape::array
	      || field.field_shape == SchemaField::shape::bytes
	      || field.field_shape == SchemaField::shape::dynamic )
		return false;
	    if ( field.resolved_kind() == SchemaField::kind::unknown )
		return false;
	}
	return true;
    }

    bool can_execute(const Query &query) const
    {
	return query.query_kind() == Query::kind::builder;
    }

    bool insert_record(const value &record, error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;
	value normalized;
	if ( !normalize_record(record, normalized, err) )
	    return false;
	_rows.push_back(normalized);
	_tombstones.push_back(false);
	if ( flush(err) )
	    return true;
	_rows.pop_back();
	_tombstones.pop_back();
	return false;
    }

    bool update_record(const std::string &key_field,
		       const value &key,
		       const value &record,
		       error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr update failed: key not found");
	    return false;
	}
	value normalized;
	if ( !normalize_record(record, normalized, err) )
	    return false;
	value previous = _rows[static_cast<std::size_t>(idx)];
	_rows[static_cast<std::size_t>(idx)] = normalized;
	if ( flush(err) )
	    return true;
	_rows[static_cast<std::size_t>(idx)] = previous;
	return false;
    }

    bool erase_record(const std::string &key_field,
		      const value &key,
		      error *err = nullptr)
    {
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr erase failed: key not found");
	    return false;
	}
	if ( uses_tombstones() )
	{
	    std::size_t pos = static_cast<std::size_t>(idx);
	    if ( _tombstones[pos] )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "flr erase failed: record is already tombstoned");
		return false;
	    }
	    _tombstones[pos] = true;
	    if ( flush(err) )
		return true;
	    _tombstones[pos] = false;
	    return false;
	}
	value removed = _rows[static_cast<std::size_t>(idx)];
	_rows.erase(_rows.begin() + idx);
	if ( flush(err) )
	    return true;
	_rows.insert(_rows.begin() + idx, removed);
	return false;
    }

    bool restore_record(const std::string &key_field,
			const value &key,
			error *err = nullptr)
    {
	if ( !uses_tombstones() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr restore requires a tombstone sidecar");
	    return false;
	}
	int idx = find_row_index(key_field, key, true);
	if ( idx >= 0 )
	{
	    std::size_t pos = static_cast<std::size_t>(idx);
	    if ( !_tombstones[pos] )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "flr restore failed: record is not tombstoned");
		return false;
	    }
	    _tombstones[pos] = false;
	    if ( flush(err) )
		return true;
	    _tombstones[pos] = true;
	    return false;
	}

	if ( _dead_record_path.empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr restore failed: key not found");
	    return false;
	}

	std::vector<value> archive_rows;
	if ( !read_rows_from_file(_dead_record_path, archive_rows, err, false) )
	    return false;

	int archive_idx = find_row_index_in_rows(archive_rows, key_field, key);
	if ( archive_idx < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr restore failed: key not found");
	    return false;
	}

	value restored = archive_rows[static_cast<std::size_t>(archive_idx)];
	std::vector<value> previous_rows = _rows;
	std::vector<bool> previous_tombstones = _tombstones;
	std::size_t insert_pos = insertion_index_for_record(restored);
	_rows.insert(_rows.begin() + static_cast<std::ptrdiff_t>(insert_pos), restored);
	_tombstones.insert(_tombstones.begin() + static_cast<std::ptrdiff_t>(insert_pos), false);

	if ( !flush(err) )
	{
	    _rows = previous_rows;
	    _tombstones = previous_tombstones;
	    return false;
	}

	archive_rows.erase(archive_rows.begin() + archive_idx);
	if ( write_rows_to_file(_dead_record_path, archive_rows, err) )
	    return true;

	_rows = previous_rows;
	_tombstones = previous_tombstones;
	flush(nullptr);
	return false;
    }

    bool compact_records(error *err = nullptr)
    {
	if ( !uses_tombstones() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr compact requires a tombstone sidecar");
	    return false;
	}
	if ( _dead_record_path.empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr compact requires a dead-record archive path");
	    return false;
	}

	std::vector<value> live_rows;
	std::vector<value> dead_rows;
	live_rows.reserve(_rows.size());
	dead_rows.reserve(_rows.size());
	for ( std::size_t i = 0; i < _rows.size(); ++i )
	{
	    if ( is_tombstoned(i) )
		dead_rows.push_back(_rows[i]);
	    else
		live_rows.push_back(_rows[i]);
	}

	if ( dead_rows.empty() )
	    return true;

	std::string live_tmp = _path + ".compact.tmp";
	std::string dead_tmp = _dead_record_path + ".compact.tmp";
	std::string tomb_tmp = _tombstone_path + ".compact.tmp";

	if ( !write_rows_to_file(live_tmp, live_rows, err) )
	    return false;

	std::vector<value> archive_rows;
	if ( !read_rows_from_file(_dead_record_path, archive_rows, err, false) )
	{
	    std::remove(live_tmp.c_str());
	    return false;
	}
	archive_rows.insert(archive_rows.end(), dead_rows.begin(), dead_rows.end());
	if ( !write_rows_to_file(dead_tmp, archive_rows, err) )
	{
	    std::remove(live_tmp.c_str());
	    return false;
	}

	std::vector<bool> live_tombstones(live_rows.size(), false);
	if ( !write_packed_bits(tomb_tmp, live_tombstones, err, "flr") )
	{
	    std::remove(live_tmp.c_str());
	    std::remove(dead_tmp.c_str());
	    return false;
	}

	std::remove(_path.c_str());
	if ( std::rename(live_tmp.c_str(), _path.c_str()) != 0 )
	{
	    std::remove(live_tmp.c_str());
	    std::remove(dead_tmp.c_str());
	    std::remove(tomb_tmp.c_str());
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr compact failed: could not replace live file");
	    return false;
	}

	std::remove(_dead_record_path.c_str());
	if ( std::rename(dead_tmp.c_str(), _dead_record_path.c_str()) != 0 )
	{
	    std::remove(dead_tmp.c_str());
	    std::remove(tomb_tmp.c_str());
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr compact failed: could not replace dead archive");
	    return false;
	}

	std::remove(_tombstone_path.c_str());
	if ( std::rename(tomb_tmp.c_str(), _tombstone_path.c_str()) != 0 )
	{
	    std::remove(tomb_tmp.c_str());
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr compact failed: could not replace tombstone sidecar");
	    return false;
	}

	_rows = live_rows;
	_tombstones = live_tombstones;
	return true;
    }

    bool get_record(const std::string &key_field,
		    const value &key,
		    value &out,
		    error *err = nullptr) const
    {
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	{
	    (void)err;
	    return false;
	}
	out = _rows[static_cast<std::size_t>(idx)];
	return true;
    }

    bool scan_records(std::vector<value> &out,
		      error *err = nullptr) const
    {
	(void)err;
	out.clear();
	for ( std::size_t i = 0; i < _rows.size(); ++i )
	{
	    if ( is_tombstoned(i) )
		continue;
	    out.push_back(_rows[i]);
	}
	return true;
    }

private:
    bool ensure_record_shape(const value &record, error *err) const
    {
	if ( !record.is_object() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr record must be an object");
	    return false;
	}
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( record.as_object().count(_schema.fields()[i].name) == 0 )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "flr record is missing field `" + _schema.fields()[i].name + "`");
		return false;
	    }
	}
	return true;
    }

    bool uses_tombstones() const
    {
	return !_tombstone_path.empty();
    }

    bool is_tombstoned(std::size_t index) const
    {
	return uses_tombstones()
	    && index < _tombstones.size()
	    && _tombstones[index];
    }

    int compare_key_values(const value &lhs, const value &rhs) const
    {
	switch ( _schema.ordered_key_compare() )
	{
	    case SchemaInfo::key_compare::numeric_signed:
	    {
		int64_t a = lhs.as_integer();
		int64_t b = rhs.as_integer();
		if ( a < b )
		    return -1;
		if ( a > b )
		    return 1;
		return 0;
	    }
	    case SchemaInfo::key_compare::numeric_unsigned:
	    {
		uint64_t a = static_cast<uint64_t>(lhs.as_integer());
		uint64_t b = static_cast<uint64_t>(rhs.as_integer());
		if ( a < b )
		    return -1;
		if ( a > b )
		    return 1;
		return 0;
	    }
	    case SchemaInfo::key_compare::fixed_text:
	    case SchemaInfo::key_compare::lexical:
	    case SchemaInfo::key_compare::none:
	    default:
	    {
		const std::string &a = lhs.as_string();
		const std::string &b = rhs.as_string();
		if ( a < b )
		    return -1;
		if ( a > b )
		    return 1;
		return 0;
	    }
	}
    }

    int find_row_index_in_rows(const std::vector<value> &rows,
			       const std::string &key_field,
			       const value &key) const
    {
	for ( std::size_t i = 0; i < rows.size(); ++i )
	{
	    const std::map<std::string, value> &record = rows[i].as_object();
	    std::map<std::string, value>::const_iterator it = record.find(key_field);
	    if ( it != record.end() && it->second == key )
		return static_cast<int>(i);
	}
	return -1;
    }

    std::size_t insertion_index_for_record(const value &record) const
    {
	if ( _schema.record_ordering() != SchemaInfo::ordering_mode::ordered_by_key )
	    return _rows.size();

	const std::string &key_field = _schema.ordered_key_field();
	if ( key_field.empty() )
	    return _rows.size();

	const std::map<std::string, value> &incoming = record.as_object();
	std::map<std::string, value>::const_iterator incoming_it = incoming.find(key_field);
	if ( incoming_it == incoming.end() )
	    return _rows.size();

	for ( std::size_t i = 0; i < _rows.size(); ++i )
	{
	    const std::map<std::string, value> &existing = _rows[i].as_object();
	    std::map<std::string, value>::const_iterator existing_it = existing.find(key_field);
	    if ( existing_it == existing.end() )
		continue;
	    if ( compare_key_values(incoming_it->second, existing_it->second) < 0 )
		return i;
	}

	return _rows.size();
    }

    int find_row_index(const std::string &key_field,
		       const value &key,
		       bool include_tombstoned = false) const
    {
	for ( std::size_t i = 0; i < _rows.size(); ++i )
	{
	    if ( !include_tombstoned && is_tombstoned(i) )
		continue;
	    const std::map<std::string, value> &record = _rows[i].as_object();
	    std::map<std::string, value>::const_iterator it = record.find(key_field);
	    if ( it != record.end() && it->second == key )
		return static_cast<int>(i);
	}
	return -1;
    }

    bool encode_record(const value &record, std::vector<char> &out, error *err) const
    {
	out.assign(_schema.record_size(), 0);
	const std::map<std::string, value> &obj = record.as_object();

	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    const SchemaField &field = _schema.fields()[i];
	    const value &input = obj.at(field.name);

	    if ( field_is_boolean(field) )
	    {
		uint8_t b = input.as_boolean() ? 1 : 0;
		write_pod_value(out, field.byte_offset, b);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 8 )
	    {
		if ( field.is_signed )
		{
		    int64_t v = input.as_integer();
		    write_pod_value(out, field.byte_offset, v);
		}
		else
		{
		    uint64_t v = static_cast<uint64_t>(input.as_integer());
		    write_pod_value(out, field.byte_offset, v);
		}
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 4 )
	    {
		if ( field.is_signed )
		{
		    int32_t v = static_cast<int32_t>(input.as_integer());
		    write_pod_value(out, field.byte_offset, v);
		}
		else
		{
		    uint32_t v = static_cast<uint32_t>(input.as_integer());
		    write_pod_value(out, field.byte_offset, v);
		}
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 2 )
	    {
		if ( field.is_signed )
		{
		    int16_t v = static_cast<int16_t>(input.as_integer());
		    write_pod_value(out, field.byte_offset, v);
		}
		else
		{
		    uint16_t v = static_cast<uint16_t>(input.as_integer());
		    write_pod_value(out, field.byte_offset, v);
		}
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 1 )
	    {
		if ( field.is_signed )
		{
		    int8_t v = static_cast<int8_t>(input.as_integer());
		    write_pod_value(out, field.byte_offset, v);
		}
		else
		{
		    uint8_t v = static_cast<uint8_t>(input.as_integer());
		    write_pod_value(out, field.byte_offset, v);
		}
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 4 )
	    {
		float v = static_cast<float>(input.as_real());
		write_pod_value(out, field.byte_offset, v);
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 8 )
	    {
		double v = input.as_real();
		write_pod_value(out, field.byte_offset, v);
		continue;
	    }
	    if ( field_is_character(field) )
	    {
		const std::string &text = input.as_string();
		char ch = text.empty() ? '\0' : text[0];
		write_pod_value(out, field.byte_offset, ch);
		continue;
	    }
	    if ( field_is_text(field) )
	    {
		std::string text = input.as_string();
		std::size_t max_bytes = field.byte_size;
		if ( max_bytes == 0 )
		{
		    if ( err )
			*err = error(error::severity::error,
				     error::phase::runtime,
				     "flr text field `" + field.name + "` has no fixed size");
		    return false;
		}
		if ( text.size() >= max_bytes )
		{
		    if ( field.text_overflow == SchemaField::overflow_policy::truncate )
			text = text.substr(0, max_bytes - 1);
		    else
		    {
			if ( err )
			    *err = error(error::severity::error,
					 error::phase::runtime,
					 "flr field `" + field.name + "` exceeds fixed size");
			return false;
		    }
		}
		std::memcpy(&out[field.byte_offset], text.data(), text.size());
		out[field.byte_offset + text.size()] = '\0';
		continue;
	    }

	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr field `" + field.name + "` uses unsupported type `" + field.type_name + "`");
	    return false;
	}

	return true;
    }

    bool decode_record(const std::vector<char> &record, value &out, error *err) const
    {
	(void)err;
	out = value::make_object();
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    const SchemaField &field = _schema.fields()[i];
	    if ( field_is_boolean(field) )
	    {
		uint8_t b = read_pod_value<uint8_t>(record, field.byte_offset);
		out.object()[field.name] = value(b != 0);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 8 )
	    {
		if ( field.is_signed )
		    out.object()[field.name] = value(read_pod_value<int64_t>(record, field.byte_offset));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<uint64_t>(record, field.byte_offset)));
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 4 )
	    {
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<int32_t>(record, field.byte_offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<uint32_t>(record, field.byte_offset)));
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 2 )
	    {
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<int16_t>(record, field.byte_offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<uint16_t>(record, field.byte_offset)));
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 1 )
	    {
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<int8_t>(record, field.byte_offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<uint8_t>(record, field.byte_offset)));
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 4 )
	    {
		out.object()[field.name] = value(static_cast<double>(read_pod_value<float>(record, field.byte_offset)));
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 8 )
	    {
		out.object()[field.name] = value(read_pod_value<double>(record, field.byte_offset));
		continue;
	    }
	    if ( field_is_character(field) )
	    {
		char ch = read_pod_value<char>(record, field.byte_offset);
		out.object()[field.name] = value(std::string(1, ch));
		continue;
	    }
	    if ( field_is_text(field) )
	    {
		const char *start = &record[field.byte_offset];
		std::size_t len = 0;
		while ( len < field.byte_size && start[len] != '\0' )
		    ++len;
		out.object()[field.name] = value(std::string(start, len));
		continue;
	    }
	    return false;
	}
	return true;
    }

    bool normalize_record(const value &record, value &out, error *err) const
    {
	std::vector<char> encoded;
	if ( !encode_record(record, encoded, err) )
	    return false;
	return decode_record(encoded, out, err);
    }

    bool write_rows_to_file(const std::string &path,
			   const std::vector<value> &rows,
			   error *err) const
    {
	std::ofstream os(path.c_str(), std::ios::binary | std::ios::trunc);
	if ( !os.good() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "failed to open flr path for write: " + path);
	    return false;
	}

	for ( std::size_t i = 0; i < rows.size(); ++i )
	{
	    std::vector<char> encoded;
	    if ( !encode_record(rows[i], encoded, err) )
		return false;
	    os.write(&encoded[0], static_cast<std::streamsize>(encoded.size()));
	}
	return true;
    }

    bool read_rows_from_file(const std::string &path,
			     std::vector<value> &rows,
			     error *err,
			     bool require_multiple_of_record_size) const
    {
	rows.clear();
	std::ifstream is(path.c_str(), std::ios::binary);
	if ( !is.good() )
	    return true;

	is.seekg(0, std::ios::end);
	std::streamoff bytes = is.tellg();
	is.seekg(0, std::ios::beg);
	if ( bytes < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "failed to inspect flr file size");
	    return false;
	}
	if ( require_multiple_of_record_size
	  && bytes % static_cast<std::streamoff>(_schema.record_size()) != 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "flr file size is not an exact multiple of record size");
	    return false;
	}

	while ( true )
	{
	    std::vector<char> record(_schema.record_size(), 0);
	    is.read(&record[0], static_cast<std::streamsize>(record.size()));
	    if ( is.gcount() == 0 )
		break;
	    if ( is.gcount() != static_cast<std::streamsize>(record.size()) )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "short read while loading flr record");
		return false;
	    }
	    value decoded;
	    if ( !decode_record(record, decoded, err) )
		return false;
	    rows.push_back(decoded);
	}
	return true;
    }

    bool flush(error *err)
    {
	if ( !write_rows_to_file(_path, _rows, err) )
	    return false;
	if ( uses_tombstones()
	  && !write_packed_bits(_tombstone_path, _tombstones, err, "flr") )
	    return false;
	return true;
    }

    SchemaInfo _schema;
    std::string _path;
    std::string _tombstone_path;
    std::string _dead_record_path;
    std::vector<value> _rows;
    std::vector<bool> _tombstones;
    bool _opened;
};

class VlrDriver : public DataDriver
{
public:
    VlrDriver()
	: _opened(false)
    {}

    const char *name() const { return "vlr"; }
    const char *scheme() const { return "vlr"; }

    DriverCapabilities capabilities() const
    {
	DriverCapabilities caps;
	caps.read = true;
	caps.write = true;
	caps.scan = true;
	caps.point_lookup = true;
	caps.soft_delete = true;
	return caps;
    }

    bool bind_schema(const SchemaInfo &schema, error *err = nullptr)
    {
	_schema = schema;
	if ( !can_bind_schema(schema) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr driver requires variable-record schema metadata");
	    return false;
	}
	return true;
    }

    bool open(const DataSource &source, error *err = nullptr)
    {
	if ( source.scheme() != "vlr" )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr driver cannot open scheme `" + source.scheme() + "`");
	    return false;
	}

	_path = source.path();
	_rows.clear();
	_row_offsets.clear();
	_tombstones.clear();
	_tombstone_path = _schema.tombstone_path();

	std::ifstream is(_path.c_str(), std::ios::binary);
	if ( !is.good() )
	{
	    _opened = true;
	    if ( uses_tombstones() )
		return read_packed_bits(_tombstone_path, 0, _tombstones, err, "vlr");
	    return true;
	}

	while ( true )
	{
	    std::streamoff record_offset = is.tellg();
	    if ( record_offset < 0 )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "failed to inspect vlr record offset");
		return false;
	    }
	    uint32_t record_size = 0;
	    is.read(reinterpret_cast<char *>(&record_size), sizeof(record_size));
	    if ( is.gcount() == 0 )
		break;
	    if ( is.gcount() != static_cast<std::streamsize>(sizeof(record_size)) )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "short read while loading vlr record length");
		return false;
	    }

	    std::vector<char> payload(record_size, 0);
	    is.read(&payload[0], static_cast<std::streamsize>(payload.size()));
	    if ( is.gcount() != static_cast<std::streamsize>(payload.size()) )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "short read while loading vlr record payload");
		return false;
	    }

	    value record;
	    if ( !decode_record(payload, record, err) )
		return false;
	    _rows.push_back(record);
	    _row_offsets.push_back(static_cast<uint64_t>(record_offset));
	}

	if ( uses_tombstones()
	  && !read_packed_bits(_tombstone_path, _rows.size(), _tombstones, err, "vlr") )
	    return false;
	if ( !uses_tombstones() )
	    _tombstones.assign(_rows.size(), false);

	_opened = true;
	return true;
    }

    void close()
    {
	_rows.clear();
	_row_offsets.clear();
	_tombstones.clear();
	_path.clear();
	_tombstone_path.clear();
	_opened = false;
    }

    bool is_open() const { return _opened; }

    bool can_bind_schema(const SchemaInfo &schema) const
    {
	if ( schema.record_layout() != SchemaInfo::layout_mode::variable_record )
	    return false;
	if ( schema.fields().empty() )
	    return false;
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
	}
	return true;
    }

    bool can_execute(const Query &query) const
    {
	return query.query_kind() == Query::kind::builder;
    }

    bool insert_record(const value &record, error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;
	value normalized;
	if ( !normalize_record(record, normalized, err) )
	    return false;
	_rows.push_back(normalized);
	_tombstones.push_back(false);
	if ( flush_rewrite(err) )
	    return true;
	_rows.pop_back();
	_tombstones.pop_back();
	return false;
    }

    bool insert_record_with_locator(const value &record,
				    RecordLocator &locator,
				    error *err = nullptr)
    {
	if ( !supports_stable_locators(err) )
	{
	    locator = RecordLocator::none();
	    return false;
	}
	if ( !ensure_record_shape(record, err) )
	    return false;
	value normalized;
	if ( !normalize_record(record, normalized, err) )
	    return false;
	_rows.push_back(normalized);
	_tombstones.push_back(false);
	if ( flush_rewrite(err) )
	{
	    locator = RecordLocator::at_byte_offset(_row_offsets.back());
	    return true;
	}
	_rows.pop_back();
	_tombstones.pop_back();
	locator = RecordLocator::none();
	return false;
    }

    bool update_record(const std::string &key_field,
		       const value &key,
		       const value &record,
		       error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr update failed: key not found");
	    return false;
	}
	value normalized;
	if ( !normalize_record(record, normalized, err) )
	    return false;
	if ( uses_tombstones() )
	{
	    std::size_t pos = static_cast<std::size_t>(idx);
	    value previous = _rows[pos];
	    _rows.push_back(normalized);
	    _tombstones.push_back(false);
	    _tombstones[pos] = true;
	    if ( flush_rewrite(err) )
		return true;
	    _tombstones[pos] = false;
	    _rows.pop_back();
	    _tombstones.pop_back();
	    _rows[pos] = previous;
	    return false;
	}
	value previous = _rows[static_cast<std::size_t>(idx)];
	_rows[static_cast<std::size_t>(idx)] = normalized;
	if ( flush_rewrite(err) )
	    return true;
	_rows[static_cast<std::size_t>(idx)] = previous;
	return false;
    }

    bool erase_record(const std::string &key_field,
		      const value &key,
		      error *err = nullptr)
    {
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr erase failed: key not found");
	    return false;
	}
	if ( uses_tombstones() )
	{
	    std::size_t pos = static_cast<std::size_t>(idx);
	    if ( _tombstones[pos] )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "vlr erase failed: record is already tombstoned");
		return false;
	    }
	    _tombstones[pos] = true;
	    if ( write_packed_bits(_tombstone_path, _tombstones, err, "vlr") )
		return true;
	    _tombstones[pos] = false;
	    return false;
	}
	value removed = _rows[static_cast<std::size_t>(idx)];
	_rows.erase(_rows.begin() + idx);
	_tombstones.erase(_tombstones.begin() + idx);
	if ( flush_rewrite(err) )
	    return true;
	_rows.insert(_rows.begin() + idx, removed);
	_tombstones.insert(_tombstones.begin() + idx, false);
	return false;
    }

    bool restore_record(const std::string &key_field,
			const value &key,
			error *err = nullptr)
    {
	if ( !uses_tombstones() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr restore requires a tombstone sidecar");
	    return false;
	}
	if ( find_row_index(key_field, key) >= 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr restore failed: live record already exists");
	    return false;
	}
	int tombstone_idx = find_tombstoned_row_index(key_field, key);
	if ( tombstone_idx < 0 )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr restore failed: tombstoned record not found");
	    return false;
	}
	std::size_t pos = static_cast<std::size_t>(tombstone_idx);
	_tombstones[pos] = false;
	if ( write_packed_bits(_tombstone_path, _tombstones, err, "vlr") )
	    return true;
	_tombstones[pos] = true;
	return false;
    }

    bool compact_records(error *err = nullptr)
    {
	if ( supports_stable_locators(nullptr) )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr compact is unsupported when stable locators are enabled");
	    return false;
	}
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 "vlr compact is unsupported");
	return false;
    }

    bool get_record(const std::string &key_field,
		    const value &key,
		    value &out,
		    error *err = nullptr) const
    {
	(void)err;
	int idx = find_row_index(key_field, key);
	if ( idx < 0 )
	    return false;
	out = _rows[static_cast<std::size_t>(idx)];
	return true;
    }

    bool get_record_by_locator(const RecordLocator &locator,
			       value &out,
			       error *err = nullptr) const
    {
	if ( !supports_stable_locators(err) )
	    return false;
	if ( locator.locator_kind != RecordLocator::kind::byte_offset )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr get_record_by_locator requires a byte-offset locator");
	    return false;
	}

	for ( std::size_t i = 0; i < _row_offsets.size(); ++i )
	{
	    if ( _row_offsets[i] == locator.byte_offset )
	    {
		if ( row_is_tombstoned(i) )
		{
		    if ( err )
			*err = error(error::severity::error,
				     error::phase::runtime,
				     "vlr get_record_by_locator failed: locator points to a tombstoned record");
		    return false;
		}
		out = _rows[i];
		return true;
	    }
	}

	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 "vlr get_record_by_locator failed: offset not found");
	return false;
    }

    bool scan_records(std::vector<value> &out,
		      error *err = nullptr) const
    {
	(void)err;
	out.clear();
	for ( std::size_t i = 0; i < _rows.size(); ++i )
	{
	    if ( row_is_tombstoned(i) )
		continue;
	    out.push_back(_rows[i]);
	}
	return true;
    }

private:
    bool ensure_record_shape(const value &record, error *err) const
    {
	if ( !record.is_object() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr record must be an object");
	    return false;
	}
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( record.as_object().count(_schema.fields()[i].name) == 0 )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "vlr record is missing field `" + _schema.fields()[i].name + "`");
		return false;
	    }
	}
	return true;
    }

    int find_row_index(const std::string &key_field, const value &key) const
    {
	for ( std::size_t i = _rows.size(); i > 0; --i )
	{
	    std::size_t pos = i - 1;
	    if ( row_is_tombstoned(pos) )
		continue;
	    const std::map<std::string, value> &record = _rows[pos].as_object();
	    std::map<std::string, value>::const_iterator it = record.find(key_field);
	    if ( it != record.end() && it->second == key )
		return static_cast<int>(pos);
	}
	return -1;
    }

    int find_tombstoned_row_index(const std::string &key_field, const value &key) const
    {
	for ( std::size_t i = _rows.size(); i > 0; --i )
	{
	    std::size_t pos = i - 1;
	    if ( !row_is_tombstoned(pos) )
		continue;
	    const std::map<std::string, value> &record = _rows[pos].as_object();
	    std::map<std::string, value>::const_iterator it = record.find(key_field);
	    if ( it != record.end() && it->second == key )
		return static_cast<int>(pos);
	}
	return -1;
    }

    bool append_string(std::vector<char> &out, const std::string &text, error *err) const
    {
	if ( text.size() > 0xffffffffu )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr string field exceeds 32-bit length limit");
	    return false;
	}
	uint32_t len = static_cast<uint32_t>(text.size());
	std::size_t old = out.size();
	out.resize(old + sizeof(len) + len);
	std::memcpy(&out[old], &len, sizeof(len));
	if ( len )
	    std::memcpy(&out[old + sizeof(len)], text.data(), len);
	return true;
    }

    bool read_string(const std::vector<char> &payload,
		     std::size_t &offset,
		     std::string &out,
		     error *err) const
    {
	if ( offset + sizeof(uint32_t) > payload.size() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr payload is truncated before string length");
	    return false;
	}
	uint32_t len = read_pod_value<uint32_t>(payload, offset);
	offset += sizeof(uint32_t);
	if ( offset + len > payload.size() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr payload is truncated inside string data");
	    return false;
	}
	out.assign(&payload[offset], &payload[offset] + len);
	offset += len;
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
		std::size_t old = out.size();
		out.resize(old + sizeof(b));
		std::memcpy(&out[old], &b, sizeof(b));
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 8 )
	    {
		int64_t v = input.as_integer();
		std::size_t old = out.size();
		out.resize(old + 8);
		if ( field.is_signed )
		    std::memcpy(&out[old], &v, 8);
		else
		{
		    uint64_t uv = static_cast<uint64_t>(v);
		    std::memcpy(&out[old], &uv, 8);
		}
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 4 )
	    {
		int32_t v = static_cast<int32_t>(input.as_integer());
		std::size_t old = out.size();
		out.resize(old + 4);
		if ( field.is_signed )
		    std::memcpy(&out[old], &v, 4);
		else
		{
		    uint32_t uv = static_cast<uint32_t>(input.as_integer());
		    std::memcpy(&out[old], &uv, 4);
		}
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 2 )
	    {
		int16_t v = static_cast<int16_t>(input.as_integer());
		std::size_t old = out.size();
		out.resize(old + 2);
		if ( field.is_signed )
		    std::memcpy(&out[old], &v, 2);
		else
		{
		    uint16_t uv = static_cast<uint16_t>(input.as_integer());
		    std::memcpy(&out[old], &uv, 2);
		}
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 1 )
	    {
		int8_t v = static_cast<int8_t>(input.as_integer());
		std::size_t old = out.size();
		out.resize(old + 1);
		if ( field.is_signed )
		    std::memcpy(&out[old], &v, 1);
		else
		{
		    uint8_t uv = static_cast<uint8_t>(input.as_integer());
		    std::memcpy(&out[old], &uv, 1);
		}
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 4 )
	    {
		float v = static_cast<float>(input.as_real());
		std::size_t old = out.size();
		out.resize(old + sizeof(v));
		std::memcpy(&out[old], &v, sizeof(v));
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 8 )
	    {
		double v = input.as_real();
		std::size_t old = out.size();
		out.resize(old + sizeof(v));
		std::memcpy(&out[old], &v, sizeof(v));
		continue;
	    }
	    if ( field_is_character(field) )
	    {
		const std::string &text = input.as_string();
		char ch = text.empty() ? '\0' : text[0];
		std::size_t old = out.size();
		out.resize(old + sizeof(ch));
		std::memcpy(&out[old], &ch, sizeof(ch));
		continue;
	    }
	    if ( field_is_text(field) )
	    {
		if ( !append_string(out, input.as_string(), err) )
		    return false;
		continue;
	    }

	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "vlr field `" + field.name + "` uses unsupported type `" + field.type_name + "`");
	    return false;
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
		    return fail_decode(err, "vlr payload truncated in bool field");
		uint8_t b = read_pod_value<uint8_t>(payload, offset);
		offset += sizeof(uint8_t);
		out.object()[field.name] = value(b != 0);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 8 )
	    {
		if ( offset + sizeof(int64_t) > payload.size() )
		    return fail_decode(err, "vlr payload truncated in int64 field");
		if ( field.is_signed )
		    out.object()[field.name] = value(read_pod_value<int64_t>(payload, offset));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<uint64_t>(payload, offset)));
		offset += sizeof(int64_t);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 4 )
	    {
		if ( offset + sizeof(int32_t) > payload.size() )
		    return fail_decode(err, "vlr payload truncated in int32 field");
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<int32_t>(payload, offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<uint32_t>(payload, offset)));
		offset += sizeof(int32_t);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 2 )
	    {
		if ( offset + sizeof(int16_t) > payload.size() )
		    return fail_decode(err, "vlr payload truncated in int16 field");
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<int16_t>(payload, offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<uint16_t>(payload, offset)));
		offset += sizeof(int16_t);
		continue;
	    }
	    if ( field_is_integer(field) && field.byte_size == 1 )
	    {
		if ( offset + sizeof(int8_t) > payload.size() )
		    return fail_decode(err, "vlr payload truncated in int8 field");
		if ( field.is_signed )
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<int8_t>(payload, offset)));
		else
		    out.object()[field.name] = value(static_cast<int64_t>(read_pod_value<uint8_t>(payload, offset)));
		offset += sizeof(int8_t);
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 4 )
	    {
		if ( offset + sizeof(float) > payload.size() )
		    return fail_decode(err, "vlr payload truncated in float field");
		out.object()[field.name] = value(static_cast<double>(read_pod_value<float>(payload, offset)));
		offset += sizeof(float);
		continue;
	    }
	    if ( field_is_real(field) && field.byte_size == 8 )
	    {
		if ( offset + sizeof(double) > payload.size() )
		    return fail_decode(err, "vlr payload truncated in double field");
		out.object()[field.name] = value(read_pod_value<double>(payload, offset));
		offset += sizeof(double);
		continue;
	    }
	    if ( field_is_character(field) )
	    {
		if ( offset + sizeof(char) > payload.size() )
		    return fail_decode(err, "vlr payload truncated in char field");
		char ch = read_pod_value<char>(payload, offset);
		offset += sizeof(char);
		out.object()[field.name] = value(std::string(1, ch));
		continue;
	    }
	    if ( field_is_text(field) )
	    {
		std::string text;
		if ( !read_string(payload, offset, text, err) )
		    return false;
		out.object()[field.name] = value(text);
		continue;
	    }
	    return fail_decode(err, "vlr payload contains unsupported field type");
	}
	return true;
    }

    bool normalize_record(const value &record, value &out, error *err) const
    {
	std::vector<char> encoded;
	if ( !encode_record(record, encoded, err) )
	    return false;
	return decode_record(encoded, out, err);
    }

    bool fail_decode(error *err, const std::string &message) const
    {
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 message);
	return false;
    }

    bool flush_rewrite(error *err)
    {
	std::ofstream os(_path.c_str(), std::ios::binary | std::ios::trunc);
	if ( !os.good() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "failed to open vlr path for write: " + _path);
	    return false;
	}

	std::vector<uint64_t> next_offsets;
	next_offsets.reserve(_rows.size());
	for ( std::size_t i = 0; i < _rows.size(); ++i )
	{
	    std::streamoff record_offset = os.tellp();
	    if ( record_offset < 0 )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "failed to inspect vlr output offset");
		return false;
	    }
	    std::vector<char> encoded;
	    if ( !encode_record(_rows[i], encoded, err) )
		return false;
	    if ( encoded.size() > 0xffffffffu )
	    {
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "vlr record exceeds 32-bit length limit");
		return false;
	    }
	    uint32_t len = static_cast<uint32_t>(encoded.size());
	    os.write(reinterpret_cast<const char *>(&len), sizeof(len));
	    if ( len )
		os.write(&encoded[0], static_cast<std::streamsize>(encoded.size()));
	    next_offsets.push_back(static_cast<uint64_t>(record_offset));
	}
	if ( !os.good() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "failed while writing vlr record payloads");
	    return false;
	}
	if ( uses_tombstones()
	  && !write_packed_bits(_tombstone_path, _tombstones, err, "vlr") )
	    return false;
	_row_offsets = next_offsets;
	return true;
    }

    bool uses_tombstones() const
    {
	return !_tombstone_path.empty();
    }

    bool supports_stable_locators(error *err) const
    {
	if ( uses_tombstones() )
	    return true;
	if ( err )
	    *err = error(error::severity::error,
			 error::phase::runtime,
			 "vlr stable locators require a tombstone sidecar");
	return false;
    }

    bool row_is_tombstoned(std::size_t index) const
    {
	return index < _tombstones.size() && _tombstones[index];
    }

    SchemaInfo _schema;
    std::string _path;
    std::string _tombstone_path;
    std::vector<value> _rows;
    std::vector<uint64_t> _row_offsets;
    std::vector<bool> _tombstones;
    bool _opened;
};

class DsvDriverFactory : public DataDriverRegistry::Factory
{
public:
    std::unique_ptr<DataDriver> create() const
    {
	return std::unique_ptr<DataDriver>(new DsvDriver());
    }
};

class FlrDriverFactory : public DataDriverRegistry::Factory
{
public:
    std::unique_ptr<DataDriver> create() const
    {
	return std::unique_ptr<DataDriver>(new FlrDriver());
    }
};

class VlrDriverFactory : public DataDriverRegistry::Factory
{
public:
    std::unique_ptr<DataDriver> create() const
    {
	return std::unique_ptr<DataDriver>(new VlrDriver());
    }
};

} // namespace


void register_core_storage_drivers(DataDriverRegistry &registry)
{
	registry.register_factory("dsv", std::unique_ptr<DataDriverRegistry::Factory>(new DsvDriverFactory()));
	registry.register_factory("flr", std::unique_ptr<DataDriverRegistry::Factory>(new FlrDriverFactory()));
	registry.register_factory("vlr", std::unique_ptr<DataDriverRegistry::Factory>(new VlrDriverFactory()));
}

} // namespace madc
