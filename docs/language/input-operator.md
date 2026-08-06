# Input Operator (cin >>)

Read from stdin using `cin` and the `>>` extraction operator.

## Syntax

```text
cin >> variable;
cin >> a >> b;       // chained input
```

`cin` is a global istream variable (also accessible as `std::cin`). The `>>` operator reads whitespace-delimited tokens from stdin and stores them in the target variable.

## Supported Types

- `string` -- reads one whitespace-delimited word
- `int` -- reads an integer
- `double` -- reads a floating-point number

## Example

```c
string name;
int age;

cin >> name;
cin >> age;
cout << "name: " << name << endl;
cout << "age: " << age << endl;
```

## Chained Input

Multiple variables can be read in a single statement:

```c
string a;
string b;
cin >> a >> b;
cout << a << " " << b << endl;
```

## Testing

`testcin.mad` requires stdin piping since it reads interactive input:

```bash
echo -e "Alice\n30\nhello world" | bin/madc tests/testcin.mad
```

`cin` and the extraction operators are the REAL `std::cin` /
`operator>>` from the active standard library, resolved mangled-direct —
formatted extraction behaves exactly as compiled C++ does under either
stdlib flavor.

## Files

- `src/parser.cpp` — `>>` operator resolution against the real istream
  overload set
- `src/cir_builder.cpp` — call lowering
