/* The fast-key-erasure random-number generator D. J. Bernstein describes
   in "Fast-key-erasure random-number generators", the cr.yp.to blog,
   2017-07-23 (docs/spec/README.md), with ChaCha20 as its stream cipher.
   The generator is deterministic: every byte it returns follows from its
   key, which only generator_inject puts entropy into. */

#ifndef NOVA_GENERATOR_H
#define NOVA_GENERATOR_H

#include "types.h"
#include "chacha20.h"
#include "sha256.h"

#define GENERATOR_KEY_SIZE CHACHA20_KEY_SIZE

/* Each refill is 768 bytes of cipher output, the size of the post's
   example, which is twelve ChaCha20 blocks: the first 32 bytes are the
   next key and the other 736 the next output. */
#define GENERATOR_REFILL_BLOCKS 12
#define GENERATOR_BUFFER_SIZE                                    \
    (GENERATOR_REFILL_BLOCKS * CHACHA20_BLOCK_SIZE - GENERATOR_KEY_SIZE)

typedef struct {
    UINT8 key[GENERATOR_KEY_SIZE];
    UINT8 buffer[GENERATOR_BUFFER_SIZE];
    UINT64 available; /* output not yet returned: the buffer's last bytes */
} GENERATOR;

/* Key 0 and no output, the state the post starts from. */
void generator_init(GENERATOR *generator);

/* The post's inject(e, elen): replace the key with
   SHA-256(key, SHA-256(e, elen)) and clear the output buffer. digest is
   SHA-256(e, elen), which the caller computes so that it can hash e in
   pieces. */
void generator_inject(GENERATOR *generator,
                      const UINT8 digest[SHA256_DIGEST_SIZE]);

/* Write count bytes of output to out. */
void generator_read(GENERATOR *generator, UINT8 *out, UINT64 count);

#endif
