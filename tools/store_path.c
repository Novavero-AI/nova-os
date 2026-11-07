/* Store path names (store_path.h). nix-base32 writes the bits of its
   input from the most significant 5-bit group down: character i of n
   holds bits 5(n-1-i) to 5(n-1-i)+4, counting from bit 0 of byte 0
   (upstream src/libutil/hash.cc; nova-cache's NovaCache.Base32). */

#include <string.h>

#include "store_path.h"

#define BITS_PER_BYTE 8
#define BITS_PER_CHAR 5
#define CHAR_MASK 0x1f
#define HEX_DIGITS_PER_BYTE 2

static const char alphabet[] = "0123456789abcdfghijklmnpqrsvwxyz";

size_t nix32_length(size_t count)
{
    return (count * BITS_PER_BYTE + BITS_PER_CHAR - 1) / BITS_PER_CHAR;
}

void nix32_encode(const uint8_t *bytes, size_t count, char *out)
{
    size_t length = nix32_length(count), n;

    for (n = length; n-- > 0;) {
        size_t bit = n * BITS_PER_CHAR, byte = bit / BITS_PER_BYTE;
        unsigned shift = (unsigned)(bit % BITS_PER_BYTE);
        unsigned value = bytes[byte] >> shift;

        if (byte + 1 < count)
            value |= (unsigned)bytes[byte + 1] << (BITS_PER_BYTE - shift);
        *out++ = alphabet[value & CHAR_MASK];
    }
    *out = 0;
}

int nix32_decode(const char *text, size_t length, uint8_t *out, size_t count)
{
    size_t i;

    if (length != nix32_length(count))
        return 0;
    memset(out, 0, count);
    for (i = 0; i < length; i++) {
        const char *found = text[i] ? strchr(alphabet, text[i]) : 0;
        size_t bit = (length - 1 - i) * BITS_PER_CHAR;
        size_t byte = bit / BITS_PER_BYTE;
        unsigned shift = (unsigned)(bit % BITS_PER_BYTE);
        unsigned value;

        if (!found)
            return 0;
        value = (unsigned)(found - alphabet);
        out[byte] |= (uint8_t)(value << shift);
        if (byte + 1 < count)
            out[byte + 1] |= (uint8_t)(value >> (BITS_PER_BYTE - shift));
        else if (value >> (BITS_PER_BYTE - shift))
            return 0;
    }
    return 1;
}

/* type ":sha256:" hex ":" store-directory ":" name, hashed and folded
   (upstream StoreDirConfig::makeStorePath and compressHash). */
void store_path_hash(const char *type, const uint8_t inner[SHA256_DIGEST_SIZE],
                     const char *name, char hash[STORE_HASH_CHARS + 1])
{
    static const char hex[] = "0123456789abcdef";
    char digits[SHA256_DIGEST_SIZE * HEX_DIGITS_PER_BYTE];
    uint8_t digest[SHA256_DIGEST_SIZE], folded[STORE_HASH_BYTES];
    SHA256_CONTEXT context;
    size_t i;

    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        digits[i * HEX_DIGITS_PER_BYTE] = hex[inner[i] >> 4];
        digits[i * HEX_DIGITS_PER_BYTE + 1] = hex[inner[i] & 0xf];
    }
    sha256_init(&context);
    sha256_update(&context, type, strlen(type));
    sha256_update(&context, ":sha256:", strlen(":sha256:"));
    sha256_update(&context, digits, sizeof digits);
    sha256_update(&context, ":" NIX_STORE_DIR ":",
                  strlen(":" NIX_STORE_DIR ":"));
    sha256_update(&context, name, strlen(name));
    sha256_final(&context, digest);
    memset(folded, 0, sizeof folded);
    for (i = 0; i < SHA256_DIGEST_SIZE; i++)
        folded[i % STORE_HASH_BYTES] ^= digest[i];
    nix32_encode(folded, sizeof folded, hash);
}

static int name_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '+' || c == '-' || c == '.' ||
           c == '_' || c == '?' || c == '=';
}

/* Upstream refuses ".", "..", and a name whose first dash-separated
   component is either. */
int store_name_valid(const char *name, size_t length)
{
    size_t i;

    if (length == 0 || length > STORE_NAME_MAX)
        return 0;
    if (name[0] == '.' &&
        (length == 1 || name[1] == '-' ||
         (name[1] == '.' && (length == 2 || name[2] == '-'))))
        return 0;
    for (i = 0; i < length; i++)
        if (!name_char(name[i]))
            return 0;
    return 1;
}

int store_base_name_valid(const char *base, size_t length)
{
    size_t i;

    if (length < STORE_HASH_CHARS + 1 || base[STORE_HASH_CHARS] != '-')
        return 0;
    for (i = 0; i < STORE_HASH_CHARS; i++)
        if (!base[i] || !strchr(alphabet, base[i]))
            return 0;
    return store_name_valid(base + STORE_HASH_CHARS + 1,
                            length - STORE_HASH_CHARS - 1);
}
