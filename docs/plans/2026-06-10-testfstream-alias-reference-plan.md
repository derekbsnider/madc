# testfstream GREEN — alias-spelled reference returns + standard-C++ rewrite

> **STATUS: COMPLETED 2026-06-10** — all tasks executed; fulltest
> **547/0/0/26** (ALL integration reds green), torture 1567/31/56/1 failset
> byte-identical, SMAUG boots. Commits: `5482124` (canon notes), `8b16fd8`
> (alias-ref returns, DataDefREF), `c4a39aa` (namespace fn-template BODY
> instantiation — to_string/stoi run), `883c26e` (testfstream rewrite +
> fortify chk builtins + [namespace.udir] call fallback). Deferred v2 gaps,
> by design: zero-element parameter packs (wstring stof/stod-shaped calls
> keep the placeholder fallback) and template-id-shaped param deduction
> (getline stays mangled-direct via Pattern A).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> (inline) or superpowers:subagent-driven-development to implement task-by-task.
> Steps use checkbox (`- [ ]`) syntax. **Read §0 (rules + state) before Task 1.**

**Goal:** the last fulltest red, `tests/testfstream.mad`, passes through REAL
libstdc++ headers — which requires madc to stop dropping the reference-ness of
alias-spelled return types (`basic_string::operator[]` returns `reference` =
`char&` through the allocator_traits alias chain).

**Architecture:** fix at the deepest layer — type-alias resolution must carry
the full type including the reference qualifier (the g++/clang model: an alias
IS a type; canonicalization preserves `&`), not patch call sites. Then rewrite
testfstream to standard C++ with `.flags`/`.expect` fixtures (testloop/testdefer
pattern).

**Tech stack:** madc parser (`src/parser.cpp`), CirBuilder
(`src/cir_builder.cpp`), real libstdc++ 13 headers, c2mir/MIR backend.

---

## §0. STATE + RULES (read first, every executor)

- Branch `feature/cpp-detection-idiom-claude` @ `4e87995` (pushed). Baseline:
  fulltest **546/1/0/26** (only testfstream red), torture **1567/31/56/1**
  (failset reference: `tmp/failset_lsq.txt` — regenerate per §0.3 if missing).
- Prior session (2026-06-09) already landed, in `b6fd773`:
  - C++ namespace free-function **overload sets** (each overload its own
    Variable/FuncDef + `Program::namespace_fn_overload_sets` + arg-type ranking
    in `CirBuilder::call_target_funcdef`).
  - **Inline-namespace plain-re-open mirroring** (`parse_namespace_block`):
    `std::to_string`/`std::stoi` now resolve as `std::` members.
  - `__retbuf` NRVO gate keyed on the **resolved** emit symbol
    (`object_returning_call_class`).
- **WARNING (do not re-trip):** a `TokenSubscriptExpr` operator[] dispatch in
  `CirBuilder::translate_expr` was tried and **REVERTED** — it regressed 9
  embedded-mode container tests (testvector/testmap/testset/testsubscript*,
  teststringref, testcontainerdtor, testmadc_ns, testtemplatestring,
  testfstream). Any subscript-path change must keep those tests green; they are
  the canaries. The receiver-generic core `class_subscript_addr_on` (kept,
  behavior-preserving) exists in cir_builder.cpp for reuse.
- **Rules:** g++ AND clang++ are canon (`.claude/rules/gcc-methodology.md`,
  `clang-methodology.md`); fix at the deepest layer; NO shims/per-type
  special-cases (`scripts/check-no-std-hardcoding.sh` gates); cap every run
  `( ulimit -t 120; timeout 180 … )`; ONE heavy job at a time.
- **§0.3 Gates after EVERY behavior change** (a task is not done without them):
  1. `( timeout 1500 make -C src fulltest )` → must stay ≥ 546 pass / ≤ 1 fail
     (testfstream may stay red until the final task; NOTHING else may break).
  2. `( timeout 3500 make -C src gcctest ) > tmp/gcctest_X.log`; then
     `diff <(grep -E "^FAIL|^TIMEOUT" tmp/gcctest_X.log | sort) tmp/failset_lsq.txt`
     → must be empty (or strictly fewer failures; update the reference file).
  3. SMAUG soak:
     `cd /workspace/MadSMAUG/runtime/area; timeout 50 /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000`
     → exit **124** and the log contains `Realms of Despair ready at`.
  4. Commit (small, one concern), push.

## §0.5 Reducers (the failing tests — already on disk)

| file | content | current behavior | done when |
|---|---|---|---|
| `tmp/ts5.mad` | `string s("abc"); char c = s[1];` print c | exits 0, prints **garbage** (operator[]'s `char*` return assigned as char — missing deref) | prints `b` |
| `tmp/ts4.mad` | `char *p = &s[1];` print `*p` | `lvalue required as unary & operand` | prints `b` |
| `tmp/ts1.mad` | `to_string(42)` + `stoi("12345")` | `basic_string.h:4171: lvalue required` (same root: `&__str[__neg]` inside to_string's own inline body) | prints `to_string: 42` and `stoi: 12345` |

Run any of them:
`cd /workspace/madc; ( ulimit -t 120; timeout 120 bin/madc --std=c++17 --no-embedded-headers tmp/ts5.mad )`

## §0.6 Root cause, already established (verify, don't re-derive)

`basic_string::operator[]` is declared `reference operator[](size_type)` —
`reference` is a class-scope alias that chains
`__gnu_cxx::__alloc_traits<allocator<char>>::reference` → `value_type&` →
`char&`. madc decides a method's reference return ONLY from a literal `&`
token after the return type (`parser.cpp:19165` region, `bool ret_is_ref`),
so an alias-spelled reference return gets `returns_ref = false` and whatever
DataDef the alias resolves to **loses the `&`**. Call sites then skip the
deref (a `T&` return travels as `T*` in madc's lowering — see
`m_cur_func_returns_ref` in cir_builder).

The emitted symbol is already correct (`_ZNSt7…ixEm` — Itanium does not mangle
return types), which is why the bug is silent until the value is used.

---

### Task 1: Canon research — how g++ and clang++ model this chain

No madc code changes. Output = a short research note appended to this plan
file (or `docs/plans/refs/2026-06-10-alias-ref-canon-notes.md`) recording the
canonical model madc must mirror. The point (user-directed): understand how
the REAL compilers get through the same fstream wall-chain before designing
the fix.

- [x] **Step 1.1: clang AST — what type does operator[] really have?**

```bash
cat > /workspace/madc/tmp/canon1.cpp <<'EOF'
#include <string>
int main() { std::string s("abc"); char c = s[1]; char *p = &s[1]; return c + (p != nullptr); }
EOF
clang++ -std=c++17 -Xclang -ast-dump -fsyntax-only /workspace/madc/tmp/canon1.cpp 2>/dev/null | grep -n -A2 "operator\[\]" | head -30
```

Expected observation: the declared return is spelled `reference` but the AST
node carries the **canonical type** `char &(size_type)` — i.e. in clang an
alias (`ElaboratedType`/`TypedefType`) is a *sugar layer over a canonical
type*; the reference qualifier lives in the canonical type, never in the
declarator. Record the exact dump lines.

- [x] **Step 1.2: g++ — same semantic model via codegen**

```bash
cat > /workspace/madc/tmp/canon2.cpp <<'EOF'
#include <string>
char get(std::string &s) { return s[1]; }
char *addr(std::string &s) { return &s[1]; }
EOF
g++ -std=c++17 -O0 -S -fverbose-asm -o - /workspace/madc/tmp/canon2.cpp | grep -A6 "call.*ixEm"
```

Expected observation: g++ calls `_ZNSt7…ixEm` and then **dereferences the
returned pointer** (`movzbl (%rax)`) for `get`, and uses `%rax` directly for
`addr` — confirming `T&` = address + deref-at-use, exactly madc's
`returns_ref` lowering. Record the asm lines.

- [x] **Step 1.3: survey the rest of testfstream's wall chain in canon**

For each ingredient the rewritten test needs, confirm with g++/clang what the
*semantics* are, and note which madc mechanism covers it (or doesn't):

```bash
printf '#include <iostream>\n#include <fstream>\n#include <string>\n#include <cstdlib>\n#include <cstring>\nusing namespace std;\nint main(){ ofstream o; string f="/tmp/x.txt"; o.open(f); o<<"hi"<<endl; o.close(); ifstream i; i.open(f); string l; getline(i,l); cout<<l<<endl; i.close(); string s=to_string(42); cout<<s<<endl; int n=stoi(string("12345")); cout<<n<<endl; n=(int)strlen(s.c_str()); cout<<n<<endl; system("rm -f /tmp/x.txt"); return 0; }\n' > /workspace/madc/tmp/canon3.cpp
g++ -std=c++17 -o /workspace/madc/tmp/canon3 /workspace/madc/tmp/canon3.cpp; /workspace/madc/tmp/canon3
clang++ -std=c++17 -o /workspace/madc/tmp/canon3c /workspace/madc/tmp/canon3.cpp; /workspace/madc/tmp/canon3c
```

Both must print `hi / 42 / 12345 / 2`. Then check which symbols are EXTERNAL
(bind mangled-direct, no madc body needed) vs INLINE (madc must compile the
header body):

```bash
nm -D /usr/lib/x86_64-linux-gnu/libstdc++.so.6 | grep -c "9to_stringB5cxx11\|4stoi"   # expect 0 -> inline, madc compiles
nm -D /usr/lib/x86_64-linux-gnu/libstdc++.so.6 | grep -c "7getline"                    # expect >0 -> external, mangled-direct
```

Also record stoi's body dependency: it calls
`__gnu_cxx::__stoa<long,int>(&std::strtol, "stoi", …)` (a function template in
`ext/string_conversions.h`) — madc must instantiate that template body. The
2026-06-09 reducer run got past stoi's CHECK with no errors, but runtime is
unverified until Task 3.

- [x] **Step 1.4: write the canon-notes file + commit (docs-only)**

The note must end with the model statement the fix follows:
*"An alias is a type, not a spelling. Resolution of any type spelling —
including class-scope aliases and template-dependent alias chains — must
produce (DataDef base, pointer depth, IS-REFERENCE) as one unit. The reference
qualifier is part of the resolved type; a declarator-level `&` token is only
ONE way a type acquires it."*

```bash
git add docs/plans/refs/2026-06-10-alias-ref-canon-notes.md
git commit -m "docs: canon research — g++/clang alias-as-canonical-type model for the testfstream chain"
```

---

### Task 2: Fix alias-spelled reference returns at the resolution layer

**Files:**
- Modify: `src/parser.cpp` (member-parse return-type region around
  `parser.cpp:19165` `bool ret_is_ref`, and wherever the return-type spelling
  resolves through the class-scope/template alias machinery — trace from
  19165 upward to where `tn`/the type tokens were resolved to a DataDef)
- Possibly modify: the alias-target tokenization (the alias machinery stores
  target TOKENS; the `&` is IN those tokens — find where they are consumed
  into a DataDef and the `&` is dropped)
- Test: reducers `tmp/ts5.mad`, `tmp/ts4.mad` (+ §0.3 gates)

- [x] **Step 2.1: trace (pre-edit checklist — do not skip)**

Instrument or read until you can answer, in writing:
(a) for `reference operator[](size_type)` parsed from the REAL
`bits/basic_string.h`, which parser function resolves the spelling
`reference` to a DataDef, (b) what DataDef comes out (char? int? a dependent
placeholder?), (c) where the alias's stored target tokens (`value_type&` /
`_CharT&`) lose the trailing `&`. Use a gated one-off diagnostic
(`#if MADC_DEBUG_ALIASREF` per `.claude/rules/debug.md` /
feedback_gated_debug_not_churn) — not ad-hoc prints added/removed per cycle.
Key greps to start: `ret_is_ref` (19165), `template_alias_map`,
`find_template_alias`, the class-scope alias handling cited in
claude_status's "class-scope aliases/static member types" checkpoint.

- [x] **Step 2.2: decide the fix point by this criterion**

The fix is correct ONLY if it lives where the alias target is RESOLVED (so
every consumer — method returns, parameter types, locals — benefits), not in
the operator[]-specific or subscript-specific code. Expected shape: the
resolver returns/records "resolved type is a reference" (the alias target
tokens end in `&`/`&&`), and the member-parse return path ORs that into
`ret_is_ref` before `mfd->returns_ref = ret_is_ref` (parser.cpp:19297).
Parameters spelled by ref-aliases should get the same treatment via
`ref_params` if reachable with low risk; if that widens the blast radius,
land returns first and file parameters as the explicit next commit.

- [x] **Step 2.3: implement, rebuild, run the two reducers**

```bash
( timeout 580 make -C /workspace/madc/src -j2 ) 2>&1 | grep -E " error|warning:" | head
cd /workspace/madc
( ulimit -t 120; timeout 120 bin/madc --std=c++17 --no-embedded-headers tmp/ts5.mad )   # expect: b
( ulimit -t 120; timeout 120 bin/madc --std=c++17 --no-embedded-headers tmp/ts4.mad )   # expect: b
```

Note ts4 (`&s[1]`) may ALSO need the address-of path to consume the now-ref
return without the reverted TokenSubscriptExpr dispatch; if ts5 passes and
ts4 still fails, the residual is in how `&<method-call-returning-T&>` lowers
(`TokenAddrExpr` over the call should already collapse to the returned
pointer once `returns_ref` is true — check `m_cur_func_returns_ref` /
class_subscript machinery before adding anything new).

- [x] **Step 2.4: run ALL §0.3 gates (especially the 9 canary container tests), commit**

```bash
git add -A && git commit -m "fix(cpp): alias-spelled reference returns keep their reference-ness"
```

---

### Task 3: to_string / stoi end-to-end (reducer ts1 green)

**Files:** expected NONE beyond Task 2 (the body error at `basic_string.h:4171`
is the same root). If ts1 still fails, each new error gets the
reduce→attribute→deepest-layer loop; known candidate next walls, in order:
- `__gnu_cxx::__stoa<long,int>` template-body instantiation (stoi runtime),
- the nine to_string overload bodies' other libstdc++ internals
  (`__detail::__to_chars_len`, `__to_chars_10_impl`),
- `std::string` by-value return ABI interplay (see pre-existing a+b SIGSEGV,
  reducers `tmp/cstr3.mad`/`tmp/cstr4.mad` — do NOT merge that fight into this
  task; if ts1 hits it, stop and report).

- [x] **Step 3.1: run the reducer**

```bash
( ulimit -t 120; timeout 120 bin/madc --std=c++17 --no-embedded-headers tmp/ts1.mad )
# expect: to_string: 42 / stoi: 12345, exit 0
```

- [x] **Step 3.2: if green — §0.3 gates + commit; if red — reduce/attribute the NEXT wall and fix at its layer (one commit per wall)**

---

### Task 4: rewrite tests/testfstream.mad to standard C++ + fixtures

**Files:**
- Modify: `tests/testfstream.mad` (replace content below)
- Create: `tests/testfstream.flags`, `tests/testfstream.expect`

The current test uses madc-dialect forms (`to_string(s, 42)` two-arg,
`strlen(num)` on a string, `system(rm)` with a string). Rewrite to standard
C++ (g++-validated — this is canon3.cpp from Task 1.3, reuse it), per the
testloop/testdefer precedent:

- [x] **Step 4.1: write the new test**

```c++
#!/../bin/madc
// Standard C++ (g++-validated) — compiled through REAL libstdc++ headers via
// the .flags fixture (--std=c++17 --no-embedded-headers).
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <cstring>
using namespace std;

int main()
{
    // write a file
    ofstream outf;
    string fname = "/tmp/madc_fstream_test.txt";
    outf.open(fname);
    outf << "Hello from madc!" << endl;
    outf << "Line 2" << endl;
    outf << "Line 3" << endl;
    outf.close();
    cout << "wrote file" << endl;

    // read it back line by line
    ifstream inf;
    string line;
    inf.open(fname);
    getline(inf, line);
    cout << "1: " << line << endl;
    getline(inf, line);
    cout << "2: " << line << endl;
    getline(inf, line);
    cout << "3: " << line << endl;
    inf.close();

    // type conversions
    string s = to_string(42);
    cout << "to_string: " << s << endl;
    string num = "12345";
    int n = stoi(num);
    cout << "stoi: " << n << endl;
    n = (int)strlen(num.c_str());
    cout << "strlen: " << n << endl;

    // cleanup
    string rm = "rm -f /tmp/madc_fstream_test.txt";
    system(rm.c_str());
    return 0;
}
```

`tests/testfstream.flags`:
```
--std=c++17 --no-embedded-headers
```

`tests/testfstream.expect`:
```
wrote file
1: Hello from madc!
2: Line 2
3: Line 3
to_string: 42
stoi: 12345
strlen: 5
```

- [x] **Step 4.2: g++-validate the rewrite, then run it under madc**

```bash
sed 1d tests/testfstream.mad > tmp/fstream_canon.cpp
g++ -std=c++17 -o tmp/fstream_canon tmp/fstream_canon.cpp && tmp/fstream_canon   # must match .expect
( ulimit -t 120; timeout 120 bin/madc --std=c++17 --no-embedded-headers tests/testfstream.mad )
```

If madc hits NEW walls here (candidates from the old handoff: fortify
`__builtin___memmove_chk` via `<cstring>`; `ifstream::open(const string&)`;
chained `getline` + stream-state), each gets its own
reduce→attribute→deepest-layer→gate→commit cycle. If the test legitimately
needs > 5s (real-header parse), add `tests/testfstream.timeout` with e.g. `15`
— a generic fixture, never a runner branch.

- [x] **Step 4.3: full §0.3 gates — fulltest must now be 547/0/0/26 — commit, push**

```bash
git add tests/testfstream.mad tests/testfstream.flags tests/testfstream.expect
git commit -m "test(cpp): testfstream GREEN through real libstdc++ headers — fulltest 547/0"
git push origin feature/cpp-detection-idiom-claude
```

---

### Task 5: sync mirrors + handoff

- [x] Update `claude_status.json` (head/branch/build_status/test_status),
  `CHANGELOG.md` ([Unreleased]), `docs/plans/ROADMAP.md` Track-1.3 red-list,
  `docs/test-status.md`, and the auto-memory RESTART-HANDOFF line; check off
  this plan's boxes; commit `docs: sync mirrors — all integration reds green`,
  push.
- [x] If ALL reds are green, note in the handoff that the next walls from the
  old list are: std::string `a+b` real-header SIGSEGV (pre-existing,
  `tmp/cstr3/4.mad`) and the W2 OPERATOR-path re-mangle (step D).

## Self-review notes

- Spec coverage: alias-ref wall (T2), its body consequence (T3), the test
  itself + the dialect rewrite + fortify wall (T4), canon research the user
  requested (T1), mirrors (T5). ✓
- This is a debugging campaign, so T2/T3 steps are investigation-gated rather
  than verbatim-code: each carries the decision criterion, the exact probe
  commands, the expected outputs, and the regression canaries — the executor
  never has to invent process, only the fix body itself.
- Type/name consistency: `ret_is_ref`→`mfd->returns_ref` (parser.cpp:19165 /
  19297), `m_cur_func_returns_ref` (cir_builder) verified against live source
  this session. ✓
