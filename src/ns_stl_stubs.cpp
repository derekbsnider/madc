// ns_stl_stubs.cpp — Linker stubs for removed ns_stl.cpp.
//
// ns_stl.cpp was a proof-of-concept with hardcoded template
// instantiations. It has been removed. These stubs exist only
// so the legacy JIT compiler code (compiler.cpp, parser.cpp,
// compiler_control_flow.cpp) links. They abort if called.
//
// Proper template instantiation is generated on demand by the
// transpiler (Cfront-style, per-translation-unit).

#include <cstdlib>
#include <cstdio>
#include <cstdint>

static void stl_stub_abort(const char *fn) {
    fprintf(stderr, "madc: %s() called — STL containers require --backend=mir\n", fn);
    abort();
}

extern "C" {

#define STUB_V(name) void name() { stl_stub_abort(#name); }
#define STUB_VP(name) void *name(void *p) { stl_stub_abort(#name); return nullptr; }
#define STUB_VD(name) void name(void *p) { stl_stub_abort(#name); }
#define STUB_I(name) int64_t name() { stl_stub_abort(#name); return 0; }

// vector
void *vector_int_construct(void *p) { stl_stub_abort("vector_int_construct"); return nullptr; }
void  vector_int_destruct(void *p) { stl_stub_abort("vector_int_destruct"); }
void  vector_int_push_back(void *p, int64_t v) { stl_stub_abort("vector_int_push_back"); }
void  vector_int_pop_back(void *p) { stl_stub_abort("vector_int_pop_back"); }
int64_t vector_int_at(void *p, int64_t i) { stl_stub_abort("vector_int_at"); return 0; }
int64_t vector_int_size(void *p) { stl_stub_abort("vector_int_size"); return 0; }
void  vector_int_clear(void *p) { stl_stub_abort("vector_int_clear"); }
int64_t vector_int_empty(void *p) { stl_stub_abort("vector_int_empty"); return 0; }
void  vector_int_set(void *p, int64_t i, int64_t v) { stl_stub_abort("vector_int_set"); }

void *vector_str_construct(void *p) { stl_stub_abort("vector_str_construct"); return nullptr; }
void  vector_str_destruct(void *p) { stl_stub_abort("vector_str_destruct"); }
void  vector_str_push_back(void *p, void *s) { stl_stub_abort("vector_str_push_back"); }
void  vector_str_pop_back(void *p) { stl_stub_abort("vector_str_pop_back"); }
void *vector_str_at(void *r, void *p, int64_t i) { stl_stub_abort("vector_str_at"); return nullptr; }
int64_t vector_str_size(void *p) { stl_stub_abort("vector_str_size"); return 0; }
void  vector_str_clear(void *p) { stl_stub_abort("vector_str_clear"); }
int64_t vector_str_empty(void *p) { stl_stub_abort("vector_str_empty"); return 0; }
void  vector_str_set(void *p, int64_t i, void *s) { stl_stub_abort("vector_str_set"); }

// map
void *map_str_int_construct(void *p) { stl_stub_abort("map_str_int_construct"); return nullptr; }
void  map_str_int_destruct(void *p) { stl_stub_abort("map_str_int_destruct"); }
void  map_str_int_set(void *p, void *k, int64_t v) { stl_stub_abort("map_str_int_set"); }
int64_t map_str_int_get(void *p, void *k) { stl_stub_abort("map_str_int_get"); return 0; }
int64_t map_str_int_contains(void *p, void *k) { stl_stub_abort("map_str_int_contains"); return 0; }
void  map_str_int_erase(void *p, void *k) { stl_stub_abort("map_str_int_erase"); }
int64_t map_str_int_size(void *p) { stl_stub_abort("map_str_int_size"); return 0; }
void  map_str_int_clear(void *p) { stl_stub_abort("map_str_int_clear"); }

void *map_str_str_construct(void *p) { stl_stub_abort("map_str_str_construct"); return nullptr; }
void  map_str_str_destruct(void *p) { stl_stub_abort("map_str_str_destruct"); }
void  map_str_str_set(void *p, void *k, void *v) { stl_stub_abort("map_str_str_set"); }
void *map_str_str_get(void *r, void *p, void *k) { stl_stub_abort("map_str_str_get"); return nullptr; }
int64_t map_str_str_contains(void *p, void *k) { stl_stub_abort("map_str_str_contains"); return 0; }
int64_t map_str_str_size(void *p) { stl_stub_abort("map_str_str_size"); return 0; }

// set
void *set_str_construct(void *p) { stl_stub_abort("set_str_construct"); return nullptr; }
void  set_str_destruct(void *p) { stl_stub_abort("set_str_destruct"); }
void  set_str_insert(void *p, void *s) { stl_stub_abort("set_str_insert"); }
int64_t set_str_contains(void *p, void *s) { stl_stub_abort("set_str_contains"); return 0; }
void  set_str_erase(void *p, void *s) { stl_stub_abort("set_str_erase"); }
int64_t set_str_size(void *p) { stl_stub_abort("set_str_size"); return 0; }
void  set_str_clear(void *p) { stl_stub_abort("set_str_clear"); }

void *set_int_construct(void *p) { stl_stub_abort("set_int_construct"); return nullptr; }
void  set_int_destruct(void *p) { stl_stub_abort("set_int_destruct"); }
void  set_int_insert(void *p, int64_t v) { stl_stub_abort("set_int_insert"); }
int64_t set_int_contains(void *p, int64_t v) { stl_stub_abort("set_int_contains"); return 0; }
int64_t set_int_size(void *p) { stl_stub_abort("set_int_size"); return 0; }

// list
void *list_int_construct(void *p) { stl_stub_abort("list_int_construct"); return nullptr; }
void  list_int_destruct(void *p) { stl_stub_abort("list_int_destruct"); }
void  list_int_push_back(void *p, int64_t v) { stl_stub_abort("list_int_push_back"); }
void  list_int_push_front(void *p, int64_t v) { stl_stub_abort("list_int_push_front"); }
int64_t list_int_size(void *p) { stl_stub_abort("list_int_size"); return 0; }
void  list_int_clear(void *p) { stl_stub_abort("list_int_clear"); }

void *list_str_construct(void *p) { stl_stub_abort("list_str_construct"); return nullptr; }
void  list_str_destruct(void *p) { stl_stub_abort("list_str_destruct"); }
void  list_str_push_back(void *p, void *s) { stl_stub_abort("list_str_push_back"); }
void  list_str_push_front(void *p, void *s) { stl_stub_abort("list_str_push_front"); }
int64_t list_str_size(void *p) { stl_stub_abort("list_str_size"); return 0; }

} // extern "C"
