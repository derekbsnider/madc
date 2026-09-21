# `:=` block scoping — an unbraced substatement is a block scope

Status: **ruled and implemented 2026-09-19 (one slice); validation on the container.**

## The question (owner, 2026-09-19)

`testwal1.mad` (script mode) printed `x = 1` for
`if (cond) x := 1; else x := 2; println(x)`, while the same body inside a
written `main()` died in c2mir with "repeated declaration x". Either both
should work or both should fail.

## Why they differed

The `:=` arm (src/parser.cpp, `parseStatement`) declares the receiver in
`compounds.top()`. An unbraced `if`/`else` arm had NO scope of its own, so
both `:=` landed in the enclosing block. In script mode the statement lowers
to a bare assignment and `translate_block` hoists one `int x;` (Nim-like
enclosing-block semantics — worked by accident). Inside a function the
statement is a `TokenDecl`; `translate_block` hoists `int x;` for every block
variable whose `TokenDecl` is not a DIRECT statement of the block, and the
nested `TokenDecl` was ALSO emitted inline in the arm → three declarations of
`x` in one C scope.

## The ruling

`:=` scopes the Go / C++ way ([stmt.select]/1, [stmt.iter]/1): the innermost
block, and an unbraced substatement IS a block. Both files now fail identically
at compile time ("use of undeclared identifier 'x'"). A same-scope
redeclaration with `:=` is an error; inner-block shadowing is legal; every
multi-return receiver is new (no Go "one new variable" exception). Python's
walrus binds in the enclosing FUNCTION scope only because Python has no block
scopes — not a precedent for a C dialect.

## The layer

Deepest = the substatement parse, not the emitter and not the `:=` arm:
`Program::parse_substatement` (composed from `pushCompound` / `popCompound` /
`parseStatement`; null search: no `pushCompound()` site wrapped a
substatement — the range-for element scope and the for-init scope are
parse-time name scopes CIR never visits). Adopted by `TokenIF::parse` (both
arms, incl. the `if constexpr` live branch), `TokenFOR::parse` (traditional
and range-for bodies), `TokenWHILE::parse`, `TokenDO::parse`. The compound is
materialized ONLY when the arm declared something, so the emitted C of every
existing unbraced arm is byte-identical (emit-C corpus control vs the T11
corpus). The redeclaration check adopts `TokenCpnd::findVariableThisScope`.
`switch` with an unbraced labeled body is left as is (its labels machinery;
no declaration can sensibly live there).

## Acceptance

- `tests/testwalrusscope` + `testwalrusarmdecl` (expect_err; the parser reports
  ONE parse error per file, so each shape is its own test), `testwalrusredecl`
  (expect_err), `testwalrusarm` (total = 14, g++ AND clang++ on the `auto`
  twin), `testwalrusscriptscope` (expect_err), `testwalrusscriptlocal` (also
  covers the pre-existing defect found on the way: a BRACED arm at script file
  scope pushed a parentless compound and could not see a top-level `:=`
  local — fixed in the same owner; a bare `{ }` block as a top-level script
  statement still has that shape: KG Gap script_scope_bare_block_parent).
- Neighbours: testcolon, testmultiret*, testscripttopmultiret.
- Container fulltest green except the four pre-existing failures; emit-C
  corpus vs t11 differs only by the new tests.
- Docs: `docs/language/short-declaration.md`, overview row.
