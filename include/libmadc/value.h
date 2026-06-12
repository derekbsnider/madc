#ifndef __LIBMADC_VALUE_H
#define __LIBMADC_VALUE_H 1

// madc::value — public host-facing value type for the libmadc embedding
// API: a thin RAII wrapper over the 32-byte `madc_value` interchange
// struct (include/madc_api.h; design doc
// docs/plans/2026-06-12-type-table-value-abi-design.md §3).
//
// Identity is the uint32 typeid (madc_typeid.h). Scalars inline in the
// struct; text and bytes are char/byte payloads (SSO or refcounted
// cells) — never a C++ library object; a typed INSTANCE of any script
// type is a refcounted cell tagged with that type's id, destroyed by
// the type's own finalizer. The `kind` enum is a coarse public
// categorization derived from the typeid.
//
// The array and object kinds keep C++ container backing for now (the
// unified script array surface); their cell representation arrives with
// the madcdis pool work.
//
// Vivification: the mutable accessors `object()` and `array()` convert
// a kind::null value into an empty object / empty array respectively
// (then return the live container), instead of throwing. Any other kind
// mismatch still throws. The const accessors never vivify.

#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "madc_api.h"

namespace madc {

class value
{
public:
    enum class kind
    {
	null,
	boolean,
	integer,
	real,
	string,
	bytes,
	array,
	object,
	instance	// a typed instance cell: type_id() names the type
    };

    value();
    value(std::nullptr_t);
    explicit value(bool b);
    explicit value(int64_t i);
    explicit value(double d);
    value(const char *s);
    value(const std::string &s);

    static value make_null();
    static value make_bytes(const void *data, size_t size);
    static value make_array();
    static value make_array(std::vector<value> elements);
    static value make_object();
    static value make_object(std::map<std::string, value> fields);
    // A typed instance: allocates a zeroed refcounted cell of `size`
    // bytes tagged `type_id`, finalized by `destroy` (may be NULL for a
    // trivially-destructible instance). The caller constructs the
    // instance into instance_data().
    static value make_instance(uint32_t type_id, size_t size,
			       void (*destroy)(void *payload));

    value(const value &other);
    value(value &&other) noexcept;
    value &operator=(const value &other);
    value &operator=(value &&other) noexcept;
    ~value();

    kind type() const;
    // Canonical type identity (madc_typeid.h slot or table id); the
    // generic surface alongside the coarse kind.
    uint32_t type_id() const;
    // Payload size in bytes (text/bytes length, instance size; scalar
    // sizeof).
    size_t size() const;
    // Payload pointer for text / bytes / instance kinds (NULL for
    // others). Shared, read-only view into the value's storage.
    const void *data() const;
    // Mutable instance payload (instance kind only; throws otherwise).
    void *instance_data();

    bool is_null()     const { return type() == kind::null; }
    bool is_boolean()  const { return type() == kind::boolean; }
    bool is_integer()  const { return type() == kind::integer; }
    bool is_real()     const { return type() == kind::real; }
    bool is_string()   const { return type() == kind::string; }
    bool is_bytes()    const { return type() == kind::bytes; }
    bool is_array()    const { return type() == kind::array; }
    bool is_object()   const { return type() == kind::object; }
    bool is_instance() const { return type() == kind::instance; }

    bool        as_boolean() const;
    int64_t     as_integer() const;
    double      as_real()    const;
    // Host convenience: a COPY of the text payload. The storage itself
    // is char bytes (SSO/cell), never a held std::string.
    std::string as_string()  const;

    const std::vector<value>           &as_array()  const;
    const std::map<std::string, value> &as_object() const;

    std::vector<value>           &array();
    std::map<std::string, value> &object();

    bool operator==(const value &other) const;
    bool operator!=(const value &other) const { return !(*this == other); }

    static const char *kind_name(kind k);

    // The raw 32-byte interchange struct (C-API bridges; the array and
    // object container kinds have no raw payload yet).
    const madc_value &raw() const { return _v; }

private:
    madc_value                                    _v;
    std::unique_ptr<std::vector<value>>           _array;
    std::unique_ptr<std::map<std::string, value>> _object;
};

} // namespace madc

#endif // __LIBMADC_VALUE_H
