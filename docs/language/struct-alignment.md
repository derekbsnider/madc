# Struct Alignment & Packing

## C ABI Alignment (Default)

Structs use natural x86-64 alignment by default, matching the C ABI. Each
field is placed at an offset that is a multiple of
`min(field_size, 8)`:

```c
struct example {
	char a;      // offset 0, size 1
	// 3 bytes padding
	int32_t b;   // offset 4, size 4
	// 0 bytes padding (already aligned)
	int64_t c;   // offset 8, size 8
};

int main()
{
	cout << sizeof(struct example) << endl;
	return 0;
}
```

Output: `16` (rounded up to `max_align = 8`).

This means madc structs pass directly to C library functions like
`stat()` and `localtime()` without field misalignment.

## Packed Structs

### `__attribute__((packed))`

```c
struct __attribute__((packed)) header {
	char magic;     // offset 0
	int32_t size;   // offset 1 (no padding)
	int64_t data;   // offset 5 (no padding)
};

int main()
{
	cout << sizeof(struct header) << endl;
	return 0;
}
```

Output: `13`

The attribute can appear before or after the struct tag name:

```text
struct __attribute__((packed)) name { ... };
struct name __attribute__((packed)) { ... };
```

### `#pragma pack`

```c
#pragma pack(push, 1)   // packed
struct a { char x; int y; };

#pragma pack(push, 2)   // 2-byte max alignment
struct b { char x; int y; };

#pragma pack(pop)       // back to pack=1
#pragma pack(pop)       // back to default (natural alignment)

int main()
{
	cout << sizeof(struct a) << " " << sizeof(struct b) << endl;
	return 0;
}
```

Output: `5 6` (packed: 1+4; 2-byte cap: 1 + 1 pad + 4).

Pack values:

- **0** (default) — natural C ABI alignment
- **1** — packed, no padding
- **N** — fields aligned to `min(natural_alignment, N)`

## `sizeof()`

`sizeof` resolves at parse time to an integer constant and works in
expressions. Note the madc dialect's default `int` is 64-bit
(`--std=c*` modes keep the standard 32-bit `int`):

```c
struct point { int32_t x; int32_t y; };

int main()
{
	cout << sizeof(int) << endl;           // 8 (madc int is 64-bit)
	cout << sizeof(int32_t) << endl;       // 4
	cout << sizeof(char) << endl;          // 1
	cout << sizeof(struct point) << endl;  // 8
	cout << sizeof(double) << endl;        // 8
	int count = 3;
	int bytes = sizeof(struct point) * count;
	cout << bytes << endl;                 // 24
	return 0;
}
```

## Implementation Details

- `DataDefSTRUCT` in `include/datadef.h` stores the `pack` value and
  `max_align`
- `addMember()` applies `align_up(offset, field_align)` before placing
  each field; `finalize()` rounds the total size up to `max_align` (for
  correct array spacing)
- Layout follows the Itanium C++ ABI once a struct is class-promoted
  (bases, vtable pointers, virtual bases) — see
  [supported C++ features](cpp-features.md)
