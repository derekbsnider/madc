#include "madcdis/driver.h"
#include "madcdis/query.h"

#ifdef HAVE_SQLITE3

#include <sqlite3.h>

#include <cstdlib>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace madc {

namespace {

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

bool is_supported_field(const SchemaField &field)
{
    return field_is_integer(field)
	|| field_is_real(field)
	|| field_is_boolean(field)
	|| field_is_character(field)
	|| field_is_text(field);
}

bool is_supported_key_field(const SchemaField &field)
{
    return field_is_integer(field)
	|| field_is_boolean(field)
	|| field_is_character(field)
	|| field_is_text(field);
}

std::string quote_identifier(const std::string &name)
{
    std::string out = "\"";
    for ( std::size_t i = 0; i < name.size(); ++i )
    {
	if ( name[i] == '"' )
	    out += "\"\"";
	else
	    out += name[i];
    }
    out += '"';
    return out;
}

std::string sqlite_type_for(const SchemaField &field)
{
    if ( field_is_boolean(field) || field_is_integer(field) )
	return "INTEGER";
    if ( field_is_real(field) )
	return "REAL";
    if ( field_is_character(field) || field_is_text(field) )
	return "TEXT";
    return "BLOB";
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

class Statement
{
public:
    Statement(sqlite3 *db, const std::string &sql)
	: _stmt(nullptr)
    {
	if ( sqlite3_prepare_v2(db, sql.c_str(), -1, &_stmt, nullptr) != SQLITE_OK )
	    _stmt = nullptr;
    }

    ~Statement()
    {
	if ( _stmt )
	    sqlite3_finalize(_stmt);
    }

    sqlite3_stmt *get() const { return _stmt; }
    bool ok() const { return _stmt != nullptr; }

private:
    sqlite3_stmt *_stmt;
};

class SqliteDriver : public DataDriver
{
public:
    SqliteDriver()
	: _db(nullptr), _opened(false), _key_field(nullptr)
    {}

    ~SqliteDriver()
    {
	close();
    }

    const char *name() const { return "sqlite"; }
    const char *scheme() const { return "sqlite"; }

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
	caps.transaction = true;
	return caps;
    }

    bool bind_schema(const SchemaInfo &schema, error *err = nullptr)
    {
	_schema = schema;
	_table_name = _schema.name().empty() ? "records" : _schema.name();
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
	    return fail(err, "sqlite driver requires a logical or variable-record schema with one supported key field");
	return true;
    }

    bool open(const DataSource &source, error *err = nullptr)
    {
	if ( source.scheme() != "sqlite" )
	    return fail(err, "sqlite driver cannot open scheme `" + source.scheme() + "`");

	close();
	_path = source.path();
	if ( sqlite3_open(_path.c_str(), &_db) != SQLITE_OK )
	{
	    std::string msg = sqlite_message("failed to open sqlite path: " + _path);
	    close();
	    return fail(err, msg);
	}

	if ( !create_table(err) )
	{
	    close();
	    return false;
	}

	_opened = true;
	return true;
    }

    void close()
    {
	if ( _db )
	    sqlite3_close(_db);
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
	    if ( !is_supported_field(field) )
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
	    && (!query.has_lower_bound()
		|| (_key_field && query.lower_bound_field() == _key_field->name))
	    && (!query.has_upper_bound()
		|| (_key_field && query.upper_bound_field() == _key_field->name));
    }

    bool insert_record(const value &record, error *err = nullptr)
    {
	if ( !ensure_record_shape(record, err) )
	    return false;

	std::string sql = "INSERT INTO " + quote_identifier(_table_name) + " (";
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( i )
		sql += ", ";
	    sql += quote_identifier(_schema.fields()[i].name);
	}
	sql += ") VALUES (";
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( i )
		sql += ", ";
	    sql += "?";
	}
	sql += ")";

	Statement stmt(_db, sql);
	if ( !stmt.ok() )
	    return fail(err, sqlite_message("sqlite insert prepare failed"));
	if ( !bind_record(stmt.get(), record, 1, err) )
	    return false;

	int rc = sqlite3_step(stmt.get());
	if ( rc == SQLITE_CONSTRAINT )
	    return fail(err, "sqlite insert failed: key already exists");
	if ( rc != SQLITE_DONE )
	    return fail(err, sqlite_message("sqlite insert failed"));
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
	    return fail(err, "sqlite update failed: record key does not match lookup key");

	std::string sql = "UPDATE " + quote_identifier(_table_name) + " SET ";
	int set_count = 0;
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    const SchemaField &field = _schema.fields()[i];
	    if ( field.key )
		continue;
	    if ( set_count++ )
		sql += ", ";
	    sql += quote_identifier(field.name) + " = ?";
	}
	sql += " WHERE " + quote_identifier(_key_field->name) + " = ?";

	Statement stmt(_db, sql);
	if ( !stmt.ok() )
	    return fail(err, sqlite_message("sqlite update prepare failed"));

	int index = 1;
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    const SchemaField &field = _schema.fields()[i];
	    if ( field.key )
		continue;
	    if ( !bind_field(stmt.get(), index++, field, obj.at(field.name), err) )
		return false;
	}
	if ( !bind_field(stmt.get(), index, *_key_field, key_value, err) )
	    return false;

	int rc = sqlite3_step(stmt.get());
	if ( rc != SQLITE_DONE )
	    return fail(err, sqlite_message("sqlite update failed"));
	if ( sqlite3_changes(_db) == 0 )
	    return fail(err, "sqlite update failed: key not found");
	return true;
    }

    bool erase_record(const std::string &key_field,
		      const value &key_value,
		      error *err = nullptr)
    {
	(void)key_field;
	std::string sql = "DELETE FROM " + quote_identifier(_table_name)
	    + " WHERE " + quote_identifier(_key_field->name) + " = ?";
	Statement stmt(_db, sql);
	if ( !stmt.ok() )
	    return fail(err, sqlite_message("sqlite erase prepare failed"));
	if ( !bind_field(stmt.get(), 1, *_key_field, key_value, err) )
	    return false;
	int rc = sqlite3_step(stmt.get());
	if ( rc != SQLITE_DONE )
	    return fail(err, sqlite_message("sqlite erase failed"));
	if ( sqlite3_changes(_db) == 0 )
	    return fail(err, "sqlite erase failed: key not found");
	return true;
    }

    bool restore_record(const std::string &key_field,
			const value &key_value,
			error *err = nullptr)
    {
	(void)key_field;
	(void)key_value;
	return fail(err, "sqlite restore is unsupported without a tombstone sidecar");
    }

    bool compact_records(error *err = nullptr)
    {
	return fail(err, "sqlite compact is unsupported");
    }

    bool get_record(const std::string &key_field,
		    const value &key_value,
		    value &out,
		    error *err = nullptr) const
    {
	(void)key_field;
	std::string sql = select_all_sql()
	    + " WHERE " + quote_identifier(_key_field->name) + " = ? LIMIT 1";
	Statement stmt(_db, sql);
	if ( !stmt.ok() )
	    return fail(err, sqlite_message("sqlite get prepare failed"));
	if ( !bind_field(stmt.get(), 1, *_key_field, key_value, err) )
	    return false;

	int rc = sqlite3_step(stmt.get());
	if ( rc == SQLITE_ROW )
	    return row_to_record(stmt.get(), std::vector<std::string>(), out, err);
	if ( rc == SQLITE_DONE )
	    return false;
	return fail(err, sqlite_message("sqlite get failed"));
    }

    bool execute_query(const Query &query,
		       std::vector<value> &out,
		       error *err = nullptr) const
    {
	out.clear();
	if ( !can_execute(query) )
	    return fail(err, "sqlite query execution does not support this builder shape");

	std::string sql = select_sql(query.selected_fields());
	if ( query.has_where_equality() )
	{
	    const SchemaField *field = find_field(query.where_field());
	    if ( !field )
		return fail(err, "sqlite query execution failed: unknown field `" + query.where_field() + "`");
	    sql += " WHERE " + quote_identifier(field->name) + " = ?";
	}
	if ( query.has_where_inequality() )
	{
	    const SchemaField *field = find_field(query.where_ne_field());
	    if ( !field )
		return fail(err, "sqlite query execution failed: unknown field `" + query.where_ne_field() + "`");
	    sql += (query.has_where_equality() ? " AND " : " WHERE ");
	    sql += quote_identifier(field->name) + " != ?";
	}
	if ( query.has_where_in() )
	{
	    const SchemaField *field = find_field(query.where_in_field());
	    if ( !field )
		return fail(err, "sqlite query execution failed: unknown field `" + query.where_in_field() + "`");
	    sql += ((query.has_where_equality() || query.has_where_inequality()) ? " AND " : " WHERE ");
	    if ( query.where_in_values().empty() )
		sql += "1 = 0";
	    else
	    {
		sql += quote_identifier(field->name) + " IN (";
		for ( std::size_t i = 0; i < query.where_in_values().size(); ++i )
		{
		    if ( i )
			sql += ", ";
		    sql += "?";
		}
		sql += ")";
	    }
	}
	if ( query.has_where_not_in() )
	{
	    const SchemaField *field = find_field(query.where_not_in_field());
	    if ( !field )
		return fail(err, "sqlite query execution failed: unknown field `" + query.where_not_in_field() + "`");
	    sql += ((query.has_where_equality() || query.has_where_inequality() || query.has_where_in()) ? " AND " : " WHERE ");
	    if ( query.where_not_in_values().empty() )
		sql += "1 = 1";
	    else
	    {
		sql += quote_identifier(field->name) + " NOT IN (";
		for ( std::size_t i = 0; i < query.where_not_in_values().size(); ++i )
		{
		    if ( i )
			sql += ", ";
		    sql += "?";
		}
		sql += ")";
	    }
	}
	if ( query.has_where_like() )
	{
	    const SchemaField *field = find_field(query.where_like_field());
	    if ( !field )
		return fail(err, "sqlite query execution failed: unknown field `" + query.where_like_field() + "`");
	    sql += ((query.has_where_equality() || query.has_where_inequality() || query.has_where_in() || query.has_where_not_in()) ? " AND " : " WHERE ");
	    sql += quote_identifier(field->name) + " LIKE ?";
	}
	if ( query.has_lower_bound() )
	{
	    const SchemaField *field = find_field(query.lower_bound_field());
	    if ( !field )
		return fail(err, "sqlite query execution failed: unknown lower-bound field `" + query.lower_bound_field() + "`");
	    sql += ((query.has_where_equality() || query.has_where_inequality() || query.has_where_in() || query.has_where_not_in() || query.has_where_like()) ? " AND " : " WHERE ");
	    sql += quote_identifier(field->name);
	    sql += query.lower_bound_inclusive() ? " >= ?" : " > ?";
	}
	if ( query.has_upper_bound() )
	{
	    const SchemaField *field = find_field(query.upper_bound_field());
	    if ( !field )
		return fail(err, "sqlite query execution failed: unknown upper-bound field `" + query.upper_bound_field() + "`");
	    sql += ((query.has_where_equality() || query.has_where_inequality() || query.has_where_in() || query.has_where_not_in() || query.has_where_like() || query.has_lower_bound()) ? " AND " : " WHERE ");
	    sql += quote_identifier(field->name);
	    sql += query.upper_bound_inclusive() ? " <= ?" : " < ?";
	}
	if ( _key_field )
	    sql += " ORDER BY " + quote_identifier(_key_field->name);
	if ( query.has_limit() )
	    sql += " LIMIT " + std::to_string(static_cast<unsigned long long>(query.row_limit()));

	Statement stmt(_db, sql);
	if ( !stmt.ok() )
	    return fail(err, sqlite_message("sqlite query prepare failed"));
	int bind_index = 1;
	if ( query.has_where_equality() )
	{
	    const SchemaField *field = find_field(query.where_field());
	    if ( !bind_field(stmt.get(), bind_index++, *field, query.where_value(), err) )
		return false;
	}
	if ( query.has_where_inequality() )
	{
	    const SchemaField *field = find_field(query.where_ne_field());
	    if ( !bind_field(stmt.get(), bind_index++, *field, query.where_ne_value(), err) )
		return false;
	}
	if ( query.has_where_in() )
	{
	    const SchemaField *field = find_field(query.where_in_field());
	    for ( std::size_t i = 0; i < query.where_in_values().size(); ++i )
	    {
		if ( !bind_field(stmt.get(), bind_index++, *field, query.where_in_values()[i], err) )
		    return false;
	    }
	}
	if ( query.has_where_not_in() )
	{
	    const SchemaField *field = find_field(query.where_not_in_field());
	    for ( std::size_t i = 0; i < query.where_not_in_values().size(); ++i )
	    {
		if ( !bind_field(stmt.get(), bind_index++, *field, query.where_not_in_values()[i], err) )
		    return false;
	    }
	}
	if ( query.has_where_like() )
	{
	    const SchemaField *field = find_field(query.where_like_field());
	    if ( !bind_field(stmt.get(), bind_index++, *field, query.where_like_value(), err) )
		return false;
	}
	if ( query.has_lower_bound() )
	{
	    const SchemaField *field = find_field(query.lower_bound_field());
	    if ( !bind_field(stmt.get(), bind_index++, *field, query.lower_bound_value(), err) )
		return false;
	}
	if ( query.has_upper_bound() )
	{
	    const SchemaField *field = find_field(query.upper_bound_field());
	    if ( !bind_field(stmt.get(), bind_index++, *field, query.upper_bound_value(), err) )
		return false;
	}

	while ( true )
	{
	    int rc = sqlite3_step(stmt.get());
	    if ( rc == SQLITE_DONE )
		return true;
	    if ( rc != SQLITE_ROW )
		return fail(err, sqlite_message("sqlite query execution failed"));

	    value record;
	    if ( !row_to_record(stmt.get(), query.selected_fields(), record, err) )
		return false;
	    out.push_back(project_record(record, query));
	}
    }

    bool scan_records(std::vector<value> &out,
		      error *err = nullptr) const
    {
	out.clear();
	std::string sql = select_all_sql()
	    + " ORDER BY " + quote_identifier(_key_field->name);
	Statement stmt(_db, sql);
	if ( !stmt.ok() )
	    return fail(err, sqlite_message("sqlite scan prepare failed"));

	while ( true )
	{
	    int rc = sqlite3_step(stmt.get());
	    if ( rc == SQLITE_DONE )
		return true;
	    if ( rc != SQLITE_ROW )
		return fail(err, sqlite_message("sqlite scan failed"));

	    value record;
	    if ( !row_to_record(stmt.get(), std::vector<std::string>(), record, err) )
		return false;
	    out.push_back(record);
	}
    }

private:
    bool create_table(error *err)
    {
	std::string sql = "CREATE TABLE IF NOT EXISTS " + quote_identifier(_table_name) + " (";
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    const SchemaField &field = _schema.fields()[i];
	    if ( i )
		sql += ", ";
	    sql += quote_identifier(field.name) + " " + sqlite_type_for(field) + " NOT NULL";
	    if ( field.key )
		sql += " PRIMARY KEY";
	}
	sql += ")";

	char *errmsg = nullptr;
	int rc = sqlite3_exec(_db, sql.c_str(), nullptr, nullptr, &errmsg);
	if ( rc == SQLITE_OK )
	    return true;
	std::string msg = errmsg ? errmsg : sqlite_message("sqlite create table failed");
	sqlite3_free(errmsg);
	return fail(err, "sqlite create table failed: " + msg);
    }

    std::string select_all_sql() const
    {
	return select_sql(std::vector<std::string>());
    }

    std::string select_sql(const std::vector<std::string> &selected_fields) const
    {
	std::string sql = "SELECT ";
	const std::vector<std::string> &fields =
	    selected_fields.empty() ? schema_field_names() : selected_fields;
	for ( std::size_t i = 0; i < fields.size(); ++i )
	{
	    if ( i )
		sql += ", ";
	    sql += quote_identifier(fields[i]);
	}
	sql += " FROM " + quote_identifier(_table_name);
	return sql;
    }

    const std::vector<std::string> &schema_field_names() const
    {
	if ( _schema_field_names.empty() )
	{
	    for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
		_schema_field_names.push_back(_schema.fields()[i].name);
	}
	return _schema_field_names;
    }

    const SchemaField *find_field(const std::string &field_name) const
    {
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( _schema.fields()[i].name == field_name )
		return &_schema.fields()[i];
	}
	return nullptr;
    }

    bool ensure_record_shape(const value &record, error *err) const
    {
	if ( !record.is_object() )
	    return fail(err, "sqlite record must be an object");
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    if ( record.as_object().count(_schema.fields()[i].name) == 0 )
		return fail(err, "sqlite record is missing field `" + _schema.fields()[i].name + "`");
	}
	return true;
    }

    bool bind_record(sqlite3_stmt *stmt,
		     const value &record,
		     int first_index,
		     error *err) const
    {
	const std::map<std::string, value> &obj = record.as_object();
	for ( std::size_t i = 0; i < _schema.fields().size(); ++i )
	{
	    const SchemaField &field = _schema.fields()[i];
	    if ( !bind_field(stmt, first_index + static_cast<int>(i), field, obj.at(field.name), err) )
		return false;
	}
	return true;
    }

    bool bind_field(sqlite3_stmt *stmt,
		    int index,
		    const SchemaField &field,
		    const value &input,
		    error *err) const
    {
	int rc = SQLITE_MISUSE;
	if ( field_is_boolean(field) )
	    rc = sqlite3_bind_int64(stmt, index, input.as_boolean() ? 1 : 0);
	else if ( field_is_integer(field) )
	    rc = sqlite3_bind_int64(stmt, index, input.as_integer());
	else if ( field_is_real(field) )
	    rc = sqlite3_bind_double(stmt, index, input.as_real());
	else if ( field_is_character(field) || field_is_text(field) )
	{
	    const std::string &text = input.as_string();
	    rc = sqlite3_bind_text(stmt, index, text.c_str(),
				   static_cast<int>(text.size()),
				   SQLITE_TRANSIENT);
	}
	else
	    return fail(err, "sqlite field `" + field.name + "` uses unsupported type `" + field.type_name + "`");

	if ( rc != SQLITE_OK )
	    return fail(err, sqlite_message("sqlite bind failed for field `" + field.name + "`"));
	return true;
    }

    bool row_to_record(sqlite3_stmt *stmt,
		       const std::vector<std::string> &selected_fields,
		       value &out,
		       error *err) const
    {
	out = value::make_object();
	const std::vector<std::string> &fields =
	    selected_fields.empty() ? schema_field_names() : selected_fields;
	for ( std::size_t i = 0; i < fields.size(); ++i )
	{
	    const SchemaField *field = find_field(fields[i]);
	    if ( !field )
		return fail(err, "sqlite row contains unknown field `" + fields[i] + "`");
	    if ( sqlite3_column_type(stmt, static_cast<int>(i)) == SQLITE_NULL )
		return fail(err, "sqlite row has NULL field `" + field->name + "`");

	    if ( field_is_boolean(*field) )
	    {
		out.object()[field->name] =
		    value(sqlite3_column_int64(stmt, static_cast<int>(i)) != 0);
		continue;
	    }
	    if ( field_is_integer(*field) )
	    {
		out.object()[field->name] =
		    value(static_cast<int64_t>(sqlite3_column_int64(stmt, static_cast<int>(i))));
		continue;
	    }
	    if ( field_is_real(*field) )
	    {
		out.object()[field->name] =
		    value(sqlite3_column_double(stmt, static_cast<int>(i)));
		continue;
	    }
	    if ( field_is_character(*field) || field_is_text(*field) )
	    {
		const unsigned char *raw = sqlite3_column_text(stmt, static_cast<int>(i));
		int bytes = sqlite3_column_bytes(stmt, static_cast<int>(i));
		out.object()[field->name] =
		    value(std::string(reinterpret_cast<const char *>(raw), bytes));
		continue;
	    }

	    return fail(err, "sqlite row contains unsupported field type");
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

    std::string sqlite_message(const std::string &prefix) const
    {
	if ( !_db )
	    return prefix;
	const char *msg = sqlite3_errmsg(_db);
	if ( !msg || !*msg )
	    return prefix;
	return prefix + ": " + msg;
    }

    SchemaInfo _schema;
    std::string _path;
    std::string _table_name;
    sqlite3 *_db;
    bool _opened;
    const SchemaField *_key_field;
    mutable std::vector<std::string> _schema_field_names;
};

class SqliteDriverFactory : public DataDriverRegistry::Factory
{
public:
    std::unique_ptr<DataDriver> create() const
    {
	return std::unique_ptr<DataDriver>(new SqliteDriver());
    }
};

} // namespace

void register_sqlite_storage_driver(DataDriverRegistry &registry)
{
    registry.register_factory("sqlite",
			      std::unique_ptr<DataDriverRegistry::Factory>(new SqliteDriverFactory()));
}

} // namespace madc

#endif // HAVE_SQLITE3
