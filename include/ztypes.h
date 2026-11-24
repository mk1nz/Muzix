/*
 * Muzix project-owned fixed-width integer types and varargs macros.
 *
 * Replaces the compiler-provided <stdint.h>, <stddef.h>, <stdbool.h> and
 * <stdarg.h>, which are forbidden by the project's libc/header policy (rule #8).
 *
 * The discriminator between the SDCC Z80 target and a host C compiler is the
 * presence of __SIZE_TYPE__: GCC/Clang define it (and their builtin type macros
 * match the system headers exactly, so there is no redefinition conflict when
 * a system header is later included); SDCC does not define it, so the explicit
 * C-type fallback is used.  SDCC z80 has int=16/long=32; host GCC has
 * int=32/long=64.  The two branches therefore use different underlying types
 * but expose the identical fixed-width type names.
 *
 * SDCC defines __SDCC_z80 when targeting the Z80; GCC/Clang do not.
 * Either condition selects the correct branch.
 */

#ifndef MUZIX_TYPES_H
#define MUZIX_TYPES_H

#if defined(__SIZE_TYPE__)
/* ------------------------------------------------------------------ */
/* Host compiler (GCC/Clang): builtin type macros match system headers */
/* ------------------------------------------------------------------ */
typedef __UINT8_TYPE__   uint8_t;
typedef __INT8_TYPE__    int8_t;
typedef __UINT16_TYPE__  uint16_t;
typedef __INT16_TYPE__   int16_t;
typedef __UINT32_TYPE__  uint32_t;
typedef __INT32_TYPE__   int32_t;
typedef __UINT64_TYPE__  uint64_t;
typedef __INT64_TYPE__   int64_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTPTR_TYPE__  intptr_t;
typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef __cplusplus
typedef _Bool bool;
#define true          1
#define false         0
#endif

/* GCC/Clang builtin varargs.  __builtin_va_list is what the system headers
 * use internally, so defining va_list as it avoids redefinition conflicts. */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(dest, src) __builtin_va_copy(dest, src)

#elif defined(__SDCC_z80)
/* ------------------------------------------------------------------ */
/* SDCC Z80 target: int=16, long=32, long long=32                    */
/* ------------------------------------------------------------------ */
typedef unsigned char   uint8_t;
typedef signed char     int8_t;
typedef unsigned int    uint16_t;
typedef signed int      int16_t;
typedef unsigned long   uint32_t;
typedef signed long     int32_t;
typedef unsigned long long uint64_t;
typedef signed long long int64_t;
typedef unsigned int    uintptr_t;
typedef signed int      intptr_t;
typedef unsigned int    size_t;
typedef int             ptrdiff_t;

#ifndef __cplusplus
typedef unsigned char bool;
#define true          1
#define false         0
#endif

/* SDCC z80 varargs: va_list is an unsigned char * walking the stack. */
typedef unsigned char * va_list;
#define va_start(marker, last)  { marker = (va_list)&last + sizeof(last); }
#define va_arg(marker, type)    *((type *)((marker += sizeof(type)) - sizeof(type)))
#define va_end(marker)          { marker = (va_list) 0; }
#define va_copy(dest, src)      { dest = src; }

#else
#error "ztypes.h: unsupported compiler (needs GCC/Clang __SIZE_TYPE__ or SDCC __SDCC_z80)"
#endif

/* NULL pointer constant. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* offsetof: usable in constant expressions on both SDCC and GCC/Clang. */
#ifndef offsetof
#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)
#endif

#endif /* MUZIX_TYPES_H */
