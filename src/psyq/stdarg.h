/*
 * File:stdarg.h
 */
/*
 * $PSLibId: Run-time Library Release 4.6$
 */

#ifndef _STDARG_H
#define _STDARG_H

#ifdef MEMORIES_PC
/* Native build: the pointer-walking macros below assume the MIPS ABI. */
typedef __builtin_va_list va_list;
#define va_start(AP, LASTARG) __builtin_va_start(AP, LASTARG)
#define va_end(AP) __builtin_va_end(AP)
#define va_arg(AP, TYPE) __builtin_va_arg(AP, TYPE)
#else

#define __va_rounded_size(TYPE)  \
  (((sizeof (TYPE) + sizeof (int) - 1) / sizeof (int)) * sizeof (int))

#define va_start(AP, LASTARG) 						\
 (AP = ((char *)&(LASTARG) + __va_rounded_size(LASTARG)))

#define va_end(AP) AP = (char *)NULL

#define va_arg(AP, TYPE)						\
 (AP = ((char *) (AP)) += __va_rounded_size (TYPE),			\
  *((TYPE *) ((char *) (AP) - __va_rounded_size (TYPE))))


typedef void *va_list;
#endif

#endif /* _STDARG_H */
