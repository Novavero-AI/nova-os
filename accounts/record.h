/* An account's record, the file /data/accounts/<name> (docs/spec/
   accounts.md A3): RECORD_SIZE bytes, every integer little-endian, each
   field at the byte offset below and read byte by byte. */

#ifndef NOVA_RECORD_H
#define NOVA_RECORD_H

#include "accounts_protocol.h"

/* "NOVAACCT", the first 8 bytes, and the layout's version. */
#define RECORD_MAGIC "NOVAACCT"
#define RECORD_MAGIC_SIZE 8
#define RECORD_VERSION 1

#define RECORD_SALT_SIZE 16
#define RECORD_TAG_SIZE 32

#define RECORD_OFFSET_MAGIC 0
#define RECORD_OFFSET_VERSION 8
#define RECORD_OFFSET_ID 12
#define RECORD_OFFSET_FLAGS 16
#define RECORD_OFFSET_NAME_LENGTH 20
#define RECORD_OFFSET_NAME 24
#define RECORD_OFFSET_LANES (RECORD_OFFSET_NAME + ACCOUNT_NAME_MAX)
#define RECORD_OFFSET_MEMORY (RECORD_OFFSET_LANES + 4)
#define RECORD_OFFSET_PASSES (RECORD_OFFSET_MEMORY + 4)
#define RECORD_OFFSET_SALT (RECORD_OFFSET_PASSES + 4)
#define RECORD_OFFSET_TAG (RECORD_OFFSET_SALT + RECORD_SALT_SIZE)
#define RECORD_SIZE (RECORD_OFFSET_TAG + RECORD_TAG_SIZE)

/* The Argon2id parameters a record may carry, so that a record changed
   on the disk cannot make the service map more memory than it holds or
   take passes without bound: at most RECORD_MEMORY_MAX_KIB, the memory
   the service maps; at most RECORD_LANES_MAX lanes; at most
   RECORD_PASSES_MAX passes, a little over three times the passes the
   service writes (A4). */
#define RECORD_MEMORY_MAX_KIB 65536
#define RECORD_LANES_MAX 16
#define RECORD_PASSES_MAX 10

/* The first id, so that a zeroed record is never valid. */
#define ACCOUNT_FIRST_ID 1

typedef struct {
    UINT32 id;
    UINT32 flags;
    UINT32 name_length;
    UINT8 name[ACCOUNT_NAME_MAX];
    UINT32 lanes;
    UINT32 memory_kib;
    UINT32 passes;
    UINT8 salt[RECORD_SALT_SIZE];
    UINT8 tag[RECORD_TAG_SIZE];
} ACCOUNT;

/* Whether the length bytes at name are an account's name (A2). */
int account_name_valid(const UINT8 *name, UINT32 length);

/* account as its record's RECORD_SIZE bytes. */
void record_encode(const ACCOUNT *account, UINT8 *bytes);

/* The account the length bytes at bytes hold, read as the record
   /data/accounts/<name> must be: exactly RECORD_SIZE bytes, the magic
   and version, an id of at least ACCOUNT_FIRST_ID, only defined flags,
   a valid name equal to the file's, zeros after it, and parameters
   within the bounds above. Returns 0 if any of that fails. */
int record_decode(const UINT8 *bytes, UINT32 length, const UINT8 *name,
                  UINT32 name_length, ACCOUNT *account);

#endif
