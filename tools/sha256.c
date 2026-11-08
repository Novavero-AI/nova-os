/* sha256: prints a file's SHA-256 digest in lowercase hexadecimal, the
   first field of the line sha256sum writes for it. build.sh checks each
   download against its line in SHA256SUMS with it, so that the check
   needs nothing from the host but its C compiler.

   usage: sha256 FILE */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sha256.h"

#define CHUNK_SIZE 65536

int main(int argc, char **argv)
{
    static UINT8 chunk[CHUNK_SIZE];
    UINT8 digest[SHA256_DIGEST_SIZE];
    SHA256_CONTEXT context;
    size_t n, i;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: sha256 FILE\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "sha256: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    sha256_init(&context);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        sha256_update(&context, chunk, n);
    if (ferror(f)) {
        fprintf(stderr, "sha256: %s: %s\n", argv[1], strerror(errno));
        fclose(f);
        return 1;
    }
    fclose(f);
    sha256_final(&context, digest);
    for (i = 0; i < SHA256_DIGEST_SIZE; i++)
        printf("%02x", (unsigned)digest[i]);
    printf("\n");
    if (fflush(stdout) != 0) {
        fprintf(stderr, "sha256: writing the digest: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
