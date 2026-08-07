# Command Line Arguments

## Usage

```c
int main(int argc, char **argv)
{
    printf("Script: %s\n", argv[0]);
    printf("Args: %d\n", argc);

    int i;
    for ( i = 1; i < argc; i++ )
        puts(argv[i]);

    return 0;
}
```

Run: `bin/madc script.mad arg1 arg2 arg3`

- `argc` = number of arguments (including the script filename)
- `argv` = `char **` pointer (passed as int64 at the ABI level)
- `argv[0]` = the `.mad` script filename
- `argv[1..]` = user arguments

## Raw-pointer subscript

`argv[i]` reads the i-th `char *` directly. madc supports raw-pointer
subscripting on `char **` (and other pointer-to-pointer / array-of-T
shapes), so the C-style idiom works as written:

```c
int main(int argc, char **argv)
{
	cout << argv[0] << endl;          // script filename
	if ( argc > 1 )
	{
		puts(argv[1]);            // first argument
		char *first = argv[1];    // bind to a local
		cout << first << endl;
	}
	return 0;
}
```

## `int main()` (no arguments)

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

## Legacy: `get_argv(argv, index)`

`get_argv(argv, index)` is a built-in that predates raw-pointer
subscripting and returns `const char *` for the i-th argument. It is
retained for backward compatibility with older scripts. New code should
use `argv[i]` directly.

```c
int main(int argc, char **argv)
{
	cout << get_argv(argv, 0) << endl;  // equivalent to argv[0]
	return 0;
}
```

## Implementation

- `Program::script_argc` and `script_argv` are set from the C++ command line
  (everything after the `.mad` filename)
- `Program::execute()` checks `FuncDef::parameters.size()` — if >= 2, calls
  `main_fn(argc, argv)` instead of `main_fn()`
- Pointer parameters (`char *`, `char **`) are mapped to `int64` at the ABI level
