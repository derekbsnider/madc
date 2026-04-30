// Typed-register IR layer sitting between AST-walking compile() methods
// and asmjit emission. Stage 0 — scaffolding only.
//
// Every value the IR knows about carries an (asmjit operand, DataDef,
// IRShape) triple. Shape tells the builder how to reach the value:
// Reg (in a virtual asmjit register), Mem (at a memory address), Imm
// (compile-time constant), or Addr (pointer to the value held in a
// Gp).
//
// The builder owns every decision about sign/zero-extension, float↔
// double conversion, and load/store size. Individual Token compile()
// methods feed shapes in, get shapes out, and never call asmjit's mov
// / movsxd / cvtss2sd / etc. directly once they are ported to the IR.
//
// Emit-as-you-build: each IRBuilder call produces asmjit instructions
// immediately. There is no deferred graph, no separate emitter pass.
// The IR exists to centralize shape/type decisions, not to enable
// optimization (DCE / CSE / folding come later if at all).
//
// See docs/rules/typed-register-ir.md for the full design rationale
// and docs/plans/typed-register-ir.md for the migration plan.

#ifndef __MADC_IR_H
#define __MADC_IR_H 1

#include <cstdint>
#include <asmjit/x86.h>

#include "datadef.h"

enum class IRShape : uint8_t
{
    Invalid = 0,
    Reg,    // Value lives in a virtual Gp (integer/pointer) or Xmm (real).
    Mem,    // Value lives at an x86::Mem location.
    Imm,    // Compile-time constant (asmjit::Imm).
    Addr,   // Pointer to the value, held in a Gp register (i.e. &value).
};

struct IRValue
{
    asmjit::Operand op;
    DataDef *type;
    IRShape shape;

    IRValue() : type(nullptr), shape(IRShape::Invalid) {}
    IRValue(const asmjit::Operand &o, DataDef *t, IRShape s)
        : op(o), type(t), shape(s) {}

    bool valid() const { return shape != IRShape::Invalid && type; }
    bool isReg()  const { return shape == IRShape::Reg;  }
    bool isMem()  const { return shape == IRShape::Mem;  }
    bool isImm()  const { return shape == IRShape::Imm;  }
    bool isAddr() const { return shape == IRShape::Addr; }

    // Factory helpers keep construction intent explicit at call sites.
    static IRValue reg(const asmjit::Operand &o, DataDef *t)
        { return IRValue(o, t, IRShape::Reg); }
    static IRValue mem(const asmjit::x86::Mem &m, DataDef *t)
        { return IRValue(m, t, IRShape::Mem); }
    static IRValue imm(const asmjit::Imm &i, DataDef *t)
        { return IRValue(i, t, IRShape::Imm); }
    static IRValue addr(const asmjit::x86::Gp &g, DataDef *pointed_to_type)
        { return IRValue(g, pointed_to_type, IRShape::Addr); }
};

class IRBuilder
{
public:
    explicit IRBuilder(asmjit::x86::Compiler &cc) : cc_(cc) {}

    // Normalize src to Reg shape so a caller can use it in arithmetic,
    // comparisons, or as a call argument. Emits a type-correct load
    // (mov / movsxd / movsx / movzx / movsd / movss) when src is Mem.
    // For Imm, materializes into a fresh Gp (integer) or Xmm (real).
    // For a Reg already carrying the requested type, returns src
    // unchanged — no instruction emitted.
    IRValue load(const IRValue &src);

    // Store src into dst. dst must be Mem or Addr; src can be any
    // shape. Coerces src.type to dst.type when they differ, choosing
    // the right widening / narrowing instruction. Emits the correctly-
    // sized store (mov / movsd / movss).
    void store(const IRValue &dst, const IRValue &src);

    // Produce a Reg holding src's value converted to `to` type.
    // Handles: same-type passthrough, integer widening (sign/zero),
    // real ↔ real size change (cvtss2sd / cvtsd2ss), and integer ↔
    // real conversion (cvtsi2sd / cvtsd2si family). Additional
    // coercions (dtSTRING → const char* via string_cstr, etc.) are
    // added per-stage as token ports require them.
    IRValue coerce(const IRValue &src, DataDef *to);

    // Accessor for ports that still need to reach asmjit directly
    // during migration. Will go away once every token is ported.
    asmjit::x86::Compiler &cc() { return cc_; }

private:
    asmjit::x86::Compiler &cc_;

    // Allocate a fresh Reg ValueRef of the given type. Picks Gp for
    // integer/pointer types and Xmm for real types.
    IRValue newReg(DataDef *type, const char *name_hint);
};

#endif
