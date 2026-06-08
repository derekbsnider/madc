# madc Project Build Driver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let one `madc` process read a `compile_commands.json`, compile each translation unit independently, link the resulting MIR modules, and JIT-run `main` — replacing the SMAUG umbrella shim.

**Architecture:** A `ProjectManifest` data model (list of TUs + per-file flags + entry) sits between a pluggable *reader* (v1: `compile_commands.json`) and a *multi-TU engine*. The engine creates a fresh `Program` per TU (correct C TU isolation), builds one MIR module per TU into a shared `MIR_context` via the existing `c2mir_compile_tree` path, then does the single `MIR_link` already present in `madc_cir_execute` and runs `main`.

**Tech Stack:** C++11, c2mir + libmir (madc fork at `/workspace/mir`), doctest unit tests, the `tests/*.mad` + fixture integration runner.

**Spec:** `docs/superpowers/specs/2026-06-08-madc-project-build-driver-design.md`

**Branch:** create `feature/project-build-driver-claude` off the current HEAD (`25441b2`, on `feature/realhdr-parse-gaps2-claude`). The engine reuses CIR plumbing only; it does not depend on the Codex parser work, but basing on current HEAD keeps SMAUG reachable later.

**Build/test note:** `./configure --enable-madcdat=no` first (core-only). `touch` an edited file before `make -C src` (NAS mtime trap). Cap every run: `( ulimit -t 120; timeout 180 <cmd> )`.

---

## File structure (locked)

| File | Responsibility |
|------|----------------|
| `include/madc_project.h` (create) | `ProjectTU`, `ProjectManifest` structs; `read_compile_commands(...)` decl; `madc_project_execute(...)` decl. |
| `src/madc_project.cpp` (create) | Minimal JSON parse + `read_compile_commands` (compile_commands.json → `ProjectManifest`). NO MIR code here. |
| `src/madc_cir.cpp` (modify) | Add `build_tu_module(...)` helper (extracted from `madc_cir_execute`); add `madc_project_execute(...)` (loop TUs → modules → link → run). Reuses the file-static `cir_import_resolver`. |
| `src/madc.cpp` (modify) | Add `--project <manifest>` CLI flag → call `madc_project_execute`. |
| `src/Makefile` (modify) | Add `madc_project.o` to the object list. |
| `tests/unit/test_project_manifest.cpp` (create) | doctest unit tests for `read_compile_commands`. |
| `tests/testproject*.{mad,flags,cc.json,c,expect}` (create) | Integration fixture: the two-TU spike through madc. |

---

## Task 1: ProjectManifest data model + minimal JSON value parser

**Files:**
- Create: `include/madc_project.h`
- Create: `src/madc_project.cpp`
- Test: `tests/unit/test_project_manifest.cpp`

- [ ] **Step 1: Create the header with the data model**

`include/madc_project.h`:
```cpp
#ifndef __MADC_PROJECT_H
#define __MADC_PROJECT_H 1

#include <string>
#include <vector>

// One translation unit in a project build, plus the flags it compiles with.
struct ProjectTU {
	std::string file;			// resolved (absolute or manifest-relative) path
	std::string working_dir;		// the entry's "directory" (for -I resolution)
	std::vector<std::string> defines;	// "NAME" or "NAME=VALUE"
	std::vector<std::string> include_dirs;	// -I paths
	std::string std_option;			// e.g. "c89"; empty = madc default
};

// A whole project: the TU list + how to link/run it.
struct ProjectManifest {
	std::vector<ProjectTU> tus;
	std::string entry = "main";	// entry symbol
	std::string output_name;	// informational in v1 (no object output)
};

// Reader: parse a compile_commands.json at `path` into `out`.
// Returns true on success; on failure returns false and sets `err`.
bool read_compile_commands(const std::string &path,
			   ProjectManifest &out, std::string &err);

// Forward decls to avoid pulling madc.h into the header.
class MadcEngine;

// Engine: build+link+JIT-run the manifest. Returns the program's exit code,
// or -1 on a build/link error. Defined in madc_cir.cpp.
int madc_project_execute(MadcEngine &engine, const ProjectManifest &manifest,
			 int user_argc, char **user_argv);

#endif
```

- [ ] **Step 2: Write the failing unit test for a minimal JSON string-array parse**

The reader needs to read `command` (a single string) or `arguments` (array of strings). Start with the JSON primitive we depend on: extracting string values and string arrays. Add to `tests/unit/test_project_manifest.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "doctest.h"
#include "madc_project.h"
#include <cstdio>

// Helper: write a temp file, return its path (in /tmp, unique per test name).
static std::string write_tmp(const char *name, const std::string &content) {
	std::string path = std::string("/tmp/madc_test_") + name;
	FILE *f = fopen(path.c_str(), "wb");
	REQUIRE(f != nullptr);
	fwrite(content.data(), 1, content.size(), f);
	fclose(f);
	return path;
}

TEST_CASE("read_compile_commands: single TU, command string") {
	std::string json =
		"[{\"directory\":\"/proj\",\"file\":\"a.c\","
		"\"command\":\"gcc -c -DFOO -DBAR=2 -I/proj/inc -std=c89 a.c -o a.o\"}]";
	std::string path = write_tmp("cc_single.json", json);
	ProjectManifest m; std::string err;
	REQUIRE(read_compile_commands(path, m, err));
	REQUIRE(m.tus.size() == 1);
	CHECK(m.tus[0].working_dir == "/proj");
	// file resolved against directory:
	CHECK(m.tus[0].file == "/proj/a.c");
	CHECK(m.tus[0].std_option == "c89");
	// defines preserved verbatim (NAME or NAME=VALUE):
	REQUIRE(m.tus[0].defines.size() == 2);
	CHECK(m.tus[0].defines[0] == "FOO");
	CHECK(m.tus[0].defines[1] == "BAR=2");
	REQUIRE(m.tus[0].include_dirs.size() == 1);
	CHECK(m.tus[0].include_dirs[0] == "/proj/inc");
}
```

- [ ] **Step 3: Run the test to verify it fails (link error / no impl)**

Run: `make -C src test 2>&1 | tail -20`
Expected: build fails — `read_compile_commands` undefined (no `src/madc_project.cpp` impl yet). (If the unit Makefile target needs the new test file registered, see Task 5's Makefile note; for now the failure may be "undefined reference".)

- [ ] **Step 4: Implement the minimal JSON reader in `src/madc_project.cpp`**

Scope: `compile_commands.json` only — a JSON array of objects whose values are strings or arrays of strings. No numbers, no nesting beyond that. Implementation outline (write it fully):
```cpp
#include "madc_project.h"
#include <fstream>
#include <sstream>
#include <cctype>

namespace {
// Tiny scanner over the JSON text. Only supports: arrays, objects,
// double-quoted strings (with \" \\ \/ \n \t escapes). Sufficient for
// compile_commands.json.
struct JsonScan {
	const std::string &s; size_t i = 0;
	explicit JsonScan(const std::string &str) : s(str) {}
	void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
	bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
	bool peek(char c) { ws(); return i < s.size() && s[i] == c; }
	bool str(std::string &out) {
		ws();
		if (i >= s.size() || s[i] != '"') return false;
		i++; out.clear();
		while (i < s.size() && s[i] != '"') {
			char c = s[i++];
			if (c == '\\' && i < s.size()) {
				char e = s[i++];
				switch (e) {
				case 'n': out += '\n'; break;
				case 't': out += '\t'; break;
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				default: out += e; break;
				}
			} else out += c;
		}
		if (i >= s.size()) return false;
		i++; // closing quote
		return true;
	}
};

// Shell-split a `command` string into argv-like tokens. Honors simple
// single/double quotes (compile_commands.json rarely needs more).
void shell_split(const std::string &cmd, std::vector<std::string> &out) {
	size_t i = 0; std::string cur; bool in = false;
	char q = 0;
	while (i < cmd.size()) {
		char c = cmd[i++];
		if (q) { if (c == q) q = 0; else cur += c; continue; }
		if (c == '"' || c == '\'') { q = c; in = true; continue; }
		if (std::isspace((unsigned char)c)) {
			if (in) { out.push_back(cur); cur.clear(); in = false; }
			continue;
		}
		cur += c; in = true;
	}
	if (in) out.push_back(cur);
}

// Resolve a (possibly relative) path against a base directory.
std::string resolve(const std::string &base, const std::string &p) {
	if (p.empty() || p[0] == '/') return p;
	if (base.empty()) return p;
	std::string b = base;
	if (b.back() != '/') b += '/';
	return b + p;
}

// Scan argv tokens for -D/-I/-std=, filling the TU. Ignores everything else.
void apply_args(const std::vector<std::string> &args,
		const std::string &dir, ProjectTU &tu) {
	for (size_t k = 0; k < args.size(); k++) {
		const std::string &a = args[k];
		if (a.rfind("-D", 0) == 0) {
			std::string v = a.substr(2);
			if (v.empty() && k + 1 < args.size()) v = args[++k];
			if (!v.empty()) tu.defines.push_back(v);
		} else if (a.rfind("-I", 0) == 0) {
			std::string v = a.substr(2);
			if (v.empty() && k + 1 < args.size()) v = args[++k];
			if (!v.empty()) tu.include_dirs.push_back(resolve(dir, v));
		} else if (a.rfind("-std=", 0) == 0) {
			tu.std_option = a.substr(5);
		}
	}
}
} // namespace

bool read_compile_commands(const std::string &path,
			   ProjectManifest &out, std::string &err) {
	std::ifstream f(path.c_str(), std::ios::binary);
	if (!f) { err = "cannot open " + path; return false; }
	std::stringstream ss; ss << f.rdbuf();
	std::string text = ss.str();

	JsonScan js(text);
	if (!js.eat('[')) { err = "expected '[' at top level"; return false; }
	if (js.peek(']')) { js.eat(']'); return true; } // empty array
	do {
		if (!js.eat('{')) { err = "expected '{' for entry"; return false; }
		ProjectTU tu;
		std::string command;
		std::vector<std::string> arguments;
		bool have_args = false, have_cmd = false;
		do {
			std::string key;
			if (!js.str(key)) { err = "expected object key"; return false; }
			if (!js.eat(':')) { err = "expected ':' after key"; return false; }
			if (key == "arguments") {
				if (!js.eat('[')) { err = "expected '[' for arguments"; return false; }
				have_args = true;
				if (!js.peek(']')) {
					do { std::string v; if (!js.str(v)) { err="bad arg"; return false; }
					     arguments.push_back(v); } while (js.eat(','));
				}
				if (!js.eat(']')) { err = "expected ']'"; return false; }
			} else {
				std::string val;
				if (!js.str(val)) { err = "expected string value for " + key; return false; }
				if (key == "file") tu.file = val;
				else if (key == "directory") tu.working_dir = val;
				else if (key == "command") { command = val; have_cmd = true; }
				// "output" etc. ignored
			}
		} while (js.eat(','));
		if (!js.eat('}')) { err = "expected '}'"; return false; }

		tu.file = resolve(tu.working_dir, tu.file);
		std::vector<std::string> args;
		if (have_args) args = arguments;
		else if (have_cmd) shell_split(command, args);
		apply_args(args, tu.working_dir, tu);
		out.tus.push_back(tu);
	} while (js.eat(','));
	if (!js.eat(']')) { err = "expected ']' to close array"; return false; }
	return true;
}
```

- [ ] **Step 5: Register the test + reader in the build, run unit tests**

See the Makefile note in Task 5 Step 1 if `test_project_manifest` is not yet built. Then:
Run: `( ulimit -t 120; timeout 180 make -C src test ) 2>&1 | tail -20`
Expected: the new `read_compile_commands` test PASSES.

- [ ] **Step 6: Add an `arguments`-array test and a multi-TU test**

Add to `tests/unit/test_project_manifest.cpp`:
```cpp
TEST_CASE("read_compile_commands: arguments array + two TUs") {
	std::string json =
		"[{\"directory\":\"/p\",\"file\":\"a.c\","
		" \"arguments\":[\"gcc\",\"-c\",\"-DA\",\"a.c\"]},"
		" {\"directory\":\"/p\",\"file\":\"b.c\","
		" \"arguments\":[\"gcc\",\"-c\",\"-DB\",\"b.c\"]}]";
	std::string path = write_tmp("cc_two.json", json);
	ProjectManifest m; std::string err;
	REQUIRE(read_compile_commands(path, m, err));
	REQUIRE(m.tus.size() == 2);
	CHECK(m.tus[0].file == "/p/a.c");
	CHECK(m.tus[1].file == "/p/b.c");
	REQUIRE(m.tus[0].defines.size() == 1);
	CHECK(m.tus[0].defines[0] == "A");
	CHECK(m.tus[1].defines[0] == "B");
	CHECK(m.entry == "main");
}
```
Run: `( ulimit -t 120; timeout 180 make -C src test ) 2>&1 | tail -20`  → PASS.

- [ ] **Step 7: Commit**
```bash
git add include/madc_project.h src/madc_project.cpp tests/unit/test_project_manifest.cpp src/Makefile
git commit -m "feat(project): ProjectManifest + compile_commands.json reader"
```

---

## Task 2: Extract `build_tu_module` from `madc_cir_execute` (no behavior change)

Refactor so the per-TU "Program → tree → module" steps are reusable by both the single-file path and the project engine (no-parallel-implementations rule).

**Files:**
- Modify: `src/madc_cir.cpp` (the body of `madc_cir_execute`, around lines 118–239)

- [ ] **Step 1: Add the helper above `madc_cir_execute`**

`src/madc_cir.cpp` (new static function; mirrors the existing translate→gate→compile steps):
```cpp
// Build one MIR module for a single parsed Program. Returns the module on
// success, or nullptr after printing a diagnostic. `c2m`/`ctx` are shared and
// already initialized by the caller. `source_name` becomes the module name and
// the diagnostic prefix.
static MIR_module_t build_tu_module(MIR_context_t ctx, c2m_ctx_t c2m,
				    Program *prog, const char *source_name,
				    CirBuilder *&out_builder)
{
	CirBuilder *builder = new CirBuilder(c2m);
	out_builder = builder;	// caller owns lifetime (node arena backs the module)
	node_t tree = builder->translate_module(prog);
	if (!tree) {
		fprintf(stderr, "%s: tree build failed\n", source_name);
		return nullptr;
	}
	if (int nerr = cir_report_errors(stderr, tree)) {
		fprintf(stderr, "%s: %d untranslatable node(s); not compiling\n",
			source_name, nerr);
		return nullptr;
	}
	if (!cir_compile(ctx, c2m, tree, source_name)) {
		fprintf(stderr, "%s: cir_compile failed\n", source_name);
		return nullptr;
	}
	return DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
}
```

- [ ] **Step 2: Rewrite `madc_cir_execute`'s middle to call the helper**

Replace the block from `CirBuilder builder(c2m);` through the `MIR_module_t mod = DLIST_TAIL(...)` retrieval (current lines ~141–204) with:
```cpp
	CirBuilder *builder = nullptr;
	// (dump_nodes / dump_tree / dump_checked behavior is preserved below;
	//  keep those branches by translating once here when a dump is requested.)
	MIR_module_t mod = build_tu_module(ctx, c2m, prog, source_name, builder);
	if (!mod) {
		delete builder;
		cir_finish(c2m); c2mir_finish(ctx);
		MIR_gen_finish(ctx); MIR_finish(ctx);
		return -1;
	}
```
NOTE for the implementer: the existing `--dump-nodes` / `--dump-cir` / `--dump-cir-checked` branches operate on `tree` *before* compile and early-return. Preserve them: either (a) keep a thin pre-pass in `madc_cir_execute` that builds the tree for the dump-only paths, or (b) thread the dump flags into `build_tu_module`. Choose (a) — keep dumps in `madc_cir_execute`, since the project engine does not need them. Ensure `delete builder;` happens after run/teardown (move the existing teardown to also `delete builder;`).

- [ ] **Step 3: Build and run the FULL suite to prove no regression**

Run: `( ulimit -t 120; timeout 600 make -C src fulltest ) 2>&1 | grep -E "passed,|FAIL: tests" | tail -8`
Expected: `534 passed / 4 failed` baseline UNCHANGED (the 4 known: testdefer, testfstream, testlargesizeofquery, testloop). If anything else fails, the refactor changed behavior — fix before continuing.

- [ ] **Step 4: Commit**
```bash
git add src/madc_cir.cpp
git commit -m "refactor(cir): extract build_tu_module from madc_cir_execute (no behavior change)"
```

---

## Task 3: The multi-TU engine `madc_project_execute`

**Files:**
- Modify: `src/madc_cir.cpp` (add the function; reuses static `cir_import_resolver`, `cir_init`, `cir_compile`, `build_tu_module`, `g_jit_module`)
- The `#include "madc_project.h"` must be added near the top of `src/madc_cir.cpp`.

- [ ] **Step 1: Add the engine function**

`src/madc_cir.cpp` (after `madc_cir_execute`):
```cpp
int madc_project_execute(MadcEngine &engine, const ProjectManifest &manifest,
			 int user_argc, char **user_argv)
{
	if (manifest.tus.empty()) {
		fprintf(stderr, "madc_project_execute: empty manifest\n");
		return -1;
	}

	MIR_context_t ctx = MIR_init();
	c2mir_init(ctx);
	MIR_gen_init(ctx);
	MIR_gen_set_optimize_level(ctx, (unsigned)madc_opt_level);

	c2m_ctx_t c2m = cir_init(ctx, false);
	if (!c2m) {
		fprintf(stderr, "madc_project_execute: cir_init failed\n");
		MIR_gen_finish(ctx); c2mir_finish(ctx); MIR_finish(ctx);
		return -1;
	}

	// Programs and builders must outlive MIR_gen()+run: their arenas back the
	// modules. Hold them for the whole call.
	std::vector<std::unique_ptr<Program>> progs;
	std::vector<CirBuilder *> builders;
	std::vector<MIR_module_t> modules;
	auto teardown = [&]() {
		for (CirBuilder *b : builders) delete b;
		cir_finish(c2m); MIR_gen_finish(ctx); c2mir_finish(ctx); MIR_finish(ctx);
	};

	for (const ProjectTU &tu : manifest.tus) {
		std::unique_ptr<Program> prog = engine.create_program();
		prog->colors = true;
		for (const std::string &inc : tu.include_dirs) {
			std::string p = inc;
			if (!p.empty() && p.back() != '/') p += '/';
			prog->include_paths.push_back(p);
		}
		for (const std::string &d : tu.defines) {
			std::string::size_type eq = d.find('=');
			std::string name = (eq == std::string::npos) ? d : d.substr(0, eq);
			std::string val  = (eq == std::string::npos) ? std::string("1") : d.substr(eq + 1);
			if (!name.empty())
				prog->cli_defines.push_back(std::make_pair(name, val));
		}
		if (!tu.std_option.empty())
			prog->set_language_standard_option("--std=" + tu.std_option);

		TokenProgram *tp = prog->tokenize(tu.file.c_str());
		if (!tp) {
			fprintf(stderr, "%s: tokenize failed\n", tu.file.c_str());
			teardown(); return -1;
		}
		if (!prog->parse(tp)) {
			fprintf(stderr, "%s: parse failed\n", tu.file.c_str());
			teardown(); return -1;
		}

		CirBuilder *builder = nullptr;
		MIR_module_t mod = build_tu_module(ctx, c2m, prog.get(),
						   tu.file.c_str(), builder);
		builders.push_back(builder);
		if (!mod) { teardown(); return -1; }
		progs.push_back(std::move(prog));
		modules.push_back(mod);
	}

	for (MIR_module_t m : modules)
		MIR_load_module(ctx, m);
	MIR_link(ctx, MIR_set_gen_interface, cir_import_resolver);

	// Find the entry symbol across all modules.
	void *code = nullptr; MIR_module_t entry_mod = nullptr;
	for (MIR_module_t m : modules) {
		for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->items);
		     item != nullptr; item = DLIST_NEXT(MIR_item_t, item)) {
			if (item->item_type == MIR_func_item &&
			    strcmp(item->u.func->name, manifest.entry.c_str()) == 0) {
				code = MIR_gen(ctx, item); entry_mod = m; break;
			}
		}
		if (code) break;
	}
	if (!code) {
		fprintf(stderr, "madc_project_execute: entry '%s' not found\n",
			manifest.entry.c_str());
		teardown(); return -1;
	}

	g_jit_module = entry_mod;
	int result = ((int (*)(int, char **))code)(user_argc, user_argv);
	g_jit_module = nullptr;

	teardown();
	return result;
}
```

- [ ] **Step 2: Add includes / declarations as needed**

At the top of `src/madc_cir.cpp` add `#include "madc_project.h"` and ensure `<memory>` and `<vector>` are included (for `unique_ptr`/`vector`). `MadcEngine` and `Program` come from `madc.h` (already included by this file — verify).

- [ ] **Step 3: Build (no test yet — wired up in Task 4)**

Run: `( ulimit -t 120; timeout 300 make -C src ) 2>&1 | grep -iE ': error|: warning' | head`
Expected: no errors, no new warnings. (`-Wunused-function` may flag `madc_project_execute` until Task 4 calls it — acceptable mid-task; it disappears in Task 4.)

- [ ] **Step 4: Commit**
```bash
git add src/madc_cir.cpp
git commit -m "feat(project): multi-TU engine — N modules, one MIR_link, run entry"
```

---

## Task 4: Wire the `--project` CLI flag

**Files:**
- Modify: `src/madc.cpp` (arg loop ~387–500; dispatch ~576–610)

- [ ] **Step 1: Add the flag variable and parse case**

In `main()` near the other flag vars (after line 385) add:
```cpp
	const char *project_manifest = NULL;  // --project <compile_commands.json>
```
In the arg loop, add a case (next to `--emit=` handling, before the `set_language_standard_option` fallthrough):
```cpp
	} else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
		project_manifest = argv[++i];
		filearg = i + 1;
```

- [ ] **Step 2: Add the dispatch before the single-file path**

Just before `if ( argc >= 2 && filearg < argc )` (line 576), add:
```cpp
	if ( project_manifest )
	{
		ProjectManifest manifest;
		std::string err;
		if ( !read_compile_commands(project_manifest, manifest, err) )
		{
			std::cerr << "madc --project: " << err << std::endl;
			return 1;
		}
		// Remaining positionals (filearg..argc) become the program's argv.
		int run_argc = argc - filearg;
		char **run_argv = argv + filearg;
		int rc = madc_project_execute(engine, manifest, run_argc, run_argv);
		return (rc < 0) ? 1 : rc;
	}
```
Add `#include "madc_project.h"` near the top of `src/madc.cpp`.

- [ ] **Step 3: Build**

Run: `( ulimit -t 120; timeout 300 make -C src ) 2>&1 | grep -iE ': error|: warning' | head`
Expected: clean build, no warnings (the `-Wunused-function` from Task 3 is now gone).

- [ ] **Step 4: Smoke-test against the original spike files (manual, throwaway)**

```bash
printf '[{"directory":"%s","file":"spike_a.c","command":"gcc -c spike_a.c"},{"directory":"%s","file":"spike_b.c","command":"gcc -c spike_b.c"}]\n' "$PWD/tmp" "$PWD/tmp" > tmp/spike_cc.json
( ulimit -t 60; bin/madc --project tmp/spike_cc.json )
```
Expected: `a_secret=100  from_b=300` (matches the c2m spike).
**If this fails with a cross-TU symbol or c2m state error**, this is spec §10 risk #1: try a fresh `c2m_ctx` per TU (call `cir_init`/`cir_finish` inside the loop instead of once), keeping the shared `MIR_context`. Re-run.

- [ ] **Step 5: Commit**
```bash
git add src/madc.cpp
git commit -m "feat(cli): --project <compile_commands.json> drives the multi-TU engine"
```

---

## Task 5: Integration fixture (the spike through madc) + suite wiring

**Files:**
- Modify: `src/Makefile` (register `test_project_manifest` unit binary if not already; add `madc_project.o`)
- Create: `tests/testproject_a.c`, `tests/testproject_b.c`, `tests/testproject.cc.json`, `tests/testproject.mad`, `tests/testproject.flags`, `tests/testproject.expect`

- [ ] **Step 1: Confirm/extend the Makefile**

Check `src/Makefile` for: (a) `madc_project.o` in the main `madc` object list, (b) the unit-test target builds `tests/unit/test_project_manifest.cpp` linked against `madc_project.o` + `parser.o` (per testing.md, `dd*` globals come from `parser.o` via `TESTOBJ`). Add both if missing. (This step may have been partly done in Task 1 Step 5 — verify it's complete.)
Run: `( ulimit -t 120; timeout 300 make -C src ) 2>&1 | grep -iE ': error' | head` → clean.

- [ ] **Step 2: Create the two TU source files**

`tests/testproject_a.c`:
```c
#include <stdio.h>
static int secret = 100;        /* file-local; same name as b.c */
int from_a(void) { return secret; }
extern int from_b(void);
int main(void) {
	printf("a_secret=%d from_b=%d\n", from_a(), from_b());
	return 0;
}
```
`tests/testproject_b.c`:
```c
static int secret = 200;        /* must not collide with a.c */
extern int from_a(void);
int from_b(void) { return secret + from_a(); }
```

- [ ] **Step 3: Create the manifest (paths relative to the manifest's own directory)**

`tests/testproject.cc.json`:
```json
[
  {"directory":".","file":"testproject_a.c","command":"gcc -c testproject_a.c"},
  {"directory":".","file":"testproject_b.c","command":"gcc -c testproject_b.c"}
]
```
NOTE: `directory:"."` resolves relative to the *cwd of the test run*, which is the repo root (the runner runs `bin/madc ...` from repo root). So `testproject_a.c` resolves to `./testproject_a.c` — WRONG (files are in `tests/`). Fix: set `"directory":"tests"` so files resolve to `tests/testproject_a.c`. Use:
```json
[
  {"directory":"tests","file":"testproject_a.c","command":"gcc -c testproject_a.c"},
  {"directory":"tests","file":"testproject_b.c","command":"gcc -c testproject_b.c"}
]
```

- [ ] **Step 4: Create the runner glue (placeholder source + flags + expectation)**

`tests/testproject.mad` (placeholder — project mode ignores the trailing positional except as program argv; keep it a no-op the runner can pass):
```
// Project-mode test. The real build is driven by testproject.cc.json via the
// --project flag in testproject.flags. This file is an ignored positional.
```
`tests/testproject.flags`:
```
--project tests/testproject.cc.json
```
`tests/testproject.expect`:
```
a_secret=100 from_b=300
```

- [ ] **Step 5: Run just this test through the runner path**

Run: `( ulimit -t 120; timeout 60 bin/madc --project tests/testproject.cc.json ) 2>&1`
Expected: `a_secret=100 from_b=300`

- [ ] **Step 6: Run the full suite — new test green, baseline intact**

Run: `( ulimit -t 120; timeout 600 make -C src fulltest ) 2>&1 | grep -E "passed,|FAIL: tests|testproject" | tail -10`
Expected: baseline `534 → 535 passed` (testproject added), still `4 failed` (the known ones), no new failures. If `testproject` fails inside the runner but passes standalone (Step 5), inspect how `.flags` + trailing `$t` interact — the `tests/testproject.mad` positional may need to be absent or the flag may need the manifest as an absolute path; adjust the fixture (NOT the runner — test-fixtures rule).

- [ ] **Step 7: Commit**
```bash
git add tests/testproject_a.c tests/testproject_b.c tests/testproject.cc.json tests/testproject.mad tests/testproject.flags tests/testproject.expect src/Makefile
git commit -m "test(project): two-TU compile+link+run fixture (cross-TU calls, static isolation)"
```

---

## Task 6: Final gates + handoff update

**Files:**
- Modify: `claude_status.json`, `docs/plans/ROADMAP.md`, `CHANGELOG.md` (mirror the new capability per session-handoff)

- [ ] **Step 1: Torture suite unchanged (run ALONE)**

Run: `( ulimit -t 120; timeout 600 python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc ) 2>&1 | tail -5`
Expected: `1566 / 31 / 57 / 1` baseline unchanged (this work adds a path, doesn't touch codegen).

- [ ] **Step 2: Re-run fulltest to confirm green**

Run: `( ulimit -t 120; timeout 600 make -C src fulltest ) 2>&1 | grep -E "passed,|FAIL: tests" | tail -8`
Expected: `535 passed / 4 failed`.

- [ ] **Step 3: Update the mirrored status surfaces**

Add a short entry to `CHANGELOG.md` (under Unreleased): "Project build driver: `madc --project compile_commands.json` compiles each TU independently, links the MIR modules, and JIT-runs `main` (multi-TU engine; first reader = compile_commands.json)." Update `claude_status.json` and `docs/plans/ROADMAP.md` to note the v1 landed and the deferred items (Makefile reader, `.o`-to-disk parity recovery, other-ecosystem readers).

- [ ] **Step 4: Commit**
```bash
git add claude_status.json docs/plans/ROADMAP.md CHANGELOG.md
git commit -m "docs: record project build driver v1 (multi-TU compile+link+JIT)"
```

---

## Self-review (against the spec)

- **Spec §5.1 ProjectManifest** → Task 1 Step 1. ✓
- **Spec §5.2 compile_commands.json reader (command + arguments, -D/-I/-std)** → Task 1 Steps 4/6. ✓ `directory`-relative resolution handled.
- **Spec §5.2 link gap convention (all TUs, entry=main)** → `ProjectManifest::entry="main"`, engine scans all modules. ✓
- **Spec §5.3 multi-TU engine (one ctx, module per TU, single MIR_link, run entry)** → Tasks 2–3. ✓
- **Spec §5.4 per-TU isolation (fresh Program)** → Task 3 Step 1 (`engine.create_program()` per TU). ✓ Proven by the same-named-`static` fixture (Task 5).
- **Spec §5.5 JIT-only sink** → engine runs, writes no object. ✓
- **Spec §6 CLI `--project`** → Task 4. ✓
- **Spec §8 testing (small fixture first, then SMAUG; gates unchanged)** → Tasks 5–6. SMAUG is intentionally NOT in this plan (follow-on once v1 is green). ✓
- **Spec §10 risk #1 (cross-TU c2m state)** → Task 4 Step 4 contingency (fresh c2m per TU). ✓
- **Spec §10 risk #3 (duplicate symbols)** → partially: `MIR_link` behavior surfaces it; explicit dup-symbol diagnostics deferred (note in CHANGELOG as known limitation). Acceptable for v1.
- **Type consistency:** `read_compile_commands`, `madc_project_execute`, `build_tu_module`, `ProjectTU`/`ProjectManifest` field names used identically across Tasks 1/3/4. ✓
- **Placeholders:** none — all code spelled out; the two NOTE blocks (dump-branch preservation; directory resolution) flag real integration judgment, not missing content.

SMAUG bring-up (generate its `compile_commands.json`, run `madc --project`, fix surfaced per-file gaps) is a **separate follow-on plan** after this v1 is green — keeping with "start small before re-tackling SMAUG."
