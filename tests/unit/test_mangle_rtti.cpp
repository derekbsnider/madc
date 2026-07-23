// Unit tests for the RTTI symbol mangling helpers (S5b.1).
// Cross-checked against c++filt:
//   _ZTI1C  -> "typeinfo for C"
//   _ZTS1C  -> "typeinfo name for C"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
thread_local bool madc_verbose = false;
#define DBG(x) do { if (madc_verbose) { x; } } while (0)
#include "doctest.h"
#include "madc_mangle.h"

TEST_CASE("RTTI symbol mangling matches c++filt") {
    CHECK(itanium_typeinfo_sym("C")           == "_ZTI1C");
    CHECK(itanium_typeinfo_name_sym("C")      == "_ZTS1C");
    CHECK(itanium_typeinfo_name_string("C")   == "1C");
    CHECK(itanium_typeinfo_sym("Dia")         == "_ZTI3Dia");
    CHECK(itanium_typeinfo_name_string("Dia") == "3Dia");
    CHECK(itanium_typeinfo_sym("Widget")      == "_ZTI6Widget");
    CHECK(itanium_typeinfo_name_string("Widget") == "6Widget");
}
