/* The program the first process runs to check the data disk (fstest.c,
   init/data.c): its file, its view, its modes and its exit codes. */

#ifndef NOVA_FSTEST_H
#define NOVA_FSTEST_H

#define FSTEST_FILE "fstest.exe"

/* Its view: the data volume's /data, read-write and read-only, and the
   namespace's root without WRITE. */
#define FSTEST_DATA "/data"
#define FSTEST_READ_ONLY "/read-only"
#define FSTEST_NAMESPACE "/namespace"
#define FSTEST_VIEW_ENTRIES 3

/* Its one argument: FSTEST_ONCE checks the volume and commits one
   generation of its crash state; FSTEST_LOOP then commits generations
   until the machine stops, for the crash test. init passes FSTEST_LOOP
   when \nova\ holds FSTEST_LOOP_MARKER. */
#define FSTEST_ONCE "once"
#define FSTEST_LOOP "loop"
#define FSTEST_LOOP_MARKER "fsloop"

/* Nonzero, so that its launcher tells it from a default. */
#define FSTEST_EXIT_PASSED 7
#define FSTEST_EXIT_FAILED 1

#endif
