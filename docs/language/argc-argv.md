# Command Line Arguments

## Usage

```c
int main(int argc, char **argv)
{
    printf("Script: %s\n", get_argv(argv, 0));
    printf("Args: %d\n", argc);

    int i;
    for ( i = 1; i < argc; i++ )
        puts(get_argv(argv, i));

    return 0;
}
```

Run: `bin/madc script.mad arg1 arg2 arg3`

- `argc` = number of arguments (including the script filename)
- `argv` = raw `char**` pointer (passed as int64 at the ABI level)
- `argv[0]` = the `.mad` script filename
- `argv[1..]` = user arguments

## `get_argv(argv, index)`

Since madc doesn't support raw pointer subscripting (`argv[i]`), the built-in
`get_argv(argv, index)` function returns `const char*` for the i-th argument.

```c
cout << get_argv(argv, 0) << endl;  // prints script filename
puts(get_argv(argv, 1));            // prints first argument
```

## `int main()` (No Arguments)

Scripts that don't need command line arguments can still use:

```c
int main()
{
    // no argc/argv
    return 0;
}
```

Both forms work. The runtime checks `main`'s parameter count and passes
argc/argv only when main declares 2+ parameters.

## Implementation

- `Program::script_argc` and `script_argv` are set from the C++ command line
  (everything after the `.mad` filename)
- `Program::execute()` checks `FuncDef::parameters.size()` — if >= 2, calls
  `main_fn(argc, argv)` instead of `main_fn()`
- Pointer parameters (`char *`, `char **`) are mapped to `int64` at the ABI level
