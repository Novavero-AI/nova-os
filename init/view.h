/* A 32-bit process's view of storage and init's server for it
   (docs/spec/compat.md C4): files the process reads, each the bytes of a
   boot file or of an earlier output, and the one file it writes, which
   grows at the end of init's output region. The server speaks the
   stand-in file protocol of file_protocol.h. */

#ifndef NOVA_VIEW_H
#define NOVA_VIEW_H

#include "init.h"

/* A file of a view. A file the process writes starts at output_next()
   and exists once the process's first open creates it. */
typedef struct {
    const char *name; /* the name the process opens it by */
    const UINT8 *data;
    UINT8 *output; /* data, for the file the process writes; else 0 */
    UINT64 size;
    int exists;
} VIEW_FILE;

/* Where the next output starts: after every output sealed so far. */
UINT8 *output_next(void);

/* Keep the size bytes at output_next() as an output, so that the next
   starts after them. */
void output_seal(UINT64 size);

/* Serve the count files at files on directory, the server end of
   process's directory channel, until process ends; then close
   directory and every file's channel. */
void view_serve(UINT32 directory, UINT32 process, VIEW_FILE *files,
                UINT32 count);

#endif
