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
#include <iosfwd>
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
    // Adopt a raw interchange struct (scalar/text/bytes/instance kinds):
    // copies the struct and retains its payload cell.
    static value from_raw(const madc_value &raw);

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

    // Freeze: mark this value read-only (MADC_VF_CONST — the value-ABI
    // design's reserved read-only bit; first consumer: the immutability
    // primitive behind madc::sys facts and embedding hosts handing
    // read-only data into scripts). Mutation entry points — array(),
    // object(), instance_data(), copy-assignment ONTO this value, and the
    // C-API setters (value_accepts_kind) — reject writes loudly; reads
    // are unaffected. Slot-local, not viral: copies of a frozen value are
    // mutable. Move-assignment onto a frozen value cannot throw (noexcept
    // — vector<value> growth relies on it): it reports to stderr and
    // leaves the value unchanged.
    void freeze()          { _v.flags |= MADC_VF_CONST; }
    bool is_frozen() const { return (_v.flags & MADC_VF_CONST) != 0; }

    static const char *kind_name(kind k);

    // The raw 32-byte interchange struct (C-API bridges; the array and
    // object container kinds have no raw payload yet).
    const madc_value &raw() const { return _v; }

private:
    madc_value                                    _v;
    std::unique_ptr<std::vector<value>>           _array;
    std::unique_ptr<std::map<std::string, value>> _object;
};

// Stream a value EXACTLY as the contained type would stream (owner ruling,
// 2026-08-19): each kind forwards to the REAL ostream inserter, so stream
// state — std::hex, setprecision, boolalpha — applies the way it does to a
// plain int / double / bool. NOT the text renderer (value_to_string):
// that produces "4.000000" where `os << 4.0` prints "4". null streams
// nothing (PHP's echo null). string/bytes write the payload byte count
// exactly, so an embedded NUL neither truncates nor is scanned for.
//
// array / object / instance follow the containers' own convention — C++
// gives std::vector no operator<< — but the kind is only known at RUN time,
// so the refusal is the ns_common report convention (a stderr notice,
// nothing streamed), NOT a C++ throw: this one symbol is also the script
// binding (<ns_madc>, mangled-direct), and no C++ exception may cross into
// JIT frames (see report_frozen, src/ns_common.cpp).
std::ostream &operator<<(std::ostream &os, const value &v);
// The container-kind refusal notice (the report convention above): ONE text
// for both renderings of the inserter — the host body and the script-side
// rendering bits/value_stream compiles when the script's stdlib flavor is not
// the host's (it binds here mangled-direct: a madc type, flavor-neutral).
void value_not_streamable_notice(const value &v);

} // namespace madc

#endif // __LIBMADC_VALUE_H
