# Struct Alignment & Packing

## C ABI Alignment (Default)

Structs use natural x86-64 alignment by default, matching the C ABI. Each field is placed
at an offset that is a multiple of `min(field_size, 8)`:

```c
struct example {
    char a;      // offset 0, size 1
    // 3 bytes padding
    int32_t b;   // offset 4, size 4
    // 0 bytes padding (already aligned)
    int64_t c;   // offset 8, size 8
};
// total size: 16 bytes (rounded up to max_align=8)
```

This means madc structs can be passed directly to C library functions like `stat()`,
`localtime()`, etc. without field misalignment.

## Packed Structs

### `__attribute__((packed))`

```c
struct __attribute__((packed)) header {
    char magic;     // offset 0
    int32_t size;   // offset 1 (no padding)
    int64_t data;   // offset 5 (no padding)
};
// total size: 13 bytes
```

The attribute can appear before or after the struct tag name:
```c
struct __attribute__((packed)) name { ... };
struct name __attribute__((packed)) { ... };
```

### `#pragma pack`

```c
#pragma pack(push, 1)   // packed
struct a { char x; int y; };   // 5 bytes

#pragma pack(push, 2)   // 2-byte max alignment
struct b { char x; int y; };   // 6 bytes (1 + 1pad + 4)

#pragma pack(pop)       // back to pack=1
#pragma pack(pop)       // back to default (natural alignment)
```

Pack values:
- **0** (default) — natural C ABI alignment
- **1** — packed, no padding
- **N** — fields aligned to `min(natural_alignment, N)`

## `sizeof()`

```c
sizeof(int)              // 8 (madc int is 64-bit)
sizeof(int32_t)          // 4
sizeof(char)             // 1
sizeof(struct point)     // depends on fields and alignment
sizeof(double)           // 8
```

`sizeof` resolves at parse time to an integer constant. It works in expressions:
```c
int size = sizeof(struct header) * count;
```

## Implementation Details

- `DataDefSTRUCT` in `datadef.h` stores a `pack` field and `max_align`
- `addMember()` applies `align_up(offset, field_align)` before placing each field
- `finalize()` rounds total struct size up to `max_align` (for correct array spacing)
- `m_offset()` applies the same alignment when computing member offsets at compile time
- Stack allocation uses `max_align` for the struct's base alignment
