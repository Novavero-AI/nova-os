/* The ChaCha20 block function, as RFC 8439 2.3 specifies it. The state is
   sixteen 32-bit words: four constants, the key, the block counter and
   the nonce, each read little-endian. Twenty rounds, alternately over the
   columns and the diagonals of the state as a 4 x 4 matrix, transform it;
   the input words are then added back and the result serialized
   little-endian. Both copies of the state hold the key, so they are wiped
   before the function returns. */

#include "chacha20.h"
#include "wipe.h"

#define STATE_WORDS 16
#define CONSTANT_WORDS 4
#define KEY_WORDS 8
#define NONCE_WORDS 3

/* Where each input starts in the state (RFC 8439 2.3). */
#define KEY_WORD 4
#define COUNTER_WORD 12
#define NONCE_WORD 13

/* Twenty rounds: ten passes of a column round and a diagonal round. */
#define DOUBLE_ROUNDS 10

#define WORD_SIZE 4

/* The first four words of every state (RFC 8439 2.3). */
static const UINT32 constants[CONSTANT_WORDS] = {0x61707865, 0x3320646e,
                                                 0x79622d32, 0x6b206574};

static UINT32 rotate_left(UINT32 x, int n)
{
    return x << n | x >> (32 - n);
}

static UINT32 load32_le(const UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

static void store32_le(UINT8 *p, UINT32 x)
{
    p[0] = (UINT8)x;
    p[1] = (UINT8)(x >> 8);
    p[2] = (UINT8)(x >> 16);
    p[3] = (UINT8)(x >> 24);
}

/* RFC 8439 2.1, on the state words at indexes a, b, c and d (2.2). */
static void quarter_round(UINT32 *x, int a, int b, int c, int d)
{
    x[a] += x[b];
    x[d] ^= x[a];
    x[d] = rotate_left(x[d], 16);
    x[c] += x[d];
    x[b] ^= x[c];
    x[b] = rotate_left(x[b], 12);
    x[a] += x[b];
    x[d] ^= x[a];
    x[d] = rotate_left(x[d], 8);
    x[c] += x[d];
    x[b] ^= x[c];
    x[b] = rotate_left(x[b], 7);
}

void chacha20_block(const UINT8 key[CHACHA20_KEY_SIZE], UINT32 counter,
                    const UINT8 nonce[CHACHA20_NONCE_SIZE],
                    UINT8 out[CHACHA20_BLOCK_SIZE])
{
    UINT32 input[STATE_WORDS], x[STATE_WORDS];
    int i;

    for (i = 0; i < CONSTANT_WORDS; i++)
        input[i] = constants[i];
    for (i = 0; i < KEY_WORDS; i++)
        input[KEY_WORD + i] = load32_le(key + WORD_SIZE * i);
    input[COUNTER_WORD] = counter;
    for (i = 0; i < NONCE_WORDS; i++)
        input[NONCE_WORD + i] = load32_le(nonce + WORD_SIZE * i);
    for (i = 0; i < STATE_WORDS; i++)
        x[i] = input[i];

    for (i = 0; i < DOUBLE_ROUNDS; i++) {
        quarter_round(x, 0, 4, 8, 12);
        quarter_round(x, 1, 5, 9, 13);
        quarter_round(x, 2, 6, 10, 14);
        quarter_round(x, 3, 7, 11, 15);
        quarter_round(x, 0, 5, 10, 15);
        quarter_round(x, 1, 6, 11, 12);
        quarter_round(x, 2, 7, 8, 13);
        quarter_round(x, 3, 4, 9, 14);
    }

    for (i = 0; i < STATE_WORDS; i++)
        store32_le(out + WORD_SIZE * i, x[i] + input[i]);
    wipe(input, sizeof(input));
    wipe(x, sizeof(x));
}
