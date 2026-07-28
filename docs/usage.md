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

Pass `-v` or `--verbose` to enable debug trace output:

```bash
bin/madc -v tests/testint.mad
```

## Data Types

| Type | Description |
|------|-------------|
| `int` / `int64_t` | 64-bit signed integer (default integer type) |
| `int8_t` .. `int32_t` | Smaller signed integers |
| `uint8_t` .. `uint64_t` | Unsigned integers |
| `float`, `double` | Floating point |
| `char` | 8-bit character |
| `std::string` | C++ std::string (`string` after explicit `using`) |
| `std::stringstream` | C++ std::stringstream |
| `std::ifstream` | File input stream |
| `std::ofstream` | File output stream |
| `std::fstream` | File input/output stream |
| `array` | Mixed-type array (MadValue-based) |

Including `<string>` or `<iostream>` makes the std surface available
under `std::`; it does not create bare global names. Use `std::string`
and `std::cout`, or import names explicitly with `using namespace std;`
or `using std::string;`.

## User-Defined Types

```c
struct Point {
    int x;
    int y;
};

class Person {
    string name;
    int age;
};

Point p;            // no 'struct' prefix needed for classes
Person bob;
```

## `auto` Keyword

Type inference for function pointer declarations:

```c
auto fn = my_function;     // fn holds my_function's address
fn(42);                     // call through pointer

auto add = [int](int a, int b) { return a + b; };
int result;
result = add(10, 20);
```

## Function Pointers

Store a function's address in a variable and call through it:

```c
void greet(string name) { cout << "Hi " << name << endl; }

int main() {
    auto fn = greet;
    fn("World");          // calls greet("World")
}
```

## Lambda Expressions

Anonymous inline functions:

```c
// void lambda
auto print = [](string s) { cout << s << endl; };

// typed-return lambda: return type inside []
auto add = [int](int a, int b) { return a + b; };
```

## Range-Based For

C++ style iteration over arrays:

```c
array names;
php::array_push(names, "Alice");
php::array_push(names, "Bob");

for (string name : names) {
    cout << name << endl;
}

// also works with integers
array nums;
php::array_push_int(nums, 10);
php::array_push_int(nums, 20);
for (int n : nums) {
    cout << n << endl;
}
```

## `register` Keyword

Declare a variable as register-only — lives entirely in a virtual register, never written to memory:

```c
register int x = 0;    // stays in a Gp register
register double d = 0; // stays in an Xmm register
```

## Control Flow

```c
if (x > 0) { ... } else { ... }
for (int i = 0; i < 10; i++) { ... }
while (x > 0) { x--; }
do { ... } while (condition);
```

### switch / case / default

```c
switch (x)
{
    case 1:
        cout << "one" << endl;
        break;
    case 2:
        cout << "two" << endl;
        break;
    default:
        cout << "other" << endl;
        break;
}
```

### Ternary Operator

```c
int result;
result = (x > 0) ? x : -x;   // conditional expression
```

The ternary operator works in assignments and expressions. Both branches must produce the same type.

## Functions

```c
int add(int a, int b) {
    return a + b;
}

void greet(string name) {
    cout << "Hello, " << name << endl;
}
```

### Multiple Return Values (Go-style)

Functions can return multiple values using comma-separated return:

```c
int, int divmod(int a, int b) {
    return a / b, a % b;
}

int main() {
    int q, r;
    q, r = divmod(17, 5);
    cout << "quotient: " << q << ", remainder: " << r << endl;
}
```

Multiple return types are declared with comma-separated types before the function name. The caller receives values via comma-separated assignment. Currently works with numeric types only.

### Class Methods

Classes support methods with an implicit `this` pointer:

```c
class Counter {
    int count;

    void increment() {
        this.count = this.count + 1;
    }

    int get() {
        return this.count;
    }
};

int main() {
    Counter c;
    c.count = 0;
    c.increment();
    cout << c.get() << endl;   // prints 1
}
```

Methods access the instance via `this.member`. The compiler passes a hidden `__this` pointer as the first argument.

## Output

```c
cout << "hello " << x << endl;
std::cout << "qualified" << std::endl;
puts("c-style");
putchar('h');
puti(42);
```

## Input

```c
string name;
cout << "Enter name: ";
cin >> name;
cout << "Hello, " << name << endl;

int x;
cout << "Enter number: ";
cin >> x;
cout << "You entered: " << x << endl;
```

`cin >> var` reads whitespace-delimited tokens from stdin. Works with `string`, `int`, and other numeric types.

## File I/O

```c
// Write
ofstream out;
string fname = "output.txt";
out.open(fname);
out << "Hello!" << endl;
out.close();

// Read
ifstream in;
in.open(fname);
string line;
while ( in.good() )
{
    getline(in, line);
    if ( in.good() )
        cout << line << endl;
}
in.close();
```

## Built-in Functions

| Function | Description |
|----------|-------------|
| `puts(s)` | Print C-string + newline |
| `putchar(c)` | Print a character |
| `puti(i)` | Print integer |
| `printstr(s)` | Print string (no newline) |
| `std::to_string(result, int)` | Integer to string |
| `std::stoi(str)` | String to integer |
| `std::stod(str)` | String to double |
| `strlen(str)` | String length |
| `system(cmd)` | Run shell command |
| `getenv(name)` | Get environment variable (real C `getenv`, returns `char *`; `string s = getenv("HOME")` ingests it) |
| `setenv(name, value, overwrite)` | Set environment variable (real POSIX `setenv`) |
| `dlopen(filename)` | Open shared library |
| `dlsym(handle, name)` | Look up symbol |
| `dlcall(funcptr, args...)` | Call through function pointer |
| `dlclose(handle)` | Close library |

## Namespaces

Access namespace members with `::`:

```c
std::cout << "hello" << std::endl;
php::trim(s);
perl::glob(files, "*.txt");
python::title(s);
ruby::squeeze(s);
js::btoa(encoded, s);
```

Import with `using`:

```c
using namespace std;       // import all members
using std::cout;           // import one member
using std::string;         // import one type
```

### madc:: Namespace

The `madc::` namespace provides built-in functions unique to Mad-C:

```c
// Regex functions
int matched;
string s = "Hello World 123";
matched = madc::regex_match(s, "[A-Za-z]+ [A-Za-z]+ [0-9]+");
matched = madc::regex_search(s, "[0-9]+");

string result;
madc::regex_replace(result, s, "[0-9]+", "456");
// result = "Hello World 456"
```

| Function | Description |
|----------|-------------|
| `madc::regex_match(str, pattern)` | Full-string regex match, returns 1/0 |
| `madc::regex_search(str, pattern)` | Search for pattern anywhere in string, returns 1/0 |
| `madc::regex_replace(result, str, pattern, replacement)` | Regex substitution |
| `madc::array` | Native array type |

See the namespace reference docs:
- [`docs/language/ns-php.md`](language/ns-php.md)
- [`docs/language/ns-perl.md`](language/ns-perl.md)
- [`docs/language/ns-python.md`](language/ns-python.md)
- [`docs/language/ns-ruby.md`](language/ns-ruby.md)
- [`docs/language/ns-js.md`](language/ns-js.md)

## `#include`

Include another `.mad` file:

```c
#include "helpers.mad"
```

## `#load` — Dynamic Libraries

Load a shared library as a namespace:

```c
#load "libc.so.6" as libc;
int result;
result = libc::abs(-42);
```

Functions are resolved lazily via `dlsym` on first use.
