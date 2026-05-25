# C++ Support Plan

Analysis performed 2026-05-24.

## Current State: ~30% of Useful C++ Features

| Feature | Status | Detail |
|---------|--------|--------|
| Classes | Constructors | Members, methods, user-defined ctors/dtors with args. No inheritance/virtual/access control |
| Templates | Hardcoded | vector/map/set/list for int64 and string only. No user-defined templates |
| Operator overloading | Working | ==, !=, <, >, <=, >=, +, -, *, / dispatch to class methods |
| References | Working | Explicit `T&` for numeric types. Non-numeric types pass by ref implicitly |
| Exceptions | Tokens only | try/catch/throw parsed but zero compilation |
| Lambdas | Working | `[]` and `[&]` capture. No selective `[x, &y]` |
| Auto | Limited | Function pointers and lambdas only. No general type deduction |
| Const | Parsed, ignored | Accepted everywhere, never enforced |
| Namespaces | Built-in only | `using namespace std;` works. No user-defined namespaces |
| Streams | Partial | `cout <<` chaining works. `cin >>` missing. No manipulators |
| Enums | C-style | No `enum class` (scoped enums) |
| new/delete | Missing | Stack allocation only for user classes |

## Industry Context

No lightweight JIT compiler implements significant C++ natively. Every project
needing full C++ JIT (Cling, ROOT, ClangJIT) builds on Clang/LLVM (~500MB).
The C++ spec is too large for a custom frontend to cover completely.

**madc's niche:** C scripting with C++ convenience features (classes, strings,
streams, lambdas, containers). Full C++ compliance is not the goal — practical
C++ features that real code needs are.

## Phase 1: Complete the Class Model (2-3 weeks)

### 1a. User-defined constructors & destructors (3-5 days)

**Existing infrastructure (do not rebuild):**
- Hidden `__this` pointer — fully working (parser injects, compiler binds)
- Method parsing/compilation — name mangling, `method_map`, `findMethod()`
- Stack allocation — `TokenCpnd::voperand()` handles btClass via `cc.newStack()`
- Placement-new pattern — `string_construct()` / `string_destruct()` for members
- Cleanup dispatch — `TokenCpnd::cleanup()` walks scope, calls dtors per type
- Deferred statements — `defer` runs LIFO before destructors

**Implementation steps:**

**Step 1 — Parse-time ctor/dtor recognition** (parser.cpp, datadef.h)
- In `TokenCLASS::parse()`, when method name == class name: treat as
  constructor. No return type required in source — internally `void`.
- When method name is `~ClassName`: treat as destructor (also `void`).
- Add `bool has_user_ctor` and `bool has_user_dtor` to `DataDefCLASS`.
- Ctor/dtor are stored in `method_map` like any method. The flags are
  just fast checks for `voperand()` / `cleanup()`.
- Single mangled names: `ClassName__ClassName` (ctor),
  `ClassName___dtor` (dtor). No Itanium C1/C2/D0/D1/D2 split needed
  (no virtual inheritance).

**Step 2 — Destructor calls at scope exit** (compiler.cpp `cleanup`)
- In `cleanup()`, btClass default case: if `has_user_dtor`, look up
  dtor in `method_map`, emit `invoke` with `__this` = object address.
- Call user dtor *before* member cleanup (string_destruct etc.) —
  matches C++ semantics (body runs first, then members in reverse).
- **LIFO ordering problem:** `operand_map` is `std::map<Variable*,
  Operand>` — ordered by pointer address, not declaration order.
  Add a `std::vector<Variable*> destruct_order` to `TokenCpnd` that
  records class-typed variables in declaration order. Walk it in
  reverse during cleanup.

**Step 3 — Constructor calls at object creation** (compiler.cpp `voperand`)
- In `voperand()` btClass case, after stack alloc + member string init:
  if `has_user_ctor`, look up ctor in `method_map`, emit `invoke` with
  `__this` = LEA of stack slot.
- Default ctor (no args) only in this step.

**Step 4 — Constructor arguments at declaration** (parser.cpp `parseDeclaration`)
- In `parseDeclaration()`, when a class-typed variable is followed by
  `(`: parse as ctor arguments (not function-pointer syntax). No
  most-vexing-parse ambiguity — madc forbids function declarations
  inside function bodies.
- Store parsed arg expressions on the declaration node (e.g., on
  `TokenDecl` or a new `ctor_args` field).
- In `voperand()`, pass the args when invoking the ctor.

**Step 5 — Early return cleanup**
- Verify `TokenRETURN::compile()` calls `cleanup()` for all return
  paths. It already does for single-return; confirm multi-return and
  void-return paths.
- Duplicate dtor calls at each return point — fine for JIT, matches
  GCC `-O0` behavior. No shared cleanup block needed.

**Step 6 — Tests**
- `testctor.mad` — default ctor/dtor, verify call order via prints
- `testctorargs.mad` — ctor with arguments
- `testctornested.mad` — class with string members + user ctor/dtor
- `testctororder.mad` — multiple objects, verify LIFO destruction

**Explicitly deferred (not in this phase):**
- Copy constructors / copy assignment (`Foo a = b;`)
- Move semantics
- Temporary objects (`foo(Bar(1,2))`)
- Member initializer lists (`: member(val)`)
- `break` / `continue` / `goto` across scopes with dtors (loop exits)
- Constructor overloading (multiple signatures)
- Inheritance constructor chaining (Phase 2a)

**Prerequisite for:** inheritance, new/delete, RAII, exception cleanup

### 1b. Operator overloading completion (2-3 days)
- Compiler already has the TODO placeholder
- When binary op encounters class-typed operand, check `method_map` for
  `operator+` etc. and emit method call instead of built-in arithmetic
- Stream `<<` already works this way (special-cased) — generalize it
- Parser side already done

### 1c. Explicit reference parameters `T&` (2-3 days)
- Parse `int &x` as reference parameter type
- At call site: pass address (LEA) instead of value
- Inside function: transparent dereference through pointer
- `vfADDRTAKEN` and LEA machinery already exist
- Type system has `rtReference` variants ready

### 1d. `new` / `delete` operators (1-2 weeks)
- `new ClassName(args)` = malloc + constructor call, return pointer
- `delete ptr` = destructor call + free
- `new[]` / `delete[]` for arrays
- malloc/free emission already exists
- **Requires:** constructors (1a)

## Phase 2: Inheritance & Polymorphism (1-2 months)

### 2a. Single inheritance (1-2 weeks)
- Parse `class Derived : public Base { ... }`
- Copy base members into derived at offset 0
- Implicit upcasting (Derived* → Base*)
- Method override via `method_map` shadowing
- **Requires:** constructors (1a) for base class init

### 2b. Virtual functions / vtables (2-3 weeks)
- Hidden `__vptr` as first class member
- vtable = array of function pointers, one per class
- Virtual call: `call [obj + vptr_offset + slot * 8]` (indirect)
- Override slots in derived vtables
- asmjit `cc.invoke()` handles indirect calls via register
- **Requires:** inheritance (2a)

### 2c. RTTI — dynamic_cast, typeid (1-2 weeks)
- Type metadata per class (name, hierarchy chain)
- `dynamic_cast` checks vtable type info at runtime
- **Requires:** vtables (2b)

## Phase 3: Exception Handling (3-4 weeks)

### 3a. try/catch/throw via SJLJ
- Use setjmp/longjmp (madc already has setjmp support)
- `try` = setjmp save point
- `throw` = longjmp with exception object
- `catch` = type-checked dispatch after longjmp
- **Hard part:** stack unwinding — ensuring destructors run for all
  objects between throw and catch. Requires cleanup chain.
- **Requires:** constructors/destructors (1a)

## Phase 4: Quality of Life (ongoing, as needed)

### 4a. const enforcement (1-2 days)
- Set `vfCONSTANT` on const-declared variables
- Error on assignment to const in `TokenAssign::compile()`
- Deep const (const methods, const propagation) is separate effort

### 4b. Scoped enums `enum class` (1-2 days)
- When `enum` followed by `class`, create scoped namespace for values

### 4c. General `auto` type deduction (1 week)
- `auto x = expr;` — evaluate expr type, assign to variable
- Requires type inference from expressions (already partially in IR work)

### 4d. More STL container types (1 day per combination)
- `vector<double>`, `map<int,int>`, etc.
- Each needs C++ helper functions in ns_stl.cpp
- Long-term: automated via template instantiation

### 4e. Selective lambda capture `[x, &y]` (2-3 days)
- Parse capture list with per-variable by-value vs by-reference
- Extend `FuncDef::captures` to track capture mode

### 4f. User-defined namespaces (1 week)
- Parse `namespace MyNS { ... }` declarations
- Add to `namespace_map` dynamically

### 4g. Stream input `cin >>` (3-5 days)
- Parse `>>` operator for istream types
- Emit calls to `streamin_string()`, `streamin_numeric()` helpers
- Analogous to existing `streamout_*` pattern

## Deferred Indefinitely

| Feature | Reason |
|---------|--------|
| General templates | Architecturally invasive; requires deferred compilation (conflicts with single-pass). Consider monomorphization-on-demand as lighter alternative |
| Template metaprogramming | SFINAE, enable_if, variadic templates — not practical without Clang |
| Full STL | Depends on general templates; hand-wrap what's needed |
| constexpr evaluation | Requires compile-time interpreter. Current constant folding sufficient |
| Multiple inheritance | Diamond problem, thunks, virtual bases — extreme complexity |
| Move semantics | Lvalue/rvalue distinction throughout expression compiler. Only if target code requires it |
| C++20 concepts/coroutines | Too far ahead for madc's current architecture |

## Effort Summary

| Phase | Effort | What You Get |
|-------|--------|-------------|
| Phase 1 (class model) | 2-3 weeks | Constructors, destructors, operator overload, references, new/delete — usable OOP |
| Phase 2 (inheritance) | 1-2 months | Single inheritance, virtual dispatch, RTTI — polymorphic OOP |
| Phase 3 (exceptions) | 3-4 weeks | try/catch/throw — error handling |
| Phase 4 (QoL) | Ongoing | const, enum class, auto, streams, namespaces — polish |

## Strategic Note

madc's C++ support serves **scripting convenience**, not standards compliance.
The SMAUG port is C89. The C++ features (classes, strings, streams, lambdas)
make madc pleasant to use as a scripting language. Deeper C++ (vtables,
exceptions) is justified when porting C++ codebases or when user demand
requires it.

The code cleanup plan (builtin dispatch table, parser simplification) should
be done BEFORE Phase 2 — a cleaner codebase makes adding inheritance and
vtables far less error-prone.
