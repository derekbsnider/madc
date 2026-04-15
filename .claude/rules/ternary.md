# Ternary Operator Compilation Rules

## Parsing

- When `?` is encountered in `parseExpression`, pop operators from opStack with precedence <= 13 only
- Do NOT pop `=` (precedence 14) or other low-precedence operators — they must remain on opStack
- Pop the condition from exStack after clearing higher-precedence operators
- Parse true expression with `conditional=true` — stops at `:` (colon-stop rule)
- Consume `:` via `nextToken()`, then parse false expression
- Push the `TokenTerQ` node onto exStack and set `done=true`

## Colon as Expression Stop

- `:` (`tkTerC`) stops expression parsing in non-bracketed context
- When encountered, push it back via `pushToken(tb)` and set `done=true`
- This is safe because `:` in other contexts (case labels, range-for) is handled before `parseExpression`

## Code Generation

- Use a **stack slot** as the merge point — do NOT write both branches to the same virtual register
- asmjit's register allocator cannot handle the same virtual register written on two divergent code paths
- Pattern:
  1. Allocate stack slot: `cc.newStack(8, 8)`
  2. Compile condition into a fresh Gp (don't clobber `regdp.first`)
  3. `test(cond_gp, cond_gp)` + `je(L_false)`
  4. True branch: compile into fresh Gp, `mov(slot, true_tmp)`
  5. `jmp(L_end)`
  6. False branch: compile into fresh Gp, `mov(slot, false_tmp)`
  7. `bind(L_end)`: load from slot into destination

## Register Safety

- Condition must compile with a clean `regdefp_t` (`first=NULL`) to avoid writing the comparison result into the caller's destination register
- Branch expressions must also compile with clean `regdefp_t` — use fresh tmp registers, write to stack slot
