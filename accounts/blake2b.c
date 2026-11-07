/* BLAKE2b (RFC 7693): the parameter block of 2.5, the IV of 2.6, the
   schedule of 2.7, G and F of 3.1 and 3.2, and the padding and final
   block of 3.3. Words are little-endian (2.4). The work vector and the
   message words hold message bytes, so F wipes both. */

#include "blake2b.h"
#include "wipe.h"

#define ROUNDS 12
#define WORK_WORDS 16
#define SCHEDULES 10
#define WORD_SIZE 8
#define BITS_PER_BYTE 8

/* G's rotations for BLAKE2b, (R1, R2, R3, R4) (2.1). */
#define R1 32
#define R2 24
#define R3 16
#define R4 63

/* p[0]'s fixed bytes 2 and 3, 0x01 each (2.5), and where kk sits. */
#define PARAMETER_FIXED 0x01010000ULL
#define PARAMETER_KEY_SHIFT 8

/* Appendix E's parameter sets and its generator's multiplier. */
#define SELF_TEST_LENGTHS 6
#define SELF_TEST_SIZES 4
#define SELF_TEST_INPUT_MAX 1024
#define SELF_TEST_DIGEST_SIZE 32
#define SELF_TEST_MULTIPLIER 0xDEAD4BADu
#define SELF_TEST_BYTE_SHIFT 24

/* 2.6: SHA-512's IV. */
static const UINT64 iv[BLAKE2B_STATE_WORDS] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

/* 2.7; rounds 10 and 11 use rows 0 and 1 again. */
static const UINT8 sigma[SCHEDULES][WORK_WORDS] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

/* Appendix A: BLAKE2b-512("abc"). */
static const UINT8 abc_digest[BLAKE2B_OUT_MAX] = {
    0xba, 0x80, 0xa5, 0x3f, 0x98, 0x1c, 0x4d, 0x0d, 0x6a, 0x27, 0x97,
    0xb6, 0x9f, 0x12, 0xf6, 0xe9, 0x4c, 0x21, 0x2f, 0x14, 0x68, 0x5a,
    0xc4, 0xb7, 0x4b, 0x12, 0xbb, 0x6f, 0xdb, 0xff, 0xa2, 0xd1, 0x7d,
    0x87, 0xc5, 0x39, 0x2a, 0xab, 0x79, 0x2d, 0xc2, 0x52, 0xd5, 0xde,
    0x45, 0x33, 0xcc, 0x95, 0x18, 0xd3, 0x8a, 0xa8, 0xdb, 0xf1, 0x92,
    0x5a, 0xb9, 0x23, 0x86, 0xed, 0xd4, 0x00, 0x99, 0x23};

/* Appendix E: blake2b_selftest's parameter sets and its grand hash. */
static const UINT32 self_test_sizes[SELF_TEST_SIZES] = {20, 32, 48, 64};
static const UINT32 self_test_lengths[SELF_TEST_LENGTHS] = {0,   3,   128,
                                                            129, 255, 1024};
static const UINT8 self_test_result[SELF_TEST_DIGEST_SIZE] = {
    0xc2, 0x3a, 0x78, 0x00, 0xd9, 0x81, 0x23, 0xbd, 0x10, 0xf5, 0x06,
    0xc6, 0x1e, 0x29, 0xda, 0x56, 0x03, 0xd7, 0x63, 0xb8, 0xbb, 0xad,
    0x2e, 0x73, 0x7f, 0x5e, 0x76, 0x5a, 0x7b, 0xcc, 0xd4, 0x75};

static UINT64 rotate_right(UINT64 x, UINT32 n)
{
    return x >> n | x << (64 - n);
}

static UINT64 load64(const UINT8 *p)
{
    UINT64 x = 0;
    UINT32 i;

    for (i = WORD_SIZE; i-- > 0;)
        x = x << BITS_PER_BYTE | p[i];
    return x;
}

/* 3.1 */
static void mix(UINT64 *v, UINT32 a, UINT32 b, UINT32 c, UINT32 d, UINT64 x,
                UINT64 y)
{
    v[a] = v[a] + v[b] + x;
    v[d] = rotate_right(v[d] ^ v[a], R1);
    v[c] = v[c] + v[d];
    v[b] = rotate_right(v[b] ^ v[c], R2);
    v[a] = v[a] + v[b] + y;
    v[d] = rotate_right(v[d] ^ v[a], R3);
    v[c] = v[c] + v[d];
    v[b] = rotate_right(v[b] ^ v[c], R4);
}

/* 3.2: F over the context's block, with t its offset and last its flag. */
static void compress(BLAKE2B_CONTEXT *context, int last)
{
    UINT64 v[WORK_WORDS], m[WORK_WORDS];
    const UINT8 *s;
    UINT32 i, round;

    for (i = 0; i < WORK_WORDS; i++)
        m[i] = load64(context->block + i * WORD_SIZE);
    for (i = 0; i < BLAKE2B_STATE_WORDS; i++) {
        v[i] = context->h[i];
        v[i + BLAKE2B_STATE_WORDS] = iv[i];
    }
    v[12] ^= context->t[0];
    v[13] ^= context->t[1];
    if (last)
        v[14] = ~v[14];
    for (round = 0; round < ROUNDS; round++) {
        s = sigma[round % SCHEDULES];
        mix(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        mix(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        mix(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        mix(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        mix(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        mix(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        mix(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        mix(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (i = 0; i < BLAKE2B_STATE_WORDS; i++)
        context->h[i] ^= v[i] ^ v[i + BLAKE2B_STATE_WORDS];
    wipe(v, sizeof v);
    wipe(m, sizeof m);
}

/* Add the bytes of a block to the offset, carrying into its high word. */
static void count(BLAKE2B_CONTEXT *context, UINT32 bytes)
{
    context->t[0] += bytes;
    if (context->t[0] < bytes)
        context->t[1]++;
}

int blake2b_init(BLAKE2B_CONTEXT *context, UINT32 out_length,
                 const UINT8 *key, UINT32 key_length)
{
    UINT32 i;

    if (out_length == 0 || out_length > BLAKE2B_OUT_MAX ||
        key_length > BLAKE2B_KEY_MAX)
        return 0;
    for (i = 0; i < BLAKE2B_STATE_WORDS; i++)
        context->h[i] = iv[i];
    context->h[0] ^= PARAMETER_FIXED |
                     (UINT64)key_length << PARAMETER_KEY_SHIFT | out_length;
    context->t[0] = 0;
    context->t[1] = 0;
    context->filled = 0;
    context->out_length = out_length;
    for (i = 0; i < BLAKE2B_BLOCK_SIZE; i++)
        context->block[i] = 0;
    /* A key, padded with zeros, is the first block (3.3). */
    if (key_length) {
        for (i = 0; i < key_length; i++)
            context->block[i] = key[i];
        context->filled = BLAKE2B_BLOCK_SIZE;
    }
    return 1;
}

/* A full block is compressed only once more bytes follow it, since the
   last block, full or not, is compressed with the final flag. */
void blake2b_update(BLAKE2B_CONTEXT *context, const VOID *data, UINT64 size)
{
    const UINT8 *in = data;

    while (size > 0) {
        if (context->filled == BLAKE2B_BLOCK_SIZE) {
            count(context, BLAKE2B_BLOCK_SIZE);
            compress(context, 0);
            context->filled = 0;
        }
        for (; size > 0 && context->filled < BLAKE2B_BLOCK_SIZE; size--)
            context->block[context->filled++] = *in++;
    }
}

void blake2b_final(BLAKE2B_CONTEXT *context, UINT8 *out)
{
    UINT32 i;

    count(context, context->filled);
    for (i = context->filled; i < BLAKE2B_BLOCK_SIZE; i++)
        context->block[i] = 0;
    compress(context, 1);
    for (i = 0; i < context->out_length; i++)
        out[i] = (UINT8)(context->h[i / WORD_SIZE] >>
                         (i % WORD_SIZE * BITS_PER_BYTE));
    wipe(context, sizeof *context);
}

/* ------------------------------------------------------------------------
   Self-test. */

/* Appendix E's selftest_seq: a Fibonacci sequence's top bytes. */
static void self_test_sequence(UINT8 *out, UINT32 length, UINT32 seed)
{
    UINT32 a = SELF_TEST_MULTIPLIER * seed, b = 1, t, i;

    for (i = 0; i < length; i++) {
        t = a + b;
        a = b;
        b = t;
        out[i] = (UINT8)(t >> SELF_TEST_BYTE_SHIFT);
    }
}

static int same(const UINT8 *a, const UINT8 *b, UINT32 count_bytes)
{
    UINT32 i;

    for (i = 0; i < count_bytes; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void hash(UINT8 *out, UINT32 out_length, const UINT8 *key,
                 UINT32 key_length, const UINT8 *in, UINT32 in_length)
{
    BLAKE2B_CONTEXT context;

    blake2b_init(&context, out_length, key, key_length);
    blake2b_update(&context, in, in_length);
    blake2b_final(&context, out);
}

int blake2b_self_test(void)
{
    static UINT8 in[SELF_TEST_INPUT_MAX];
    UINT8 md[BLAKE2B_OUT_MAX], key[BLAKE2B_KEY_MAX];
    BLAKE2B_CONTEXT grand;
    UINT32 i, j, out_length, in_length;

    hash(md, BLAKE2B_OUT_MAX, 0, 0, (const UINT8 *)"abc", 3);
    if (!same(md, abc_digest, BLAKE2B_OUT_MAX))
        return 0;
    blake2b_init(&grand, SELF_TEST_DIGEST_SIZE, 0, 0);
    for (i = 0; i < SELF_TEST_SIZES; i++) {
        out_length = self_test_sizes[i];
        for (j = 0; j < SELF_TEST_LENGTHS; j++) {
            in_length = self_test_lengths[j];
            self_test_sequence(in, in_length, in_length);
            hash(md, out_length, 0, 0, in, in_length);
            blake2b_update(&grand, md, out_length);
            self_test_sequence(key, out_length, out_length);
            hash(md, out_length, key, out_length, in, in_length);
            blake2b_update(&grand, md, out_length);
        }
    }
    blake2b_final(&grand, md);
    return same(md, self_test_result, SELF_TEST_DIGEST_SIZE);
}
