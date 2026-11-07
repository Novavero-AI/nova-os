/* BLAKE2b, as RFC 7693 specifies it, over a message given in pieces,
   with an optional key (3.3). Argon2 hashes with it unkeyed (RFC 9106
   3.2); the key serves RFC 7693's self-test, which hashes keyed and
   unkeyed messages alike (Appendix E). */

#ifndef NOVA_BLAKE2B_H
#define NOVA_BLAKE2B_H

#include "types.h"

/* bb, the block size, and the largest nn and kk (2.1). */
#define BLAKE2B_BLOCK_SIZE 128
#define BLAKE2B_OUT_MAX 64
#define BLAKE2B_KEY_MAX 64
#define BLAKE2B_STATE_WORDS 8

typedef struct {
    UINT64 h[BLAKE2B_STATE_WORDS];
    UINT64 t[2]; /* the 128-bit byte offset, low word first */
    UINT8 block[BLAKE2B_BLOCK_SIZE];
    UINT32 filled;     /* bytes in block, not yet compressed */
    UINT32 out_length; /* nn */
} BLAKE2B_CONTEXT;

/* Start a hash of out_length bytes, 1 to BLAKE2B_OUT_MAX, keyed by the
   key_length bytes at key, 0 to BLAKE2B_KEY_MAX. Returns 0 if either
   length is out of range. */
int blake2b_init(BLAKE2B_CONTEXT *context, UINT32 out_length,
                 const UINT8 *key, UINT32 key_length);

/* Take size more bytes of the message. */
void blake2b_update(BLAKE2B_CONTEXT *context, const VOID *data, UINT64 size);

/* Write the out_length bytes of the digest, then wipe the context, which
   holds message bytes. */
void blake2b_final(BLAKE2B_CONTEXT *context, UINT8 *out);

/* Whether this BLAKE2b gives RFC 7693's values: BLAKE2b-512("abc")
   (Appendix A) and the self-test's hash of keyed and unkeyed hashes of
   several lengths (Appendix E). */
int blake2b_self_test(void);

#endif
