// Unit tests for datadef.h: DataType enum, varflag_t, DataDef type system

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <asmjit/x86.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

// Global instances are defined in parser.cpp (linked via TESTOBJ)

TEST_SUITE("DataType enum") {
    TEST_CASE("primitive types have expected values") {
        CHECK((int)DataType::dtVOID == 0);
        CHECK((int)DataType::dtBOOL == 1);
        CHECK(DataType::dtINT == DataType::dtINT64);
        CHECK(DataType::dtCHAR == DataType::dtINT8);
    }

    TEST_CASE("pointer variants are base + 10000") {
        CHECK((uint32_t)DataType::dtINTptr == (uint32_t)DataType::dtINT + 10000);
        CHECK((uint32_t)DataType::dtSTRINGptr == (uint32_t)DataType::dtSTRING + 10000);
    }

    TEST_CASE("reference variants are base + 20000") {
        CHECK((uint32_t)DataType::dtINTref == (uint32_t)DataType::dtINT + 20000);
        CHECK((uint32_t)DataType::dtSTRINGref == (uint32_t)DataType::dtSTRING + 20000);
    }

    TEST_CASE("rtPtr/rtDePtr macros are inverses") {
        DataType t = DataType::dtINT32;
        CHECK(rtDePtr(rtPtr(t)) == t);
    }

    TEST_CASE("rtRef/rtDeRef macros are inverses") {
        DataType t = DataType::dtINT32;
        CHECK(rtDeRef(rtRef(t)) == t);
    }
}

TEST_SUITE("varflag_t") {
    TEST_CASE("flags can be combined with OR") {
        uint16_t flags = vfLOCAL | vfSTACK | vfMODIFIED;
        CHECK((flags & vfLOCAL) != 0);
        CHECK((flags & vfSTACK) != 0);
        CHECK((flags & vfMODIFIED) != 0);
        CHECK((flags & vfREGISTER) == 0);
    }

    TEST_CASE("vfREGISTER flag can be set and tested") {
        uint16_t flags = vfLOCAL | vfREGISTER;
        CHECK((flags & vfREGISTER) != 0);
        CHECK((flags & vfMODIFIED) == 0);
    }

    TEST_CASE("vfMODIFIED can be cleared") {
        uint16_t flags = vfMODIFIED | vfLOCAL;
        flags &= ~vfMODIFIED;
        CHECK((flags & vfMODIFIED) == 0);
        CHECK((flags & vfLOCAL) != 0);
    }
}

TEST_SUITE("DataDef type queries") {
    TEST_CASE("ddINT is numeric and integer") {
        CHECK(ddINT.is_numeric());
        CHECK(ddINT.is_integer());
        CHECK(!ddINT.is_real());
        CHECK(!ddINT.is_string());
        CHECK(!ddINT.is_object());
    }

    TEST_CASE("ddDOUBLE is numeric and real but not integer") {
        CHECK(ddDOUBLE.is_numeric());
        CHECK(ddDOUBLE.is_real());
        CHECK(!ddDOUBLE.is_integer());
    }

    TEST_CASE("ddSTRING is string and object, not numeric") {
        CHECK(ddSTRING.is_string());
        CHECK(ddSTRING.is_object());
        CHECK(!ddSTRING.is_numeric());
        CHECK(!ddSTRING.is_integer());
    }

    TEST_CASE("unsigned types are detected correctly") {
        CHECK(ddUINT8.is_unsigned());
        CHECK(ddUINT32.is_unsigned());
        CHECK(!ddINT.is_unsigned());
        CHECK(!ddINT32.is_unsigned());
    }

    TEST_CASE("struct type has btStruct basetype") {
        CHECK(ddTESTSTRUCT.basetype() == BaseType::btStruct);
        CHECK(ddTESTSTRUCT.is_struct());
        CHECK(!ddTESTSTRUCT.is_numeric());
    }

    TEST_CASE("rawtype strips pointer and reference variants") {
        DataType ptr_int = rtPtr(DataType::dtINT);
        DataType ref_int = rtRef(DataType::dtINT);
        CHECK(DataDef::rawtype(ptr_int) == DataType::dtINT);
        CHECK(DataDef::rawtype(ref_int) == DataType::dtINT);
        CHECK(DataDef::rawtype(DataType::dtINT) == DataType::dtINT);
    }

    TEST_CASE("reftype detection") {
        CHECK(ddINT.reftype() == RefType::rtValue);
    }

    TEST_CASE("is_compatible: same type is compatible") {
        CHECK(ddINT.is_compatible(ddINT));
        CHECK(ddINT.is_compatible(ddINT32));  // both numeric
    }

    TEST_CASE("is_compatible: numeric types are compatible with each other") {
        CHECK(ddINT8.is_compatible(ddINT64));
        CHECK(ddUINT32.is_compatible(ddINT16));
    }
}

TEST_SUITE("DataDefSTRUCT") {
    TEST_CASE("teststruct has expected members and size") {
        CHECK(ddTESTSTRUCT.members.size() == 4);
        CHECK(ddTESTSTRUCT.members[0].first == "name");
        CHECK(ddTESTSTRUCT.members[1].first == "id");
        CHECK(ddTESTSTRUCT.members[2].first == "age");
        CHECK(ddTESTSTRUCT.members[3].first == "sex");
    }

    TEST_CASE("member offsets are sequential") {
        std::string name_s = "name", id_s = "id", age_s = "age";
        CHECK(ddTESTSTRUCT.m_offset(name_s) == 0);
        CHECK(ddTESTSTRUCT.m_offset(id_s) == (ssize_t)sizeof(std::string));
        CHECK(ddTESTSTRUCT.m_offset(age_s) == (ssize_t)(sizeof(std::string) + 8));
    }

    TEST_CASE("m_type returns correct DataDef pointer") {
        std::string id_s = "id";
        DataDef *t = ddTESTSTRUCT.m_type(id_s);
        CHECK(t != nullptr);
        CHECK(t->is_integer());
    }

    TEST_CASE("m_offset returns -1 for unknown member") {
        std::string unknown = "nonexistent";
        CHECK(ddTESTSTRUCT.m_offset(unknown) == -1);
    }
}

TEST_SUITE("Variable") {
    TEST_CASE("Variable set/get for integer types") {
        Variable v("test_var", ddINT32, 1);
        v.set(42);
        CHECK(v.get<int32_t>() == 42);
        v.set(-1);
        CHECK(v.get<int32_t>() == -1);
    }

    TEST_CASE("Variable inc/dec") {
        Variable v("counter", ddINT32, 1);
        v.set(10);
        v.inc();
        CHECK(v.get<int32_t>() == 11);
        v.dec();
        CHECK(v.get<int32_t>() == 10);
    }

    TEST_CASE("Variable modified flag") {
        Variable v("x", ddINT, 1);
        CHECK((v.flags & vfMODIFIED) == 0);
        v.modified();
        CHECK((v.flags & vfMODIFIED) != 0);
    }

    TEST_CASE("Variable cmp for integers") {
        Variable v("y", ddINT32, 1);
        v.set(99);
        CHECK(v.cmp(99));
        CHECK(!v.cmp(100));
    }
}

static std::string write_temp_mad_source(const char *tag, const char *source)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/%s_%ld.mad", tag, (long)getpid());
    FILE *fp = fopen(path, "w");
    if ( !fp )
	return "";
    fputs(source, fp);
    fclose(fp);
    return std::string(path);
}

TEST_SUITE("Program isolation") {
    TEST_CASE("separate Program instances do not leak typedefs or macros") {
	std::string good_path = write_temp_mad_source(
	    "madc_prog_good",
	    "#define ANSWER 42\n"
	    "typedef int myint;\n"
	    "int main() { myint x = ANSWER; return 0; }\n");
	REQUIRE(!good_path.empty());

	std::string bad_path = write_temp_mad_source(
	    "madc_prog_bad",
	    "int main() { myint x = ANSWER; return 0; }\n");
	REQUIRE(!bad_path.empty());

	Program good_prog;
	TokenProgram *good_tp = good_prog.tokenize(good_path.c_str());
	CHECK(good_tp != nullptr);
	REQUIRE(good_tp != nullptr);
	CHECK(good_prog.parse(good_tp));
	CHECK(good_prog.compile());

	Program bad_prog;
	TokenProgram *bad_tp = bad_prog.tokenize(bad_path.c_str());
	CHECK(bad_tp != nullptr);
	REQUIRE(bad_tp != nullptr);
	CHECK_FALSE(bad_prog.parse(bad_tp));

	unlink(good_path.c_str());
	unlink(bad_path.c_str());
    }

    TEST_CASE("separate Program instances can use different builtin registration policies") {
	std::string path = write_temp_mad_source(
	    "madc_prog_builtins",
	    "int main() { puti(42); return 0; }\n");
	REQUIRE(!path.empty());

	Program default_prog;
	TokenProgram *default_tp = default_prog.tokenize(path.c_str());
	CHECK(default_tp != nullptr);
	REQUIRE(default_tp != nullptr);
	CHECK(default_prog.parse(default_tp));

	Program restricted_prog;
	restricted_prog.registration_policy.enable_core_functions = false;
	TokenProgram *restricted_tp = restricted_prog.tokenize(path.c_str());
	CHECK(restricted_tp != nullptr);
	REQUIRE(restricted_tp != nullptr);
	CHECK_FALSE(restricted_prog.parse(restricted_tp));

	unlink(path.c_str());
    }
}
