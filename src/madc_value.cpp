// madc::value — see include/libmadc/value.h.
//
// This TU is the WHOLE value layer: the refcounted cell runtime, the
// C `madc_value_*` storage/gradual-typing helpers, and the C++ RAII
// wrapper class — self-contained so it links everywhere madc::value
// does (including the no-compiler-internals test_libmadc_value binary).
// Design doc: docs/plans/2026-06-12-type-table-value-abi-design.md §3-§4.

#include "libmadc/value.h"
#include "madc_value_cell.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

// --- madc_cell runtime (refcounted payload cells) --------------------------

extern "C" {

void *madc_cell_alloc_dtor(size_t payload_size, void (*destroy)(void *payload))
{
	madc_cell *cell = static_cast<madc_cell *>(std::malloc(sizeof(madc_cell) + payload_size));

	if ( cell == NULL )
		return NULL;
	cell->refcount = 1;
	cell->cell_flags = 0;
	cell->destroy = destroy;
	std::memset(cell + 1, 0, payload_size);
	return cell + 1;
}

void *madc_cell_alloc(size_t payload_size)
{
	return madc_cell_alloc_dtor(payload_size, NULL);
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
	{
		if ( cell->destroy != NULL )
			cell->destroy(payload);
		std::free(cell);
	}
}

uint32_t madc_cell_refcount(const void *payload)
{
	if ( payload == NULL )
		return 0;
	return (static_cast<const madc_cell *>(payload) - 1)->refcount;
}

} // extern "C"

// --- madc_value storage + gradual typing (design doc §4) -------------------

namespace {

// Semantic (contract) flags: the typing contract (and, for LOCKED/COERCE,
// the type_id domain) survive a clear; storage bits and the payload do not.
const uint32_t MADC_VF_SEMANTIC =
    MADC_VF_TYPE_LOCKED | MADC_VF_TYPE_COERCE | MADC_VF_NULLABLE | MADC_VF_CONST;

bool is_numeric_kind(uint32_t k)
{
    return k == MADC_TYPEID_BOOL || k == MADC_TYPEID_INT64 || k == MADC_TYPEID_DOUBLE;
}

void clear_c_value(madc_value *value)
{
    if ( value == NULL )
	return;
    if ( (value->flags & MADC_VF_HEAP) && value->data_ptr != NULL )
	madc_cell_release(value->data_ptr);
    uint32_t kept_flags = value->flags & MADC_VF_SEMANTIC;
    uint32_t kept_type = (kept_flags & (MADC_VF_TYPE_LOCKED | MADC_VF_TYPE_COERCE))
	? value->type_id : (uint32_t)MADC_TYPEID_INVALID;
    std::memset(value, 0, sizeof(*value));
    value->flags = kept_flags;
    value->type_id = kept_type;
}

// Gradual-typing gate (design doc §4): may `value` accept a payload of
// kind `incoming`? Plain values re-tag freely; COERCE admits the numeric
// family (the caller converts toward the existing domain); LOCKED admits
// only its own domain; NULLABLE gates null under either. CONST rejects all.
bool value_accepts_kind(const madc_value *value, uint32_t incoming, bool is_null_set)
{
    if ( value->flags & MADC_VF_CONST )
	return false;
    if ( (value->flags & (MADC_VF_TYPE_LOCKED | MADC_VF_TYPE_COERCE)) == 0 )
	return true;
    if ( is_null_set )
	return (value->flags & MADC_VF_NULLABLE) != 0;
    if ( value->type_id == incoming || value->type_id == MADC_TYPEID_INVALID )
	return true;
    if ( value->flags & MADC_VF_TYPE_COERCE )
	return is_numeric_kind(value->type_id) && is_numeric_kind(incoming);
    return false;
}

// Write a numeric payload honoring the contract: plain values re-tag to
// `incoming`; COERCE converts toward the existing domain. `ival` carries
// bool/integer payloads, `rval` the real payload (selected by `incoming`).
int set_numeric_value(madc_value *value, uint32_t incoming, int64_t ival, double rval)
{
    if ( value == NULL )
	return MADC_ERROR;
    if ( !value_accepts_kind(value, incoming, false) )
	return MADC_ERROR;
    uint32_t target = incoming;
    if ( (value->flags & MADC_VF_TYPE_COERCE) && value->type_id != MADC_TYPEID_INVALID
    &&   value->type_id != incoming && is_numeric_kind(value->type_id) )
	target = value->type_id;
    clear_c_value(value);
    value->type_id = target;
    switch ( target )
    {
	case MADC_TYPEID_BOOL:
	    value->integer_value =
		(incoming == MADC_TYPEID_DOUBLE ? rval != 0.0 : ival != 0) ? 1 : 0;
	    value->size = 1;
	    break;
	case MADC_TYPEID_INT64:
	    value->integer_value = (incoming == MADC_TYPEID_DOUBLE) ? (int64_t)rval : ival;
	    value->size = 8;
	    break;
	case MADC_TYPEID_DOUBLE:
	    value->real_value = (incoming == MADC_TYPEID_DOUBLE) ? rval : (double)ival;
	    value->size = 8;
	    break;
	default:
	    return MADC_ERROR;
    }
    return MADC_OK;
}

bool set_c_string(madc_value *value, const char *data, size_t length)
{
    if ( value == NULL )
	return false;
    if ( !value_accepts_kind(value, MADC_TYPEID_TEXT, false) )
	return false;
    clear_c_value(value);
    value->type_id = MADC_TYPEID_TEXT;
    value->size = length;
    if ( length <= 15 )
    {
	if ( length > 0 && data != NULL )
	    std::memcpy(value->inline_text, data, length);
	value->inline_text[length] = '\0';
	value->flags |= MADC_VF_INLINE_TEXT;
	return true;
    }
    char *copy = static_cast<char *>(madc_cell_alloc(length + 1));
    if ( copy == NULL )
    {
	value->size = 0;
	return false;	// value left as a typed null
    }
    if ( data != NULL )
	std::memcpy(copy, data, length);
    copy[length] = '\0';
    value->text_value = copy;
    value->flags |= MADC_VF_HEAP;
    return true;
}

} // namespace

extern "C" {

void madc_value_init(madc_value *value)
{
    if ( value == NULL )
	return;
    std::memset(value, 0, sizeof(*value));
}

void madc_value_clear(madc_value *value)
{
    clear_c_value(value);
}

int madc_value_set_null(madc_value *value)
{
    if ( value == NULL )
	return MADC_ERROR;
    if ( !value_accepts_kind(value, MADC_TYPEID_INVALID, true) )
	return MADC_ERROR;
    clear_c_value(value);
    return MADC_OK;
}

int madc_value_set_bool(madc_value *value, int boolean_value)
{
    return set_numeric_value(value, MADC_TYPEID_BOOL, boolean_value ? 1 : 0, 0.0);
}

int madc_value_set_integer(madc_value *value, int64_t integer_value)
{
    return set_numeric_value(value, MADC_TYPEID_INT64, integer_value, 0.0);
}

int madc_value_set_real(madc_value *value, double real_value)
{
    return set_numeric_value(value, MADC_TYPEID_DOUBLE, 0, real_value);
}

int madc_value_set_string(madc_value *value, const char *text_value)
{
    if ( text_value == NULL )
	return madc_value_set_string_n(value, "", 0);
    return madc_value_set_string_n(value, text_value, std::strlen(text_value));
}

int madc_value_set_string_n(madc_value *value,
			    const char *text_value,
			    size_t text_length)
{
    if ( value == NULL )
	return MADC_ERROR;
    return set_c_string(value, text_value, text_length) ? MADC_OK : MADC_ERROR;
}

int madc_value_set_bytes_n(madc_value *value, const void *data, size_t size)
{
    if ( value == NULL )
	return MADC_ERROR;
    if ( !value_accepts_kind(value, MADC_TYPEID_BYTES, false) )
	return MADC_ERROR;
    clear_c_value(value);
    void *copy = madc_cell_alloc(size ? size : 1);
    if ( copy == NULL )
	return MADC_ERROR;
    if ( size > 0 && data != NULL )
	std::memcpy(copy, data, size);
    value->type_id = MADC_TYPEID_BYTES;
    value->size = size;
    value->data_ptr = copy;
    value->flags |= MADC_VF_HEAP;
    return MADC_OK;
}

void *madc_value_make_instance(madc_value *value, uint32_t type_id,
			       uint64_t size, void (*destroy)(void *payload))
{
    if ( value == NULL || type_id == MADC_TYPEID_INVALID )
	return NULL;
    if ( !value_accepts_kind(value, type_id, false) )
	return NULL;
    clear_c_value(value);
    void *payload = madc_cell_alloc_dtor((size_t)(size ? size : 1), destroy);
    if ( payload == NULL )
	return NULL;
    value->type_id = type_id;
    value->size = size;
    value->data_ptr = payload;
    value->flags |= MADC_VF_HEAP;
    return payload;
}

int madc_value_copy(madc_value *dst, const madc_value *src)
{
    if ( dst == NULL || src == NULL )
	return MADC_ERROR;
    if ( dst == src )
	return MADC_OK;
    // Retain BEFORE releasing dst — src may alias dst's cell.
    if ( (src->flags & MADC_VF_HEAP) && src->data_ptr != NULL )
	madc_cell_retain(src->data_ptr);
    if ( (dst->flags & MADC_VF_HEAP) && dst->data_ptr != NULL )
	madc_cell_release(dst->data_ptr);
    // Copy is INITIALIZATION semantics: dst becomes a full copy of src,
    // including src's gradual-typing contract (a copy of a locked value is
    // locked). Contract-honoring assignment is a separate package-C surface.
    *dst = *src;
    return MADC_OK;
}

const char *madc_value_text(const madc_value *value, size_t *text_length)
{
    if ( text_length != NULL )
	*text_length = 0;
    if ( value == NULL )
	return NULL;
    const char *text = NULL;
    if ( value->flags & MADC_VF_INLINE_TEXT )
	text = value->inline_text;
    else if ( (value->flags & MADC_VF_HEAP) && value->type_id == MADC_TYPEID_TEXT )
	text = value->text_value;
    if ( text != NULL && text_length != NULL )
	*text_length = (size_t)value->size;
    return text;
}

const void *madc_value_data(const madc_value *value, size_t *size)
{
    if ( size != NULL )
	*size = 0;
    if ( value == NULL )
	return NULL;
    const void *payload = NULL;
    if ( value->flags & MADC_VF_INLINE_TEXT )
	payload = value->inline_text;
    else if ( (value->flags & MADC_VF_HEAP) && value->data_ptr != NULL )
	payload = value->data_ptr;
    if ( payload != NULL && size != NULL )
	*size = (size_t)value->size;
    return payload;
}

} // extern "C"

// --- madc::value (C++ RAII wrapper over the struct) ------------------------

namespace madc {

value::value()
{
    madc_value_init(&_v);
}

value::value(std::nullptr_t)
{
    madc_value_init(&_v);
}

value::value(bool b)
{
    madc_value_init(&_v);
    madc_value_set_bool(&_v, b ? 1 : 0);
}

value::value(int64_t i)
{
    madc_value_init(&_v);
    madc_value_set_integer(&_v, i);
}

value::value(double d)
{
    madc_value_init(&_v);
    madc_value_set_real(&_v, d);
}

value::value(const char *s)
{
    madc_value_init(&_v);
    madc_value_set_string(&_v, s);
}

value::value(const std::string &s)
{
    madc_value_init(&_v);
    madc_value_set_string_n(&_v, s.data(), s.size());
}

value value::make_null()
{
    return value();
}

value value::make_bytes(const void *data, size_t size)
{
    value v;
    madc_value_set_bytes_n(&v._v, data, size);
    return v;
}

value value::make_array()
{
    value v;
    v._v.type_id = MADC_TYPEID_ARRAY;
    v._array.reset(new std::vector<value>());
    return v;
}

value value::make_array(std::vector<value> elements)
{
    value v;
    v._v.type_id = MADC_TYPEID_ARRAY;
    v._array.reset(new std::vector<value>(std::move(elements)));
    return v;
}

value value::make_object()
{
    value v;
    v._v.type_id = MADC_TYPEID_OBJECT;
    v._object.reset(new std::map<std::string, value>());
    return v;
}

value value::make_object(std::map<std::string, value> fields)
{
    value v;
    v._v.type_id = MADC_TYPEID_OBJECT;
    v._object.reset(new std::map<std::string, value>(std::move(fields)));
    return v;
}

value value::make_instance(uint32_t type_id, size_t size,
			   void (*destroy)(void *payload))
{
    value v;
    madc_value_make_instance(&v._v, type_id, size, destroy);
    return v;
}

value::value(const value &other)
{
    madc_value_init(&_v);
    madc_value_copy(&_v, &other._v);
    if ( other._array )
	_array.reset(new std::vector<value>(*other._array));
    if ( other._object )
	_object.reset(new std::map<std::string, value>(*other._object));
}

value::value(value &&other) noexcept
    : _array(std::move(other._array)),
      _object(std::move(other._object))
{
    _v = other._v;
    madc_value_init(&other._v);
}

value &value::operator=(const value &other)
{
    if ( this == &other )
	return *this;
    madc_value_copy(&_v, &other._v);
    if ( other._array )
	_array.reset(new std::vector<value>(*other._array));
    else
	_array.reset();
    if ( other._object )
	_object.reset(new std::map<std::string, value>(*other._object));
    else
	_object.reset();
    return *this;
}

value &value::operator=(value &&other) noexcept
{
    if ( this == &other )
	return *this;
    madc_value_clear(&_v);
    _v = other._v;
    madc_value_init(&other._v);
    _array  = std::move(other._array);
    _object = std::move(other._object);
    return *this;
}

value::~value()
{
    madc_value_clear(&_v);
}

value::kind value::type() const
{
    switch ( _v.type_id )
    {
	case MADC_TYPEID_INVALID: return kind::null;
	case MADC_TYPEID_BOOL:    return kind::boolean;
	case MADC_TYPEID_INT64:   return kind::integer;
	case MADC_TYPEID_DOUBLE:  return kind::real;
	case MADC_TYPEID_TEXT:    return kind::string;
	case MADC_TYPEID_BYTES:   return kind::bytes;
	case MADC_TYPEID_ARRAY:   return kind::array;
	case MADC_TYPEID_OBJECT:  return kind::object;
	default:                  return kind::instance;
    }
}

uint32_t value::type_id() const
{
    return _v.type_id;
}

size_t value::size() const
{
    return (size_t)_v.size;
}

const void *value::data() const
{
    return madc_value_data(&_v, NULL);
}

void *value::instance_data()
{
    if ( type() != kind::instance || !(_v.flags & MADC_VF_HEAP) )
	throw std::runtime_error("madc::value::instance_data: kind is not instance");
    return _v.data_ptr;
}

bool value::as_boolean() const
{
    if ( _v.type_id != MADC_TYPEID_BOOL )
	throw std::runtime_error("madc::value::as_boolean: kind is not boolean");
    return _v.integer_value != 0;
}

int64_t value::as_integer() const
{
    if ( _v.type_id != MADC_TYPEID_INT64 )
	throw std::runtime_error("madc::value::as_integer: kind is not integer");
    return _v.integer_value;
}

double value::as_real() const
{
    if ( _v.type_id != MADC_TYPEID_DOUBLE )
	throw std::runtime_error("madc::value::as_real: kind is not real");
    return _v.real_value;
}

std::string value::as_string() const
{
    if ( _v.type_id != MADC_TYPEID_TEXT )
	throw std::runtime_error("madc::value::as_string: kind is not string");
    size_t len = 0;
    const char *text = madc_value_text(&_v, &len);
    return std::string(text ? text : "", len);
}

const std::vector<value> &value::as_array() const
{
    if ( type() != kind::array || !_array )
	throw std::runtime_error("madc::value::as_array: kind is not array");
    return *_array;
}

const std::map<std::string, value> &value::as_object() const
{
    if ( type() != kind::object || !_object )
	throw std::runtime_error("madc::value::as_object: kind is not object");
    return *_object;
}

std::vector<value> &value::array()
{
    if ( type() == kind::null )
	_v.type_id = MADC_TYPEID_ARRAY;     // vivify: null -> empty array
    if ( type() != kind::array )
	throw std::runtime_error("madc::value::array: kind is not array");
    if ( !_array )
	_array.reset(new std::vector<value>());
    return *_array;
}

std::map<std::string, value> &value::object()
{
    if ( type() == kind::null )
	_v.type_id = MADC_TYPEID_OBJECT;    // vivify: null -> empty object
    if ( type() != kind::object )
	throw std::runtime_error("madc::value::object: kind is not object");
    if ( !_object )
	_object.reset(new std::map<std::string, value>());
    return *_object;
}

bool value::operator==(const value &other) const
{
    if ( type() != other.type() )
	return false;
    switch ( type() )
    {
	case kind::null:    return true;
	case kind::boolean:
	case kind::integer: return _v.integer_value == other._v.integer_value;
	case kind::real:    return _v.real_value == other._v.real_value;
	case kind::string:
	case kind::bytes:
	{
	    if ( _v.size != other._v.size )
		return false;
	    size_t len = 0, olen = 0;
	    const void *a = madc_value_data(&_v, &len);
	    const void *b = madc_value_data(&other._v, &olen);
	    if ( !a && !b ) return true;
	    if ( !a || !b ) return false;
	    return std::memcmp(a, b, len) == 0;
	}
	case kind::instance:
	    // Typed instances compare by IDENTITY (same cell): bytewise
	    // comparison would be wrong for non-trivial instances.
	    return _v.type_id == other._v.type_id
		&& _v.data_ptr == other._v.data_ptr;
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
	case kind::null:     return "null";
	case kind::boolean:  return "boolean";
	case kind::integer:  return "integer";
	case kind::real:     return "real";
	case kind::string:   return "string";
	case kind::bytes:    return "bytes";
	case kind::array:    return "array";
	case kind::object:   return "object";
	case kind::instance: return "instance";
    }
    return "null";
}

} // namespace madc
