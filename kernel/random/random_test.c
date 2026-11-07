/* The random number generator's boot-time self-test: ChaCha20 against
   RFC 8439, SHA-256 against RFC 6234, and the fast-key-erasure
   construction against ChaCha20's published output under key 0, before
   the kernel trusts any of them with a key. */

#include "random.h"
#include "chacha20.h"
#include "generator.h"
#include "sha256.h"

/* RFC 8439 2.3.2: the key 00 01 ... 1f, the nonce
   (00:00:00:09:00:00:00:4a:00:00:00:00) and block count 1 give this
   serialized block. */
static const UINT8 block_key[CHACHA20_KEY_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const UINT8 block_nonce[CHACHA20_NONCE_SIZE] = {
    0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
#define BLOCK_COUNT 1
static const UINT8 block_expected[CHACHA20_BLOCK_SIZE] = {
    0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
    0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
    0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03,
    0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
    0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09,
    0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
    0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9,
    0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e};

/* RFC 8439 A.1, test vectors 1 and 2: the blocks for counters 0 and 1
   under the all-zero key and nonce. A generator starting from key 0
   takes its next key from the first half of block 0, returns the second
   half, then returns block 1. */
static const UINT8 zero_key_block0[CHACHA20_BLOCK_SIZE] = {
    0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90,
    0x40, 0x5d, 0x6a, 0xe5, 0x53, 0x86, 0xbd, 0x28,
    0xbd, 0xd2, 0x19, 0xb8, 0xa0, 0x8d, 0xed, 0x1a,
    0xa8, 0x36, 0xef, 0xcc, 0x8b, 0x77, 0x0d, 0xc7,
    0xda, 0x41, 0x59, 0x7c, 0x51, 0x57, 0x48, 0x8d,
    0x77, 0x24, 0xe0, 0x3f, 0xb8, 0xd8, 0x4a, 0x37,
    0x6a, 0x43, 0xb8, 0xf4, 0x15, 0x18, 0xa1, 0x1c,
    0xc3, 0x87, 0xb6, 0x69, 0xb2, 0xee, 0x65, 0x86};
static const UINT8 zero_key_block1[CHACHA20_BLOCK_SIZE] = {
    0x9f, 0x07, 0xe7, 0xbe, 0x55, 0x51, 0x38, 0x7a,
    0x98, 0xba, 0x97, 0x7c, 0x73, 0x2d, 0x08, 0x0d,
    0xcb, 0x0f, 0x29, 0xa0, 0x48, 0xe3, 0x65, 0x69,
    0x12, 0xc6, 0x53, 0x3e, 0x32, 0xee, 0x7a, 0xed,
    0x29, 0xb7, 0x21, 0x76, 0x9c, 0xe6, 0x4e, 0x43,
    0xd5, 0x71, 0x33, 0xb0, 0x74, 0xd8, 0x39, 0xd5,
    0x31, 0xed, 0x1f, 0x28, 0x51, 0x0a, 0xfb, 0x45,
    0xac, 0xe1, 0x0a, 0x1f, 0x4b, 0x79, 0x4d, 0x6f};

/* RFC 6234 8.5: TEST1, TEST2_1, and TEST4 repeated ten times, with the
   SHA-256 results its test driver expects. TEST2_1 leaves no room for
   the length in its block, so its padding takes a second block; TEST4 is
   exactly one block. */
static const char test1[] = "abc";
static const UINT8 test1_digest[SHA256_DIGEST_SIZE] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
static const char test2[] =
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const UINT8 test2_digest[SHA256_DIGEST_SIZE] = {
    0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
    0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
    0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
    0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1};
static const char test4[] =
    "0123456701234567012345670123456701234567012345670123456701234567";
_Static_assert(sizeof(test4) - 1 == SHA256_BLOCK_SIZE, "TEST4 is one block");
#define TEST4_REPEATS 10
static const UINT8 test4_digest[SHA256_DIGEST_SIZE] = {
    0x59, 0x48, 0x47, 0x32, 0x84, 0x51, 0xbd, 0xfa,
    0x85, 0x05, 0x62, 0x25, 0x46, 0x2c, 0xc1, 0xd8,
    0x67, 0xd8, 0x77, 0xfb, 0x38, 0x8d, 0xf0, 0xce,
    0x35, 0xf2, 0x5a, 0xb5, 0x56, 0x2b, 0xfb, 0xb5};

/* SHA-256 of the first 32 bytes of zero_key_block0 followed by
   test1_digest: the key generator_inject makes from the digest of "abc"
   once the generator above has refilled from key 0. */
static const UINT8 inject_expected[SHA256_DIGEST_SIZE] = {
    0xe7, 0xcf, 0x26, 0xcb, 0xbd, 0x0c, 0xb9, 0x0c,
    0xa0, 0x4b, 0x0f, 0x14, 0x40, 0xe8, 0x81, 0x8c,
    0xe1, 0x11, 0x98, 0x11, 0xe1, 0x2b, 0x77, 0xc7,
    0x9f, 0x57, 0x8c, 0xaf, 0x4d, 0x01, 0x9f, 0x56};

static int equal(const UINT8 *a, const UINT8 *b, UINT64 size)
{
    UINT64 i;

    for (i = 0; i < size; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int all_zero(const UINT8 *bytes, UINT64 size)
{
    UINT64 i;

    for (i = 0; i < size; i++)
        if (bytes[i])
            return 0;
    return 1;
}

static const char *test_chacha20(void)
{
    UINT8 out[CHACHA20_BLOCK_SIZE];

    chacha20_block(block_key, BLOCK_COUNT, block_nonce, out);
    if (!equal(out, block_expected, sizeof(out)))
        return "ChaCha20 differs from RFC 8439 2.3.2";
    return 0;
}

/* TEST2_1 goes in a byte at a time and TEST4 a block at a time, so the
   message reaches the context in pieces of both kinds. */
static const char *test_sha256(void)
{
    SHA256_CONTEXT context;
    UINT8 digest[SHA256_DIGEST_SIZE];
    UINT64 i;

    sha256_init(&context);
    sha256_update(&context, test1, sizeof(test1) - 1);
    sha256_final(&context, digest);
    if (!equal(digest, test1_digest, sizeof(digest)))
        return "SHA-256 of TEST1 differs from RFC 6234";

    sha256_init(&context);
    for (i = 0; i < sizeof(test2) - 1; i++)
        sha256_update(&context, test2 + i, 1);
    sha256_final(&context, digest);
    if (!equal(digest, test2_digest, sizeof(digest)))
        return "SHA-256 of TEST2_1 differs from RFC 6234";

    sha256_init(&context);
    for (i = 0; i < TEST4_REPEATS; i++)
        sha256_update(&context, test4, sizeof(test4) - 1);
    sha256_final(&context, digest);
    if (!equal(digest, test4_digest, sizeof(digest)))
        return "SHA-256 of TEST4 differs from RFC 6234";
    return 0;
}

static const char *test_generator(void)
{
    GENERATOR generator;
    UINT8 out[CHACHA20_BLOCK_SIZE];
    const UINT64 half = CHACHA20_BLOCK_SIZE - GENERATOR_KEY_SIZE;

    generator_init(&generator);
    generator_read(&generator, out, half);
    if (!equal(out, zero_key_block0 + GENERATOR_KEY_SIZE, half))
        return "the generator's first output is not block 0's second half";
    if (!equal(generator.key, zero_key_block0, GENERATOR_KEY_SIZE))
        return "the generator's next key is not block 0's first half";
    if (!all_zero(generator.buffer, half))
        return "the generator kept output it had returned";
    generator_read(&generator, out, CHACHA20_BLOCK_SIZE);
    if (!equal(out, zero_key_block1, CHACHA20_BLOCK_SIZE))
        return "the generator's second output is not block 1";

    generator_inject(&generator, test1_digest);
    if (!equal(generator.key, inject_expected, GENERATOR_KEY_SIZE))
        return "generator_inject made the wrong key";
    if (generator.available != 0 ||
        !all_zero(generator.buffer, sizeof(generator.buffer)))
        return "generator_inject kept output";
    return 0;
}

const char *random_self_test(void)
{
    const char *failure = test_chacha20();

    if (!failure)
        failure = test_sha256();
    if (!failure)
        failure = test_generator();
    return failure;
}
