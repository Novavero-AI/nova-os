/* SHA-256, as FIPS 180-4 specifies it, over a message given in pieces. */

#ifndef NOVA_SHA256_H
#define NOVA_SHA256_H

#include "types.h"

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE 64
#define SHA256_STATE_WORDS 8

typedef struct {
    UINT32 hash[SHA256_STATE_WORDS]; /* the intermediate hash value */
    UINT64 length;                   /* message bytes taken so far */
    UINT8 block[SHA256_BLOCK_SIZE];  /* the message block being filled */
} SHA256_CONTEXT;

void sha256_init(SHA256_CONTEXT *context);

/* Take size more bytes of the message. */
void sha256_update(SHA256_CONTEXT *context, const VOID *data, UINT64 size);

/* Write the message's digest, then wipe the context, which holds message
   bytes. */
void sha256_final(SHA256_CONTEXT *context, UINT8 digest[SHA256_DIGEST_SIZE]);

#endif
