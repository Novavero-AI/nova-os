/* SHA-256 (FIPS 180-4): the padding of 5.1.1, the initial hash value of
   5.3.3 and the computation of 6.2.2, with the functions of 4.1.2 and the
   constants of 4.2.2. Words are big-endian (3.1). The message schedule
   and the working variables hold message bytes, and the generator hashes
   its key, so both are wiped after each block. */

#include "sha256.h"
#include "wipe.h"

#define SCHEDULE_WORDS 64
#define BLOCK_WORDS 16
#define WORD_SIZE 4

/* The padded message ends with its length in bits as a 64-bit
   big-endian integer, in the last bytes of a block (5.1.1). */
#define LENGTH_SIZE 8
#define LENGTH_OFFSET (SHA256_BLOCK_SIZE - LENGTH_SIZE)
#define FIRST_PAD_BYTE 0x80
#define BITS_PER_BYTE 8

/* 5.3.3 */
static const UINT32 initial_hash[SHA256_STATE_WORDS] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

/* 4.2.2 */
static const UINT32 k[SCHEDULE_WORDS] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/* The working variables a to h of 6.2.2, as indexes. */
enum { A, B, C, D, E, F, G, H };

static UINT32 rotate_right(UINT32 x, int n)
{
    return x >> n | x << (32 - n);
}

/* 4.1.2: Ch, Maj, the upper-case sigmas (4.4, 4.5) and the lower-case
   sigmas (4.6, 4.7). */
static UINT32 ch(UINT32 x, UINT32 y, UINT32 z) { return (x & y) ^ (~x & z); }

static UINT32 maj(UINT32 x, UINT32 y, UINT32 z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static UINT32 big_sigma0(UINT32 x)
{
    return rotate_right(x, 2) ^ rotate_right(x, 13) ^ rotate_right(x, 22);
}

static UINT32 big_sigma1(UINT32 x)
{
    return rotate_right(x, 6) ^ rotate_right(x, 11) ^ rotate_right(x, 25);
}

static UINT32 small_sigma0(UINT32 x)
{
    return rotate_right(x, 7) ^ rotate_right(x, 18) ^ x >> 3;
}

static UINT32 small_sigma1(UINT32 x)
{
    return rotate_right(x, 17) ^ rotate_right(x, 19) ^ x >> 10;
}

static UINT32 load32_be(const UINT8 *p)
{
    return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 |
           (UINT32)p[3];
}

/* 6.2.2, steps 1 to 4, for one message block. */
static void compress(UINT32 hash[SHA256_STATE_WORDS],
                     const UINT8 block[SHA256_BLOCK_SIZE])
{
    UINT32 w[SCHEDULE_WORDS], v[SHA256_STATE_WORDS], t1, t2;
    int t;

    for (t = 0; t < BLOCK_WORDS; t++)
        w[t] = load32_be(block + WORD_SIZE * t);
    for (t = BLOCK_WORDS; t < SCHEDULE_WORDS; t++)
        w[t] = small_sigma1(w[t - 2]) + w[t - 7] + small_sigma0(w[t - 15]) +
               w[t - 16];
    for (t = 0; t < SHA256_STATE_WORDS; t++)
        v[t] = hash[t];
    for (t = 0; t < SCHEDULE_WORDS; t++) {
        t1 = v[H] + big_sigma1(v[E]) + ch(v[E], v[F], v[G]) + k[t] + w[t];
        t2 = big_sigma0(v[A]) + maj(v[A], v[B], v[C]);
        v[H] = v[G];
        v[G] = v[F];
        v[F] = v[E];
        v[E] = v[D] + t1;
        v[D] = v[C];
        v[C] = v[B];
        v[B] = v[A];
        v[A] = t1 + t2;
    }
    for (t = 0; t < SHA256_STATE_WORDS; t++)
        hash[t] += v[t];
    wipe(w, sizeof(w));
    wipe(v, sizeof(v));
    wipe(&t1, sizeof(t1));
    wipe(&t2, sizeof(t2));
}

void sha256_init(SHA256_CONTEXT *context)
{
    int i;

    for (i = 0; i < SHA256_STATE_WORDS; i++)
        context->hash[i] = initial_hash[i];
    context->length = 0;
}

void sha256_update(SHA256_CONTEXT *context, const VOID *data, UINT64 size)
{
    const UINT8 *bytes = data;
    UINT64 used = context->length % SHA256_BLOCK_SIZE;

    context->length += size;
    for (; size > 0; size--) {
        context->block[used++] = *bytes++;
        if (used == SHA256_BLOCK_SIZE) {
            compress(context->hash, context->block);
            used = 0;
        }
    }
}

/* 5.1.1: a 1 bit, zero bits up to the length field, which takes one more
   block when the message leaves no room for it, then the length. 6.2.2:
   the digest is the final hash value, its words in order. */
void sha256_final(SHA256_CONTEXT *context, UINT8 digest[SHA256_DIGEST_SIZE])
{
    UINT64 used = context->length % SHA256_BLOCK_SIZE;
    UINT64 bits = context->length * BITS_PER_BYTE;
    int i;

    context->block[used++] = FIRST_PAD_BYTE;
    if (used > LENGTH_OFFSET) {
        while (used < SHA256_BLOCK_SIZE)
            context->block[used++] = 0;
        compress(context->hash, context->block);
        used = 0;
    }
    while (used < LENGTH_OFFSET)
        context->block[used++] = 0;
    for (i = 0; i < LENGTH_SIZE; i++)
        context->block[LENGTH_OFFSET + i] =
            (UINT8)(bits >> BITS_PER_BYTE * (LENGTH_SIZE - 1 - i));
    compress(context->hash, context->block);
    for (i = 0; i < SHA256_STATE_WORDS; i++) {
        UINT32 word = context->hash[i];

        digest[WORD_SIZE * i] = (UINT8)(word >> 24);
        digest[WORD_SIZE * i + 1] = (UINT8)(word >> 16);
        digest[WORD_SIZE * i + 2] = (UINT8)(word >> 8);
        digest[WORD_SIZE * i + 3] = (UINT8)word;
    }
    wipe(context, sizeof(*context));
}
