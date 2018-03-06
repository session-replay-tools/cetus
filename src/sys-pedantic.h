#ifndef _SYS_PEDANTIC_H_
#define _SYS_PEDANTIC_H_

/** @file
 * a set of macros to make programming C easier 
 */

#ifdef UNUSED_PARAM
#elif defined(__GNUC__)
# define UNUSED_PARAM(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED_PARAM(x) /*@unused@*/ x
#else
# define UNUSED_PARAM(x) x
#endif

#define F_SIZE_T "%"G_GSIZE_FORMAT
#define F_U64 "%"G_GUINT64_FORMAT

#endif
