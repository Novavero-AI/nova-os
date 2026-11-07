/* Account records (record.h). */

#include "record.h"

#define BITS_PER_BYTE 8

/* Argon2's least memory: 8 KiB per lane (RFC 9106 3.1). */
#define MEMORY_PER_LANE_MIN 8

static UINT32 load32(const UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

static void store32(UINT8 *p, UINT32 x)
{
    UINT32 i;

    for (i = 0; i < 4; i++)
        p[i] = (UINT8)(x >> (i * BITS_PER_BYTE));
}

static void copy(UINT8 *to, const UINT8 *from, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

static int is_lower(UINT8 c)
{
    return c >= 'a' && c <= 'z';
}

static int is_digit(UINT8 c)
{
    return c >= '0' && c <= '9';
}

int account_name_valid(const UINT8 *name, UINT32 length)
{
    UINT32 i;

    if (length == 0 || length > ACCOUNT_NAME_MAX || !is_lower(name[0]))
        return 0;
    for (i = 1; i < length; i++)
        if (!is_lower(name[i]) && !is_digit(name[i]) && name[i] != '-')
            return 0;
    return 1;
}

void record_encode(const ACCOUNT *account, UINT8 *bytes)
{
    UINT32 i;

    for (i = 0; i < RECORD_SIZE; i++)
        bytes[i] = 0;
    copy(bytes + RECORD_OFFSET_MAGIC, (const UINT8 *)RECORD_MAGIC,
         RECORD_MAGIC_SIZE);
    store32(bytes + RECORD_OFFSET_VERSION, RECORD_VERSION);
    store32(bytes + RECORD_OFFSET_ID, account->id);
    store32(bytes + RECORD_OFFSET_FLAGS, account->flags);
    store32(bytes + RECORD_OFFSET_NAME_LENGTH, account->name_length);
    copy(bytes + RECORD_OFFSET_NAME, account->name, account->name_length);
    store32(bytes + RECORD_OFFSET_LANES, account->lanes);
    store32(bytes + RECORD_OFFSET_MEMORY, account->memory_kib);
    store32(bytes + RECORD_OFFSET_PASSES, account->passes);
    copy(bytes + RECORD_OFFSET_SALT, account->salt, RECORD_SALT_SIZE);
    copy(bytes + RECORD_OFFSET_TAG, account->tag, RECORD_TAG_SIZE);
}

int record_decode(const UINT8 *bytes, UINT32 length, const UINT8 *name,
                  UINT32 name_length, ACCOUNT *account)
{
    UINT32 i;

    if (length != RECORD_SIZE)
        return 0;
    for (i = 0; i < RECORD_MAGIC_SIZE; i++)
        if (bytes[RECORD_OFFSET_MAGIC + i] != (UINT8)RECORD_MAGIC[i])
            return 0;
    account->id = load32(bytes + RECORD_OFFSET_ID);
    account->flags = load32(bytes + RECORD_OFFSET_FLAGS);
    account->name_length = load32(bytes + RECORD_OFFSET_NAME_LENGTH);
    account->lanes = load32(bytes + RECORD_OFFSET_LANES);
    account->memory_kib = load32(bytes + RECORD_OFFSET_MEMORY);
    account->passes = load32(bytes + RECORD_OFFSET_PASSES);
    if (load32(bytes + RECORD_OFFSET_VERSION) != RECORD_VERSION ||
        account->id < ACCOUNT_FIRST_ID ||
        (account->flags & ~(UINT32)ACCOUNT_ADMINISTRATOR) ||
        account->name_length != name_length ||
        !account_name_valid(bytes + RECORD_OFFSET_NAME, name_length) ||
        account->lanes == 0 || account->lanes > RECORD_LANES_MAX ||
        account->memory_kib < MEMORY_PER_LANE_MIN * account->lanes ||
        account->memory_kib > RECORD_MEMORY_MAX_KIB ||
        account->passes == 0 || account->passes > RECORD_PASSES_MAX)
        return 0;
    for (i = 0; i < name_length; i++)
        if (bytes[RECORD_OFFSET_NAME + i] != name[i])
            return 0;
    for (i = name_length; i < ACCOUNT_NAME_MAX; i++)
        if (bytes[RECORD_OFFSET_NAME + i] != 0)
            return 0;
    copy(account->name, bytes + RECORD_OFFSET_NAME, ACCOUNT_NAME_MAX);
    copy(account->salt, bytes + RECORD_OFFSET_SALT, RECORD_SALT_SIZE);
    copy(account->tag, bytes + RECORD_OFFSET_TAG, RECORD_TAG_SIZE);
    return 1;
}
