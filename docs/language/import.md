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

### Alias form — `import name as ns;`

Binds the module's library under a namespace of your choosing. Members
resolve **by name, at first call**:

```c
import c as libc;

int main()
{
    println(format("abs(-42): {}", libc::abs(-42)));   // 42
    var num = "999";
    println(format("atoi: {}", libc::atoi(num)));      // 999
    return 0;
}
```

A member has no declared signature: it is called with the actual argument
types (the variadic convention; string values coerce to `const char *`) and
returns a 64-bit integer. Use the module form or a real header when you need
typed prototypes. The member must be exported by the library — an unknown
member is a compile-time diagnostic (`dlsym failed for 'x' in 'ns'`).

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
