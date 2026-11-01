#ifndef TYPES_H
#define TYPES_H
#include <stdint.h>

/* Only the types used by the RSA MD5 implementation belong here.
 * In particular, LONG and DWORD are Windows SDK names. */
typedef uint8_t *POINTER;
typedef uint8_t UCHAR;
typedef uint32_t UINT4;
#endif
