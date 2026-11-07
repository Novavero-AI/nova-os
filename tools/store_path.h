/* Store paths as Nix names them (docs/spec/storage.md, "What the store
   holds"): the nix-base32 encoding, the hash part makeStorePath computes
   and the rules a base name follows, as upstream Nix 2.24.10 has them
   (src/libstore/store-api.cc and path.cc) and nova-nix's Nix.Hash and
   Nix.Store.Path match. mknar and mkstore share this file. */

#ifndef NOVA_STORE_PATH_H
#define NOVA_STORE_PATH_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"
#include "store_image.h"

/* Every path is named as if under /nix/store, on every host (fact 2). */
#define NIX_STORE_DIR "/nix/store"
#define NIX_STORE_PREFIX NIX_STORE_DIR "/"

/* The nix-base32 characters of a SHA-256 digest. */
#define NIX32_DIGEST_CHARS 52

/* The bytes of a store path's hash: the digest XOR-folded to 160 bits
   (upstream compressHash), which nix-base32 writes as 32 characters. */
#define STORE_HASH_BYTES 20

/* The characters of count bytes in nix-base32. */
size_t nix32_length(size_t count);

/* Write count bytes in nix-base32 to out, nix32_length(count)
   characters and a NUL. */
void nix32_encode(const uint8_t *bytes, size_t count, char *out);

/* Decode length characters of nix-base32 into the count bytes at out.
   Returns 0 if a character is outside the alphabet, the length does not
   encode count bytes, or the bits past them are not zero. */
int nix32_decode(const char *text, size_t length, uint8_t *out, size_t count);

/* The hash part of the store path makeStorePath names from type, the
   SHA-256 inner digest and name, as STORE_HASH_CHARS characters and a
   NUL at hash. */
void store_path_hash(const char *type, const uint8_t inner[SHA256_DIGEST_SIZE],
                     const char *name, char hash[STORE_HASH_CHARS + 1]);

/* Whether length bytes at name are a store path name upstream's
   checkName accepts. */
int store_name_valid(const char *name, size_t length);

/* Whether length bytes at base are a store path's base name: a hash
   part of nix-base32 characters, '-', and a valid name. */
int store_base_name_valid(const char *base, size_t length);

#endif
