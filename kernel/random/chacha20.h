/* The ChaCha20 block function of RFC 8439, 2.3: a 256-bit key, a 32-bit
   block counter and a 96-bit nonce give 64 bytes of output. */

#ifndef NOVA_CHACHA20_H
#define NOVA_CHACHA20_H

#include "types.h"

#define CHACHA20_KEY_SIZE 32
#define CHACHA20_NONCE_SIZE 12
#define CHACHA20_BLOCK_SIZE 64

void chacha20_block(const UINT8 key[CHACHA20_KEY_SIZE], UINT32 counter,
                    const UINT8 nonce[CHACHA20_NONCE_SIZE],
                    UINT8 out[CHACHA20_BLOCK_SIZE]);

#endif
