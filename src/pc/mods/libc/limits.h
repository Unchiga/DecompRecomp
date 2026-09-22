#ifndef MEMORIES_MOD_LIMITS_H
#define MEMORIES_MOD_LIMITS_H
/* A mod's <limits.h> (README.md): 32-bit x86, where int and long are both 32
 * bits wide. */
#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535
#define INT_MIN (-INT_MAX - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295u
#define LONG_MIN (-LONG_MAX - 1l)
#define LONG_MAX 2147483647l
#define ULONG_MAX 4294967295ul
#define LLONG_MIN (-LLONG_MAX - 1ll)
#define LLONG_MAX 9223372036854775807ll
#define ULLONG_MAX 18446744073709551615ull
#endif
