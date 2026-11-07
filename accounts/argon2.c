/* Argon2id (RFC 9106): H_0 of 3.2, the variable-length hash H' of 3.3,
   the indexing of 3.4, the compression function G of 3.5 and the
   permutation P of 3.6, with BLAKE2b as H. Every block, H_0 and the
   address blocks are derived from the password, so the memory, the
   working blocks and the stack below the hash are wiped before it
   returns, the memory-wiping option of section 4, step 9. */

#include "argon2.h"
#include "blake2b.h"
#include "wipe.h"

#define VERSION 0x13
#define TYPE_ID 2

/* SL, the slices of a lane (3.4), and the slices of the first pass whose
   indices Argon2id takes as Argon2i does (3.4.1.3). */
#define SLICES 4
#define INDEPENDENT_SLICES (SLICES / 2)

/* The blocks a lane starts with (Figures 3 and 4). */
#define FIRST_BLOCKS 2

/* p's largest value (3.1), and the bytes of H_0 (Figure 1). */
#define LANES_MAX 0xffffffu
#define H0_SIZE 64

/* H' of 3.3: the bytes of each V_i and of each W_i it keeps. */
#define V_SIZE 64
#define W_SIZE 32

/* The 8-byte values one address block holds (3.4.1.2). */
#define ADDRESSES_PER_BLOCK ARGON2_BLOCK_WORDS

/* Z's words in an address block's input (Figure 11), then the counter. */
#define Z_PASS 0
#define Z_LANE 1
#define Z_SLICE 2
#define Z_BLOCKS 3
#define Z_PASSES 4
#define Z_TYPE 5
#define Z_COUNTER 6

/* P's input: eight 16-byte registers, sixteen words (3.6). G applies it
   to each row of eight registers, then to each column (Figure 15). */
#define P_WORDS 16
#define REGISTERS_PER_ROW 8
#define ROW_PAIR_STEP 2
#define COLUMN_PAIR_STEP (REGISTERS_PER_ROW * 2)

/* GB's rotations (Figure 19). */
#define GB_R1 32
#define GB_R2 24
#define GB_R3 16
#define GB_R4 63

#define LOW_32 0xffffffffULL
#define WORD_SIZE 8
#define BITS_PER_BYTE 8

/* The stack wipe_stack clears: well past the frames the hash's callees
   take, and under the 4096-byte frame at which TinyCC calls __chkstk,
   which no runtime here provides. */
#define WIPE_STACK_SIZE 2048

/* RFC 9106 5.3. */
#define TEST_LANES 4
#define TEST_MEMORY_KIB 32
#define TEST_PASSES 3
#define TEST_TAG_SIZE 32
#define TEST_PASSWORD_SIZE 32
#define TEST_SALT_SIZE 16
#define TEST_SECRET_SIZE 8
#define TEST_DATA_SIZE 12
#define TEST_PASSWORD_BYTE 0x01
#define TEST_SALT_BYTE 0x02
#define TEST_SECRET_BYTE 0x03
#define TEST_DATA_BYTE 0x04

static const UINT8 test_tag[TEST_TAG_SIZE] = {
    0x0d, 0x64, 0x0d, 0xf5, 0x8d, 0x78, 0x76, 0x6c, 0x08, 0xc0, 0x37,
    0xa3, 0x4a, 0x8b, 0x53, 0xc9, 0xd0, 0x1e, 0xf0, 0x45, 0x2d, 0x75,
    0xb6, 0x5e, 0xb5, 0x25, 0x20, 0xe9, 0x6b, 0x01, 0xe6, 0x59};

/* One hash's shape: m', p, q and the segment length q / SL. */
typedef struct {
    ARGON2_BLOCK *memory;
    UINT64 blocks;
    UINT32 lanes;
    UINT32 lane_length;
    UINT32 segment_length;
    UINT32 passes;
} SHAPE;

/* G's working blocks, and an address block with its input: kept out of
   the stack, whose frames TinyCC would need __chkstk for, and wiped
   after each hash. */
static ARGON2_BLOCK r_block, q_block, address, address_input, zero_block;
static UINT8 block_bytes[ARGON2_BLOCK_SIZE];

static UINT64 rotate_right(UINT64 x, UINT32 n)
{
    return x >> n | x << (64 - n);
}

static void store32(UINT8 *p, UINT32 x)
{
    UINT32 i;

    for (i = 0; i < 4; i++)
        p[i] = (UINT8)(x >> (i * BITS_PER_BYTE));
}

static UINT64 load64(const UINT8 *p)
{
    UINT64 x = 0;
    UINT32 i;

    for (i = WORD_SIZE; i-- > 0;)
        x = x << BITS_PER_BYTE | p[i];
    return x;
}

static void store64(UINT8 *p, UINT64 x)
{
    UINT32 i;

    for (i = 0; i < WORD_SIZE; i++)
        p[i] = (UINT8)(x >> (i * BITS_PER_BYTE));
}

/* Clear the stack below the caller's frame, where the hash's callees
   kept their locals and TinyCC spilled its temporaries, which wipe
   cannot name. TinyCC never inlines a function, so this one's array
   lies where theirs did. */
static void wipe_stack(void)
{
    UINT8 below[WIPE_STACK_SIZE];

    wipe(below, sizeof below);
}

static void update32(BLAKE2B_CONTEXT *context, UINT32 x)
{
    UINT8 bytes[4];

    store32(bytes, x);
    blake2b_update(context, bytes, sizeof bytes);
}

/* ------------------------------------------------------------------------
   H' (3.3). */

/* H'^T(A), T = out_length, written to out: for T over 64, W_1 to W_r,
   the first halves of V_1 to V_r, then V_{r+1} (Figure 8). */
static void hash_long(UINT8 *out, UINT32 out_length, const UINT8 *in,
                      UINT32 in_length)
{
    BLAKE2B_CONTEXT context;
    UINT8 v[V_SIZE];
    UINT32 r, k, i;

    if (out_length <= BLAKE2B_OUT_MAX) {
        blake2b_init(&context, out_length, 0, 0);
        update32(&context, out_length);
        blake2b_update(&context, in, in_length);
        blake2b_final(&context, out);
        return;
    }
    r = (out_length + W_SIZE - 1) / W_SIZE - 2;
    blake2b_init(&context, V_SIZE, 0, 0);
    update32(&context, out_length);
    blake2b_update(&context, in, in_length);
    blake2b_final(&context, v);
    for (k = 1;; k++) {
        for (i = 0; i < W_SIZE; i++)
            out[(k - 1) * W_SIZE + i] = v[i];
        if (k == r)
            break;
        blake2b_init(&context, V_SIZE, 0, 0);
        blake2b_update(&context, v, V_SIZE);
        blake2b_final(&context, v);
    }
    blake2b_init(&context, out_length - r * W_SIZE, 0, 0);
    blake2b_update(&context, v, V_SIZE);
    blake2b_final(&context, out + r * W_SIZE);
    wipe(v, sizeof v);
}

/* ------------------------------------------------------------------------
   G (3.5) and P (3.6). */

static UINT64 blamka(UINT64 x, UINT64 y)
{
    return x + y + 2 * ((x & LOW_32) * (y & LOW_32));
}

static void gb(UINT64 *v, UINT32 a, UINT32 b, UINT32 c, UINT32 d)
{
    v[a] = blamka(v[a], v[b]);
    v[d] = rotate_right(v[d] ^ v[a], GB_R1);
    v[c] = blamka(v[c], v[d]);
    v[b] = rotate_right(v[b] ^ v[c], GB_R2);
    v[a] = blamka(v[a], v[b]);
    v[d] = rotate_right(v[d] ^ v[a], GB_R3);
    v[c] = blamka(v[c], v[d]);
    v[b] = rotate_right(v[b] ^ v[c], GB_R4);
}

/* P over eight registers of block, each two words, the low first
   (Figure 17's S_i = v_{2i+1} || v_{2i}): register k at
   base + k * pair_step. A row's registers are adjacent; a column's lie a
   row of registers apart. */
static void permute(UINT64 *block, UINT32 base, UINT32 pair_step)
{
    UINT64 v[P_WORDS];
    UINT32 k;

    for (k = 0; k < P_WORDS; k++)
        v[k] = block[base + k / 2 * pair_step + k % 2];
    gb(v, 0, 4, 8, 12);
    gb(v, 1, 5, 9, 13);
    gb(v, 2, 6, 10, 14);
    gb(v, 3, 7, 11, 15);
    gb(v, 0, 5, 10, 15);
    gb(v, 1, 6, 11, 12);
    gb(v, 2, 7, 8, 13);
    gb(v, 3, 4, 9, 14);
    for (k = 0; k < P_WORDS; k++)
        block[base + k / 2 * pair_step + k % 2] = v[k];
}

/* next = G(x, y), or next ^= G(x, y) for a pass after the first
   (Figure 6). next may be x or y. */
static void compress(const ARGON2_BLOCK *x, const ARGON2_BLOCK *y,
                     ARGON2_BLOCK *next, int with_xor)
{
    UINT32 i;

    for (i = 0; i < ARGON2_BLOCK_WORDS; i++) {
        r_block.v[i] = x->v[i] ^ y->v[i];
        q_block.v[i] = r_block.v[i];
    }
    for (i = 0; i < REGISTERS_PER_ROW; i++)
        permute(q_block.v, i * P_WORDS, ROW_PAIR_STEP);
    for (i = 0; i < REGISTERS_PER_ROW; i++)
        permute(q_block.v, i * 2, COLUMN_PAIR_STEP);
    for (i = 0; i < ARGON2_BLOCK_WORDS; i++) {
        if (with_xor)
            next->v[i] ^= q_block.v[i] ^ r_block.v[i];
        else
            next->v[i] = q_block.v[i] ^ r_block.v[i];
    }
}

/* ------------------------------------------------------------------------
   Indexing (3.4). */

/* The next address block: the counter after Z, then
   G(ZERO, G(ZERO, Z || LE64(counter) || ZERO(968))). */
static void next_addresses(void)
{
    address_input.v[Z_COUNTER]++;
    compress(&zero_block, &address_input, &address, 0);
    compress(&zero_block, &address, &address, 0);
}

/* The column of lane ref_lane that block index of the segment at pass
   and slice references, from J_1, for a lane that is (same) or is not
   the current one: the zz-th index of W (3.4.2, Figure 13). W runs from
   the segment after the current slice's, in passes after the first, or
   from the lane's start in the first, through the blocks rules 1 and 2
   allow. */
static UINT32 reference_column(const SHAPE *shape, UINT32 pass, UINT32 slice,
                               UINT32 index, UINT32 j1, int same)
{
    UINT32 area, start = 0, before_segment = index == 0;
    UINT64 x, y;

    if (pass == 0) {
        if (slice == 0)
            area = index - 1;
        else if (same)
            area = slice * shape->segment_length + index - 1;
        else
            area = slice * shape->segment_length - before_segment;
    } else {
        if (slice != SLICES - 1)
            start = (slice + 1) * shape->segment_length;
        area = shape->lane_length - shape->segment_length;
        area = same ? area + index - 1 : area - before_segment;
    }
    x = (UINT64)j1 * j1 >> 32;
    y = (UINT64)area * x >> 32;
    return (UINT32)((start + (UINT64)area - 1 - y) % shape->lane_length);
}

/* Fill one lane's segment of one slice (Figures 5 and 6). */
static void fill_segment(const SHAPE *shape, UINT32 pass, UINT32 lane,
                         UINT32 slice)
{
    ARGON2_BLOCK *memory = shape->memory;
    int independent = pass == 0 && slice < INDEPENDENT_SLICES;
    UINT32 first = pass == 0 && slice == 0 ? FIRST_BLOCKS : 0;
    UINT32 index, column, previous, ref_lane, i;
    UINT64 lane_base = (UINT64)lane * shape->lane_length, pseudo;

    if (independent) {
        for (i = 0; i < ARGON2_BLOCK_WORDS; i++)
            address_input.v[i] = 0;
        address_input.v[Z_PASS] = pass;
        address_input.v[Z_LANE] = lane;
        address_input.v[Z_SLICE] = slice;
        address_input.v[Z_BLOCKS] = shape->blocks;
        address_input.v[Z_PASSES] = shape->passes;
        address_input.v[Z_TYPE] = TYPE_ID;
    }
    for (index = first; index < shape->segment_length; index++) {
        column = slice * shape->segment_length + index;
        previous = column ? column - 1 : shape->lane_length - 1;
        if (independent) {
            if (index == first || index % ADDRESSES_PER_BLOCK == 0)
                next_addresses();
            pseudo = address.v[index % ADDRESSES_PER_BLOCK];
        } else {
            pseudo = memory[lane_base + previous].v[0];
        }
        ref_lane = pass == 0 && slice == 0
                       ? lane
                       : (UINT32)((pseudo >> 32) % shape->lanes);
        compress(&memory[lane_base + previous],
                 &memory[(UINT64)ref_lane * shape->lane_length +
                         reference_column(shape, pass, slice, index,
                                          (UINT32)(pseudo & LOW_32),
                                          ref_lane == lane)],
                 &memory[lane_base + column], pass > 0);
    }
}

/* ------------------------------------------------------------------------
   The hash. */

UINT64 argon2_blocks(const ARGON2_INPUT *input)
{
    UINT64 group = (UINT64)SLICES * input->lanes;

    return group ? input->memory_kib / group * group : 0;
}

/* H_0 (Figure 1). */
static void initial_hash(const ARGON2_INPUT *input, UINT8 *h0)
{
    BLAKE2B_CONTEXT context;

    blake2b_init(&context, H0_SIZE, 0, 0);
    update32(&context, input->lanes);
    update32(&context, input->tag_length);
    update32(&context, input->memory_kib);
    update32(&context, input->passes);
    update32(&context, VERSION);
    update32(&context, TYPE_ID);
    update32(&context, input->password_length);
    blake2b_update(&context, input->password, input->password_length);
    update32(&context, input->salt_length);
    blake2b_update(&context, input->salt, input->salt_length);
    update32(&context, input->secret_length);
    blake2b_update(&context, input->secret, input->secret_length);
    update32(&context, input->data_length);
    blake2b_update(&context, input->data, input->data_length);
    blake2b_final(&context, h0);
}

static void load_block(ARGON2_BLOCK *block, const UINT8 *bytes)
{
    UINT32 i;

    for (i = 0; i < ARGON2_BLOCK_WORDS; i++)
        block->v[i] = load64(bytes + i * WORD_SIZE);
}

/* B[i][0] and B[i][1] of every lane (Figures 3 and 4). */
static void first_blocks(const SHAPE *shape, const UINT8 *h0)
{
    UINT8 seed[H0_SIZE + 8];
    UINT32 lane, j, i;

    for (i = 0; i < H0_SIZE; i++)
        seed[i] = h0[i];
    for (lane = 0; lane < shape->lanes; lane++) {
        for (j = 0; j < FIRST_BLOCKS; j++) {
            store32(seed + H0_SIZE, j);
            store32(seed + H0_SIZE + 4, lane);
            hash_long(block_bytes, ARGON2_BLOCK_SIZE, seed, sizeof seed);
            load_block(&shape->memory[(UINT64)lane * shape->lane_length + j],
                       block_bytes);
        }
    }
    wipe(seed, sizeof seed);
}

int argon2id(const ARGON2_INPUT *input, ARGON2_BLOCK *memory,
             UINT64 memory_blocks, UINT8 *tag)
{
    UINT8 h0[H0_SIZE];
    SHAPE shape;
    UINT32 pass, slice, lane, i;
    UINT64 last;

    if (input->lanes == 0 || input->lanes > LANES_MAX ||
        input->tag_length < ARGON2_TAG_MIN || input->passes == 0 ||
        (UINT64)input->memory_kib < 2ULL * SLICES * input->lanes)
        return 0;
    shape.memory = memory;
    shape.blocks = argon2_blocks(input);
    shape.lanes = input->lanes;
    shape.lane_length = (UINT32)(shape.blocks / input->lanes);
    shape.segment_length = shape.lane_length / SLICES;
    shape.passes = input->passes;
    if (shape.blocks > memory_blocks)
        return 0;
    initial_hash(input, h0);
    first_blocks(&shape, h0);
    for (pass = 0; pass < input->passes; pass++)
        for (slice = 0; slice < SLICES; slice++)
            for (lane = 0; lane < input->lanes; lane++)
                fill_segment(&shape, pass, lane, slice);
    /* C, the XOR of the last column (Figure 7), into the first lane's
       last block. */
    last = shape.lane_length - 1;
    for (lane = 1; lane < input->lanes; lane++)
        for (i = 0; i < ARGON2_BLOCK_WORDS; i++)
            memory[last].v[i] ^=
                memory[(UINT64)lane * shape.lane_length + last].v[i];
    for (i = 0; i < ARGON2_BLOCK_WORDS; i++)
        store64(block_bytes + i * WORD_SIZE, memory[last].v[i]);
    hash_long(tag, input->tag_length, block_bytes, ARGON2_BLOCK_SIZE);
    wipe(memory, shape.blocks * sizeof *memory);
    wipe(h0, sizeof h0);
    wipe(block_bytes, sizeof block_bytes);
    wipe(&r_block, sizeof r_block);
    wipe(&q_block, sizeof q_block);
    wipe(&address, sizeof address);
    wipe(&address_input, sizeof address_input);
    wipe_stack();
    return 1;
}

static void fill(UINT8 *bytes, UINT32 count, UINT8 value)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        bytes[i] = value;
}

int argon2_self_test(ARGON2_BLOCK *memory, UINT64 memory_blocks)
{
    UINT8 password[TEST_PASSWORD_SIZE], salt[TEST_SALT_SIZE];
    UINT8 secret[TEST_SECRET_SIZE], data[TEST_DATA_SIZE];
    UINT8 tag[TEST_TAG_SIZE];
    ARGON2_INPUT input;
    UINT32 i, differ = 0;

    fill(password, sizeof password, TEST_PASSWORD_BYTE);
    fill(salt, sizeof salt, TEST_SALT_BYTE);
    fill(secret, sizeof secret, TEST_SECRET_BYTE);
    fill(data, sizeof data, TEST_DATA_BYTE);
    input.password = password;
    input.password_length = sizeof password;
    input.salt = salt;
    input.salt_length = sizeof salt;
    input.secret = secret;
    input.secret_length = sizeof secret;
    input.data = data;
    input.data_length = sizeof data;
    input.lanes = TEST_LANES;
    input.memory_kib = TEST_MEMORY_KIB;
    input.passes = TEST_PASSES;
    input.tag_length = TEST_TAG_SIZE;
    if (!argon2id(&input, memory, memory_blocks, tag))
        return 0;
    for (i = 0; i < TEST_TAG_SIZE; i++)
        differ |= tag[i] ^ test_tag[i];
    return differ == 0;
}
