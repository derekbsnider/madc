# madc Usage

## Running a Program

```bash
bin/madc <file.mad>
```

Or make the file executable (it has the shebang `#!/../bin/madc`):

```bash
chmod +x tests/testint.mad
tests/testint.mad
```

## Verbose Mode

Pass `-v` or `--verbose` to enable debug trace output. This emits tokenization,
parsing, and JIT compilation details to stdout before program execution:

```bash
bin/madc -v tests/testint.mad
```

Without the flag, only program output is printed.

## Command-Line Reference

```
madc [-v|--verbose] <file.mad>
```

| Flag | Description |
|------|-------------|
| `-v`, `--verbose` | Enable verbose debug output (tokenizer, parser, compiler traces) |

## Language Quick Reference

madc is a C-style language with JIT-compiled execution. Key features:

### Data Types

| Type | Description |
|------|-------------|
| `int` / `int64_t` | 64-bit signed integer (default integer type) |
| `int8_t` .. `int32_t` | Smaller signed integers |
| `uint8_t` .. `uint64_t` | Unsigned integers |
| `float`, `double` | Floating point |
| `char` | 8-bit character |
| `string` | C++ std::string |
| `stringstream` | C++ std::stringstream |

### `register` Keyword

Declare a variable as register-only — it lives entirely in a virtual register
and is never written to memory. Maximum performance for hot loop counters:

```c
register int x = 0;    // stays in a Gp register
register double d = 0; // stays in an Xmm register
```

This is the default behavior for local scalar variables. The keyword makes it
explicit and prevents future writeback if the memory model is refined.

### Structs

Hardcoded `teststruct` is available. User-defined structs are planned for Phase 2.

```c
struct teststruct test;
test.name = "Alice";
test.id   = 1;
test.age  = 30;
cout << "name: " << test.name << endl;
```

### Built-in Functions

| Function | Description |
|----------|-------------|
| `puts(s)` | Print string + newline |
| `putchar(c)` | Print a character |
| `puti(i)` | Print integer |
| `printstr(s)` | Print string (no newline) |
| `printf(fmt, ...)` | C-style formatted print |

### Output Operators

```c
cout << "hello " << x << endl;
```

### Control Flow

```c
if (x > 0) { ... } else { ... }
for (int i = 0; i < 10; i++) { ... }
while (x > 0) { x--; }
do { ... } while (condition);
```

### Functions

```c
int add(int a, int b) {
    return a + b;
}
```
