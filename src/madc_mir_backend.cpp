// madc_mir_backend.cpp — Glue between the C emitter and MIR.
//
// Feeds generated C11 text to c2mir → MIR → machine code, then
// executes the result.
//
// Import resolution uses dlsym — all runtime helpers are either
// C-linkage builtins or extern "C" namespace wrappers.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <new>
#include <map>
#include <list>
#include <vector>
#include <deque>
#include <queue>
#include <stack>
#include <mutex>
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_dl.h"
#include "madc_modules.h"	// __madc_dl_member: madc_module_open (the import binder's opener)
#include "ns_common.h"
#include "rt/rt_except.h"	// __madc_throw_cstr (madarray_count raises script errors)
#include "rt/rt_task.h"	// __madc_task_join_all (root-scope join after jitted main)
#include "libmadc/sysinfo.h"

extern "C" {
#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"
}

// -----------------------------------------------------------------------
// madc runtime builtins — exported with C linkage so dlsym can find them.
// These wrap the C++ implementations in parser.cpp.
// -----------------------------------------------------------------------

extern "C" {

void *__madc_get_stdout(void) { return (void *)stdout; }
void *__madc_get_stdin(void)  { return (void *)stdin; }
void *__madc_get_stderr(void) { return (void *)stderr; }

// import (alias form): the runtime half of a dynamic-module member call.
// The CIR builder lowers `ns::member(args)` for a namespace bound by
// `import name as ns;` to a per-member slot resolved on first call:
//   ((long (*)()) (slot ? slot : (slot = __madc_dl_member(LIB, MEMBER))))(args)
// — the same lowering in every lane (JIT, exe, obj, --emit=c11), replacing
// the JIT-only __dl_<ns>_<member> import thunks. LIB is the TARGET
// spelling the module map chose at compile time. Never returns NULL: a
// library or member that cannot be resolved raises a script exception.
//
// THREAD-SAFETY CONTRACT: the handle cache is guarded by its own mutex;
// concurrent first calls on one slot may both resolve (the loader
// refcounts; dlsym is idempotent) — the slot store is a benign race on a
// pointer-sized value, the same as any lazily-bound PLT.
void *__madc_dl_member(const char *lib, const char *member)
{
	static std::mutex cache_lock;
	static std::map<std::string, void *> handles;
	void *h = NULL;
	{
		std::lock_guard<std::mutex> g(cache_lock);
		std::map<std::string, void *>::iterator it = handles.find(lib);
		if (it != handles.end())
			h = it->second;
	}
	if (!h) {
		std::string err;
		h = madc_module_open(lib, err);
		if (!h) {
			std::string msg = std::string("import: cannot load '") + lib + "': " + err;
			__madc_throw_cstr(msg.c_str());
		}
		std::lock_guard<std::mutex> g(cache_lock);
		handles[lib] = h;
	}
	void *sym = madcdl_sym(h, member);
	if (!sym) {
		const char *e = madcdl_error();
		std::string msg = std::string("import: '") + member + "' is not exported by '" + lib
				  + "'" + (e ? std::string(": ") + e : std::string());
		__madc_throw_cstr(msg.c_str());
	}
	return sym;
}

void madc_puti(int64_t i)    { std::cout << i << std::endl; }
void madc_putu(uint64_t i)   { std::cout << i << std::endl; }
void madc_putd(double d)     { std::cout << d << std::endl; }
void madc_putf(float f)      { std::cout << f << std::endl; }
void madc_puts(const char *s) { if (s) puts(s); }
void madc_printstr(const char *s) { if (s) std::cout << s << std::endl; }

// madc array runtime — construct/destruct/size for transpiled array
// variables. The script `array` IS the public madc::value
// (include/libmadc/value.h, pulled in via datadef.h). A default-constructed
// value is kind::null (reads as empty; mutators vivify it to kind::array).
// The CIR builder lowers `array a;` to an _Alignas(alignof(madc::value))
// long[] buffer (CirBuilder::array_storage_decl passes the alignment, the
// 16 of the embedded madc_value) that placement-new constructs into.
void *madarray_construct(void *ptr)
    { return new(ptr) madc::value; }
void madarray_destruct(void *ptr)
    { ((madc::value *)ptr)->~value(); }
// Value-carrying placement construction into the same buffer — the ctor
// family add_array_methods registers on ddARRAY (`value(-7)` temporaries,
// `value v(7);` direct-init). Placement-new like madarray_construct: the
// storage is RAW here, so the assign family (whose operator= reads the OLD
// kind to release the previous payload) must not run. Returns the receiver.
void *madarray_construct_cstr(void *ptr, const char *s)
    { return new(ptr) madc::value(s ? s : ""); }
void *madarray_construct_int(void *ptr, int64_t i)
    { return new(ptr) madc::value(i); }
void *madarray_construct_real(void *ptr, double d)
    { return new(ptr) madc::value(d); }
void *madarray_construct_bool(void *ptr, int64_t b)
    { return new(ptr) madc::value(b != 0); }
void *madarray_construct_value(void *ptr, void *src)
    { return new(ptr) madc::value(*(const madc::value *)src); }
// Range-for length over a script array OR object bag. Arrays count their
// elements; the object kind counts its map entries (`for (value v : bag)`
// visits the VALUES in key order — php_array_get_value's object arm is the
// matching fill). Scalars still read 0. Intentionally NOT
// ns_common::value_count (that answers .count() with text-length semantics
// and a script exception for uncountable kinds).
// int64_t, never `long`: the CIR declares these thunk slots as long long
// (the i64 spelling law), and host `long` is 32-bit on win64 (LLP64).
int64_t madarray_size(void *ptr)
    {
	madc::value *v = (madc::value *)ptr;
	if ( v->is_array() )
	    return (int64_t)v->as_array().size();
	if ( v->is_object() )
	    return (int64_t)v->as_object().size();
	return 0;
    }

// The script .count()/.size() methods (add_array_methods binds both here).
// NOT madarray_size: that is the range-for BOUND above, where an object-kind
// carrier must read 0; a question the user asked gets the owner semantics
// (ns_common::value_length — containers count elements, text kinds count
// length) and a kind with no answer is a real madc exception, catchable as
// `catch (const char *)`, never a silent 0. The longjmp crosses only this
// frame, which holds no destructors. The message is a LITERAL: the throw
// stores the pointer and the handler reads it after the jump.
int64_t madarray_count(void *ptr)
    {
	madc::value *v = (madc::value *)ptr;
	bool ok = true;
	size_t n = ns_common::value_length(*v, &ok);
	if ( !ok )
	{
	    const char *msg = "count(): value of this kind is not countable";
	    switch ( v->type() )
	    {
	    case madc::value::kind::boolean:
		msg = "count(): boolean value is not countable"; break;
	    case madc::value::kind::integer:
		msg = "count(): integer value is not countable"; break;
	    case madc::value::kind::real:
		msg = "count(): real value is not countable"; break;
	    default: break;
	    }
	    __madc_throw_cstr(msg);
	}
	return (int64_t)n;
    }

// Strict kind accessors — the script .as_integer()/.as_boolean()/.as_real()
// methods and the .is_null() predicate (add_array_methods binds them here),
// mirroring madc::value's own accessor contract: the value's kind must BE
// the asked-for kind; a mismatch is a real madc exception (catchable as
// `catch (const char *)`), never a silent coercion. The carrier's C++
// throw is converted at this boundary — a C++ exception must not cross
// the MIR frame.
int64_t madarray_as_integer(void *ptr)
    {
	madc::value *v = (madc::value *)ptr;
	if ( !v->is_integer() )
	    __madc_throw_cstr("as_integer(): value kind is not integer");
	return v->as_integer();
    }
int64_t madarray_as_boolean(void *ptr)
    {
	madc::value *v = (madc::value *)ptr;
	if ( !v->is_boolean() )
	    __madc_throw_cstr("as_boolean(): value kind is not boolean");
	return v->as_boolean() ? 1 : 0;
    }
double madarray_as_real(void *ptr)
    {
	madc::value *v = (madc::value *)ptr;
	if ( !v->is_real() )
	    __madc_throw_cstr("as_real(): value kind is not real");
	return v->as_real();
    }
int64_t madarray_is_null(void *ptr)
    { return ((madc::value *)ptr)->is_null() ? 1 : 0; }
// The rest of madc::value's kind predicates (value.h contract) — a script
// walking mixed-shape data (a manifest's bare-string vs object TUs) must
// ask the kind BEFORE a keyed access, which is a script error on scalars.
int64_t madarray_is_string(void *ptr)
    { return ((madc::value *)ptr)->is_string() ? 1 : 0; }
int64_t madarray_is_object(void *ptr)
    { return ((madc::value *)ptr)->is_object() ? 1 : 0; }
int64_t madarray_is_array(void *ptr)
    { return ((madc::value *)ptr)->is_array() ? 1 : 0; }
int64_t madarray_is_boolean(void *ptr)
    { return ((madc::value *)ptr)->is_boolean() ? 1 : 0; }
int64_t madarray_is_integer(void *ptr)
    { return ((madc::value *)ptr)->is_integer() ? 1 : 0; }
int64_t madarray_is_real(void *ptr)
    { return ((madc::value *)ptr)->is_real() ? 1 : 0; }
int64_t madarray_is_bytes(void *ptr)
    { return ((madc::value *)ptr)->is_bytes() ? 1 : 0; }
int64_t madarray_is_instance(void *ptr)
    { return ((madc::value *)ptr)->is_instance() ? 1 : 0; }
// The payload bytes of a text kind (string / bytes): value.h's data(), no
// copy, with the length count() answers — what the stream inserter's
// script-side rendering (bits/value_stream) writes. Other kinds have none.
const char *madarray_data(void *ptr)
    {
	const madc::value *v = (const madc::value *)ptr;
	return v->is_string() || v->is_bytes() ? (const char *)v->data() : NULL;
    }

// Scalar (re)assignment surface for the intrinsic value/array carrier —
// the native operator= family add_array_methods registers on ddARRAY.
// Each retags the carrier in place through madc::value's own operator=
// (so freeze() rejection and payload-cell release ride the one owner).
// Returns the receiver: operator= yields *this.
void *madarray_assign_cstr(void *ptr, const char *s)
    { *(madc::value *)ptr = madc::value(s ? s : ""); return ptr; }
void *madarray_assign_int(void *ptr, int64_t i)
    { *(madc::value *)ptr = madc::value(i); return ptr; }
void *madarray_assign_real(void *ptr, double d)
    { *(madc::value *)ptr = madc::value(d); return ptr; }
void *madarray_assign_bool(void *ptr, int64_t b)
    { *(madc::value *)ptr = madc::value(b != 0); return ptr; }
void *madarray_assign_value(void *ptr, void *src)
    { *(madc::value *)ptr = *(const madc::value *)src; return ptr; }

// String-keyed SLOT on the object kind — the lvalue behind `bag["key"]`
// (and the parser's `bag.key` member spelling over the carrier). Vivifies
// a null carrier to an empty object, then returns the address of the
// (default-constructed-if-missing) map entry — Perl-model autovivification:
// ACCESS creates the slot, reads included; the non-mutating existence
// question is php::array_key_exists, never this. std::map nodes are
// stable, so the returned slot stays valid for the entry's lifetime.
// A scalar already in the carrier is a real script error (replacing it
// with an object would eat data — retag the carrier first), and freeze()
// rejection rides madc::value::object() (the one owner); both surface as
// catchable script errors, never a C++ throw across the MIR boundary.
void *madarray_key_slot(void *ptr, const char *key)
    {
	madc::value *v = (madc::value *)ptr;
	if ( !v->is_null() && !v->is_object() )
	    __madc_throw_cstr("[\"key\"]: value of this kind has no keyed members");
	try {
	    return &v->object()[key ? key : ""];
	} catch ( const std::exception & ) {
	    __madc_throw_cstr("[\"key\"]: value is frozen");
	}
	return NULL;	// unreachable: __madc_throw_cstr does not return
    }

// Integer-indexed SLOT on the array kind — the lvalue behind `arr[i]`,
// the keyed slot's index twin (owner law 2026-08-21: the carrier owns its
// element surface; the old string-first element model typed element reads
// as a std::string TEMP, so `arr[i] = x` was a silent no-op and element
// methods needed <string>). Vivifies a null carrier to an empty array and
// extends with null elements through idx (access creates, the keyed
// model's rule — `arr[3] = x` on a shorter array must land), then returns
// the element's address. vector storage: the slot stays valid until the
// array next GROWS (the C++ vector-iterator contract) — expression
// lifetime, exactly how the CIR consumes it. A negative index or a
// non-array kind is a catchable script error, never a crash or a temp.
void *madarray_index_slot(void *ptr, int64_t idx)
    {
	madc::value *v = (madc::value *)ptr;
	if ( idx < 0 )
	    __madc_throw_cstr("[index]: negative index on a madc array");
	if ( !v->is_null() && !v->is_array() )
	    __madc_throw_cstr("[index]: value of this kind has no indexed elements");
	try {
	    std::vector<madc::value> &vec = v->array();
	    if ( (size_t)idx >= vec.size() )
		vec.resize((size_t)idx + 1);
	    return &vec[(size_t)idx];
	} catch ( const std::exception & ) {
	    __madc_throw_cstr("[index]: value is frozen");
	}
	return NULL;	// unreachable: __madc_throw_cstr does not return
    }

// `bag[v]` with a CARRIER index — dispatch on the INDEX's live kind
// (owner 2026-08-31, the elegance ruling: `m[kn]` keys without .c_str()
// ceremony): a string-kind index KEYS the object kind (madarray_key_slot),
// integer/bool kinds INDEX the array kind, a real truncates (the numeric
// subscript rule). Deliberately NOT PHP's coercion table: "8" stays the
// string key "8", never index 8. A null or container-kind index refuses
// loudly — indexing by nothing (or by a container) is a bug, never an
// intent. Before this entry the compile-time index path coerced the
// carrier's POINTER to the element index (a silent address-as-index).
void *madarray_value_slot(void *ptr, void *idx)
    {
	const madc::value *iv = (const madc::value *)idx;
	if ( iv->is_string() )
	{
	    std::string key((const char *)iv->data(), iv->size());
	    return madarray_key_slot(ptr, key.c_str());
	}
	if ( iv->is_integer() || iv->is_boolean() || iv->is_real() )
	    return madarray_index_slot(ptr, iv->as_integer());
	__madc_throw_cstr(iv->is_null()
	    ? "[var]: null index — a key is a string, an index an integer"
	    : "[var]: index value must be a string (key) or a number (index)");
	return NULL;	// unreachable: __madc_throw_cstr does not return
    }

// Text view of a value — the carrier's c_str() and the coercion the CIR
// builder applies to a value in char*-consuming positions (varargs args,
// char* returns). EVERY kind answers with RING-lifetime text (the
// thread-local ring — the inet_ntoa model; a pointer stays valid until
// its slot recycles): value-first.md's pre-L3 text-return convention.
// String kind COPIES its payload into a slot — never the payload pointer
// itself: a payload borrow dies with the value, so `return a.c_str();`
// crossing a frame read freed memory (the silent-empty return gap; the
// value's cleanup dtor runs before any caller-side copy). Deliberate,
// documented divergence from std::string::c_str(). Other kinds render
// through the ONE value->text owner (ns_common::value_to_string);
// container kinds render a diagnostic tag, never crash. Several value
// args in one call keep distinct slots.
const char *madarray_cstr(void *ptr)
    {
	const madc::value *v = (const madc::value *)ptr;
	if (v->is_string())
	{
	    std::string &slot = ns_common::ring_slot();
	    slot.assign((const char *)v->data(), v->size());
	    return slot.c_str();
	}
	std::string &slot = ns_common::ring_slot();
	if (v->is_null())
	    slot.clear();
	else if (v->is_boolean())
	    slot = v->as_boolean() ? "true" : "false";
	else if (!ns_common::value_to_string(*v, slot))
	    slot = std::string("[") + madc::value::kind_name(v->type())
		 + ":" + std::to_string((long long)v->size()) + "]";
	return slot.c_str();
    }

// ---- string surface (value-first.md): a string-kind value is usable
// like a std::string. Equality is a QUESTION — a kind mismatch answers
// false, never a throw. Mutation (+=) and extraction (substr) keep the
// strict-kind contract with catchable script errors. substr returns
// ring-lifetime text (the c_str contract) until value-by-value returns
// (L3) land.
int64_t madarray_eq_cstr(void *ptr, const char *s)
    {
	const madc::value *v = (const madc::value *)ptr;
	if (!s || !v->is_string())
	    return 0;
	size_t len = v->size();
	return strlen(s) == len
	    && memcmp((const char *)v->data(), s, len) == 0;
    }
int64_t madarray_ne_cstr(void *ptr, const char *s)
    { return !madarray_eq_cstr(ptr, s); }
int64_t madarray_eq_value(void *ptr, void *other)
    { return *(const madc::value *)ptr == *(const madc::value *)other; }
int64_t madarray_ne_value(void *ptr, void *other)
    { return !(*(const madc::value *)ptr == *(const madc::value *)other); }

// First byte position of needle[0..n) in hay[0..len): memchr rides the
// libc vectorized scan, memcmp confirms. No allocations, portable (no
// memmem — the win64 lane's C runtime lacks it).
static int64_t text_index_of(const char *hay, size_t len,
			     const char *s, size_t n)
    {
	if (n == 0)
	    return 0;
	if (n > len)
	    return -1;
	const char *end = hay + len - n;
	for (const char *p = hay; p <= end; )
	{
	    const char *c = (const char *)memchr(p, s[0],
						 (size_t)(end - p) + 1);
	    if (!c)
		return -1;
	    if (memcmp(c, s, n) == 0)
		return (int64_t)(c - hay);
	    p = c + 1;
	}
	return -1;
    }

// index(needle): Python's list.index / str.find shape. An array-kind
// receiver answers the first position whose element equals the needle;
// a string-kind receiver answers the byte position of the needle
// substring; -1 when absent (a question, never a throw). One strlen,
// zero allocations.
int64_t madarray_index_cstr(void *ptr, const char *s)
    {
	const madc::value *v = (const madc::value *)ptr;
	if (!s)
	    return -1;
	size_t n = strlen(s);
	if (v->is_string())
	    return text_index_of((const char *)v->data(), v->size(), s, n);
	if (!v->is_array())
	    return -1;
	const std::vector<madc::value> &data = v->as_array();
	for (size_t i = 0; i < data.size(); ++i)
	    if (data[i].is_string() && data[i].size() == n
		&& memcmp(data[i].data(), s, n) == 0)
		return (int64_t)i;
	return -1;
    }
int64_t madarray_index_value(void *ptr, void *other)
    {
	const madc::value *v = (const madc::value *)ptr;
	const madc::value *o = (const madc::value *)other;
	if (v->is_string())
	{
	    if (!o->is_string())
		return -1;
	    return text_index_of((const char *)v->data(), v->size(),
				 (const char *)o->data(), o->size());
	}
	if (!v->is_array())
	    return -1;
	const std::vector<madc::value> &data = v->as_array();
	for (size_t i = 0; i < data.size(); ++i)
	    if (data[i] == *o)
		return (int64_t)i;
	return -1;
    }

// Append text onto a string-kind (or null — vivifies to string) value.
// One exact-reserved temp + one copy into the cell (NUL-transparent via
// the std::string ctor → madc_value_set_string_n; no strlen). The
// rebuilt payload rides madc::value's own assignment, so freeze
// rejection stays with the one owner. A true in-place cell append is a
// future carrier primitive if appends ever get hot.
static void madarray_append_text(madc::value *v, const char *s, size_t n)
    {
	if (!v->is_null() && !v->is_string())
	    __madc_throw_cstr("+=: value of this kind cannot append text");
	size_t old = v->is_string() ? v->size() : 0;
	if (n == 0 && old > 0)
	    return;
	std::string t;
	t.reserve(old + n);
	if (old)
	    t.append((const char *)v->data(), old);
	t.append(s, n);
	try {
	    *v = madc::value(t);
	} catch (const std::exception &) {
	    __madc_throw_cstr("+=: value is frozen");
	}
    }
void *madarray_append_cstr(void *ptr, const char *s)
    {
	madarray_append_text((madc::value *)ptr, s ? s : "",
			     s ? strlen(s) : 0);
	return ptr;
    }
void *madarray_append_value(void *ptr, void *other)
    {
	const madc::value *o = (const madc::value *)other;
	if (!o->is_string())
	    __madc_throw_cstr("+=: appended value is not string kind");
	madarray_append_text((madc::value *)ptr, (const char *)o->data(),
			     o->size());
	return ptr;
    }

// value.push(x) — append one ELEMENT (array-kind append; operator+= owns
// TEXT append). A null receiver vivifies to an empty array; any other
// non-array kind is a catchable script error, the carrier-method
// convention (at/substr/count). Returns the receiver so pushes chain —
// a script `v.push(x)` binds these entries, and the brace-list
// declaration lowering (`var v = { a, b, c };`) emits construct + one
// push per element through the same rows.
static std::vector<madc::value> &madarray_push_target(madc::value *v)
    {
	if (v->is_frozen())
	    __madc_throw_cstr("push(): value is frozen");
	if (!v->is_null() && !v->is_array())
	    __madc_throw_cstr("push(): value of this kind cannot take elements");
	return v->array();
    }
void *madarray_push_cstr(void *ptr, const char *s)
    {
	madarray_push_target((madc::value *)ptr)
	    .push_back(madc::value(std::string(s ? s : "")));
	return ptr;
    }
void *madarray_push_int(void *ptr, int64_t i)
    {
	madarray_push_target((madc::value *)ptr).push_back(madc::value(i));
	return ptr;
    }
void *madarray_push_real(void *ptr, double d)
    {
	madarray_push_target((madc::value *)ptr).push_back(madc::value(d));
	return ptr;
    }
void *madarray_push_bool(void *ptr, int64_t b)
    {
	madarray_push_target((madc::value *)ptr)
	    .push_back(madc::value(b != 0));
	return ptr;
    }
void *madarray_push_value(void *ptr, void *other)
    {
	madarray_push_target((madc::value *)ptr)
	    .push_back(*(const madc::value *)other);
	return ptr;
    }

// `arr[] = x` — PHP's empty append accessor (owner 2026-08-31): push one
// null element and return ITS slot address, so the parser's registered
// operator= rows land the RHS in the appended element — the index slot's
// append twin (one assignment vocabulary for keyed, indexed, and appended
// slots). Vivify/kind/freeze errors ride madarray_push_target. vector
// storage: the slot stays valid until the array next grows, exactly the
// index-slot contract the CIR consumes within one expression.
void *madarray_append_slot(void *ptr)
    {
	std::vector<madc::value> &vec =
	    madarray_push_target((madc::value *)ptr);
	vec.push_back(madc::value());
	return &vec.back();
    }

// `var v = {};` — an EMPTY brace list is an empty ARRAY, not a null
// value: the braces spell a container. The declaration lowering calls
// this once after construct; size() reads 0 and is_null() answers false.
void *madarray_make_array(void *ptr)
    {
	*(madc::value *)ptr = madc::value::make_array();
	return ptr;
    }

const char *madarray_substr(void *ptr, int64_t pos, int64_t len)
    {
	const madc::value *v = (const madc::value *)ptr;
	if (!v->is_string())
	    __madc_throw_cstr("substr(): value kind is not string");
	size_t sz = v->size();
	if (pos < 0 || (size_t)pos > sz)
	    __madc_throw_cstr("substr(): position out of range");
	size_t n = sz - (size_t)pos;
	if (len >= 0 && (size_t)len < n)
	    n = (size_t)len;
	std::string &slot = ns_common::ring_slot();
	slot.assign((const char *)v->data() + (size_t)pos, n);
	return slot.c_str();
    }

int64_t madarray_at(void *ptr, int64_t pos)
    {
	const madc::value *v = (const madc::value *)ptr;
	if (!v->is_string())
	    __madc_throw_cstr("at(): value kind is not string");
	if (pos < 0 || (size_t)pos >= v->size())
	    __madc_throw_cstr("at(): position out of range");
	return (int64_t)(unsigned char)((const char *)v->data())[pos];
    }

int64_t madarray_empty(void *ptr)
    {
	const madc::value *v = (const madc::value *)ptr;
	if (v->is_null())
	    return 1;
	bool ok = true;
	size_t n = ns_common::value_length(*v, &ok);
	if (!ok)
	    __madc_throw_cstr("empty(): value of this kind is not countable");
	return n == 0;
    }

// madc::sys population (task #91) — injected by the CIR builder in TUs
// that included <ns_madc>. Two entries over one population:
// __madc_sys_init is the explicit RUN entry (the JIT lane's main wrap) —
// unconditional, so a multi-session embedding host repopulates per run.
// __madc_sys_init_once is the LOAD-time entry (the top of a TU's
// .init_array init, ELF-completion S3) — it yields to any prior
// population: several TU inits may call it (same argc/argv, harmless),
// but a madc-built module dlopen'd MID-program is handed the main
// program's arguments by ld.so, and repopulating then would stomp the
// sys.path/sys.argv mutations the running script already made.
static bool madc_sys_populated = false;
void __madc_sys_init(int64_t argc, void *argv)
    {
	madc_sys_populated = true;
	madc::sys_populate_args((int)argc, (char **)argv);
    }
void __madc_sys_init_once(int64_t argc, void *argv)
    {
	if (!madc_sys_populated)
	    __madc_sys_init(argc, argv);
    }


} // extern "C"

// -----------------------------------------------------------------------
// Import resolver — dlsym finds C-linkage symbols directly.
// Namespace functions have extern "C" wrappers in ns_*.cpp.
// -----------------------------------------------------------------------

static void *madc_import_resolver(const char *name)
{
    void *addr = madcdl_sym_default(name);
    if (!addr)
	DBG(std::cerr << "madc_import_resolver: unresolved: "
		      << name << std::endl);
    return addr;
}

// -----------------------------------------------------------------------
// String reader for c2mir
// -----------------------------------------------------------------------

struct CStringReader {
    const char *data;
    size_t pos;
    size_t len;
};

static int c_string_getc(void *data)
{
    CStringReader *r = (CStringReader *)data;
    if (r->pos >= r->len) return EOF;
    return (unsigned char)r->data[r->pos++];
}

// -----------------------------------------------------------------------
// madc_mir_execute — compile C text via c2mir and execute main()
//
// Returns the exit code from main(), or -1 on compilation failure.
// -----------------------------------------------------------------------

int madc_mir_execute(const std::string &c_source, const std::string &source_name,
		     int user_argc, char **user_argv)
{
    MIR_context_t ctx = MIR_init();
    c2mir_init(ctx);
    MIR_gen_init(ctx);
    // Optimization level from the `-O<n>` flag (default 1). Level 1
    // (RA+combiner only) is the safe default: level >= 2 enables an addr-
    // elimination pass with an SSA-version bug when a pointer variable is
    // address-taken and then reassigned later. -O2/-O3 may hit it; -O0 is fine.
    MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);

    struct c2mir_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.message_file = stderr;
    opts.ignore_warnings_p = madc_no_warnings ? 1 : 0; /* -w */

    CStringReader reader = {c_source.c_str(), 0, c_source.size()};
    int ok = c2mir_compile(ctx, &opts, c_string_getc, &reader,
			   source_name.c_str(), nullptr);
    if (!ok) {
	fprintf(stderr, "madc_mir_execute: c2mir compilation failed\n");
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    MIR_module_t mod = nullptr;
    for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx));
	 m != nullptr; m = DLIST_NEXT(MIR_module_t, m))
	mod = m;

    if (!mod) {
	fprintf(stderr, "madc_mir_execute: no module produced\n");
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    MIR_load_module(ctx, mod);
    MIR_link(ctx, MIR_set_gen_interface, madc_import_resolver);

    void *code = nullptr;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, mod->items);
	 item != nullptr; item = DLIST_NEXT(MIR_item_t, item)) {
	if (item->item_type == MIR_func_item &&
	    strcmp(item->u.func->name, "main") == 0) {
	    code = MIR_gen(ctx, item);
	    break;
	}
    }

    if (!code) {
	fprintf(stderr, "madc_mir_execute: main() not found\n");
	MIR_gen_finish(ctx);
	c2mir_finish(ctx);
	MIR_finish(ctx);
	return -1;
    }

    int result = ((int (*)(int, char **))code)(user_argc, user_argv);

    // Root-scope join (the ruled Kotlin-scope semantic): main's return waits
    // for every live task. MUST run before MIR_gen_finish — task bodies are
    // jitted MIR code. Idempotent no-op when the program never spawned; the
    // atexit copy registered by the runtime then no-ops too (that copy is
    // for native artifacts, whose process ends with the program).
    __madc_task_join_all();

    MIR_gen_finish(ctx);
    c2mir_finish(ctx);
    MIR_finish(ctx);

    return result;
}
