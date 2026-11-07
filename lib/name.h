/** @file
    Names in the namespace (docs/spec/objects.md O8, storage.md fact 4). */

#ifndef NOVA_NAME_H
#define NOVA_NAME_H

#include "native_abi.h"

/** The longest name, NAME_MAX of POSIX file systems, which the store
    image keeps to as well (store_image.h). */
#define NAME_MAX_BYTES 255

/** Whether the length bytes at name are one path component: not empty,
    "." or "..", no '/' or NUL, well-formed UTF-8 (RFC 3629), and at most
    NAME_MAX_BYTES. */
int name_valid(const UINT8 *name, UINT32 length);

#endif
