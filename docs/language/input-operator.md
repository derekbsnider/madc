# Input Operator (cin >>)

Read from stdin using `cin` and the `>>` extraction operator.

## Syntax

```c
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

## Files

- `src/compiler.cpp` -- `>>` operator handling for istream types
