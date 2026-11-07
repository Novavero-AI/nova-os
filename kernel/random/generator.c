/* The fast-key-erasure generator, as the post describes it: "Starting
   from the 256-bit key k, generate (say) 48 blocks", 768 bytes;
   "immediately overwrite the key k" with the first 32 bytes; "use the
   other blocks ... as 736 bytes of RNG output, of course erasing each
   byte as soon as it is consumed. Start over with the new key."

   Here a refill runs the ChaCha20 block function under the current key
   with an all-zero nonce and block counters 0 to 11, as the post's RNG
   always takes the cipher's nonce to be 0 and starts its counter at 0.
   Every block is generated before the key is overwritten, and the key is
   overwritten before any byte of the refill is returned. A read takes
   bytes out of the buffer before it copies them, so that no byte is
   returned twice even if the copy stops partway, and zeroes them after.
   So after any read the state holds the next key and output not yet
   returned, and nothing from which an output already returned can be
   computed.

   generator_inject is the post's inject(e, elen), with SHA-256(k, x)
   taken as the hash of the 64 bytes k followed by x. */

#include "generator.h"
#include "wipe.h"

/* The cipher's nonce for every block. */
static const UINT8 zero_nonce[CHACHA20_NONCE_SIZE];

static void copy(UINT8 *to, const UINT8 *from, UINT64 size)
{
    UINT64 i;

    for (i = 0; i < size; i++)
        to[i] = from[i];
}

/* Block b of the refill is its bytes 64b to 64b + 63. The key takes the
   first 32, and the buffer holds the rest in order, so block b from 1 on
   lands 32 bytes before its place in the refill. */
static void refill(GENERATOR *generator)
{
    UINT8 first[CHACHA20_BLOCK_SIZE];
    UINT32 block;

    chacha20_block(generator->key, 0, zero_nonce, first);
    for (block = 1; block < GENERATOR_REFILL_BLOCKS; block++)
        chacha20_block(generator->key, block, zero_nonce,
                       generator->buffer + block * CHACHA20_BLOCK_SIZE -
                           GENERATOR_KEY_SIZE);
    copy(generator->key, first, GENERATOR_KEY_SIZE);
    copy(generator->buffer, first + GENERATOR_KEY_SIZE,
         CHACHA20_BLOCK_SIZE - GENERATOR_KEY_SIZE);
    wipe(first, sizeof(first));
    generator->available = GENERATOR_BUFFER_SIZE;
}

void generator_init(GENERATOR *generator)
{
    wipe(generator, sizeof(*generator));
}

void generator_inject(GENERATOR *generator,
                      const UINT8 digest[SHA256_DIGEST_SIZE])
{
    SHA256_CONTEXT context;

    sha256_init(&context);
    sha256_update(&context, generator->key, GENERATOR_KEY_SIZE);
    sha256_update(&context, digest, SHA256_DIGEST_SIZE);
    sha256_final(&context, generator->key);
    wipe(generator->buffer, sizeof(generator->buffer));
    generator->available = 0;
}

void generator_read(GENERATOR *generator, UINT8 *out, UINT64 count)
{
    while (count > 0) {
        UINT8 *next;
        UINT64 take;

        if (generator->available == 0)
            refill(generator);
        take = count < generator->available ? count : generator->available;
        next = generator->buffer +
               (GENERATOR_BUFFER_SIZE - generator->available);
        generator->available -= take;
        copy(out, next, take);
        wipe(next, take);
        out += take;
        count -= take;
    }
}
