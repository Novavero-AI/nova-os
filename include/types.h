/** @file
    The integer types nova-os's freestanding code uses, named as in UEFI.
    x86_64 PE is LLP64: long is 32 bits, so 64-bit types are long long. */

#ifndef NOVA_TYPES_H
#define NOVA_TYPES_H

typedef unsigned char UINT8;       /**< an unsigned 8-bit integer */
typedef unsigned short UINT16;     /**< an unsigned 16-bit integer */
typedef unsigned int UINT32;       /**< an unsigned 32-bit integer */
typedef unsigned long long UINT64; /**< an unsigned 64-bit integer */
typedef int INT32;                 /**< a signed 32-bit integer */
typedef long long INT64;           /**< a signed 64-bit integer */
/** UEFI's unsigned integer of native width, 64 bits here in every
    build. */
typedef UINT64 UINTN;
typedef void VOID; /**< UEFI's name for void */

#endif
