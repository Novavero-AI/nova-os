/* The type specimen the first process starts from /store, on a boot
   disk that asks for it, and hands the display in place of the console
   (specimen.c, init/specimen_start.c): the name of its store path, its
   file in that path, and the exit codes it ends with. Nothing waits
   for it; its log says whether it drew, and the codes tell a debugger
   the same. */

#ifndef NOVA_SPECIMEN_H
#define NOVA_SPECIMEN_H

#define SPECIMEN_NAME "specimen"
#define SPECIMEN_FILE "bin/specimen.exe"

#define SPECIMEN_EXIT_PASSED 7
#define SPECIMEN_EXIT_FAILED 1

#endif
