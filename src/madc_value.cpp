// madc::value — see include/libmadc/value.h

#include "libmadc/value.h"

#include <stdexcept>

namespace madc {

namespace {

std::vector<value> clone_array(const std::vector<value> &src)
{
    return std::vector<value>(src);
}

std::map<std::string, value> clone_object(const std::map<std::string, value> &src)
{
    return std::map<std::string, value>(src);
}

} // namespace

value::value()
    : _kind(kind::null), _bool(false), _int(0), _real(0.0)
{
}

value::value(std::nullptr_t)
    : _kind(kind::null), _bool(false), _int(0), _real(0.0)
{
}

value::value(bool b)
    : _kind(kind::boolean), _bool(b), _int(0), _real(0.0)
{
}

value::value(int64_t i)
    : _kind(kind::integer), _bool(false), _int(i), _real(0.0)
{
}

value::value(double d)
    : _kind(kind::real), _bool(false), _int(0), _real(d)
{
}

value::value(const char *s)
    : _kind(kind::string), _bool(false), _int(0), _real(0.0),
      _string(s ? s : "")
{
}

value::value(const std::string &s)
    : _kind(kind::string), _bool(false), _int(0), _real(0.0), _string(s)
{
}

value::value(std::string &&s)
    : _kind(kind::string), _bool(false), _int(0), _real(0.0),
      _string(std::move(s))
{
}

value value::make_null()
{
    return value();
}

value value::make_bytes(std::vector<uint8_t> bytes)
{
    value v;
    v._kind = kind::bytes;
    v._bytes = std::move(bytes);
    return v;
}

value value::make_array()
{
    value v;
    v._kind = kind::array;
    v._array.reset(new std::vector<value>());
    return v;
}

value value::make_array(std::vector<value> elements)
{
    value v;
    v._kind = kind::array;
    v._array.reset(new std::vector<value>(std::move(elements)));
    return v;
}

value value::make_object()
{
    value v;
    v._kind = kind::object;
    v._object.reset(new std::map<std::string, value>());
    return v;
}

value value::make_object(std::map<std::string, value> fields)
{
    value v;
    v._kind = kind::object;
    v._object.reset(new std::map<std::string, value>(std::move(fields)));
    return v;
}

value::value(const value &other)
    : _kind(other._kind),
      _bool(other._bool),
      _int(other._int),
      _real(other._real),
      _string(other._string),
      _bytes(other._bytes)
{
    if ( other._array )
	_array.reset(new std::vector<value>(clone_array(*other._array)));
    if ( other._object )
	_object.reset(new std::map<std::string, value>(clone_object(*other._object)));
}

value::value(value &&other) noexcept
    : _kind(other._kind),
      _bool(other._bool),
      _int(other._int),
      _real(other._real),
      _string(std::move(other._string)),
      _bytes(std::move(other._bytes)),
      _array(std::move(other._array)),
      _object(std::move(other._object))
{
    other._kind = kind::null;
    other._bool = false;
    other._int  = 0;
    other._real = 0.0;
}

value &value::operator=(const value &other)
{
    if ( this == &other )
	return *this;
    _kind   = other._kind;
    _bool   = other._bool;
    _int    = other._int;
    _real   = other._real;
    _string = other._string;
    _bytes  = other._bytes;
    if ( other._array )
	_array.reset(new std::vector<value>(clone_array(*other._array)));
    else
	_array.reset();
    if ( other._object )
	_object.reset(new std::map<std::string, value>(clone_object(*other._object)));
    else
	_object.reset();
    return *this;
}

value &value::operator=(value &&other) noexcept
{
    if ( this == &other )
	return *this;
    _kind   = other._kind;
    _bool   = other._bool;
    _int    = other._int;
    _real   = other._real;
    _string = std::move(other._string);
    _bytes  = std::move(other._bytes);
    _array  = std::move(other._array);
    _object = std::move(other._object);
    other._kind = kind::null;
    other._bool = false;
    other._int  = 0;
    other._real = 0.0;
    return *this;
}

value::~value() = default;

bool value::as_boolean() const
{
    if ( _kind != kind::boolean )
	throw std::runtime_error("madc::value::as_boolean: kind is not boolean");
    return _bool;
}

int64_t value::as_integer() const
{
    if ( _kind != kind::integer )
	throw std::runtime_error("madc::value::as_integer: kind is not integer");
    return _int;
}

double value::as_real() const
{
    if ( _kind != kind::real )
	throw std::runtime_error("madc::value::as_real: kind is not real");
    return _real;
}

const std::string &value::as_string() const
{
    if ( _kind != kind::string )
	throw std::runtime_error("madc::value::as_string: kind is not string");
    return _string;
}

const std::vector<uint8_t> &value::as_bytes() const
{
    if ( _kind != kind::bytes )
	throw std::runtime_error("madc::value::as_bytes: kind is not bytes");
    return _bytes;
}

const std::vector<value> &value::as_array() const
{
    if ( _kind != kind::array || !_array )
	throw std::runtime_error("madc::value::as_array: kind is not array");
    return *_array;
}

const std::map<std::string, value> &value::as_object() const
{
    if ( _kind != kind::object || !_object )
	throw std::runtime_error("madc::value::as_object: kind is not object");
    return *_object;
}

std::vector<value> &value::array()
{
    if ( _kind == kind::null )
	_kind = kind::array;            // vivify: null -> empty array
    if ( _kind != kind::array )
	throw std::runtime_error("madc::value::array: kind is not array");
    if ( !_array )
	_array.reset(new std::vector<value>());
    return *_array;
}

std::map<std::string, value> &value::object()
{
    if ( _kind == kind::null )
	_kind = kind::object;           // vivify: null -> empty object
    if ( _kind != kind::object )
	throw std::runtime_error("madc::value::object: kind is not object");
    if ( !_object )
	_object.reset(new std::map<std::string, value>());
    return *_object;
}

bool value::operator==(const value &other) const
{
    if ( _kind != other._kind )
	return false;
    switch ( _kind )
    {
	case kind::null:    return true;
	case kind::boolean: return _bool == other._bool;
	case kind::integer: return _int  == other._int;
	case kind::real:    return _real == other._real;
	case kind::string:  return _string == other._string;
	case kind::bytes:   return _bytes  == other._bytes;
	case kind::array:
	{
	    const std::vector<value> *a = _array.get();
	    const std::vector<value> *b = other._array.get();
	    if ( !a && !b ) return true;
	    if ( !a || !b ) return false;
	    return *a == *b;
	}
	case kind::object:
	{
	    const std::map<std::string, value> *a = _object.get();
	    const std::map<std::string, value> *b = other._object.get();
	    if ( !a && !b ) return true;
	    if ( !a || !b ) return false;
	    return *a == *b;
	}
    }
    return false;
}

const char *value::kind_name(kind k)
{
    switch ( k )
    {
	case kind::null:    return "null";
	case kind::boolean: return "boolean";
	case kind::integer: return "integer";
	case kind::real:    return "real";
	case kind::string:  return "string";
	case kind::bytes:   return "bytes";
	case kind::array:   return "array";
	case kind::object:  return "object";
    }
    return "null";
}

} // namespace madc

// --- madc_value cell runtime (refcounted payload cells) -------------------
// docs/plans/2026-06-12-type-table-value-abi-design.md §3. Lives in this TU
// so the cell runtime links everywhere madc::value does (incl. the
// no-compiler-internals test_libmadc_value binary).
#include "madc_value_cell.h"
#include <cstdlib>
#include <cstring>

extern "C" {

void *madc_cell_alloc(size_t payload_size)
{
	madc_cell *cell = static_cast<madc_cell *>(std::malloc(sizeof(madc_cell) + payload_size));

	if ( cell == NULL )
		return NULL;
	cell->refcount = 1;
	cell->cell_flags = 0;
	std::memset(cell + 1, 0, payload_size);
	return cell + 1;
}

madc_cell *madc_cell_of(void *payload)
{
	if ( payload == NULL )
		return NULL;
	return static_cast<madc_cell *>(payload) - 1;
}

void madc_cell_retain(void *payload)
{
	madc_cell *cell = madc_cell_of(payload);

	if ( cell == NULL || cell->refcount == MADC_CELL_PERMANENT )
		return;
	if ( cell->refcount == MADC_CELL_PERMANENT - 1 )
		cell->refcount = MADC_CELL_PERMANENT;	// saturate
	else
		++cell->refcount;
}

void madc_cell_release(void *payload)
{
	madc_cell *cell = madc_cell_of(payload);

	if ( cell == NULL || cell->refcount == MADC_CELL_PERMANENT )
		return;
	if ( --cell->refcount == 0 )
		std::free(cell);
}

uint32_t madc_cell_refcount(const void *payload)
{
	if ( payload == NULL )
		return 0;
	return (static_cast<const madc_cell *>(payload) - 1)->refcount;
}

} // extern "C"
