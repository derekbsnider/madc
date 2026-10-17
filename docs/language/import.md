# `import` — module binding

`import` binds a **module**: its interface (declarations) *and* its library,
with no platform spelling anywhere in the source. It is C++20's `import`
directive made whole — the standard leaves *which library* to the build
system; madc's module map answers it — and it works the same on the JIT and
in native artifacts (`-o`, `-c`, `--emit=c11`) on Linux, macOS and Windows.

**Available in the madc dialect (`--std=madc`, the default) and in
`--std=c++20` and later.** It is never active in C or in earlier C++
standards, and it is never a reserved word (see *Directive position*).

## The three forms

```c
import m;                 // module: interface (<math.h>) + library (libm)
import c as libc;         // alias form: the library only, under a namespace
import <stdio.h>;         // header unit (C++20 spelling) — the include
import "myheader.h";      //   ... quote form, likewise
```

### Module form — `import name;`

The module's **interface** is tokenized first (a registry row names the
embedded header), then its **library** is bound. After `import m;` every
declaration and macro of `<math.h>` is visible and `sqrt`, `M_PI`, … work,
exactly as after `#include <math.h>` — plus the library is bound for the
program (below). A module with no interface (`c` today) cannot be imported
this way; the diagnostic points at the alias form.

### madc modules — `import git;`

A **madc module** is a library that describes itself (Python's modules are
the model; a madc module is madc's built form of a C++20 named module). It
is found **by name**, never registered in madc: `import git;` searches the
program's own directory, then each `--module-path=<dir>`, then madc's module
directory (`<prefix>/lib/madc/modules`) for `git.so` / `git.dylib` /
`git.dll`, opens it and reads its description through the one entry point
it exports (`madc_module_git`). The description holds the module's
declarations — generated from its face (`git.madi`) when the module was
built — and what it brings a host: REPL commands (`%git`) and a tool
server's tools. Whether a module is installed is answered by importing it:

- no such module: `import: no module named 'git'`;
- a file that is not a madc module, or one built for another module ABI:
  `import: module 'git': <the reason>`.

madc ships three: `git` (a read-only view of a repository, libgit2 inside),
`markdown` (cmark-gfm inside) and `ui_web` (the web UI level's host over the
platform webview: importing it registers the host, so `ui::open(ui::WEB)`
gets a window). An imported module opens at program start in every lane —
the JIT, an executable and an object (its module list names it) — so its own
initializer runs before `main`; a native program whose module will not open
exits 69 with the import error before `main`. From the command line, `--import=<name>`
imports a module ahead of the program; with no source it only tries the
import and exits 0, or 69 with the import error — a launcher's test before
it starts (`python -c "import git"`). The configuration file's
`modules = <name>` is the same import.

### Alias form — `import name as ns;`

Binds the module's library under a namespace of your choosing. Members
resolve **by name, at first call**:

```c
import c as libc;

int main()
{
    println("abs(-42): {}", libc::abs(-42));            // 42
    var num = "999";
    println("atoi: {}", libc::atoi(num));               // 999
    libc::puts("a statement-position call");            // the same member call
    long long a = (long long)libc::abs(-7);             // as a cast operand
    return 0;
}
```

A member has no declared signature: it is called with the actual argument
types (the variadic convention; string values coerce to `const char *`) and
returns a 64-bit integer. Use the module form or a real header when you need
typed prototypes. A member resolves the same way wherever the qualified name
appears — as an expression operand, as a whole statement, under a cast, or
spelled `::libc::abs` — because the namespace's members ARE the library's
exports, materialized by the one lookup. The member must be exported by the
library — an unknown member is a compile-time diagnostic (`'x' is not a
member of namespace 'ns': libc.so.6 exports no such symbol`).

### Header units — `import <h>;` / `import "h";`

The C++20 header-unit spellings are served as the corresponding `#include`.
This is an approximation: a true header unit does not leak macros; madc's
does, exactly as the include would.

## The module map and the platform rule

Library names are spelled by **one owner**, `src/madc_modules.cpp`, keyed on
the *target* OS (never the host):

| name | interface | Linux/ELF | macOS/Mach-O | Windows/PE |
|------|-----------|-----------|--------------|------------|
| `c`  | —         | `libc.so.6` | `libSystem.B.dylib` | `ucrtbase.dll` |
| `m`  | `<math.h>` | `libm.so.6` | `libSystem.B.dylib` | `ucrtbase.dll` |

A name with no row follows the linker's rule: `lib<name>.so`,
`lib<name>.dylib`, `<name>.dll`. A path (either separator) or an
already-spelled name (`libfoo.so`, `libfoo.so.2`, `bar.dll`) passes verbatim.
The rows name the *real* runtime images: `libm.so` is a `-dev` symlink and
glibc's `libc.so` is a linker script, so the bare rule alone could not open
either.

## How the library binds — JIT and native

- **JIT (`madc file.mad`)**: the spelled library is opened into the default
  symbol scope (`RTLD_GLOBAL`), beside the running binary's `../lib` first,
  then as the loader searches. Unprefixed calls resolve through the usual
  dlsym fallback as well.
- **Native (`-o`, `-c`, `-shared`)**: a module-form library joins the link
  closure — a `DT_NEEDED` entry on ELF, an `LC_LOAD_DYLIB` load command on
  Mach-O, an import on PE — so the artifact runs on a machine without madc.
  Alias-form members are resolved at run time by the artifact itself (a
  per-member slot filled on first call by the runtime helper
  `__madc_dl_member`); no linker involvement, the same lowering `--emit=c11`
  prints.
- **A relocatable `.o` carries its module list**: the unit records every
  module-form library's target spelling in a local data symbol
  `__madc_module_deps` (NUL-separated; `--emit=c11` prints it as
  `static const char __madc_module_deps[] = "libm.so.6\0";`), and `madc
  file.o` opens each spelling before loading — no `-l` on the command line
  for an imported module (the MSVC `.drectve` / Rust `#[link]` shape; a
  `.o` has no `DT_NEEDED`). An explicit `-l` still opens first and stays an
  override; a `.o` built before the table existed loads as before. A lazy
  row's spelling is listed with a `?` prefix — optional: the loader opens it
  when it can and never fails on it. A madc module is listed by name and
  file (`@-git=<file>`; `@g` for a GUI module, which lifts an armed memory
  guard before it opens), required, and the program's own init opens it.
- **Lazy rows**: an OPTIONAL library's row is marked lazy —
  the interface form binds nothing at parse and joins no link closure; each
  declared function is a first-call slot through a pointer of its declared
  type (`__madc_dl_member`, as the alias form, but typed). The program
  compiles and runs on a machine without the library; ask
  `madc::module_available("<name>")` before the first call. No shipped row
  is lazy any more (the web target is the `ui_web` module). The alias form of
  a lazy row keeps the eager binding — a namespace answers member lookups
  from the exports.
- **`--no-auto-load`**: do not act on `import` library bindings — nothing is
  opened; namespaces bind to the program's own symbol scope, so symbols come
  from explicit linking (`-l`, the host). `tests/testnoautoload.mad` pins it.
- **Registration policy**: an embedding host that disables the dlfcn family
  (`enable_dlfcn_functions = false`) disables `import`'s library binding too.

## `-l<name>` — the same binding, from the build line

`madc -lcrypt file.mad` and `-lm` resolve `crypt` / `m` through the same
module map (`libcrypt.so`, `libm.so.6`, `ucrtbase.dll` …), open them on the
JIT, and record them for native artifacts. `-l` is how a build system spells
a binding; `import` is how the source spells it.

## Directive position — `import` is contextual

Per C++20 [cpp.pre], `import` is a directive only as the **first token of a
logical line** followed by a module name, `<` or `"`. Everywhere else it is
an ordinary identifier:

```c
int import = 3;           // fine — not directive position
import <stdio.h>;         // a directive
```

The whole directive sits on one logical line and ends with `;`.

## And `#load`

`#load "<file>" as ns;` is the low-level directive underneath the alias
form, kept for tooling and fixtures the way `#pragma` is: you spell the
exact file, you own the platform. It binds through the same machinery
(`tests/testdlopen.mad`; see [preprocessor.md](preprocessor.md)). Programs
write `import`.
