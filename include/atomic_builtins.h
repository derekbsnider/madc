#ifndef __ATOMIC_BUILTINS_H
#define __ATOMIC_BUILTINS_H 1
/* atomic_builtins.h — the GCC `__atomic_*` builtin family, and the legacy
 * `__sync_*` family over it, as data.
 *
 * gcc and clang implement these builtins inline; they are not library symbols.
 * madc registers each name (parser.cpp, populate_builtin_registry), types each
 * call from its FORM (Program::apply_atomic_builtin_result_type) and lowers it
 * to a gcc-compiled runtime helper named `__madc` + the builtin's name minus
 * one leading underscore (CirBuilder::lower_atomic_builtin -> va_helpers.cpp).
 * This table is the one list of names and shapes all three read.
 *
 * A `__sync_*` row is gcc's "Legacy __sync Built-in Functions": the same
 * operation as an `__atomic_*` builtin at the memory order gcc implies
 * (SEQ_CST; ACQUIRE for lock_test_and_set, RELEASE for lock_release), which
 * the call does not spell. It reaches that builtin's helper (`helper_of`);
 * only the two compare-and-swap forms, whose expected value is an operand
 * rather than an address, have helpers of their own.
 *
 * Thread-safety contract: every helper is atomic with respect to every other
 * `__atomic_*` access to the same object, at the requested memory order or a
 * stronger one. Sizes 1, 2, 4 and 8 on a naturally aligned object use the
 * host's lock-free instructions; any other size or alignment takes one of a
 * fixed set of address-hashed spin locks (atomic state, no mutable globals).
 */
#include <string.h>

// The shape of a call: its operands and what it yields.
enum class AtomicForm : unsigned char {
	ValueRead,	// (T *p, int mo) -> T                        load_n
	ValueWrite,	// (T *p, T v, int mo) -> void                store_n
	ValueExchange,	// (T *p, T v, int mo) -> T                   exchange_n
	ValueArith,	// (T *p, T v, int mo) -> T, T not bool       fetch_OP, OP_fetch
	ValueCompare,	// (T *p, T *exp, T des, bool weak, int s, int f) -> bool
	ObjectRead,	// (T *p, T *ret, int mo) -> void             load
	ObjectWrite,	// (T *p, T *val, int mo) -> void             store
	ObjectExchange,	// (T *p, T *val, T *ret, int mo) -> void     exchange
	ObjectCompare,	// (T *p, T *exp, T *des, bool weak, int s, int f) -> bool
	FlagSet,	// (void *p, int mo) -> bool                  test_and_set
	FlagClear,	// (void *p, int mo) -> void                  clear
	LockFreeQuery,	// (size_t n, void *p) -> bool                is_lock_free
	LockFreeConstant, // (size_t n, void *p) -> bool, a constant  always_lock_free
	Fence,		// (int mo) -> void                           thread_fence, signal_fence
	SyncCompareValue, // (T *p, T old, T new, mo) -> T            __sync_val_compare_and_swap
	SyncCompareBool,  // (T *p, T old, T new, mo) -> bool         __sync_bool_compare_and_swap
	SyncRelease	// (T *p, mo) -> void, stores 0               __sync_lock_release
};

struct AtomicBuiltin {
	const char *name;
	AtomicForm form;
	// A __sync_ row: the memory order gcc implies, in place of the form's
	// order operand (0 on an __atomic_ row, whose orders are operands — no
	// __sync_ builtin implies RELAXED), and the __atomic_ builtin whose
	// helper it reaches (NULL: a helper named after this row).
	int implied_order;
	const char *helper_of;
};

inline const AtomicBuiltin *atomic_builtin_table(size_t *count)
{
	static const AtomicBuiltin table[] = {
		{ "__atomic_load_n",		  AtomicForm::ValueRead },
		{ "__atomic_store_n",		  AtomicForm::ValueWrite },
		{ "__atomic_exchange_n",	  AtomicForm::ValueExchange },
		{ "__atomic_compare_exchange_n",  AtomicForm::ValueCompare },
		{ "__atomic_fetch_add",		  AtomicForm::ValueArith },
		{ "__atomic_fetch_sub",		  AtomicForm::ValueArith },
		{ "__atomic_fetch_and",		  AtomicForm::ValueArith },
		{ "__atomic_fetch_or",		  AtomicForm::ValueArith },
		{ "__atomic_fetch_xor",		  AtomicForm::ValueArith },
		{ "__atomic_fetch_nand",	  AtomicForm::ValueArith },
		{ "__atomic_add_fetch",		  AtomicForm::ValueArith },
		{ "__atomic_sub_fetch",		  AtomicForm::ValueArith },
		{ "__atomic_and_fetch",		  AtomicForm::ValueArith },
		{ "__atomic_or_fetch",		  AtomicForm::ValueArith },
		{ "__atomic_xor_fetch",		  AtomicForm::ValueArith },
		{ "__atomic_nand_fetch",	  AtomicForm::ValueArith },
		{ "__atomic_load",		  AtomicForm::ObjectRead },
		{ "__atomic_store",		  AtomicForm::ObjectWrite },
		{ "__atomic_exchange",		  AtomicForm::ObjectExchange },
		{ "__atomic_compare_exchange",	  AtomicForm::ObjectCompare },
		{ "__atomic_test_and_set",	  AtomicForm::FlagSet },
		{ "__atomic_clear",		  AtomicForm::FlagClear },
		{ "__atomic_is_lock_free",	  AtomicForm::LockFreeQuery },
		{ "__atomic_always_lock_free",	  AtomicForm::LockFreeConstant },
		{ "__atomic_thread_fence",	  AtomicForm::Fence },
		{ "__atomic_signal_fence",	  AtomicForm::Fence },
		{ "__sync_fetch_and_add",  AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_fetch_add" },
		{ "__sync_fetch_and_sub",  AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_fetch_sub" },
		{ "__sync_fetch_and_or",   AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_fetch_or" },
		{ "__sync_fetch_and_and",  AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_fetch_and" },
		{ "__sync_fetch_and_xor",  AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_fetch_xor" },
		{ "__sync_fetch_and_nand", AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_fetch_nand" },
		{ "__sync_add_and_fetch",  AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_add_fetch" },
		{ "__sync_sub_and_fetch",  AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_sub_fetch" },
		{ "__sync_or_and_fetch",   AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_or_fetch" },
		{ "__sync_and_and_fetch",  AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_and_fetch" },
		{ "__sync_xor_and_fetch",  AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_xor_fetch" },
		{ "__sync_nand_and_fetch", AtomicForm::ValueArith, __ATOMIC_SEQ_CST, "__atomic_nand_fetch" },
		{ "__sync_bool_compare_and_swap", AtomicForm::SyncCompareBool, __ATOMIC_SEQ_CST, NULL },
		{ "__sync_val_compare_and_swap",  AtomicForm::SyncCompareValue, __ATOMIC_SEQ_CST, NULL },
		{ "__sync_synchronize",    AtomicForm::Fence, __ATOMIC_SEQ_CST, "__atomic_thread_fence" },
		{ "__sync_lock_test_and_set", AtomicForm::ValueExchange, __ATOMIC_ACQUIRE, "__atomic_exchange_n" },
		{ "__sync_lock_release",   AtomicForm::SyncRelease, __ATOMIC_RELEASE, "__atomic_store_n" },
	};
	*count = sizeof(table) / sizeof(table[0]);
	return table;
}

// The sizes madc accesses lock-free (on a naturally aligned object): the one
// set __atomic_always_lock_free, __atomic_is_lock_free and the runtime's
// choice between an instruction and a lock all read. The mask has bit n set
// for each size n, for the CIR's constant expression over a size operand.
inline bool atomic_lock_free_size(unsigned long long n)
{
	return n == 1 || n == 2 || n == 4 || n == 8;
}
static const unsigned atomic_lock_free_size_mask =
	(1u << 1) | (1u << 2) | (1u << 4) | (1u << 8);
static const unsigned atomic_lock_free_size_max = 8;

// The entry for a callee name, or NULL when it is not an `__atomic_` or a
// `__sync_` builtin.
inline const AtomicBuiltin *atomic_builtin_lookup(const char *name)
{
	if ( !name || (strncmp(name, "__atomic_", 9) != 0
		       && strncmp(name, "__sync_", 7) != 0) )
		return NULL;
	size_t count = 0;
	const AtomicBuiltin *table = atomic_builtin_table(&count);
	for ( size_t i = 0; i < count; ++i )
		if ( strcmp(table[i].name, name) == 0 )
			return &table[i];
	return NULL;
}

// The operand count the form takes.
inline unsigned atomic_form_arity(AtomicForm f)
{
	switch ( f )
	{
	case AtomicForm::ValueRead:	   return 2;
	case AtomicForm::ValueWrite:	   return 3;
	case AtomicForm::ValueExchange:	   return 3;
	case AtomicForm::ValueArith:	   return 3;
	case AtomicForm::ValueCompare:	   return 6;
	case AtomicForm::ObjectRead:	   return 3;
	case AtomicForm::ObjectWrite:	   return 3;
	case AtomicForm::ObjectExchange:   return 4;
	case AtomicForm::ObjectCompare:	   return 6;
	case AtomicForm::FlagSet:	   return 2;
	case AtomicForm::FlagClear:	   return 2;
	case AtomicForm::LockFreeQuery:	   return 2;
	case AtomicForm::LockFreeConstant: return 2;
	case AtomicForm::Fence:		   return 1;
	case AtomicForm::SyncCompareValue: return 4;
	case AtomicForm::SyncCompareBool:  return 4;
	case AtomicForm::SyncRelease:	   return 2;
	}
	return 0;
}

// The operand count a call spells: its form's, less the order a __sync_ row
// implies (every form's count includes its order operand).
inline unsigned atomic_builtin_arity(const AtomicBuiltin &ab)
{
	unsigned n = atomic_form_arity(ab.form);
	return ab.implied_order != 0 ? n - 1 : n;
}

// The runtime helper a call reaches: `__madc` + the builtin's name (its own,
// or the __atomic_ one a __sync_ row is) minus one leading underscore.
inline const char *atomic_builtin_helper_base(const AtomicBuiltin &ab)
{
	return (ab.helper_of ? ab.helper_of : ab.name) + 1;
}

// The call yields the object's own type (the pointee of operand 0).
inline bool atomic_form_yields_object(AtomicForm f)
{
	return f == AtomicForm::ValueRead || f == AtomicForm::ValueExchange
	    || f == AtomicForm::ValueArith || f == AtomicForm::SyncCompareValue;
}

// The object is passed by value: an integer or pointer of 1, 2, 4, 8 or 16
// bytes (gcc's sync_resolve_size), the value travelling as a scalar.
inline bool atomic_form_is_value(AtomicForm f)
{
	return f == AtomicForm::ValueRead || f == AtomicForm::ValueWrite
	    || f == AtomicForm::ValueExchange || f == AtomicForm::ValueArith
	    || f == AtomicForm::ValueCompare || f == AtomicForm::SyncCompareValue
	    || f == AtomicForm::SyncCompareBool || f == AtomicForm::SyncRelease;
}

// The object is passed by address, any size (gcc's get_atomic_generic_size).
inline bool atomic_form_is_object(AtomicForm f)
{
	return f == AtomicForm::ObjectRead || f == AtomicForm::ObjectWrite
	    || f == AtomicForm::ObjectExchange || f == AtomicForm::ObjectCompare;
}

// The call yields a truth value.
inline bool atomic_form_yields_bool(AtomicForm f)
{
	return f == AtomicForm::ValueCompare || f == AtomicForm::ObjectCompare
	    || f == AtomicForm::FlagSet || f == AtomicForm::LockFreeQuery
	    || f == AtomicForm::LockFreeConstant || f == AtomicForm::SyncCompareBool;
}

// The helper takes the object's size as a leading operand.
inline bool atomic_form_is_sized(AtomicForm f)
{
	return atomic_form_is_value(f) || atomic_form_is_object(f);
}

#endif
