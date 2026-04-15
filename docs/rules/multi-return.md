# Multiple Return Values — Design Rationale

## Why inject __retbuf at compile time, not parse time?

Multi-return is detected when `TokenRETURN::parse()` encounters `return a, b;` — which can appear anywhere in the function body, potentially after the function signature and parameters have already been parsed. Injecting `__retbuf` at compile time in `TokenFunc::compile()` means the parser does not need to speculatively modify the parameter list. The compiler has full knowledge of whether the function is multi-return before it emits any code.

## Why void return type for multi-return functions?

asmjit's `FuncSignature` supports only a single return type. Rather than picking one of the return values to carry in a register and the rest via buffer (which would complicate both caller and callee), all values go through the buffer and the function signature is set to void. This keeps the calling convention uniform regardless of how many values are returned.

## Why a stack buffer instead of RAX+RDX?

The RAX+RDX approach limits multi-return to exactly two integer-sized values. A stack buffer (`[retbuf + i*8]`) generalizes to any number of return values of any type. The caller allocates the buffer, passes its address, and reads back the values — no special register conventions needed beyond the standard calling convention.

## Why skip cleanup for multi-return paths?

When a function has multiple `return` statements (e.g., early return in an if-branch and normal return at the end), each return path writes to the buffer and jumps to the function epilogue. If `cleanup()` runs destructors on the return-value temporaries at each return site, the same object can be destructed multiple times — once at the early return and again at the normal return's cleanup. Skipping cleanup on multi-return paths prevents this double-destruct.

## Why does brace-less if fail with multi-return?

A brace-less `if` parses a single statement as its body. When that statement is `return a, b;`, the comma after `a` is ambiguous — the single-statement parser may interpret it as the end of the expression rather than a continuation of the return value list. Braces remove this ambiguity by clearly delimiting the full body, allowing the return statement to parse all comma-separated values correctly.
