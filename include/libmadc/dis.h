#ifndef __LIBMADC_DIS_H
#define __LIBMADC_DIS_H 1

// libmadc/dis.h — the PUBLIC madc::dis data-substrate surface.
//
// The one curated entry point a C++ host embedding libmadc includes to reach
// the position-independent serialization primitives madc itself is built on
// (docs/plans/madcdis-plan.md Principle 1: the substrate ships in core libmadc
// and is available to every madc program; docs/plans/2026-07-06-madcdis-export-
// surface.md: expose what is already general). Include this, not the individual
// madcdis/*.h headers — those are the internal file layout and may move; the
// names re-exported here are the stable public subset.
//
// The surface (all in namespace madc::dis):
//   pod_record.h   pod_words<T>() / pod_append(buf,rec) / pod_read(buf,off,out)
//                  — fixed-stride POD (de)serialization over a uint32 buffer.
//   intern_table.h intern_table / frozen_intern_table — index-linked string
//                  interner; the id IS the entry index; three zero-fixup blocks.
//   id_table.h     id_table<T> — segmented stable uint32-id <-> object registry
//                  (add/get/base/size); ids survive a save/load round trip.
//   value_pool.h   value_pool — deduping uint32 handles over wide-literal limbs
//                  (values wider than 64 bits); a handle compare IS a value compare.
//   snapshot.h     snapshot_writer / snapshot_reader — the container: header /
//                  16-aligned compressed segment frames / directory / footer;
//                  a standalone file OR appended to a binary.
//
// Everything here is DataDef-agnostic and free of compiler/parser knowledge —
// a general substrate, not madc-internal machinery. Templates (id_table<T>,
// pod_*<T>) are header-only and instantiate in the including translation unit;
// value_pool / intern_table are header-only inline; snapshot_writer/_reader are
// declared here and defined in libmadc (src/madcdis_snapshot.cpp), so a host
// links libmadc to use them.

#include "madcdis/pod_record.h"
#include "madcdis/intern_table.h"
#include "madcdis/id_table.h"
#include "madcdis/value_pool.h"
#include "madcdis/snapshot.h"

#endif
