# Ternary Operator — Reasoning

See `.claude/rules/ternary.md` for the rules themselves.

## Why the precedence-13 pop limit

The `?` operator itself sits at precedence 13 (right-associative). When
the parser sees `?`, any operators on the stack with precedence STRICTLY
less than 13 must be left in place — they apply to the whole ternary
expression, not to just the condition. In particular, `=` at
precedence 14 must remain:

```c
x = cond ? a : b;
// parses as: x = (cond ? a : b)
// NOT as:    (x = cond) ? a : b
```

Popping `=` would break assignment-of-ternary patterns that are
everywhere in idiomatic C.

## Why `:` stops expression parsing in non-bracketed context

Ternary true-branches can be arbitrarily complex expressions, but they
end at the `:`. Without the colon-stop, parsing `cond ? a+b : c` would
treat `: c` as a syntax error in the middle of arithmetic. With the
stop, `a+b` parses as the true-branch, and the outer ternary handler
consumes the `:` and parses `c` as the false-branch.

`:` in other contexts (case labels, range-for, namespace `::`) is
handled by the caller before `parseExpression` runs — the stop only
fires when no other handler claimed the token.

## Why a stack-slot merge, not a shared Gp

asmjit's register allocator cannot handle the same virtual register
being written on two divergent code paths. Its liveness analysis
assumes a virtual register has one defining point; two writes confuse
the analysis and produce wrong code or bail out of allocation entirely.

The fix is to give each branch its own fresh Gp and use a stack slot
as the merge point:

```cpp
// 1. Allocate stack slot
x86::Mem slot = pgm.cc.newStack(8, 8);

// 2. Compile condition into a fresh Gp (don't clobber regdp.first)
x86::Gp cond_gp = pgm.cc.newGpq("ter_cond");
regdefp_t crdp = {&cond_gp_op, nullptr, nullptr};
condition->compile(pgm, crdp);

// 3. Test + conditional jump
pgm.cc.test(cond_gp, cond_gp);
pgm.cc.je(L_false);

// 4. True branch — fresh Gp, store into slot
x86::Gp true_tmp = pgm.cc.newGpq("ter_true");
// ... compile true branch into true_tmp ...
pgm.cc.mov(slot, true_tmp);
pgm.cc.jmp(L_end);

// 5. False branch — fresh Gp, store into slot
pgm.cc.bind(L_false);
x86::Gp false_tmp = pgm.cc.newGpq("ter_false");
// ... compile false branch into false_tmp ...
pgm.cc.mov(slot, false_tmp);

// 6. Merge — load from slot into destination
pgm.cc.bind(L_end);
x86::Gp result = pgm.cc.newGpq("ter_result");
pgm.cc.mov(result, slot);
```

This is the same pattern used by any codegen that needs to merge two
branches — the compound-assignment operators use a simpler form of it
for single-Mem writes.

## Why clean `regdefp_t` for each sub-compilation

If the condition's compile writes its result into `regdp.first`, and
`regdp.first` happens to point to the caller's destination register,
the condition's comparison result would clobber the eventual ternary
result. The same applies to each branch.

Resetting `regdp.first = NULL` before each sub-compile forces the
child node to allocate its own destination, keeping the ternary's
merge logic the sole owner of the final output register.
