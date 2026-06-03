// Unit tests for DataDefCLASS::compute_layout() — Itanium MI/virtual layout.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#include <stdint.h>
#include <sys/stat.h>
#include <syslog.h>
#include <stdio.h>
#include <unistd.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
// Global dd* instances come from parser.o via TESTOBJ.

// Build a polymorphic-or-not class with `ndata` 8-byte (ddINT64) own members.
// ddINT64 is the 8-byte primitive global (there is no ddLONG); declared extern
// in datadef.h, defined in parser.cpp, linked via TESTOBJ.
static DataDefCLASS *mkclass(const char *name, int ndata, bool poly) {
    DataDefCLASS *c = new DataDefCLASS(name, 0, DataType::dtRESERVED);
    c->has_vtable = poly;
    for (int i = 0; i < ndata; i++) {
        char m[8]; m[0]='m'; m[1]=char('0'+i); m[2]=0;
        c->addMember(m, ddINT64, 1);   // 8-byte member; addMember sets offset+size+max_align
    }
    return c;
}

TEST_SUITE("class layout — data structures") {
    TEST_CASE("bases vector starts empty; BaseSpec records virtuality") {
        DataDefCLASS *a = mkclass("A", 1, false);
        CHECK(a->bases.empty());
        DataDefCLASS *b = mkclass("B", 1, false);
        b->bases.push_back(BaseSpec{a, 0, false, 0u, false});
        CHECK(b->bases.size() == 1);
        CHECK(b->bases[0].base == a);
        CHECK(b->bases[0].is_virtual == false);
    }
}

TEST_SUITE("class layout — single non-virtual base") {
    TEST_CASE("B : A  -> A@0, size 16") {
        DataDefCLASS *a = mkclass("A", 1, false);   // {long a} size 8
        a->compute_layout();
        CHECK(a->size == 8);
        CHECK(a->nvsize == 8);

        DataDefCLASS *b = mkclass("B", 1, false);   // own {long b}
        b->bases.push_back(BaseSpec{a, 0, false, 0u, false});
        b->compute_layout();
        CHECK(b->bases[0].offset == 0);
        CHECK(b->size == 16);
        CHECK(b->nvsize == 16);
        // own member b shifted to sit after base A (offset 8)
        CHECK(b->member_offsets[0] == 8);
    }
}

TEST_SUITE("class layout — polymorphic vptr") {
    TEST_CASE("polymorphic leaf reserves vptr at 0") {
        DataDefCLASS *v = mkclass("Vbase", 1, true); // {vptr; long v}
        v->compute_layout();
        CHECK(v->size == 16);
        CHECK(v->nvsize == 16);
        CHECK(v->member_offsets[0] == 8); // long v after vptr
    }
}
