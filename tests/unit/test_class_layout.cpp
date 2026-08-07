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
        b->member_origin.assign(b->members.size(), -1); // all own (no flatten in unit test)
        b->apply_member_layout();
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
        v->member_origin.assign(v->members.size(), -1);
        v->apply_member_layout();
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
        mic->member_origin.assign(mic->members.size(), -1);
        mic->apply_member_layout();
        CHECK(mic->bases[0].is_primary == true);
        CHECK(mic->bases[0].offset == 0);
        CHECK(mic->bases[1].offset == 16);            // P2 with its own vptr
        CHECK(mic->secondary_vptr_owners.size() == 1);
        CHECK(mic->secondary_vptr_owners[0].first == p2);
        CHECK(mic->secondary_vptr_owners[0].second == 16);
        CHECK(mic->member_offsets[0] == 32);          // c after both bases
        CHECK(mic->size == 40);
    }
}

TEST_SUITE("class layout — diamond") {
    TEST_CASE("Diamond : L,R (both : virtual Top) -> L@0,R@16,Top@40 shared, size 56") {
        DataDefCLASS *top = mkclass("Top", 1, true); top->compute_layout(); // nvsize 16
        DataDefCLASS *l = mkclass("L", 1, true);
        l->bases.push_back(BaseSpec{top, 0, true, 0u, false}); l->compute_layout(); // nvsize 16, size 32
        DataDefCLASS *r = mkclass("R", 1, true);
        r->bases.push_back(BaseSpec{top, 0, true, 0u, false}); r->compute_layout();
        DataDefCLASS *dia = mkclass("Diamond", 1, true); // own {long d}
        dia->bases.push_back(BaseSpec{l, 0, false, 0u, false});
        dia->bases.push_back(BaseSpec{r, 0, false, 0u, false});
        dia->compute_layout();
        dia->member_origin.assign(dia->members.size(), -1);
        dia->apply_member_layout();
        CHECK(dia->bases[0].offset == 0);
        CHECK(dia->bases[1].offset == 16);
        CHECK(dia->member_offsets[0] == 32);          // d
        CHECK(dia->vbase_offset[top] == 40);          // Top appears ONCE, at the end
        CHECK(dia->size == 56);
    }
}

TEST_SUITE("class layout — vtable groups") {
    TEST_CASE("MI: primary group + one secondary group") {
        DataDefCLASS *p1 = mkclass("P1", 1, true);  // 1 virtual slot
        p1->vtable_slots.push_back("f1"); p1->virtual_methods["f1"] = true;
        p1->compute_layout(); p1->build_vtable_groups();
        DataDefCLASS *p2 = mkclass("P2", 1, true);
        p2->vtable_slots.push_back("f2"); p2->virtual_methods["f2"] = true;
        p2->compute_layout(); p2->build_vtable_groups();
        DataDefCLASS *mic = mkclass("MIc", 1, true);
        mic->vtable_slots.push_back("f1"); mic->vtable_slots.push_back("f2");
        mic->bases.push_back(BaseSpec{p1,0,false,0u,false});
        mic->bases.push_back(BaseSpec{p2,0,false,0u,false});
        mic->compute_layout(); mic->build_vtable_groups();
        CHECK(mic->vtable_groups.size() == 2);          // primary(P1) + secondary(P2)
        CHECK(mic->vtable_groups[0].this_offset == 0);
        CHECK(mic->vtable_groups[1].this_offset == 16); // P2 subobject
        size_t g; int s;
        CHECK(mic->find_vslot("f2", g, s)); CHECK(g == 1); CHECK(s == 0);
        CHECK(mic->find_vslot("f1", g, s)); CHECK(g == 0);
    }

    TEST_CASE("S5a: vtable group address points include the 2-word RTTI prologue") {
        DataDefCLASS *s = mkclass("S5a_S", 0, /*poly=*/true);   // 1 virtual method -> 1 slot
        s->vtable_slots.push_back("f"); s->virtual_methods["f"] = true;
        s->compute_layout();
        s->build_vtable_groups();
        REQUIRE(s->vtable_groups.size() == 1);
        CHECK(s->vtable_groups[0].addr_point == 2);   // prologue = offset_to_top, &_ZTI

        // MI: each group's address point sits past its own 2-word prologue.
        DataDefCLASS *p1 = mkclass("S5a_P1", 1, true);
        p1->vtable_slots.push_back("a"); p1->virtual_methods["a"] = true;
        p1->compute_layout(); p1->build_vtable_groups();
        DataDefCLASS *p2 = mkclass("S5a_P2", 1, true);
        p2->vtable_slots.push_back("b"); p2->virtual_methods["b"] = true;
        p2->compute_layout(); p2->build_vtable_groups();
        DataDefCLASS *mi = mkclass("S5a_MI", 1, true);
        mi->vtable_slots.push_back("a"); mi->vtable_slots.push_back("b");
        mi->bases.push_back(BaseSpec{p1,0,false,0u,true});
        mi->bases.push_back(BaseSpec{p2,16,false,0u,false});
        mi->compute_layout(); mi->build_vtable_groups();
        REQUIRE(mi->vtable_groups.size() == 2);
        // primary group: [otop,&TI, a]  -> addr_point 2
        CHECK(mi->vtable_groups[0].addr_point == 2);
        // secondary group follows: prev = 2+1(slot) = 3, +2 prologue = 5
        CHECK(mi->vtable_groups[1].addr_point == 5);
    }

    TEST_CASE("S5b: type_info flavor selection") {
        DataDefCLASS *a = mkclass("S5b_A", 0, true);
        a->compute_layout();
        CHECK(a->typeinfo_flavor() == DataDefCLASS::TI_CLASS);     // no bases

        DataDefCLASS *b = mkclass("S5b_B", 0, true);
        b->compute_layout();

        DataDefCLASS *c = mkclass("S5b_C", 0, true);
        c->bases.push_back(BaseSpec{a, 0, false, 0u, true});
        c->compute_layout();
        CHECK(c->typeinfo_flavor() == DataDefCLASS::TI_SI);        // single public non-virtual base

        DataDefCLASS *d = mkclass("S5b_D", 0, true);
        d->bases.push_back(BaseSpec{a, 0, false, 0u, true});
        d->bases.push_back(BaseSpec{b, 16, false, 0u, false});
        d->compute_layout();
        CHECK(d->typeinfo_flavor() == DataDefCLASS::TI_VMI);       // multiple bases

        DataDefCLASS *e = mkclass("S5b_E", 0, true);
        e->bases.push_back(BaseSpec{a, 0, true, 0u, false});       // virtual base
        e->compute_layout();
        CHECK(e->typeinfo_flavor() == DataDefCLASS::TI_VMI);       // virtual base -> vmi

        // unique public non-virtual base predicate
        size_t off = 999;
        CHECK(c->is_unique_public_nonvirtual_base(a, &off));
        CHECK(off == 0);
        // transitive: F : C (: A) -> A is a unique public non-virtual base of F
        DataDefCLASS *f = mkclass("S5b_F", 0, true);
        f->bases.push_back(BaseSpec{c, 0, false, 0u, true});
        f->compute_layout();
        off = 999;
        CHECK(f->is_unique_public_nonvirtual_base(a, &off));
        CHECK(off == 0);
        // d derives from a AND b; a is unique, but a non-base is not found
        DataDefCLASS *g = mkclass("S5b_G", 0, false);
        CHECK_FALSE(d->is_unique_public_nonvirtual_base(g, &off));
    }

    TEST_CASE("dtor D1/D0 markers keep declaration order and group layout") {
        // mirror `class A { virtual void f(); virtual ~A(); virtual void g(); }`:
        // declaration order f, ~ (D1), ~$deleting (D0), g.
        DataDefCLASS *a = mkclass("A", 0, true);   // poly=true -> has_vtable
        a->vtable_slots.push_back("f");
        a->vtable_slots.push_back("~");
        a->vtable_slots.push_back("~$deleting");
        a->vtable_slots.push_back("g");
        a->compute_layout();
        a->build_vtable_groups();
        REQUIRE(a->vtable_groups.size() == 1);     // single class -> one (primary) group
        const auto &g0 = a->vtable_groups[0];
        REQUIRE(g0.slots.size() == 4);
        CHECK(g0.slots[0] == "f");
        CHECK(g0.slots[1] == "~");                 // D1 right after f
        CHECK(g0.slots[2] == "~$deleting");        // D0 right after D1
        CHECK(g0.slots[3] == "g");                 // g after the dtor pair
        CHECK(g0.addr_point == 2);                 // 2-slot Itanium prologue precedes slot 0
        size_t grp; int s1 = -1, s0 = -1;
        CHECK(a->find_vslot("~", grp, s1));
        CHECK(a->find_vslot("~$deleting", grp, s0));
        CHECK(s0 == s1 + 1);
    }
}
