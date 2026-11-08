/* The test program the first process starts from /store on a test disk
   (hello.c, init/store.c): the name of its store path, its file in that
   path, and the exit codes it ends with. */

#ifndef NOVA_HELLO_H
#define NOVA_HELLO_H

#define HELLO_NAME "hello"
#define HELLO_FILE "bin/hello.exe"

/* Nonzero, so that its launcher tells it from a default. */
#define HELLO_EXIT_PASSED 7
#define HELLO_EXIT_FAILED 1

#endif
