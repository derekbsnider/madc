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

TEST_SUITE("class layout — single virtual base") {
    TEST_CASE("Mid : virtual Vbase -> Vbase@16, size 32, nvsize 16") {
        DataDefCLASS *v = mkclass("Vbase", 1, true);
        v->compute_layout();                 // size 16, nvsize 16
        DataDefCLASS *mid = mkclass("Mid", 1, true); // own {long m}
        mid->bases.push_back(BaseSpec{v, 0, /*virtual*/true, 0u, false});
        mid->compute_layout();
        CHECK(mid->nvsize == 16);            // vptr + m
        CHECK(mid->size == 32);              // + Vbase(16) at end
        CHECK(mid->vbase_offset[v] == 16);
    }
    TEST_CASE("Leaf : Mid (primary) -> Mid@0, Vbase@24, size 40, nvsize 24") {
        DataDefCLASS *v = mkclass("Vbase", 1, true); v->compute_layout();
        DataDefCLASS *mid = mkclass("Mid", 1, true);
        mid->bases.push_back(BaseSpec{v, 0, true, 0u, false}); mid->compute_layout();
        DataDefCLASS *leaf = mkclass("Leaf", 1, true); // own {long l}
        leaf->bases.push_back(BaseSpec{mid, 0, false, 0u, false}); // non-virtual primary
        leaf->compute_layout();
        CHECK(leaf->bases[0].is_primary == true);
        CHECK(leaf->bases[0].offset == 0);
        CHECK(leaf->nvsize == 24);          // shares Mid vptr@0, m@8, l@16
        CHECK(leaf->vbase_offset[v] == 24); // Vbase hoisted once to the end
        CHECK(leaf->size == 40);
    }
}

TEST_SUITE("class layout — MI non-virtual") {
    TEST_CASE("MIc : P1, P2 -> P1@0, P2@16, size 40") {
        DataDefCLASS *p1 = mkclass("P1", 1, true); p1->compute_layout(); // nvsize 16
        DataDefCLASS *p2 = mkclass("P2", 1, true); p2->compute_layout(); // nvsize 16
        DataDefCLASS *mic = mkclass("MIc", 1, true); // own {long c}
        mic->bases.push_back(BaseSpec{p1, 0, false, 0u, false});
        mic->bases.push_back(BaseSpec{p2, 0, false, 0u, false});
        mic->compute_layout();
        CHECK(mic->bases[0].is_primary == true);
        CHECK(mic->bases[0].offset == 0);
        CHECK(mic->bases[1].offset == 16);            // P2 with its own vptr
        CHECK(mic->secondary_vptr_owners.size() == 1);
        CHECK(mic->secondary_vptr_owners[0] == p2);
        CHECK(mic->member_offsets[0] == 32);          // c after both bases
        CHECK(mic->size == 40);
    }
}
