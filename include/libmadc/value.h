#ifndef __LIBMADC_VALUE_H
#define __LIBMADC_VALUE_H 1

// madc::value — public host-facing tagged value type for the libmadc
// embedding API. Carries the eight value kinds the host and a script
// can exchange: null, boolean, integer, real, string, bytes, array,
// and object.
//
// Vivification: the mutable accessors `object()` and `array()` convert
// a kind::null value into an empty object / empty array respectively
// (then return the live container), instead of throwing. A
// default-constructed value therefore works with both keyed and indexed
// helpers — the unified script array starts life null. Any other kind
// mismatch still throws. The const `as_object()` / `as_array()`
// accessors never vivify.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
	object
    };

    value();
    value(std::nullptr_t);
    explicit value(bool b);
    explicit value(int64_t i);
    explicit value(double d);
    value(const char *s);
    value(const std::string &s);
    value(std::string &&s);

    static value make_null();
    static value make_bytes(std::vector<uint8_t> bytes);
    static value make_array();
    static value make_array(std::vector<value> elements);
    static value make_object();
    static value make_object(std::map<std::string, value> fields);

    value(const value &other);
    value(value &&other) noexcept;
    value &operator=(const value &other);
    value &operator=(value &&other) noexcept;
    ~value();

    kind type() const { return _kind; }
    bool is_null()    const { return _kind == kind::null; }
    bool is_boolean() const { return _kind == kind::boolean; }
    bool is_integer() const { return _kind == kind::integer; }
    bool is_real()    const { return _kind == kind::real; }
    bool is_string() const { return _kind == kind::string; }
    bool is_bytes()  const { return _kind == kind::bytes; }
    bool is_array()  const { return _kind == kind::array; }
    bool is_object() const { return _kind == kind::object; }

    bool                                      as_boolean() const;
    int64_t                                   as_integer() const;
    double                                    as_real()    const;
    const std::string                        &as_string()  const;
    const std::vector<uint8_t>               &as_bytes()   const;
    const std::vector<value>                 &as_array()   const;
    const std::map<std::string, value>       &as_object()  const;

    std::vector<value>                       &array();
    std::map<std::string, value>             &object();

    bool operator==(const value &other) const;
    bool operator!=(const value &other) const { return !(*this == other); }

    static const char *kind_name(kind k);

private:
    kind                                          _kind;
    bool                                          _bool;
    int64_t                                       _int;
    double                                        _real;
    std::string                                   _string;
    std::vector<uint8_t>                          _bytes;
    std::unique_ptr<std::vector<value>>           _array;
    std::unique_ptr<std::map<std::string, value>> _object;
};

} // namespace madc

#endif // __LIBMADC_VALUE_H
