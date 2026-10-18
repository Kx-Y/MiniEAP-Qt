#ifndef TYPES_H
#define TYPES_H

/*
 * Fixed width primitives used by the MD5/Ruijie hash code.
 *
 * On Windows the same names (LONG, DWORD, ...) also belong to the Win32 API
 * that packet_builder.c pulls in through winsock2.h. Redefining them here would
 * be a conflicting declaration, so defer to the Windows SDK types: they have
 * exactly the sizes this code assumes.
 */
#ifdef _WIN32
#include <windows.h>

/* UINT4 is this project's own name for a 32 bit word, not a Win32 type */
#ifndef UINT4
typedef unsigned int UINT4;
#endif

/* The MD5 implementation predates uint8_t* and still uses these names */
#ifndef POINTER
typedef unsigned char *POINTER;
#endif
#ifndef BYTE
typedef unsigned char BYTE;
#endif
#ifndef UCHAR
typedef unsigned char UCHAR;
#endif
#ifndef WORD
typedef unsigned short int WORD;
#endif
#else
typedef unsigned char *POINTER;
typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef unsigned short int WORD;
typedef int LONG;
typedef unsigned int DWORD;
typedef unsigned int UINT4;
#endif

#endif
