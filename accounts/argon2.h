/* Argon2id, as RFC 9106 specifies it, version 0x13, over memory its
   caller provides. */

#ifndef NOVA_ARGON2_H
#define NOVA_ARGON2_H

#include "types.h"

#define ARGON2_BLOCK_SIZE 1024
#define ARGON2_BLOCK_WORDS (ARGON2_BLOCK_SIZE / 8)

/* The tag's least length (3.1). */
#define ARGON2_TAG_MIN 4

typedef struct {
    UINT64 v[ARGON2_BLOCK_WORDS];
} ARGON2_BLOCK;

/* The inputs of 3.1. A secret and associated data of no bytes are
   absent, with their pointers unused. */
typedef struct {
    const UINT8 *password;
    UINT32 password_length;
    const UINT8 *salt;
    UINT32 salt_length;
    const UINT8 *secret;
    UINT32 secret_length;
    const UINT8 *data;
    UINT32 data_length;
    UINT32 lanes;      /* p */
    UINT32 memory_kib; /* m */
    UINT32 passes;     /* t */
    UINT32 tag_length; /* T */
} ARGON2_INPUT;

/* m', the blocks the hash of input fills (3.2, Figure 2). */
UINT64 argon2_blocks(const ARGON2_INPUT *input);

/* The Argon2id tag of input, tag_length bytes into tag, filling the
   first argon2_blocks(input) blocks at memory, of which there are
   memory_blocks, and wiping them before it returns. Returns 0, with tag
   untouched, if a parameter is outside the ranges of 3.1 or memory is
   too small. */
int argon2id(const ARGON2_INPUT *input, ARGON2_BLOCK *memory,
             UINT64 memory_blocks, UINT8 *tag);

/* Whether argon2id gives the tag of RFC 9106's Argon2id test vector
   (5.3), computed in the 32 blocks it needs at memory, of which there
   are memory_blocks. */
int argon2_self_test(ARGON2_BLOCK *memory, UINT64 memory_blocks);

#endif
