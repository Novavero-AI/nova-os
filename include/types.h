/* The integer types nova-os's freestanding code uses, named as in UEFI.
   x86_64 PE is LLP64: long is 32 bits, so 64-bit types are long long. */

#ifndef NOVA_TYPES_H
#define NOVA_TYPES_H

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef long long INT64;
typedef UINT64 UINTN;
typedef void VOID;

#endif
