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
| `string` | C++ std::string |
| `stringstream` | C++ std::stringstream |
| `ifstream` | File input stream |
| `ofstream` | File output stream |
| `fstream` | File input/output stream |
| `array` | Mixed-type array (MadValue-based) |

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

## Functions

```c
int add(int a, int b) {
    return a + b;
}

void greet(string name) {
    cout << "Hello, " << name << endl;
}
```

## Output

```c
cout << "hello " << x << endl;
std::cout << "qualified" << std::endl;
puts("c-style");
putchar('h');
puti(42);
```

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
| `to_string(result, int)` | Integer to string |
| `stoi(str)` | String to integer |
| `stod(str)` | String to double |
| `strlen(str)` | String length |
| `system(cmd)` | Run shell command |
| `getenv(result, name)` | Get environment variable |
| `setenv(name, value)` | Set environment variable |
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
```

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
