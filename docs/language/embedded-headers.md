# Embedded Headers

madc supports C/C++ style `#include` directives with **embedded headers** — header files
baked into the binary at build time so no external files are needed at runtime.

## Usage

```c
#include <iostream>    // angle brackets → embedded headers first, then filesystem
#include "myfile.mad"  // quotes → filesystem only (relative to current file)
```

## Implemented Headers

### `<iostream>`

Registers `std::cout`, `std::cin`, `std::cerr`, and `std::endl`.
Including the header does not add bare global names; use `std::` or
import the names explicitly with `using`.

```c
#include <iostream>

int main()
{
    std::cout << "Hello, world!" << std::endl;
    return 0;
}
```

### `<string>`

Registers the `std::string` type plus `std::to_string`,
`std::stoi`, and `std::stod`. Bare `string` is available only after
`using namespace std;` or `using std::string;`.

```c
#include <string>

int main()
{
    std::string value;
    std::to_string(value, 42);
    return 0;
}
```

### `<math.h>`

Auto-loads libm via `#load "libm.so.6"`. Defines math constants and makes all libm
functions available via the dlsym fallback.

**Constants:** `M_PI`, `M_PI_2`, `M_PI_4`, `M_E`, `M_LOG2E`, `M_LOG10E`, `M_LN2`,
`M_LN10`, `M_SQRT2`, `M_SQRT1_2`, `HUGE_VAL`, `INFINITY`

**Functions (via dlsym):** `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`,
`pow`, `exp`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `fabs`, `fmod`, `hypot`

```c
#include <math.h>

int main()
{
    double x = sqrt(16.0);    // 4.0
    double y = pow(2.0, 10.0); // 1024.0
    double z = sin(M_PI / 2); // 1.0
    return 0;
}
```

### `<stdio.h>`

Defines standard I/O constants. `printf` family available via dlsym (libc always loaded).

**Constants:** `EOF` (-1), `SEEK_SET` (0), `SEEK_CUR` (1), `SEEK_END` (2), `BUFSIZ` (8192), `NULL` (0)

**Functions (via dlsym):** `printf`, `fprintf`, `sprintf`, `snprintf`, `puts`, `putchar`

```c
#include <stdio.h>

int main()
{
    printf("Hello %s, you are %d years old\n", "Alice", 30);
    return 0;
}
```

## Build System

Embedded headers live in `include/madc/`. At build time, `scripts/gen_embedded_headers.sh`
converts them to C++ string literals in `src/embedded_headers.cpp`. The Makefile regenerates
this file when any header in `include/madc/` changes.

## Adding New Headers

1. Create the header file in `include/madc/` (e.g., `include/madc/stdlib.h`)
2. Use `#define` for constants and `#load` if a shared library is needed
3. If the header needs to register built-in globals/functions:
   - Add a flag (`_include_xxx`) to the Program class in `include/madc.h`
   - Set the flag in the lexer's `#include` handler
   - Add namespace registrations or an `add_xxx()` function that
     populates `lazy_map`
   - Add cases in `add_namespaces()`, `lazy_resolve()`, or
     `lazy_resolve_type()`
4. Run `make -C src` — the gen script runs automatically

## dlsym Fallback

Functions don't need explicit registration. Any unresolved function call followed by `(`
automatically tries `dlsym(RTLD_DEFAULT, name)`. If the symbol exists in the process
(libc is always loaded, libm after `#include <math.h>`), it becomes callable.

The variadic dlsym path builds the calling convention from actual argument types — int args
use Gp registers, double args use Xmm registers, strings are auto-coerced to `const char*`.
