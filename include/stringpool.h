#ifndef __STRINGPOOL_H
#define __STRINGPOOL_H 1

// stringpool.h — BACK-COMPAT SHIM.
//
// The intern-table implementation moved to its permanent home as the first
// madc::dis substrate primitive: include/madcdis/intern_table.h
// (madc::dis::intern_table / madc::dis::intern_keyed_map). See
// docs/plans/2026-06-29-madc-development-substrate-vision.md.
//
// These global aliases keep every existing `StringPool` / `InternKeyedMap` call
// site compiling unchanged — ONE implementation, two names, no parallel path.
// Migrate call sites to the madc::dis names over time, then retire this shim.

#include "madcdis/intern_table.h"

using StringPool = madc::dis::intern_table;
template<class V> using InternKeyedMap = madc::dis::intern_keyed_map<V>;

#endif
