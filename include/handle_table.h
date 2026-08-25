#ifndef __HANDLE_TABLE_H
#define __HANDLE_TABLE_H 1

#include <cstdint>
#include <cstddef>
#include <vector>

// handle_table<T> — THE slot+1 handle-registry rule, one owner (AST-1
// dupaudit: four hand-rolled copies — ui_sessions, ui_tuis, and the two
// parse-handle registries — implemented it identically):
//   handle = slot index + 1 (so 0 is always "no handle");
//   get() bounds-checks and returns null for a closed slot;
//   close() deletes the object and NULLS the slot — handles are never
//   reused within a run.
// Extra per-object teardown (e.g. a TUI target's close) stays at the
// consumer's call site, before close(). Thread contract: confinement —
// each consumer's accessor states whose thread owns the table.
// Gated by scripts/check-one-handle-table.sh (fulltest).
template <typename T>
class handle_table
{
    std::vector<T *> slots;
public:
    int64_t open(T *obj)
    {
	slots.push_back(obj);
	return (int64_t)slots.size();
    }
    T *get(int64_t handle) const
    {
	if ( handle < 1 || (size_t)handle > slots.size() )
	    return (T *)0;
	return slots[(size_t)handle - 1];
    }
    bool close(int64_t handle)
    {
	T *obj = get(handle);
	if ( !obj )
	    return false;
	delete obj;
	slots[(size_t)handle - 1] = (T *)0;
	return true;
    }
    size_t size() const { return slots.size(); }
};

#endif // __HANDLE_TABLE_H
