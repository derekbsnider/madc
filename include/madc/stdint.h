// madc embedded stdint.h — exact-width integer types
// Note: int8_t through uint64_t are native madc types
// This header provides the min/max constants for completeness

#define INT8_MIN    -128
#define INT8_MAX    127
#define UINT8_MAX   255
#define INT16_MIN   -32768
#define INT16_MAX   32767
#define UINT16_MAX  65535
#define INT32_MIN   -2147483648
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295
#define INT64_MIN   -9223372036854775807
#define INT64_MAX   9223372036854775807
#define UINT64_MAX  0xFFFFFFFFFFFFFFFF
#define SIZE_MAX    0xFFFFFFFFFFFFFFFF
#define INTMAX_MIN  -9223372036854775807
#define INTMAX_MAX  9223372036854775807
#define UINTMAX_MAX 0xFFFFFFFFFFFFFFFF
#define PTRDIFF_MIN -9223372036854775807
#define PTRDIFF_MAX 9223372036854775807

typedef long int intptr_t;
typedef unsigned long int uintptr_t;
