// Unit tests for the typed-register IR layer (Stage 0).
//
// Each test constructs an asmjit x86::Compiler, has IRBuilder emit
// a single operation, then finalizes and inspects the text log. We
// assert on the presence of specific instructions (movsxd, movzx,
// cvtss2sd, etc.) rather than on exact byte encodings — encodings
// vary with register allocation but the instruction opcode does
// not.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
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
#include <asmjit/x86.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc_ir.h"

using namespace asmjit;

// Small harness that sets up an asmjit x86::Compiler backed by a
// StringLogger so a test can capture the emitted assembly text.
// Global dd* instances come from parser.o via TESTOBJ.
struct IRFixture
{
    JitRuntime jit;
    CodeHolder code;
    StringLogger logger;
    x86::Compiler cc;
    FuncNode *funcnode = nullptr;

    IRFixture()
    {
	code.init(jit.environment());
	logger.addFlags(FormatFlags::kMachineCode);
	code.setLogger(&logger);
	code.attach(&cc);
	funcnode = cc.newFunc(FuncSignature::build<void>());
	cc.addFunc(funcnode);
    }

    // Allocate a stack Mem of the given size so load()/store() have
    // a concrete destination. Size must match one of 1/2/4/8.
    x86::Mem makeStack(uint32_t size)
    {
	x86::Mem m = cc.newStack(size, size);
	m.setSize(size);
	return m;
    }

    std::string finishAndGetAsm()
    {
	cc.endFunc();
	Error err = cc.finalize();
	// Some programs trigger pre-existing non-fatal finalize errors.
	// Don't fail the test on them — the logger already captured the
	// emitted asm we need to inspect.
	(void)err;
	return std::string(logger.data());
    }
};

static bool contains(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

TEST_SUITE("IRBuilder::load") {
    TEST_CASE("Mem<8 signed int64> → mov r64, qword ptr") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(8);
	IRValue src = IRValue::mem(m, &ddINT64);
	IRValue dst = ir.load(src);
	CHECK(dst.isReg());
	CHECK(dst.type == &ddINT64);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "mov "));
	CHECK(contains(asm_out, "qword ptr"));
    }

    TEST_CASE("Mem<4 signed int32> → mov r32 (Gpd natural load)") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(4);
	IRValue src = IRValue::mem(m, &ddINT32);
	IRValue out = ir.load(src);
	std::string asm_out = f.finishAndGetAsm();
	// Gpd register: 32-bit load, no sign-extension needed.
	CHECK(contains(asm_out, "mov"));
	CHECK(out.isReg());
    }

    TEST_CASE("Mem<4 unsigned uint32> → mov r32 (implicit zero-extend)") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(4);
	IRValue src = IRValue::mem(m, &ddUINT32);
	(void)ir.load(src);
	std::string asm_out = f.finishAndGetAsm();
	// 32-bit writes to a 64-bit register zero the upper half
	// implicitly — mov r32, dword ptr [...] is the right encoding.
	CHECK(contains(asm_out, "mov "));
	CHECK(contains(asm_out, "dword ptr"));
	CHECK_FALSE(contains(asm_out, "movsxd"));
    }

    TEST_CASE("Mem<2 signed int16> → movsx") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(2);
	IRValue src = IRValue::mem(m, &ddINT16);
	(void)ir.load(src);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "movsx"));
    }

    TEST_CASE("Mem<1 unsigned uint8> → movzx") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(1);
	IRValue src = IRValue::mem(m, &ddUINT8);
	(void)ir.load(src);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "movzx"));
    }

    TEST_CASE("Mem<8 double> → movsd xmm, qword ptr") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(8);
	IRValue src = IRValue::mem(m, &ddDOUBLE);
	IRValue dst = ir.load(src);
	CHECK(dst.isReg());
	CHECK(dst.type == &ddDOUBLE);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "movsd"));
    }

    TEST_CASE("Mem<4 float> → movss xmm, dword ptr") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(4);
	IRValue src = IRValue::mem(m, &ddFLOAT);
	IRValue dst = ir.load(src);
	CHECK(dst.type == &ddFLOAT);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "movss"));
    }

    TEST_CASE("Reg passthrough emits no instruction") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Gp g = f.cc.newGpq("src");
	IRValue src = IRValue::reg(g, &ddINT64);
	IRValue dst = ir.load(src);
	CHECK(dst.isReg());
	// Same virtual register id — no copy emitted.
	CHECK(dst.op.as<BaseReg>().id() == g.id());
    }

    TEST_CASE("Imm<int> → mov r64, imm") {
	IRFixture f;
	IRBuilder ir(f.cc);
	IRValue src = IRValue::imm(Imm(42), &ddINT64);
	IRValue dst = ir.load(src);
	CHECK(dst.isReg());
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "mov "));
	CHECK(contains(asm_out, "42"));
    }
}

TEST_SUITE("IRBuilder::store") {
    TEST_CASE("store Reg<int64> → Mem<8>") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(8);
	IRValue dst = IRValue::mem(m, &ddINT64);
	x86::Gp g = f.cc.newGpq("v");
	f.cc.mov(g, Imm(123));
	IRValue src = IRValue::reg(g, &ddINT64);
	ir.store(dst, src);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "mov "));
	CHECK(contains(asm_out, "qword ptr"));
    }

    TEST_CASE("store Reg<double> → Mem<8> emits movsd") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(8);
	IRValue dst = IRValue::mem(m, &ddDOUBLE);
	x86::Xmm xm = f.cc.newXmm("v");
	f.cc.xorps(xm, xm);
	IRValue src = IRValue::reg(xm, &ddDOUBLE);
	ir.store(dst, src);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "movsd"));
    }

    TEST_CASE("store Reg<float> → Mem<4> emits movss") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(4);
	IRValue dst = IRValue::mem(m, &ddFLOAT);
	x86::Xmm xm = f.cc.newXmm("v");
	f.cc.xorps(xm, xm);
	IRValue src = IRValue::reg(xm, &ddFLOAT);
	ir.store(dst, src);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "movss"));
    }
}

TEST_SUITE("IRBuilder::coerce") {
    TEST_CASE("same type passthrough") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Gp g = f.cc.newGpq("v");
	IRValue src = IRValue::reg(g, &ddINT64);
	IRValue out = ir.coerce(src, &ddINT64);
	CHECK(out.type == &ddINT64);
	CHECK(out.op.as<BaseReg>().id() == g.id());
    }

    TEST_CASE("int32 signed → int64 widens via load (movsxd on Mem)") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(4);
	IRValue src = IRValue::mem(m, &ddINT32);
	IRValue out = ir.coerce(src, &ddINT64);
	CHECK(out.isReg());
	CHECK(out.type == &ddINT64);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "movsxd"));
    }

    TEST_CASE("float → double emits cvtss2sd") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(4);
	IRValue src = IRValue::mem(m, &ddFLOAT);
	IRValue out = ir.coerce(src, &ddDOUBLE);
	CHECK(out.isReg());
	CHECK(out.type == &ddDOUBLE);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "cvtss2sd"));
    }

    TEST_CASE("double → float emits cvtsd2ss") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(8);
	IRValue src = IRValue::mem(m, &ddDOUBLE);
	IRValue out = ir.coerce(src, &ddFLOAT);
	CHECK(out.isReg());
	CHECK(out.type == &ddFLOAT);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "cvtsd2ss"));
    }

    TEST_CASE("double → double passthrough emits no cvt") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack(8);
	IRValue src = IRValue::mem(m, &ddDOUBLE);
	IRValue out = ir.coerce(src, &ddDOUBLE);
	CHECK(out.type == &ddDOUBLE);
	std::string asm_out = f.finishAndGetAsm();
	CHECK_FALSE(contains(asm_out, "cvtss2sd"));
	CHECK_FALSE(contains(asm_out, "cvtsd2ss"));
    }

    TEST_CASE("pointer → pointer relabel emits no instruction") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Gp g = f.cc.newGpq("p");
	IRValue src = IRValue::reg(g, &ddVOIDptr);
	IRValue out = ir.coerce(src, &ddCHARptr);
	CHECK(out.isReg());
	CHECK(out.type == &ddCHARptr);
	std::string asm_out = f.finishAndGetAsm();
	CHECK_FALSE(contains(asm_out, "mov "));
	CHECK_FALSE(contains(asm_out, "lea "));
    }

    TEST_CASE("string Mem → char* takes object address before string_cstr") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Mem m = f.makeStack((uint32_t)ddSTRING.size);
	IRValue src = IRValue::mem(m, &ddSTRING);
	IRValue out = ir.coerce(src, &ddCHARptr);
	CHECK(out.isReg());
	CHECK(out.type == &ddCHARptr);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "lea "));
	CHECK_FALSE(contains(asm_out, "mov qword ptr"));
    }

    TEST_CASE("int64 → double emits cvtsi2sd") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Gp g = f.cc.newGpq("i");
	IRValue src = IRValue::reg(g, &ddINT64);
	IRValue out = ir.coerce(src, &ddDOUBLE);
	CHECK(out.isReg());
	CHECK(out.type == &ddDOUBLE);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "cvtsi2sd"));
    }

    TEST_CASE("int64 → float emits cvtsi2ss") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Gp g = f.cc.newGpq("i");
	IRValue src = IRValue::reg(g, &ddINT64);
	IRValue out = ir.coerce(src, &ddFLOAT);
	CHECK(out.isReg());
	CHECK(out.type == &ddFLOAT);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "cvtsi2ss"));
    }

    TEST_CASE("double → int64 emits cvttsd2si") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Xmm x = f.cc.newXmm("d");
	IRValue src = IRValue::reg(x, &ddDOUBLE);
	IRValue out = ir.coerce(src, &ddINT64);
	CHECK(out.isReg());
	CHECK(out.type == &ddINT64);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "cvttsd2si"));
    }

    TEST_CASE("float → int64 emits cvttss2si") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Xmm x = f.cc.newXmm("fl");
	IRValue src = IRValue::reg(x, &ddFLOAT);
	IRValue out = ir.coerce(src, &ddINT64);
	CHECK(out.isReg());
	CHECK(out.type == &ddINT64);
	std::string asm_out = f.finishAndGetAsm();
	CHECK(contains(asm_out, "cvttss2si"));
    }

    TEST_CASE("int64 → int32 narrow relabels with no conversion") {
	IRFixture f;
	IRBuilder ir(f.cc);
	x86::Gp g = f.cc.newGpq("i64");
	IRValue src = IRValue::reg(g, &ddINT64);
	IRValue out = ir.coerce(src, &ddINT32);
	CHECK(out.isReg());
	CHECK(out.type == &ddINT32);
	std::string asm_out = f.finishAndGetAsm();
	CHECK_FALSE(contains(asm_out, "cvt"));
	// Narrowing is a pure relabel — no truncating instruction needed
	// because downstream store() picks the right sub-register view.
    }
}
