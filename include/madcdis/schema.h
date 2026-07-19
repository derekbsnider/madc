#ifndef __LIBMADCDIS_SCHEMA_H
#define __LIBMADCDIS_SCHEMA_H 1

#include <cstddef>
#include <string>
#include <vector>

namespace madc {

struct SchemaField
{
    enum class kind
    {
	unknown,
	boolean,
	integer,
	real,
	character,
	text,
	object,
	array,
	dynamic,
	bytes
    };

    enum class overflow_policy
    {
	fail,
	truncate
    };

    enum class shape
    {
	scalar,
	object,
	array,
	dynamic,
	bytes
    };

    std::string name;
    std::string type_name;
    kind field_kind = kind::unknown;
    shape field_shape = shape::scalar;
    std::size_t byte_offset = 0;
    std::size_t byte_size = 0;
    bool is_signed = false;
    bool nullable = false;
    bool repeated = false;
    bool persisted = true;
    bool key = false;
    overflow_policy text_overflow = overflow_policy::fail;

    kind resolved_kind() const
    {
	if ( field_kind != kind::unknown )
	    return field_kind;

	if ( type_name == "bool" )
	    return kind::boolean;
	if ( type_name == "float" || type_name == "double" )
	    return kind::real;
	if ( type_name == "char" )
	    return kind::character;
	if ( type_name == "string" || type_name == "std::string"
	  || type_name.find("char[") == 0 )
	    return kind::text;
	if ( type_name.find("int") != std::string::npos
	  || type_name.find("uint") != std::string::npos
	  || type_name == "size_t" )
	    return kind::integer;
	return kind::unknown;
    }

    bool needs_scalar_width() const
    {
	kind k = resolved_kind();
	return k == kind::integer || k == kind::real;
    }
};

class SchemaInfo
{
public:
    enum class role
    {
	primary,
	index,
	payload,
	tombstone,
	sidecar,
	metadata
    };

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

    enum class kind
    {
	scalar,
	structured,
	dynamic,
	document
    };

    enum class ordering_mode
    {
	unordered,
	ordered_by_key
    };

    enum class key_compare
    {
	none,
	lexical,
	fixed_text,
	numeric_signed,
	numeric_unsigned
    };

    SchemaInfo()
	: _kind(kind::scalar),
	  _role(role::primary),
	  _packed_binary_compatible(false),
	  _fixed_size(0),
	  _record_layout(layout_mode::logical),
	  _record_size(0),
	  _record_overflow(overflow_policy::fail),
	  _ordering_mode(ordering_mode::unordered),
	  _ordered_key_compare(key_compare::none)
    {}
    explicit SchemaInfo(const std::string &type_name)
	: _name(type_name),
	  _kind(kind::scalar),
	  _role(role::primary),
	  _packed_binary_compatible(false),
	  _fixed_size(0),
	  _record_layout(layout_mode::logical),
	  _record_size(0),
	  _record_overflow(overflow_policy::fail),
	  _ordering_mode(ordering_mode::unordered),
	  _ordered_key_compare(key_compare::none)
    {}

    const std::string &name() const { return _name; }
    kind schema_kind() const { return _kind; }
    role dataset_role() const { return _role; }
    bool packed_binary_compatible() const { return _packed_binary_compatible; }
    std::size_t fixed_size() const { return _fixed_size; }
    layout_mode record_layout() const { return _record_layout; }
    std::size_t record_size() const { return _record_size; }
    overflow_policy record_overflow_policy() const { return _record_overflow; }
    ordering_mode record_ordering() const { return _ordering_mode; }
    const std::string &ordered_key_field() const { return _ordered_key_field; }
    key_compare ordered_key_compare() const { return _ordered_key_compare; }
    bool has_tombstone_sidecar() const { return !_tombstone_path.empty(); }
    const std::string &tombstone_path() const { return _tombstone_path; }
    bool has_dead_record_archive() const { return !_dead_record_path.empty(); }
    const std::string &dead_record_path() const { return _dead_record_path; }
    const std::vector<SchemaField> &fields() const { return _fields; }

    void set_name(const std::string &type_name) { _name = type_name; }
    void set_kind(kind k) { _kind = k; }
    void set_dataset_role(role r) { _role = r; }
    void set_packed_binary_compatible(bool packed) { _packed_binary_compatible = packed; }
    void set_fixed_size(std::size_t size) { _fixed_size = size; }
    void set_record_layout(layout_mode mode) { _record_layout = mode; }
    void set_record_size(std::size_t size) { _record_size = size; }
    void set_record_overflow_policy(overflow_policy policy) { _record_overflow = policy; }
    void set_ordered_key(const std::string &field_name, key_compare compare)
    {
	if ( field_name.empty() )
	{
	    _ordering_mode = ordering_mode::unordered;
	    _ordered_key_field.clear();
	    _ordered_key_compare = key_compare::none;
	    return;
	}
	_ordering_mode = ordering_mode::ordered_by_key;
	_ordered_key_field = field_name;
	_ordered_key_compare = compare;
    }
    void set_tombstone_path(const std::string &path)
    {
	_tombstone_path = path;
    }
    void set_dead_record_path(const std::string &path)
    {
	_dead_record_path = path;
    }
    void add_field(const SchemaField &field) { _fields.push_back(field); }

private:
    std::string _name;
    kind _kind;
    role _role;
    bool _packed_binary_compatible;
    std::size_t _fixed_size;
    layout_mode _record_layout;
    std::size_t _record_size;
    overflow_policy _record_overflow;
    ordering_mode _ordering_mode;
    std::string _ordered_key_field;
    key_compare _ordered_key_compare;
    std::string _tombstone_path;
    std::string _dead_record_path;
    std::vector<SchemaField> _fields;
};

} // namespace madc

#endif // __LIBMADCDIS_SCHEMA_H
