# `import` — the module binding: implementation plan (slice 0 of the web-target arc)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `import name [as alias];` binds a module — its interface (declarations) AND its library — with no platform spelling in any source, on the JIT and in native artifacts; `#load` retires; `-l` maps through the same resolver (fixing the Windows-less suffix rule).

> **OWNER RULING DURING EXECUTION (2026-09-06):** `#load` is NOT retired. It stays as the low-level directive underneath the alias form — like `#pragma`, for tooling and fixtures: the file is spelled verbatim, the author owns the platform (the vector ABI gate binding a freshly built `.so` by absolute path is the standing use). It is three lines over the shared binder, so it costs nothing. Task 7's "delete the `#load` arm / `git rm tests/testdlopen.*`" steps are therefore NOT executed; the docs/grammar/rules present `import` as the language form and `#load` as the low-level one. Everything else in Task 7 stands.

**Architecture:** One data-driven module map + ONE platform-spelling owner (`src/madc_modules.cpp`) serve three consumers: the lexer's `import` directive (recognized at the start of a logical line, C++20's own rule, gated by `--std=`), the `-l` flag, and the native lanes' link closure. Alias-form members (`libc::abs`) lower to a runtime-resolved indirect call through a per-member slot and the runtime helper `__madc_dl_member(lib, member)` — one lowering for JIT, exe, obj and `--emit=c11` — replacing the JIT-only `__dl_` import thunks. Interface-form modules link (JIT: open RTLD_GLOBAL; native: the library joins the link closure).

**Tech Stack:** C++11 (madc), c2mir node API (`node1/2/3/4`, `N_COND`, `N_ASSIGN`, `N_CAST`, `N_CALL`, `N_SPEC_DECL`), doctest, `scripts/run_tests.sh` fixtures, `scripts/remote_build.sh` (the container builds and tests; the NAS never does).

**Spec:** [docs/plans/2026-09-06-ui-web-target-and-madcide-gui.md](2026-09-06-ui-web-target-and-madcide-gui.md) §3.1 (`import`), §2.2 (today's facts), §5 slice 0, §6.

## Global Constraints

- Branch: `feature/import-module-binding-claude` off `develop`; commit early; never `git checkout` over uncommitted work; never add the owner's untracked `donut.c`, `test.mad`, `testsort.mad`.
- Every commit touching `src/` or `include/` carries the four trailers `Hypothesis:` / `Layer:` / `Searched:` / `Oracle:` (`scripts/check-rule-trailers.sh` in fulltest fails without them).
- Shell hygiene: one simple command per Bash call, no `&&` chains; scripts are fine.
- Build/test on the container only: `scripts/remote_build.sh sync build`, `scripts/remote_build.sh unittest`, `TESTS='<globs>' scripts/remote_build.sh tests-all` (JIT + exe + obj on the subset). Every test run capped (the runner does it). One heavy container job at a time.
- Targeted tests per commit; `make -C src fulltest` + the battery ONCE at the merge wave (`scripts/remote_build.sh battery`, then `wine`, `c-testsuite`, `macos` per the push gate `scripts/lane_ledger.sh check --promote`).
- No platform spelling (`.so`, `.dll`, `.dylib`, `lib` prefix) anywhere except `src/madc_modules.cpp` (the ONE owner) and the artifact-naming macro `MADC_DSO_SUFFIX` (out of scope). *(Task 8's `/dupaudit` reported the macro as the family's one DIFFERING site — no `.dll` arm, so a Windows-hosted `-shared` output was named `.so` — and it was folded into the owner: `madc_target_dso_suffix(madc_target_os)` names the artifact; the macro is deleted.)*
- `import` is recognized ONLY when `cpp_keyword_active(STD_CPP20)` (madc dialect, C++20 and later) AND the word is the first token on its logical line AND it is followed by `<`, `"` or an identifier. `int import = 3;` keeps compiling everywhere (contextual — the hard-reservation trap is a recorded KG lesson: 49 tests broke).
- Code style: tabs, C++11, `DBG()` for debug output, `Throw <<` for user-facing errors (never gated behind DBG).
- Thread-safety contract for every new runtime state: stated in a comment at the definition.

---

## File structure

| File | Responsibility |
|------|----------------|
| `include/madc_modules.h` (new) | Module map row type, `madc_module_find`, `madc_module_library_spelling` (target and explicit-OS forms), `madc_target_dso_suffix`, `madc_module_open` |
| `src/madc_modules.cpp` (new) | The rows (`c`, `m`), the ONE platform-spelling rule, the search path (beside the running binary's `../lib`, then the loader's own rules) |
| `include/datadef.h` | `TargetOS` gains `Darwin`; comment on `MADC_DSO_SUFFIX` scope |
| `src/parser.cpp` | `madc_target_os` initializer (Darwin arm); the two `dlopen_map` member sites stamp the new FuncDef fields; delete `dl_symbol_map` writes |
| `include/madc.h` | `FuncDef::dyn_module_library/dyn_module_member`; `Program::_last_real_token_line`, `module_link_libs`, `dl_library_spelling`; `import_directive_position()`, `tokenize_import_directive()`, `bind_module_namespace()` |
| `src/lexer.cpp` | The `import` directive (identifier arm hook + handler); `#load` routed through `bind_module_namespace` (sugar), then deleted |
| `src/madc.cpp` | `-l` mapping → resolver; `module_link_libs` → `cc_link_args` after parse; help text |
| `src/madc_cir.cpp` | `user_libs` `-l` mapping → resolver; Apple: user dylibs become load commands; delete `cir_active_dl_syms`; project lane collects per-TU `module_link_libs` |
| `src/cir_builder.cpp` / `.h` | Dynamic-member call lowering (slot + `__madc_dl_member`), slot declarations via `m_output_externs` |
| `src/madc_mir_backend.cpp` | `extern "C" void *__madc_dl_member(const char *lib, const char *member)` |
| `src/Makefile` | `madc_modules.o` in `CORE_OFILES` |
| `tests/unit/test_modules.cpp` (new) | Spelling rule + rows |
| `tests/testimport.mad` (+`.expect`), `tests/testimportiface.mad` (+`.flags`, `.expect`), `tests/testimportiface_neg.mad` (+`.flags`, `.expect_err`), `tests/testnoautoload.mad` (migrated), `tests/testdlopen.*` (deleted) | Reducers |
| Docs: `docs/language/import.md` (new), `docs/language/preprocessor.md`, `docs/language/overview.md`, `docs/language/embedded-headers.md`, `docs/language/sys-object.md`, `docs/man/madc.1`, `docs/grammar/madc.ebnf`, `docs/architecture.md`, `.claude/rules/embedded-headers.md` + `docs/rules/embedded-headers.md`, `include/madc/ns_madc` comment, `scripts/c-abi-internal-exports.txt` comments, `docs/plans/ROADMAP.md` 1.8, `CHANGELOG.md` Unreleased, `claude_status.json` | Surface |

---

### Task 1: The module map and the ONE platform-spelling owner

**Files:**
- Create: `include/madc_modules.h`, `src/madc_modules.cpp`
- Modify: `include/datadef.h:71-74` (TargetOS), `src/parser.cpp:17886-17893` (initializer), `src/Makefile:135` (`CORE_OFILES`)
- Test: `tests/unit/test_modules.cpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class TargetOS { Posix, Darwin, Windows };          // datadef.h (Darwin new)
  struct MadcModuleSpec { const char *name; const char *interface; const char *posix; const char *darwin; const char *windows; };
  const MadcModuleSpec *madc_module_find(const std::string &name);
  const char *madc_target_dso_suffix(TargetOS os);          // ".so" ".dylib" ".dll"
  std::string madc_module_library_spelling(const std::string &name, TargetOS os);
  std::string madc_module_library_spelling(const std::string &name);   // for madc_target_os
  void *madc_module_open(const std::string &spelling, std::string &error);
  ```

- [ ] **Step 1: Write the failing unit test**

`tests/unit/test_modules.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;	// the prologue every tests/unit file uses
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include "datadef.h"
#include "madc_modules.h"

TEST_CASE("module spelling: the platform rule for a bare library name")
{
	CHECK(madc_module_library_spelling("foo", TargetOS::Posix)   == "libfoo.so");
	CHECK(madc_module_library_spelling("foo", TargetOS::Darwin)  == "libfoo.dylib");
	CHECK(madc_module_library_spelling("foo", TargetOS::Windows) == "foo.dll");
}

TEST_CASE("module spelling: a registry row names the real runtime image per OS")
{
	CHECK(madc_module_library_spelling("m", TargetOS::Posix)   == "libm.so.6");
	CHECK(madc_module_library_spelling("m", TargetOS::Darwin)  == "libSystem.B.dylib");
	CHECK(madc_module_library_spelling("m", TargetOS::Windows) == "ucrtbase.dll");
	CHECK(madc_module_library_spelling("c", TargetOS::Posix)   == "libc.so.6");
	CHECK(madc_module_library_spelling("c", TargetOS::Windows) == "ucrtbase.dll");
}

TEST_CASE("module spelling: a path or an already-spelled name passes verbatim (the -l contract)")
{
	CHECK(madc_module_library_spelling("/opt/x/libfoo.so", TargetOS::Posix) == "/opt/x/libfoo.so");
	CHECK(madc_module_library_spelling("libfoo.so", TargetOS::Posix)        == "libfoo.so");
	CHECK(madc_module_library_spelling("bar.dll", TargetOS::Windows)        == "bar.dll");
	CHECK(madc_module_library_spelling("libz.dylib", TargetOS::Darwin)      == "libz.dylib");
}

TEST_CASE("module rows: interface presence")
{
	REQUIRE(madc_module_find("m") != NULL);
	CHECK(std::string(madc_module_find("m")->interface) == "math.h");
	REQUIRE(madc_module_find("c") != NULL);
	CHECK(madc_module_find("c")->interface == NULL);
	CHECK(madc_module_find("no-such-module") == NULL);
}

TEST_CASE("dso suffix per OS")
{
	CHECK(std::string(madc_target_dso_suffix(TargetOS::Posix))   == ".so");
	CHECK(std::string(madc_target_dso_suffix(TargetOS::Darwin))  == ".dylib");
	CHECK(std::string(madc_target_dso_suffix(TargetOS::Windows)) == ".dll");
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `scripts/remote_build.sh sync unittest` (the new file is picked up by `TESTSRCS = $(wildcard $(TESTDIR)/*.cpp)`).
Expected: FAIL — `madc_modules.h: No such file or directory`.

- [ ] **Step 3: Add `Darwin` to `TargetOS` and the initializer**

`include/datadef.h:71-74` becomes:
```cpp
// The native-emit target's OS personality. Posix = ELF hosts (Linux, the
// BSDs); Darwin = Mach-O (macOS/iOS); Windows = PE. Library SPELLING is
// decided from this enum in src/madc_modules.cpp (the one owner) — never
// from a host #ifdef at a call site.
enum class TargetOS { Posix, Darwin, Windows };
extern TargetOS madc_target_os;
inline bool madc_target_windows_p()
{ return madc_target_os == TargetOS::Windows; }
```
`src/parser.cpp:17889` initializer becomes:
```cpp
TargetOS madc_target_os =
#ifdef _WIN32
	TargetOS::Windows;
#elif MADC_TARGET_APPLE_P
	TargetOS::Darwin;
#else
	TargetOS::Posix;
#endif
```
(`MADC_TARGET_APPLE_P` is defined in `include/datadef.h:172` from `MADC_CROSS_APPLE || __APPLE__`, so the Linux-hosted darwin cross madc spells `.dylib`.)

- [ ] **Step 4: Write the header**

`include/madc_modules.h`:
```cpp
#ifndef __MADC_MODULES_H
#define __MADC_MODULES_H 1

// madc_modules — the module map and the ONE platform-spelling owner
// (design: docs/plans/2026-09-06-ui-web-target-and-madcide-gui.md §3.1).
// `import name;` (source), `-l<name>` (build line) and the native lanes'
// link closure all resolve a MODULE or bare library NAME here; no other
// file spells .so / .dylib / .dll or the lib prefix.
//
// A module row names an optional INTERFACE (an embedded header the import
// tokenizes before binding) and the runtime image per target OS. A name
// with no row follows the platform rule: lib<name>.so / lib<name>.dylib /
// <name>.dll. A name containing '/' or already carrying the target's
// suffix is verbatim (the -l contract).
//
// THREAD-SAFETY CONTRACT: the map is a constant table; the functions are
// pure except madc_module_open, which is the madc_dl seam's contract.

#include <string>
#include "datadef.h"	// TargetOS

struct MadcModuleSpec {
	const char *name;
	const char *interface;	// embedded header name, or NULL (interface-less)
	const char *posix;	// ELF runtime image spelling
	const char *darwin;	// Mach-O install name (bare file name)
	const char *windows;	// PE module name
};

const MadcModuleSpec *madc_module_find(const std::string &name);
const char *madc_target_dso_suffix(TargetOS os);
std::string madc_module_library_spelling(const std::string &name, TargetOS os);
std::string madc_module_library_spelling(const std::string &name);	// for madc_target_os

// Open a spelled library through the madc_dl seam: a bare spelling is tried
// beside the running binary first (<exe dir>/../lib/<spelling>, the
// relocatable install shape the runpath uses), then as the loader sees it.
// NULL + `error` on failure.
void *madc_module_open(const std::string &spelling, std::string &error);

#endif
```

- [ ] **Step 5: Write the implementation**

`src/madc_modules.cpp`:
```cpp
// madc_modules — module map + the ONE platform-spelling owner. Contract in
// include/madc_modules.h.
#include <string.h>
#include "madc_modules.h"
#include "madc_dl.h"

// The rows. `c` and `m` name the C runtime images by their REAL sonames /
// install names / module names — a dev-symlink spelling (libm.so) needs the
// -dev package and libc.so is a linker script on glibc, so the -l rule alone
// cannot open them. Windows keeps both in the UCRT. The interface column is
// the embedded header `import` tokenizes first (NULL = interface-less: the
// alias form's variadic member convention is all there is).
static const MadcModuleSpec madc_modules[] = {
	{ "c", NULL,     "libc.so.6", "libSystem.B.dylib", "ucrtbase.dll" },
	{ "m", "math.h", "libm.so.6", "libSystem.B.dylib", "ucrtbase.dll" },
	{ NULL, NULL, NULL, NULL, NULL }
};

const MadcModuleSpec *madc_module_find(const std::string &name)
{
	for (int i = 0; madc_modules[i].name; i++)
		if (name == madc_modules[i].name)
			return &madc_modules[i];
	return NULL;
}

const char *madc_target_dso_suffix(TargetOS os)
{
	switch (os) {
	case TargetOS::Darwin:  return ".dylib";
	case TargetOS::Windows: return ".dll";
	default:                return ".so";
	}
}

static bool ends_with(const std::string &s, const std::string &sfx)
{
	return s.size() >= sfx.size()
	       && s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
}

std::string madc_module_library_spelling(const std::string &name, TargetOS os)
{
	if (name.find('/') != std::string::npos || ends_with(name, madc_target_dso_suffix(os)))
		return name;
	if (const MadcModuleSpec *row = madc_module_find(name)) {
		switch (os) {
		case TargetOS::Darwin:  return row->darwin;
		case TargetOS::Windows: return row->windows;
		default:                return row->posix;
		}
	}
	if (os == TargetOS::Windows)
		return name + ".dll";
	return "lib" + name + madc_target_dso_suffix(os);
}

std::string madc_module_library_spelling(const std::string &name)
{
	return madc_module_library_spelling(name, madc_target_os);
}

void *madc_module_open(const std::string &spelling, std::string &error)
{
	error.clear();
	if (spelling.find('/') == std::string::npos) {
		std::string exe = madc_self_exe_path();
		size_t slash = exe.rfind('/');
		if (slash != std::string::npos) {
			std::string beside = exe.substr(0, slash) + "/../lib/" + spelling;
			if (void *h = madcdl_open_global(beside.c_str()))
				return h;
			(void)madcdl_error();	// consume; the loader's own search follows
		}
	}
	void *h = madcdl_open_global(spelling.c_str());
	if (!h) {
		const char *e = madcdl_error();
		error = e ? e : "cannot open";
	}
	return h;
}
```
`madc_self_exe_path()` is declared in `include/datadef.h:149`.

- [ ] **Step 6: Register the object**

`src/Makefile:135`: add `madc_modules.o` immediately after `madc_dl.o` in `CORE_OFILES`.

- [ ] **Step 7: Build and run the unit test**

Run: `scripts/remote_build.sh sync build unittest`
Expected: build clean (zero warnings), `test_modules` all assertions pass, every other unit binary unchanged.

- [ ] **Step 8: Commit**

```
git add include/madc_modules.h src/madc_modules.cpp include/datadef.h src/parser.cpp src/Makefile tests/unit/test_modules.cpp
git commit -F - <<'MSG'
modules: the module map + the ONE platform library-spelling owner (madc_modules), TargetOS gains Darwin

<what/why in prose>

Hypothesis: library spelling (lib prefix, .so/.dylib/.dll, real sonames) is decided in three places (madc.cpp -l, madc_cir.cpp DT_NEEDED, the lexer's #load taking it verbatim) and none knows Windows or Darwin correctly; one data-driven owner keyed on the target enum fixes all three.
Layer: import directive / -l flag / native link closure -> library spelling -> this new unit is the origin; nothing lower exists (madc_dl opens what it is given).
Searched: git grep -n 'MADC_DSO_SUFFIX\|"lib" +' (concept: platform library name mapping) -> madc.cpp:899, madc_cir.cpp:2080, datadef.h:160 — no shared owner.
Oracle: n/a — new pure mapping unit; the doctest battery is the oracle (gcc/clang have no equivalent; ld's -l rule is the model for the bare-name arm).
MSG
```

---

### Task 2: `-l` resolves through the owner (fixes the Windows-less suffix rule)

**Files:**
- Modify: `src/madc.cpp:893-910`, `src/madc_cir.cpp:2078-2083`

**Interfaces:**
- Consumes: `madc_module_library_spelling(const std::string &)` (Task 1).

- [ ] **Step 1: Replace the CLI mapping**

`src/madc.cpp:893-910` — the `-l` arm becomes:
```cpp
        } else if (strncmp(argv[i], "-l", 2) == 0 && argv[i][2] != '\0') {
            // -l<name>: bind a library so its symbols resolve at link time
            // (e.g. -lcrypt). The NAME is a module or bare library name; the
            // platform spelling comes from the ONE owner (madc_modules):
            // a registry row's real image, else lib<name>.<dso>, else the
            // verbatim path/spelling. JIT: opened RTLD_GLOBAL below. AOT:
            // the spelling joins the link closure (DT_NEEDED / load command
            // / PE import) — verbatim, so every lane agrees on the image.
            std::string lib = madc_module_library_spelling(argv[i] + 2);
            cc_link_args.push_back(lib);
            link_libs.push_back(lib);
            filearg = i + 1;
```
Add `#include "madc_modules.h"` near the other includes at the top of `src/madc.cpp`.

- [ ] **Step 2: Replace the native mapping**

`src/madc_cir.cpp:2078-2083` becomes:
```cpp
    for (const std::string &l : user_libs) {
	if (l.compare(0, 2, "-l") == 0)
	    needed.push_back(madc_module_library_spelling(l.substr(2)));
	else
	    needed.push_back(l);
    }
```
(The CLI now passes spellings, so the `-l` arm here serves only `madc_cir_link_objects`-style callers that still forward raw `-l` words; keep it, through the owner.) Add `#include "madc_modules.h"` to `src/madc_cir.cpp`.

- [ ] **Step 3: Build, run the targeted tests**

Run: `scripts/remote_build.sh sync build`; then `TESTS='testdlfcnportable testdlcall testlibc' scripts/remote_build.sh tests-all`
Expected: build clean; the three pass in JIT, exe and obj as before (no test uses `-l`; these are the neighbours of the dl surface).

- [ ] **Step 4: Commit** (with trailers; `Oracle: n/a — behaviour-preserving on Linux (lib<name>.so unchanged); on win64 the old rule produced lib<name>.so, ld's rule is <name>.dll — verified by test_modules`).

---

### Task 3: The runtime helper `__madc_dl_member`

**Files:**
- Modify: `src/madc_mir_backend.cpp:46-50` (inside the `extern "C" {` block)

**Interfaces:**
- Produces: `extern "C" void *__madc_dl_member(const char *lib, const char *member);` — opens `lib` (a target spelling) through `madc_module_open`, caches the handle per spelling, returns `madcdl_sym(handle, member)`; on failure raises a script error through `__madc_throw_cstr` (never returns NULL).

- [ ] **Step 1: Add the helper**

After `void *__madc_get_stderr(void) ...` in `src/madc_mir_backend.cpp`:
```cpp
// import (alias form): the runtime half of a dynamic-module member call.
// The CIR builder lowers `ns::member(args)` for a namespace bound by
// `import name as ns;` to a per-member slot resolved on first call:
//   ((long (*)()) (slot ? slot : (slot = __madc_dl_member(LIB, MEMBER))))(args)
// — the same lowering in every lane (JIT, exe, obj, --emit=c11), replacing
// the JIT-only __dl_<ns>_<member> import thunks. LIB is the TARGET
// spelling the module map chose at compile time.
//
// THREAD-SAFETY CONTRACT: the handle cache is guarded by its own mutex;
// concurrent first calls on one slot may both resolve (the loader
// refcounts; dlsym is idempotent) — the slot store is a benign race on a
// pointer-sized value, the same as any lazily-bound PLT.
void *__madc_dl_member(const char *lib, const char *member)
{
	static std::mutex cache_lock;
	static std::map<std::string, void *> handles;
	void *h = NULL;
	{
		std::lock_guard<std::mutex> g(cache_lock);
		std::map<std::string, void *>::iterator it = handles.find(lib);
		if (it != handles.end())
			h = it->second;
	}
	if (!h) {
		std::string err;
		h = madc_module_open(lib, err);
		if (!h) {
			std::string msg = std::string("import: cannot load '") + lib + "': " + err;
			__madc_throw_cstr(msg.c_str());
		}
		std::lock_guard<std::mutex> g(cache_lock);
		handles[lib] = h;
	}
	void *sym = madcdl_sym(h, member);
	if (!sym) {
		const char *e = madcdl_error();
		std::string msg = std::string("import: '") + member + "' is not exported by '" + lib
				  + "'" + (e ? std::string(": ") + e : std::string());
		__madc_throw_cstr(msg.c_str());
	}
	return sym;
}
```
Add `#include <map>`, `#include <mutex>`, `#include "madc_modules.h"` to the file's includes (`madc_dl.h` and `rt/rt_except.h` are already included).

- [ ] **Step 2: Build**

Run: `scripts/remote_build.sh sync build`
Expected: clean. (`__madc*` is a known internal export class for `scripts/check-c-abi-surface.sh` — no allowlist entry.)

- [ ] **Step 3: Commit** (trailers; `Oracle: n/a — runtime helper, exercised by Task 4's reducer in all lanes`).

---

### Task 4: Alias-form members lower to a runtime-resolved call (one lowering, every lane)

**Files:**
- Modify: `include/madc.h` (FuncDef fields near line 304; Program: replace `dl_symbol_map` at 4191 with `std::map<std::string, std::string> dl_library_spelling;`), `src/parser.cpp:29792-29808` and `:37688-37707`, `src/cir_builder.cpp:22210-22262`, `src/cir_builder.h` (declare `dyn_module_callee`), `src/madc_cir.cpp:125-129,202-207,1084,1120,1144` (delete `cir_active_dl_syms`)
- Test: `tests/testdlopen.mad` (still `#load` at this task; the exe pass must now pass — delete `tests/testdlopen.exe_skip`)

**Interfaces:**
- Consumes: `__madc_dl_member` (Task 3).
- Produces: `FuncDef::dyn_module_library`, `FuncDef::dyn_module_member` (both `std::string`; non-empty = a dynamic-module member); `Program::dl_library_spelling` (namespace → target spelling, filled by the binder in Task 5; at this task by the `#load` arm).

- [ ] **Step 1: Make the exe lane the failing test**

Delete `tests/testdlopen.exe_skip`. Run: `TESTS='testdlopen' scripts/remote_build.sh tests-all`
Expected: JIT PASS, `FAIL(exe)` / `FAIL(obj)` with an undefined `__dl_libc_abs` import.

- [ ] **Step 2: FuncDef fields**

`include/madc.h` after `std::string inline_builtin_kind;` (line 304):
```cpp
    // import (alias form): a member of a namespace bound to a dynamic module
    // by `import name as ns;`. Non-empty dyn_module_member marks the FuncDef;
    // the CIR builder lowers every call to a runtime-resolved indirect call
    // (__madc_dl_member(dyn_module_library, dyn_module_member)) — one
    // lowering for JIT and native. dyn_module_library is the TARGET
    // spelling the module map chose (madc_modules).
    std::string dyn_module_library;
    std::string dyn_module_member;
```
Add `dyn_module_library(), dyn_module_member(),` to the FuncDef constructor initializer list (line 442, after `inline_builtin_kind()`).

`include/madc.h:4191`: replace
```cpp
    std::map<std::string, void *> dl_symbol_map;
```
with
```cpp
    // import/#load: namespace -> the TARGET library spelling it is bound to
    // (what the alias-form call lowering passes to __madc_dl_member).
    std::map<std::string, std::string> dl_library_spelling;
```

- [ ] **Step 3: Stamp the fields at the two parser sites**

`src/parser.cpp:29803-29807` becomes:
```cpp
			std::string func_id = "__dl_" + aname + "_" + member_name;
			ns_var = addFunction(func_id,
			    datatype_vec_t{DataType::dtINT64}, (fVOIDFUNC)sym);
			stamp_dynamic_module_member(ns_var, aname, member_name);
			namespace_variables_for_write(aname)[member_name] = ns_var;
```
`src/parser.cpp:37701-37707` becomes:
```cpp
			std::string func_id = "__dl_" + ns_name + "_" + member_name;
			var = addFunction(func_id,
			    datatype_vec_t{DataType::dtINT64},
			    (fVOIDFUNC)sym);
			if ( !var )
			    Throw(member_tb) << "Failed to register dlsym function '" << member_name << "'" << flush;
			stamp_dynamic_module_member(var, ns_name, member_name);
			// Cache the resolved symbol for the next qualified call.
			namespace_variables_for_write(ns_name)[member_name] = var;
```
Add the helper (a `Program` member, declared in `include/madc.h` beside `is_auto_library_loading_enabled`) next to `Program::addFunction` in `src/parser.cpp`:
```cpp
// import (alias form): mark a freshly registered namespace member as a
// dynamic-module member so the CIR builder lowers its calls to the
// runtime-resolved shape. The library spelling is the one the binder
// recorded for the namespace (dl_library_spelling).
void Program::stamp_dynamic_module_member(Variable *var, const std::string &ns,
					  const std::string &member)
{
    FuncDef *fd = var ? dynamic_cast<FuncDef *>(var->type) : NULL;
    if ( !fd )
	return;
    std::map<std::string, std::string>::const_iterator it = dl_library_spelling.find(ns);
    fd->dyn_module_library = it != dl_library_spelling.end() ? it->second : ns;
    fd->dyn_module_member = member;
}
```
In the `#load` arm (`src/lexer.cpp:6478`, right after `dlopen_map[ns_name] = handle;`) add `dl_library_spelling[ns_name] = libname;` (Task 5 moves this into the shared binder).

- [ ] **Step 4: The CIR lowering**

`src/cir_builder.h` (near `need_output_extern`): `node_t dyn_module_callee(FuncDef *fd, const std::string &callee_name, TokenBase *origin);`

`src/cir_builder.cpp:22210-22262` — inside the `else` branch, after `std::string callee_name = call_target_emit_name(tcf, &cdf);` insert:
```cpp
				if (cdf && !cdf->dyn_module_member.empty()) {
					func_id = dyn_module_callee(cdf, callee_name, tb);
				} else {
```
and close the new `else {` after the existing `func_id = id(callee_name.c_str(), tb);` line (re-indent the enclosed block). Then add the method (beside `synth_host_trampoline`, ~line 28620):
```cpp
// import (alias form): the callee of `ns::member(args)` for a namespace
// bound to a dynamic module. One per-member slot (`static void *<id>_slot`)
// resolved on first use through the runtime helper:
//   (long (*)()) (slot ? slot : (slot = __madc_dl_member(LIB, MEMBER)))
// K&R unprototyped call, exactly the dlcall lowering's shape, so every lane
// performs the real call through the pointer. Replaces the JIT-only
// __dl_<ns>_<member> import (an address the JIT linker knew and no native
// artifact could — tests/testdlopen's old exe_skip).
node_t CirBuilder::dyn_module_callee(FuncDef *fd, const std::string &callee_name,
				     TokenBase *origin)
{
	std::string slot = callee_name + "_slot";
	if (!m_output_externs.count(slot)) {
		// static void *SLOT;  — rides the extern-declaration flush (the
		// same map, the same splice into the unit's top level).
		node_t specs = list();
		append(specs, simple(N_STATIC));
		append(specs, simple(N_VOID));
		node_t sd = simple(N_SPEC_DECL, origin);
		append(sd, node1(N_SHARE, specs));
		append(sd, node2(N_DECL, id(slot.c_str(), origin),
				 node1(N_LIST, pointer())));
		append(sd, ignore());
		append(sd, ignore());
		append(sd, ignore());
		m_output_externs[slot] = sd;
	}
	need_output_extern("__madc_dl_member", true,
			   { { {N_CHAR}, true }, { {N_CHAR}, true } });
	node_t args = list();
	append(args, str(fd->dyn_module_library.c_str(), fd->dyn_module_library.size(), origin));
	append(args, str(fd->dyn_module_member.c_str(), fd->dyn_module_member.size(), origin));
	node_t resolve = node2(N_CALL, id("__madc_dl_member", origin), args, origin);
	node_t assign = node2(N_ASSIGN, id(slot.c_str(), origin), resolve, origin);
	node_t pick = node3(N_COND, id(slot.c_str(), origin), id(slot.c_str(), origin), assign);
	node_t fdecl = list();
	append(fdecl, pointer());
	append(fdecl, node1(N_FUNC, list()));
	node_t fptr_t = node2(N_TYPE, i64_list(origin), node2(N_DECL, ignore(), fdecl));
	return node2(N_CAST, fptr_t, pick, origin);
}
```
Mirror the `N_SPEC_DECL` operand layout of the `bind_decl` at `src/cir_builder.cpp:~15488` (spec share, declarator, two ignores, initializer) — if that shape has a different operand count, copy it exactly; `ignore()` in the initializer position means "no initializer". Confirm `m_output_externs` values are spliced into the unit unconditionally (find the loop that appends its values; if it filters on a proto shape, register the slot there too).

Do NOT `referenced_funcs.insert(callee_name)` for the dynamic branch (there is no `__dl_` function to prototype).

- [ ] **Step 5: Delete the JIT-only binding**

`src/madc_cir.cpp`: remove `cir_active_dl_syms` (lines 125-129 comment + declaration, the lookup at 202-207, the resets at 1084 and 1144, the set at 1120). Build with `-Wall`: the compiler confirms nothing else reads `dl_symbol_map` (the header member is gone).

- [ ] **Step 6: Build, run the reducer in all lanes, and the neighbours**

Run: `scripts/remote_build.sh sync build`; `TESTS='testdlopen testdlcall testdlfcnportable testlibc testnoautoload' scripts/remote_build.sh tests-all`
Expected: all PASS in JIT, exe and obj; `testdlopen` prints `abs(-42): 42`, `abs(100): 100`, `atoi: 999` in every lane. Also run `scripts/remote_build.sh unittest` (the CIR unit batteries).

- [ ] **Step 7: Emitted-C sanity**

Run on the container: `bin/madc --emit=c11 tests/testdlopen.mad | grep -n -E '__madc_dl_member|_slot'`
Expected: one `static void *__dl_libc_abs_slot;` declaration, the extern `void *__madc_dl_member(char *, char *);`, and the conditional callee at each call.

- [ ] **Step 8: Commit** (trailers; `Layer: ns::member call -> CIR callee selection (call_target_emit_name -> id(__dl_...)) -> JIT-only address binding in cir_import_resolver; the callee LOWERING is the deepest layer (the import name was the shim)`, `Oracle: gcc/clang have no dynamic-namespace form; the oracle is the K&R indirect call shape they emit for ((long(*)())p)(args) — testdlopen output identical in JIT/exe/obj`).

---

### Task 5: The `import` directive

**Files:**
- Modify: `include/madc.h` (Program members + declarations), `src/lexer.cpp:8072` (identifier arm hook), `src/lexer.cpp:9355-9390` (`getRealToken` records the line), `src/lexer.cpp:6424-6480` (`#load` → shared binder), new handler functions beside `tokenize_synthetic_system_include` (~line 1572)
- Test: `tests/testimport.mad` + `.expect`; `tests/testimportiface.mad` + `.flags` + `.expect`; `tests/testimportiface_neg.mad` + `.flags` + `.expect_err`

**Interfaces:**
- Consumes: `madc_module_find`, `madc_module_library_spelling`, `madc_module_open` (Task 1); `dl_library_spelling` (Task 4).
- Produces: `Program::module_link_libs` (`std::vector<std::string>`, TARGET spellings of interface-form modules, for Task 6); `bool Program::import_directive_position()`; `TokenBase *Program::tokenize_import_directive()`; `void Program::bind_module_namespace(const std::string &ns, const std::string &spelling, bool link_form)`.

- [ ] **Step 1: Write the failing tests**

`tests/testimport.mad` (madc dialect, alias form; value-first: bare `println`/`format`):
```c
// import (alias form): `import c as libc;` binds the C runtime under a
// namespace with NO platform spelling in the source — the module map picks
// libc.so.6 / libSystem.B.dylib / ucrtbase.dll per target. Members resolve
// by name at first call through __madc_dl_member (the same lowering in JIT,
// exe and obj — this test has NO exe_skip). `import` is contextual: the
// identifier `import` stays an identifier off the directive position.
import c as libc;

int main()
{
    int import = 3;
    println(format("abs(-42): {}", libc::abs(-42)));
    println(format("abs(100): {}", libc::abs(100)));
    var num = "999";
    println(format("atoi: {}", libc::atoi(num)));
    println(format("import={}", import));
    return 0;
}
```
`tests/testimport.expect`:
```
abs(-42): 42
abs(100): 100
atoi: 999
import=3
```
`tests/testimportiface.mad` (interface form + C++20 header-unit spelling; strict C++ mode, so C I/O):
```c++
// import (interface form) under --std=c++20: `import m;` tokenizes the
// module's interface (<math.h>: M_PI is a MACRO no fallback can supply) and
// links libm by the target's spelling; `import <stdio.h>;` is the C++20
// header-unit spelling (approximated as the include). `int import` stays an
// identifier: `import` is a directive only at the start of a logical line
// followed by a module name, '<' or '"'.
import <stdio.h>;
import m;

int main()
{
    int import = 3;
    printf("pi=%.5f\n", M_PI);
    printf("sqrt=%g\n", sqrt(16.0));
    printf("import=%d\n", import);
    return 0;
}
```
`tests/testimportiface.flags`: `--std=c++20`
`tests/testimportiface.expect`:
```
pi=3.14159
sqrt=4
import=3
```
`tests/testimportiface_neg.mad` (the negative control — the interface is what brought `M_PI`):
```c++
// Negative control for testimportiface: without `import m;` (and no
// #include <math.h>) M_PI is an undeclared identifier under --std=c++20.
// Pins that the interface-form import is what supplies the declarations.
import <stdio.h>;

int main()
{
    printf("pi=%.5f\n", M_PI);
    return 0;
}
```
`tests/testimportiface_neg.flags`: `--std=c++20`
`tests/testimportiface_neg.expect_err`: one line, `M_PI` (the diagnostic names the identifier; write the fixture AFTER seeing the message in Step 6 — it must be a substring of stderr).

- [ ] **Step 2: Run them to verify they fail**

Run: `TESTS='testimport*' scripts/remote_build.sh tests`
Expected: `testimport` FAIL (parse error at `import c as libc;`), `testimportiface` FAIL, `testimportiface_neg` — currently FAIL too (it must exit nonzero WITH `M_PI` in stderr; today `import <stdio.h>;` itself is the error).

- [ ] **Step 3: Program members**

`include/madc.h` — beside `loaded_lib_paths` (line 4192):
```cpp
    // import (interface form): the TARGET spellings of the modules whose
    // library must join a native artifact's link closure (DT_NEEDED / load
    // command / PE import); madc.cpp appends them to the link line after the
    // parse. The alias form resolves at runtime and never lands here.
    std::vector<std::string> module_link_libs;
    // The line of the last non-trivia token getRealToken returned: the
    // import directive is recognized only as the FIRST token of a logical
    // line ([cpp.pre]: `import` is an identifier with special meaning in
    // directive position — never a reserved word).
    int _last_real_token_line = 0;
```
Declarations beside `tokenize_synthetic_system_include` (line 5443):
```cpp
	bool import_directive_position();
	TokenBase *tokenize_import_directive();
	void bind_module_namespace(const std::string &ns, const std::string &spelling,
				   bool link_form);
```

- [ ] **Step 4: Record the line in getRealToken**

`src/lexer.cpp` in `getRealToken()`'s `default:` arm (line ~9384), before `return tb;`:
```cpp
		_last_real_token_line = tb->line;
```

- [ ] **Step 5: The identifier-arm hook**

`src/lexer.cpp:8072` — immediately BEFORE `if ( (kmi=keyword_map.find(sid)) != keyword_map.end() )`:
```cpp
		// C++20 [cpp.pre]: `import` is a DIRECTIVE when it heads a logical
		// line and a module name, '<' or '"' follows; an identifier
		// otherwise (`int import = 3;` compiles). Gated: the madc dialect
		// and C++20+ only; never in C or earlier C++.
		if ( word == "import" && import_directive_position() )
		    return tokenize_import_directive();
```

- [ ] **Step 6: The handler and the shared binder**

Add after `Program::tokenize_synthetic_system_include` (`src/lexer.cpp:~1588`):
```cpp
// The directive position test: gate, line head, and the token that follows.
// Consumes only horizontal whitespace (trivia either way).
bool Program::import_directive_position()
{
	if ( !cpp_keyword_active(STD_CPP20) )
		return false;
	if ( source.line() == _last_real_token_line )
		return false;
	while ( source.peek() == ' ' || source.peek() == '\t' )
		source.get();
	int c = source.peek();
	return c == '<' || c == '"' || isalpha(c) || c == '_';
}

// import <header>;  import "header";  import module[.part] [as alias];
// The header-unit forms reuse the include machinery verbatim (a header
// unit's declarations, plus its macros — the documented approximation).
// A module: its interface (a registry row's embedded header) is tokenized
// FIRST, then its library is bound. Both arrive through pushback so the
// ONE directive reader serves them.
TokenBase *Program::tokenize_import_directive()
{
	auto skip_ws = [this]() {
		while ( source.peek() == ' ' || source.peek() == '\t' )
			source.get();
	};
	auto finish_semicolon = [this, &skip_ws]() {
		skip_ws();
		if ( source.peek() != ';' )
			Throw << "import: expected ';'" << flush;
		source.get();
	};
	skip_ws();
	int c = source.peek();
	if ( c == '<' || c == '"' )
	{
		char close = c == '<' ? '>' : '"';
		source.get();
		std::string header;
		while ( source.good() && !source.eof() && source.peek() != close
		     && source.peek() != '\n' && source.peek() != '\r' )
			header += source.get();
		if ( source.peek() != close )
			Throw << "import: unterminated header name" << flush;
		source.get();
		finish_semicolon();
		source.pushback_reread(std::string("#include ") + (close == '>' ? "<" : "\"")
				       + header + (close == '>' ? ">" : "\"") + "\n");
		return getToken();
	}
	std::string name;
	while ( source.good() && !source.eof()
	     && (isalnum(source.peek()) || source.peek() == '_' || source.peek() == '.') )
		name += source.get();
	if ( name.empty() )
		Throw << "import: expected a module name" << flush;
	skip_ws();
	std::string alias;
	if ( source.peek() == 'a' )
	{
		std::string kw;
		while ( source.good() && !source.eof() && isalpha(source.peek()) )
			kw += source.get();
		if ( kw != "as" )
			Throw << "import: expected 'as' or ';' after module name '" << name << "'" << flush;
		skip_ws();
		while ( source.good() && !source.eof()
		     && (isalnum(source.peek()) || source.peek() == '_') )
			alias += source.get();
		if ( alias.empty() )
			Throw << "import: expected an alias after 'as'" << flush;
	}
	finish_semicolon();
	const MadcModuleSpec *row = madc_module_find(name);
	std::string spelling = madc_module_library_spelling(name);
	if ( alias.empty() && !(row && row->interface) )
		Throw << "import: module '" << name << "' has no interface; bind it with"
		      << " `import " << name << " as <alias>;`" << flush;
	if ( !alias.empty() )
		bind_module_namespace(alias, spelling, /*link_form=*/false);
	else
		bind_module_namespace(std::string(), spelling, /*link_form=*/true);
	DBG(std::cout << "import " << name << (alias.empty() ? "" : " as " + alias)
		      << " -> " << spelling << std::endl);
	if ( row && row->interface )
		source.pushback_reread(std::string("#include <") + row->interface + ">\n");
	return getToken();
}

// The binder shared by `import` and (until it retires) `#load`. JIT: open
// the spelled library into the default symbol scope (or, under
// --no-auto-load / an emit-only cross madc, bind to the program's own
// scope — the library is linked, not loaded). A namespace (alias form)
// records the spelling the member lowering passes to __madc_dl_member; the
// link form records the spelling for the native link closure.
void Program::bind_module_namespace(const std::string &ns, const std::string &spelling,
				    bool link_form)
{
	void *handle = NULL;
	bool opened = false;
#ifndef MADC_CROSS_TARGET
	if ( is_auto_library_loading_enabled() )
	{
		std::string err;
		handle = madc_module_open(spelling, err);
		if ( !handle )
			Throw << "import: cannot load '" << spelling << "': " << err << flush;
		opened = true;
	}
#endif
	if ( !handle )
		handle = madcdl_open_self();
	if ( opened )
		loaded_lib_paths.push_back(spelling);
	if ( link_form )
		module_link_libs.push_back(spelling);
	if ( !ns.empty() )
	{
		dlopen_map[ns] = handle;
		dl_library_spelling[ns] = spelling;
		namespace_variables_for_write(ns);
	}
}
```
Add `#include "madc_modules.h"` to `src/lexer.cpp`. Confirm `Source::pushback_reread(const std::string &)` is the synthesized-text pushback (`include/madc.h:~1740`, used at `src/lexer.cpp:8069` for consumed type words); if the newline handling of pushback text needs the line counter untouched, use the form the `#include` synthesizer uses (`tokenize_synthetic_system_include`) for the interface header instead of pushback — the OBSERVABLE contract is the same: the header's tokens arrive before the next source token.

Route `#load` through the binder: `src/lexer.cpp:6455-6479` (from `// dlopen the library` to `namespace_variables_for_write(ns_name);`) becomes:
```cpp
		    // #load "spelling" as ns; — sugar over the import binder with a
		    // VERBATIM spelling (retires with the import slice; testdlopen).
		    bind_module_namespace(ns_name, libname, /*link_form=*/false);
		    return getToken();
```

- [ ] **Step 7: Build, run the reducers in all lanes**

Run: `scripts/remote_build.sh sync build`; `TESTS='testimport* testdlopen testnoautoload' scripts/remote_build.sh tests-all`
Expected: `testimport`, `testimportiface`, `testdlopen`, `testnoautoload` PASS in JIT, exe, obj; `testimportiface_neg` exits nonzero with a diagnostic naming `M_PI` — copy the identifier into `tests/testimportiface_neg.expect_err` now if it is not already there, re-run, PASS.

- [ ] **Step 8: The contextual-identifier guard across the suite's C++ headers**

Run: `TESTS='testcout* testvector* teststring* testmap*' scripts/remote_build.sh tests` (real libstdc++ headers parsed under the C++20 presentation).
Expected: unchanged PASS — no header line starts with an identifier `import` followed by a name.

- [ ] **Step 9: Commit** (trailers; `Hypothesis: #load put the platform's business in the source and was JIT-only; the C++20 import made whole (interface + binding), recognized in directive position and gated by --std=, resolves both through the module map; Layer: source directive -> module resolver -> madc_dl (the directive reader is the deepest NEW layer; the spelling owner and the loader already exist below it); Searched: git grep -n '"import"\|kwIMPORT' src include (concept: an existing import/module keyword path) -> none; the registry comment names import contextual by decision; Oracle: g++ -std=c++20 -fmodules-ts accepts import <stdio.h>; and int import = 3; — madc now does both; the module form has no gcc twin (the standard leaves binding to the build)`).

---

### Task 6: Interface-form modules join the native link closure

**Files:**
- Modify: `src/madc.cpp:~1425` (after `bool parse_ok = prog->parse(tp);`), `src/madc_cir.cpp:1940-1975` (Apple load list), `src/madc_cir.cpp:6255+` (`madc_project_emit_native` per-TU collection)

**Interfaces:**
- Consumes: `Program::module_link_libs` (Task 5).

- [ ] **Step 1: Single-TU lane**

`src/madc.cpp`, right after the parse succeeds (line ~1425-1430):
```cpp
	// import (interface form): the modules' TARGET spellings join the
	// native link closure exactly as -l spellings do (verbatim entries).
	for ( const std::string &l : prog->module_link_libs )
	    cc_link_args.push_back(l);
```

- [ ] **Step 2: Apple load commands for user libraries**

`src/madc_cir.cpp` `cir_write_native_image`, APPLE branch (after `cir_apple_extra_dylibs(imports, libs);`):
```cpp
    // A user library (-l / import) is a real load command on Mach-O: the
    // cover stems (libc++, libsystem_, libSystem) never are, and libSystem
    // is implicit. The writer binds imports flat across the load list.
    for (const std::string &l : other)
	if (l.size() > 6 && l.compare(l.size() - 6, 6, ".dylib") == 0
	    && l != "libSystem.B.dylib" && l.compare(0, 6, "libc++") != 0)
	    libs.push_back(l.c_str());
```

- [ ] **Step 3: Project lane**

In `madc_project_emit_native` (`src/madc_cir.cpp:6255+`), where `user_libs` is handed to `cir_native_link_env`, build the merged list first:
```cpp
	std::vector<std::string> all_libs(user_libs);
	for (CirParsedTU &pt : parsed)
	    for (const std::string &l : pt.prog->module_link_libs)
		if (std::find(all_libs.begin(), all_libs.end(), l) == all_libs.end())
		    all_libs.push_back(l);
```
and pass `all_libs` in place of `user_libs` to `cir_native_link_env` (read the exact member name of the parsed TU's Program in `CirParsedTU` at implementation time — the struct is defined in the same file).

- [ ] **Step 4: Verify**

Run: `scripts/remote_build.sh sync build`; `TESTS='testimportiface testimport' scripts/remote_build.sh tests-all`; on the container: `bin/madc --exe -o tmp/imp/iface tests/testimportiface.mad` then `readelf -d tmp/imp/iface | grep NEEDED`.
Expected: all lanes PASS; the ELF lists `libm.so.6` once (no duplicate).

- [ ] **Step 5: Commit** (trailers; `Oracle: gcc -lm records libm.so.6 as DT_NEEDED — readelf on madc's artifact shows the same`).

---

### Task 7: Retire `#load`; migrate its tests; docs, grammar, rules

**Files:**
- Modify: `src/lexer.cpp:6424-6455` (delete the `#load` arm), `src/madc.cpp:423,559` (help text), `include/madc/ns_madc:338-344` (comment), `scripts/c-abi-internal-exports.txt:23-25` (comments), `tests/testnoautoload.mad` (migrate to `import m;`), `docs/language/preprocessor.md:80-91`, `docs/language/overview.md:58` (new row), `docs/language/embedded-headers.md`, `docs/language/sys-object.md`, `docs/man/madc.1`, `docs/grammar/madc.ebnf:48,57-59`, `docs/architecture.md`, `.claude/rules/embedded-headers.md` (step 3), `docs/rules/embedded-headers.md`, `docs/plans/cpp-support.md` (a Modules line)
- Create: `docs/language/import.md`
- Delete: `tests/testdlopen.mad`, `tests/testdlopen.darwin_skip`, `tests/testdlopen.win64_skip` (its content lives on as `tests/testimport.mad`)

- [ ] **Step 1: Migrate testnoautoload**

`tests/testnoautoload.mad` header comment and body become:
```c
// --no-auto-load: `import m;` must NOT open libm (the flag says the library
// is linked, not loaded — the JIT spelling of the native "link" policy) and
// must bind to the program's own symbol scope; sqrt still resolves because
// madc is itself linked against libm, proving the skip path keeps
// resolution intact. Fixture: tests/testnoautoload.flags.
import m;
#include <stdio.h>

int main()
{
	double x = sqrt(16.0);
	printf("sqrt=%g\n", x);
	return 0;
}
```
(keep the file's existing `.flags` and `.expect`; add `--std=c++20` to the flags only if `import` is not already active — the madc dialect activates it, so leave the flags as they are).

- [ ] **Step 2: Delete the `#load` arm and the dlopen test**

Remove `src/lexer.cpp:6424-6455` (`if ( directive == "load" ) { ... }` whole arm). `git rm tests/testdlopen.mad tests/testdlopen.darwin_skip tests/testdlopen.win64_skip`. Build with `-Wall`; grep the tree: `git grep -n '#load' -- src include tests` must return only the `<ns_madc>` comment (fixed in Step 4) and nothing executable.

- [ ] **Step 3: Grammar**

`docs/grammar/madc.ebnf`: remove `| LoadDirective` (line 48) and the `LoadDirective` production (57-59); add at the top-level declaration alternatives:
```
/* C++20 [cpp.pre] made whole (madc + C++20 and later): a module's interface
   AND its library bind; `import` is a directive only at the head of a
   logical line, an identifier elsewhere. Header-unit spellings include. */
ImportDirective  ::= 'import' ( ModuleName [ 'as' Identifier ]
                              | '<' HeaderName '>'
                              | StringLiteral ) ';'
ModuleName       ::= Identifier { '.' Identifier }
```

- [ ] **Step 4: Docs**

`docs/language/import.md` (new): the three forms with examples (the two reducers), the module map rows (`c`, `m`) and the platform rule, the JIT vs native binding policy, `--no-auto-load`, the contextual rule (`int import` compiles), `-l` as the build-system spelling of the same binding, the header-unit approximation (macros leak), and "was `#load` — retired 2026-09 (v0.99)". `docs/language/preprocessor.md:80-91`: replace the `#load` section with two lines pointing at `import.md`. `docs/language/overview.md`: add `| `import name [as ns];` — module binding (interface + library, platform-agnostic) | [import.md](import.md) |` after the function-pointers row. `docs/man/madc.1`: replace the `#load` lines with `import` and the `--no-auto-load` text ("do not act on `import` library bindings"). `src/madc.cpp:423` help text likewise. `docs/language/embedded-headers.md`, `docs/architecture.md`, `docs/language/sys-object.md`, `include/madc/ns_madc:341`: `#load` → `import`. `.claude/rules/embedded-headers.md` step 3 → "Use `import <module>;` (a module map row in `src/madc_modules.cpp`) if the header needs a shared library"; `docs/rules/embedded-headers.md` gets the reasoning line (the platform spelling lives in the map, never in a header). `scripts/c-abi-internal-exports.txt:23-25`: `#load/dlopen runtime entry` → `script dlopen()/dlsym() runtime entry`. `docs/plans/cpp-support.md`: one line under the C++20 section: "Modules — `import` directive landed (interface + binding, the resolver serves module interface units later); `export module` / BMI: open."

- [ ] **Step 5: Verify**

Run: `scripts/remote_build.sh sync build`; `TESTS='testimport* testnoautoload testdlcall testdlfcnportable testlibc' scripts/remote_build.sh tests-all`; `bash scripts/check-dialect-lean.sh`; `bash scripts/check-rule-trailers.sh` (both in fulltest; run them early).
Expected: all PASS; `git grep -n '#load' -- src include tests include/madc` empty.

- [ ] **Step 6: Commit** (two commits: the code retirement with trailers — `Layer: the #load arm was sugar over bind_module_namespace since Task 5; deleting it leaves one directive over one resolver`; the docs/rules/grammar as a docs commit).

---

### Task 8: Merge wave, mirrors, hand-off

- [ ] **Step 1: The battery ONCE** — `scripts/remote_build.sh battery` (fulltest + exe + obj + release + packed + headerless), then `scripts/remote_build.sh win release-win wine`, `scripts/remote_build.sh c-testsuite`, `scripts/remote_build.sh macos` (the darwin cross build + forest pack gate; the win64 domain proves `ucrtbase.dll`; the darwin suite lane runs at the release tier). Record each green with `scripts/lane_ledger.sh record <lane> "<tally>"`. Fix, never skip, anything red (a platform failure is fixed or formally skipped with a stated reason).
- [ ] **Step 2: `/dupaudit` scoped to `src/madc_modules.cpp src/lexer.cpp src/madc.cpp src/madc_cir.cpp` (the spelling family: exactly one owner must remain).**
- [ ] **Step 3: Mirrors** — `CHANGELOG.md` `[Unreleased]`: the `import` entry (features first); `docs/plans/ROADMAP.md` row 1.8 → landed; `claude_status.json` `current_focus`/`in_progress`; KG: `Feature import_module_binding` (landed, commit hashes), `Gap dso_suffix_no_windows_branch` → fixed, `Gap load_directive_jit_only` → fixed, `Decision import_module_binding_replaces_load` gets `landed=`; the design doc §5 slice 0 gets a LANDED line.
- [ ] **Step 4: Merge** `feature/import-module-binding-claude` → `develop` (the pre-push hook runs `check --promote`); hand-off note: branch, tree state, validation, what slice 1 (the spike) needs.

---

## Self-review

- **Spec coverage (§3.1):** module map as data → Task 1; ONE platform-spelling owner shared with `-l` → Tasks 1–2; per-target binding policy (JIT open / link-time import / runtime open) → Tasks 4–6 (the startup-time-open policy for the interface form on "open"-policy targets is the Android slice's — recorded in §6, not here); namespaces from the interface, `as alias` secondary form → Task 5; gating via the registry predicate → Task 5; `#load` sugar day one → Task 5 Step 6, retire → Task 7; the `exe_skip` follow-on closed → Task 4; Windows suffix gap → Tasks 1–2; reducers cross-platform → Tasks 5–7; docs/rules/grammar → Task 7.
- **Placeholders:** none; the one deferred value (`.expect_err` line) is captured by an explicit step.
- **Type consistency:** `TargetOS::{Posix,Darwin,Windows}`; `madc_module_library_spelling(name)` / `(name, os)`; `madc_module_open(spelling, err)`; `FuncDef::dyn_module_library/dyn_module_member`; `Program::dl_library_spelling`, `module_link_libs`, `_last_real_token_line`, `bind_module_namespace(ns, spelling, link_form)`, `stamp_dynamic_module_member(var, ns, member)`; `__madc_dl_member(lib, member)`; slot = `<callee_name>_slot`.
