#ifndef __LIBMADCDIS_MAPPER_H
#define __LIBMADCDIS_MAPPER_H 1

#include "libmadc/error.h"
#include "madcdis/schema.h"
#include "libmadc/value.h"

#include <algorithm>
#include <istream>
#include <functional>
#include <cstring>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace madc {

template <typename T>
class DataMapper
{
public:
    virtual ~DataMapper() {}

    virtual SchemaInfo schema() const = 0;
    virtual value encode(const T &input) const = 0;
    virtual T decode(const value &input) const = 0;

    virtual const char *name() const = 0;
};

template <typename T>
class FormatAdapter
{
public:
    virtual ~FormatAdapter() {}

    virtual const char *name() const = 0;
    virtual bool read_one(std::istream &is, T &out, error *err = nullptr) const = 0;
    virtual bool write_one(std::ostream &os, const T &input, error *err = nullptr) const = 0;
};

template <typename T>
class MappingSpec
{
public:
    enum class layout_mode
    {
	logical,
	fixed_record,
	variable_record
    };

    enum class overflow_policy
    {
	fail,
	truncate
    };

    struct text_limit
    {
	std::size_t max_bytes = 0;
	overflow_policy policy = overflow_policy::truncate;
    };

    MappingSpec()
	: _packed_binary(false),
	  _document_mode(false),
	  _dataset_role(SchemaInfo::role::primary),
	  _layout_mode(layout_mode::logical),
	  _fixed_record_size(0),
	  _fixed_record_overflow(overflow_policy::fail),
	  _ordered_key_compare(SchemaInfo::key_compare::none)
    {}

    MappingSpec &key(const std::string &field_name)
    {
	_keys.push_back(field_name);
	return *this;
    }
    MappingSpec &field_name(const std::string &field_name,
			    const std::string &storage_name)
    {
	_field_names[field_name] = storage_name;
	return *this;
    }
    MappingSpec &exclude(const std::string &field_name)
    {
	_excluded_fields.push_back(field_name);
	return *this;
    }
    MappingSpec &packed_binary(bool enabled = true)
    {
	_packed_binary = enabled;
	return *this;
    }
    MappingSpec &document_mode(bool enabled = true)
    {
	_document_mode = enabled;
	return *this;
    }
    MappingSpec &role(SchemaInfo::role dataset_role)
    {
	_dataset_role = dataset_role;
	return *this;
    }
    MappingSpec &fixed_record_size(std::size_t bytes,
				   overflow_policy policy = overflow_policy::fail)
    {
	_layout_mode = layout_mode::fixed_record;
	_fixed_record_size = bytes;
	_fixed_record_overflow = policy;
	return *this;
    }
    MappingSpec &ordered_by(const std::string &field_name,
			    SchemaInfo::key_compare compare = SchemaInfo::key_compare::lexical)
    {
	_ordered_key_field = field_name;
	_ordered_key_compare = compare;
	if ( std::find(_keys.begin(), _keys.end(), field_name) == _keys.end() )
	    _keys.push_back(field_name);
	return *this;
    }
    MappingSpec &variable_record(bool enabled = true)
    {
	if ( enabled )
	    _layout_mode = layout_mode::variable_record;
	else if ( _layout_mode == layout_mode::variable_record )
	    _layout_mode = layout_mode::logical;
	return *this;
    }
    MappingSpec &text_field_limit(const std::string &field_name,
				  std::size_t max_bytes,
				  overflow_policy policy = overflow_policy::truncate)
    {
	text_limit lim;
	lim.max_bytes = max_bytes;
	lim.policy = policy;
	_text_limits[field_name] = lim;
	return *this;
    }
    MappingSpec &tombstone_file(const std::string &path)
    {
	_tombstone_path = path;
	return *this;
    }
    MappingSpec &dead_record_file(const std::string &path)
    {
	_dead_record_path = path;
	return *this;
    }

    const std::vector<std::string> &keys() const { return _keys; }
    const std::vector<std::string> &excluded_fields() const { return _excluded_fields; }
    const std::map<std::string, std::string> &field_names() const { return _field_names; }
    const std::map<std::string, text_limit> &text_limits() const { return _text_limits; }
    bool packed_binary_enabled() const { return _packed_binary; }
    bool document_mode_enabled() const { return _document_mode; }
    SchemaInfo::role dataset_role() const { return _dataset_role; }
    layout_mode record_layout() const { return _layout_mode; }
    std::size_t record_size() const { return _fixed_record_size; }
    overflow_policy record_overflow_policy() const { return _fixed_record_overflow; }
    bool ordered_records() const { return !_ordered_key_field.empty(); }
    const std::string &ordered_key_field() const { return _ordered_key_field; }
    SchemaInfo::key_compare ordered_key_compare() const { return _ordered_key_compare; }
    bool has_tombstone_file() const { return !_tombstone_path.empty(); }
    const std::string &tombstone_path() const { return _tombstone_path; }
    bool has_dead_record_file() const { return !_dead_record_path.empty(); }
    const std::string &dead_record_path() const { return _dead_record_path; }

private:
    std::vector<std::string> _keys;
    std::vector<std::string> _excluded_fields;
    std::map<std::string, std::string> _field_names;
    std::map<std::string, text_limit> _text_limits;
    bool _packed_binary;
    bool _document_mode;
    SchemaInfo::role _dataset_role;
    layout_mode _layout_mode;
    std::size_t _fixed_record_size;
    overflow_policy _fixed_record_overflow;
    std::string _ordered_key_field;
    SchemaInfo::key_compare _ordered_key_compare;
    std::string _tombstone_path;
    std::string _dead_record_path;
};

template <typename T>
class AutoDataMapper : public DataMapper<T>
{
public:
    struct field_codec
    {
	SchemaField schema;
	std::function<void(const T &, value &)> encode;
	std::function<void(const value &, T &)> decode;
    };

    AutoDataMapper(const std::string &type_name,
		   const std::vector<field_codec> &fields)
	: _name(type_name),
	  _fields(fields)
    {
	_schema.set_name(type_name);
	_schema.set_kind(SchemaInfo::kind::structured);
	for ( std::size_t i = 0; i < _fields.size(); ++i )
	    _schema.add_field(_fields[i].schema);
    }

    SchemaInfo schema() const
    {
	return _schema;
    }

    value encode(const T &input) const
    {
	value record = value::make_object();
	for ( std::size_t i = 0; i < _fields.size(); ++i )
	    _fields[i].encode(input, record.object()[_fields[i].schema.name]);
	return record;
    }

    T decode(const value &input) const
    {
	T out = T();
	const std::map<std::string, value> &obj = input.as_object();
	for ( std::size_t i = 0; i < _fields.size(); ++i )
	{
	    std::map<std::string, value>::const_iterator it = obj.find(_fields[i].schema.name);
	    if ( it == obj.end() )
		continue;
	    _fields[i].decode(it->second, out);
	}
	return out;
    }

    const char *name() const
    {
	return _name.c_str();
    }

private:
    std::string _name;
    std::vector<field_codec> _fields;
    SchemaInfo _schema;
};

template <typename T>
class MapperBuilder
{
public:
    explicit MapperBuilder(const std::string &type_name)
	: _type_name(type_name)
    {}

    template <typename M>
    static std::size_t member_storage_size(M T::*)
    {
	return sizeof(M);
    }

    template <typename M>
    typename std::enable_if<std::is_integral<M>::value
			    && !std::is_same<M, bool>::value
			    && !std::is_same<M, char>::value,
			    MapperBuilder &>::type
    field(const std::string &name,
	  M T::*member,
	  bool key = false,
	  std::size_t byte_offset = 0)
    {
	typename AutoDataMapper<T>::field_codec codec;
	codec.schema = make_scalar_schema(name,
					  "integer",
					  SchemaField::kind::integer,
					  sizeof(M),
					  std::is_signed<M>::value,
					  key,
					  byte_offset);
	codec.encode = [member](const T &input, value &out) {
	    out = value(static_cast<int64_t>(input.*member));
	};
	codec.decode = [member](const value &input, T &out) {
	    out.*member = static_cast<M>(input.as_integer());
	};
	_fields.push_back(codec);
	return *this;
    }

    template <typename M>
    typename std::enable_if<std::is_enum<M>::value,
			    MapperBuilder &>::type
    field(const std::string &name,
	  M T::*member,
	  bool key = false,
	  std::size_t byte_offset = 0)
    {
	typedef typename std::underlying_type<M>::type underlying_type;
	typename AutoDataMapper<T>::field_codec codec;
	codec.schema = make_scalar_schema(name,
					  "enum",
					  SchemaField::kind::integer,
					  sizeof(underlying_type),
					  std::is_signed<underlying_type>::value,
					  key,
					  byte_offset);
	codec.encode = [member](const T &input, value &out) {
	    out = value(static_cast<int64_t>(input.*member));
	};
	codec.decode = [member](const value &input, T &out) {
	    out.*member = static_cast<M>(input.as_integer());
	};
	_fields.push_back(codec);
	return *this;
    }

    template <typename M>
    typename std::enable_if<std::is_floating_point<M>::value,
			    MapperBuilder &>::type
    field(const std::string &name,
	  M T::*member,
	  bool key = false,
	  std::size_t byte_offset = 0)
    {
	typename AutoDataMapper<T>::field_codec codec;
	codec.schema = make_scalar_schema(name,
					  "real",
					  SchemaField::kind::real,
					  sizeof(M),
					  true,
					  key,
					  byte_offset);
	codec.encode = [member](const T &input, value &out) {
	    out = value(static_cast<double>(input.*member));
	};
	codec.decode = [member](const value &input, T &out) {
	    out.*member = static_cast<M>(input.as_real());
	};
	_fields.push_back(codec);
	return *this;
    }

    MapperBuilder &field(const std::string &name,
			 bool T::*member,
			 bool key = false,
			 std::size_t byte_offset = 0)
    {
	typename AutoDataMapper<T>::field_codec codec;
	codec.schema = make_scalar_schema(name,
					  "bool",
					  SchemaField::kind::boolean,
					  sizeof(bool),
					  false,
					  key,
					  byte_offset);
	codec.encode = [member](const T &input, value &out) {
	    out = value(input.*member);
	};
	codec.decode = [member](const value &input, T &out) {
	    out.*member = input.as_boolean();
	};
	_fields.push_back(codec);
	return *this;
    }

    MapperBuilder &field(const std::string &name,
			 char T::*member,
			 bool key = false,
			 std::size_t byte_offset = 0)
    {
	typename AutoDataMapper<T>::field_codec codec;
	codec.schema = make_scalar_schema(name,
					  "char",
					  SchemaField::kind::character,
					  sizeof(char),
					  true,
					  key,
					  byte_offset);
	codec.encode = [member](const T &input, value &out) {
	    out = value(std::string(1, input.*member));
	};
	codec.decode = [member](const value &input, T &out) {
	    const std::string &text = input.as_string();
	    out.*member = text.empty() ? '\0' : text[0];
	};
	_fields.push_back(codec);
	return *this;
    }

    MapperBuilder &field(const std::string &name,
			 std::string T::*member,
			 bool key = false,
			 std::size_t byte_offset = 0)
    {
	typename AutoDataMapper<T>::field_codec codec;
	codec.schema = make_scalar_schema(name,
					  "string",
					  SchemaField::kind::text,
					  member_storage_size(member),
					  false,
					  key,
					  byte_offset);
	codec.encode = [member](const T &input, value &out) {
	    out = value(input.*member);
	};
	codec.decode = [member](const value &input, T &out) {
	    out.*member = input.as_string();
	};
	_fields.push_back(codec);
	return *this;
    }

    MapperBuilder &field(const std::string &name,
			 std::string T::*member,
			 std::size_t byte_size,
			 bool key,
			 std::size_t byte_offset)
    {
	typename AutoDataMapper<T>::field_codec codec;
	codec.schema = make_scalar_schema(name,
					  "string",
					  SchemaField::kind::text,
					  byte_size,
					  false,
					  key,
					  byte_offset);
	codec.encode = [member](const T &input, value &out) {
	    out = value(input.*member);
	};
	codec.decode = [member](const value &input, T &out) {
	    out.*member = input.as_string();
	};
	_fields.push_back(codec);
	return *this;
    }

    template <std::size_t N>
    MapperBuilder &field(const std::string &name,
			 char (T::*member)[N],
			 bool key = false,
			 std::size_t byte_offset = 0)
    {
	typename AutoDataMapper<T>::field_codec codec;
	std::ostringstream type_name;
	type_name << "char[" << N << "]";
	codec.schema = make_scalar_schema(name,
					  type_name.str(),
					  SchemaField::kind::text,
					  N,
					  false,
					  key,
					  byte_offset);
	codec.encode = [member](const T &input, value &out) {
	    const char (&raw)[N] = input.*member;
	    std::size_t len = 0;
	    while ( len < N && raw[len] != '\0' )
		++len;
	    out = value(std::string(raw, len));
	};
	codec.decode = [member](const value &input, T &out) {
	    const std::string &text = input.as_string();
	    char (&raw)[N] = out.*member;
	    std::size_t i = 0;
	    for ( ; i + 1 < N && i < text.size(); ++i )
		raw[i] = text[i];
	    raw[i] = '\0';
	    for ( ++i; i < N; ++i )
		raw[i] = '\0';
	};
	_fields.push_back(codec);
	return *this;
    }

    std::shared_ptr<DataMapper<T> > build() const
    {
	return std::shared_ptr<DataMapper<T> >(new AutoDataMapper<T>(_type_name, _fields));
    }

private:
    static SchemaField make_scalar_schema(const std::string &name,
					  const std::string &type_name,
					  SchemaField::kind field_kind,
					  std::size_t byte_size,
					  bool is_signed,
					  bool key,
					  std::size_t byte_offset)
    {
	SchemaField field;
	field.name = name;
	field.type_name = type_name;
	field.field_kind = field_kind;
	field.byte_size = byte_size;
	field.is_signed = is_signed;
	field.key = key;
	field.byte_offset = byte_offset;
	return field;
    }

    std::string _type_name;
    std::vector<typename AutoDataMapper<T>::field_codec> _fields;
};

template <typename T>
struct MapperRegistration
{
    static std::shared_ptr<DataMapper<T> > make(const MappingSpec<T> &spec)
    {
	(void)spec;
	return std::shared_ptr<DataMapper<T> >();
    }
};

template <typename T>
std::shared_ptr<DataMapper<T>> infer_mapper(const MappingSpec<T> &spec = MappingSpec<T>())
{
    return MapperRegistration<T>::make(spec);
}

template <typename T>
std::shared_ptr<FormatAdapter<T>> no_format_adapter()
{
    return std::shared_ptr<FormatAdapter<T>>();
}

} // namespace madc

#define MADC_MAP_FIELD(builder, Type, field_name) \
    (builder).field(#field_name, &Type::field_name, false, offsetof(Type, field_name))

#define MADC_MAP_KEY_FIELD(builder, Type, field_name) \
    (builder).field(#field_name, &Type::field_name, true, offsetof(Type, field_name))

#endif // __LIBMADCDIS_MAPPER_H
