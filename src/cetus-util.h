#ifndef _GLIB_EXT_STRING_LEN_H_
#define _GLIB_EXT_STRING_LEN_H_

#include <inttypes.h>

/**
 * simple macros to get the data and length of a "string"
 *
 * C() is for constant strings like "foo"
 * S() is for GString's 
 */
#define C(x) x, sizeof(x) - 1
#define S(x) (x) ? (x)->str : NULL, (x) ? (x)->len : 0
#define L(x) x, strlen(x)

typedef int32_t BitArray;
#define SetBit(A,k)     (A[(k/32)] |= (1 << (k%32)))
#define ClearBit(A,k)   (A[(k/32)] &= ~(1 << (k%32)))
#define TestBit(A,k)    (A[(k/32)] & (1 << (k%32)))

void cetus_string_dequote(char *z);

#define KB 1024
#define MB 1024 * KB
#define GB 1024 * MB

gboolean read_file_to_buffer(const char *filename, char **buffer);

#endif
