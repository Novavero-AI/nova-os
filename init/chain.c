/* stage0-pe32's chain on nova-os (docs/spec/compat.md C4). init takes
   the steps nova-nix's stage0-pe32 package takes to build the chain's
   links from the hex0 seed to M1, in the same order and with the same
   arguments, each in a 32-bit process of its own. A step's view holds
   the files its command line names, each the bytes of a boot file or
   of an earlier step's output, and the one file it writes, which init
   keeps for the steps after it; the process holds nothing else
   (DESIGN.md, principle 7). Every step's program is an i386 PE32 with no
   imports, whose one section is readable, writable and executable, the
   case PROCESS_OPTION_WRITE_EXECUTE exists for (docs/spec/objects.md
   O5).

   init logs the size and sha256 of each output and checks each link
   against the sha256 nova-nix's Stage0 (Windows) CI job pins for it
   (in nova-nix's own .github/workflows/ci.yml), so the chain on nova-os
   is held to the chain on Windows byte for byte. M2 is the first link
   built on the C library of libc-core.M1, and nothing pins the file it
   writes, so the chain runs on to M1, whose link is pinned.

   The chain, the bootstrap check, runs when the boot volume holds all
   of its inputs, as the test disk does. A volume that holds none, the
   system's disk, has no check to run, and init says so and goes on. A
   volume that holds some but not all is refused: it is a test disk with
   inputs missing, and passing it would report the checks passed with
   the chain unrun. */

#include "init.h"
#include "compat.h"
#include "nt32.h"
#include "sha256.h"
#include "view.h"

#define NTDLL_FILE "ntdll.dll"

/* The boot volume's names for the chain's inputs, 8.3 as
   docs/spec/disk-layout.md requires, and the names a step's view gives
   them, those of the trees they come from: stage0-pe32's x86/ and seed,
   M2libc's, M2-Planet's and mescc-tools'. */
typedef struct {
    const char *boot;
    const char *name;
} SOURCE;

static const SOURCE sources[] = {
    {"hex0seed.exe", "hex0-seed.exe"},
    {"hex0_x86.hex", "hex0_x86.hex0"},
    {"hex1_x86.hex", "hex1_x86.hex0"},
    {"hex2_x86.hex", "hex2_x86.hex1"},
    {"catm_x86.hex", "catm_x86.hex2"},
    {"pe32i386.hex", "PE32-i386.hex2"},
    {"ntdll386.hex", "ntdll-i386.hex2"},
    {"m0_x86.hex", "M0_x86.hex2"},
    {"cc_x86.m1", "cc_x86.M1"},
    {"libccore.m1", "libc-core.M1"},
    {"libcboot.m1", "libc-bootstrap.M1"},
    {"pe-end.m1", "pe-end-shim.M1"},
    {"x86_defs.m1", "x86_defs.M1"},
    {"bootstrp.c", "bootstrap.c"},
    {"bootable.c", "bootstrappable.c"},
    {"cc.h", "cc.h"},
    {"cc_globl.c", "cc_globals.c"},
    {"cc_readr.c", "cc_reader.c"},
    {"cc_strng.c", "cc_strings.c"},
    {"cc_types.c", "cc_types.c"},
    {"cc_emit.c", "cc_emit.c"},
    {"cc_core.c", "cc_core.c"},
    {"cc_macro.c", "cc_macro.c"},
    {"cc.c", "cc.c"},
    {"stringfy.c", "stringify.c"},
    {"m1-macro.c", "M1-macro.c"},
};

#define SOURCE_COUNT (sizeof sources / sizeof sources[0])

/* A step: the image it runs, a source or an earlier step's output; its
   command line after the program's name, whose words are separated by
   single spaces; the file it writes; and, for a link, the name nova-nix
   gives it and the sha256 nova-nix pins, in hexadecimal. */
typedef struct {
    const char *program;
    const char *arguments;
    const char *output;
    const char *link;
    const char *pin;
} STEP;

/* nova-nix's pkgs/windows/by-name/st/stage0-pe32/package.nix, step by
   step: hex0 to cc_x86, then M2-Planet compiled by cc_x86, then M1-macro
   compiled by M2. hex0's pin is the seed's own hash. */
static const STEP steps[] = {
    {"hex0-seed.exe", "hex0_x86.hex0 hex0.exe", "hex0.exe", "hex0",
     "b9db7be0fed7770be9fca46b7aa551b611fc60eaf1112a053531aae14aa46742"},
    {"hex0.exe", "hex1_x86.hex0 hex1.exe", "hex1.exe", "hex1",
     "889d18f1de1ffba70b2a8fa3915ed67b8ccff874fb20e63841cc5c301f890929"},
    {"hex1.exe", "hex2_x86.hex1 hex2.exe", "hex2.exe", "hex2",
     "5b74721141d8788e8334d68016d9360928e5e17e8364485a7b69279b0c2f657d"},
    {"hex2.exe", "catm_x86.hex2 catm.exe", "catm.exe", "catm",
     "b45fa38c731889344b37f91b124fddc643eb975b0d3b68bf6efbba07ed2e8c24"},
    {"catm.exe", "M0.hex2 PE32-i386.hex2 ntdll-i386.hex2 M0_x86.hex2",
     "M0.hex2", 0, 0},
    {"hex2.exe", "M0.hex2 M0.exe", "M0.exe", "M0",
     "62519c24170f747b9cf2b4514c5967a2cc068f1d485743fedba736c73eb0dcf1"},
    {"M0.exe", "cc_x86.M1 cc_x86-0.hex2", "cc_x86-0.hex2", 0, 0},
    {"catm.exe", "cc_x86.hex2 PE32-i386.hex2 ntdll-i386.hex2 cc_x86-0.hex2",
     "cc_x86.hex2", 0, 0},
    {"hex2.exe", "cc_x86.hex2 cc_x86.exe", "cc_x86.exe", "cc_x86",
     "a47b7cef346a2279c3aae684ad21d9316f04187730161997ab1a2a3a27b990e0"},
    {"catm.exe",
     "M2-0.c bootstrap.c cc.h bootstrappable.c cc_globals.c cc_reader.c "
     "cc_strings.c cc_types.c cc_emit.c cc_core.c cc_macro.c cc.c",
     "M2-0.c", 0, 0},
    {"cc_x86.exe", "M2-0.c M2-0.M1", "M2-0.M1", 0, 0},
    {"catm.exe", "M2-0-0.M1 x86_defs.M1 libc-core.M1 libc-bootstrap.M1 M2-0.M1",
     "M2-0-0.M1", 0, 0},
    {"M0.exe", "M2-0-0.M1 M2-0.hex2", "M2-0.hex2", 0, 0},
    {"catm.exe", "M2-0-0.hex2 PE32-i386.hex2 ntdll-i386.hex2 M2-0.hex2",
     "M2-0-0.hex2", 0, 0},
    {"hex2.exe", "M2-0-0.hex2 M2.exe", "M2.exe", "M2",
     "9109ba7e5fb65553aff021eef4152c19c2b774e7972c9e23b0266b21f56d0db2"},
    {"M2.exe",
     "--architecture x86 --bootstrap-mode -f bootstrap.c "
     "-f bootstrappable.c -f stringify.c -f M1-macro.c -o M1-macro.M1",
     "M1-macro.M1", 0, 0},
    {"catm.exe",
     "M1-macro-0.M1 x86_defs.M1 libc-core.M1 libc-bootstrap.M1 "
     "M1-macro.M1 pe-end-shim.M1",
     "M1-macro-0.M1", 0, 0},
    {"M0.exe", "M1-macro-0.M1 M1-macro.hex2", "M1-macro.hex2", 0, 0},
    {"catm.exe",
     "M1-macro-0.hex2 PE32-i386.hex2 ntdll-i386.hex2 M1-macro.hex2",
     "M1-macro-0.hex2", 0, 0},
    {"hex2.exe", "M1-macro-0.hex2 M1.exe", "M1.exe", "M1",
     "ffa56be32281b490070b75ed1817d3d824bea884ae104c00b2052f7a94ec534f"},
};

#define STEP_COUNT (sizeof steps / sizeof steps[0])

/* The most files one step's view holds, and the longest command line,
   in ASCII characters with its NUL. */
#define VIEW_FILES_MAX 16
#define COMMAND_LINE_MAX 256

/* Each step's output, once it has ended. */
typedef struct {
    const UINT8 *data;
    UINT64 size;
} OUTPUT;

static OUTPUT outputs[STEP_COUNT];
static char command_line[COMMAND_LINE_MAX];

/* ------------------------------------------------------------------------
   Names. */

/* Whether the length characters at word are name. */
static int word_is(const char *word, UINT32 length, const char *name)
{
    UINT32 i;

    for (i = 0; i < length; i++)
        if (name[i] != word[i])
            return 0;
    return !name[length];
}

/* Whether the length characters at word name a file of the boot volume
   or the output of a step before step; if so, its name, bytes and size
   go to *name, *data and *size. */
static int file_named(const char *word, UINT32 length, UINT32 step,
                      const char **name, const UINT8 **data, UINT64 *size)
{
    const BOOT_FILE_VIEW *boot;
    UINT32 i;

    for (i = 0; i < SOURCE_COUNT; i++)
        if (word_is(word, length, sources[i].name) &&
            (boot = boot_file(sources[i].boot))) {
            *name = sources[i].name;
            *data = boot->data;
            *size = boot->size;
            return 1;
        }
    for (i = 0; i < step; i++)
        if (word_is(word, length, steps[i].output)) {
            *name = steps[i].output;
            *data = outputs[i].data;
            *size = outputs[i].size;
            return 1;
        }
    return 0;
}

static UINT32 length_of(const char *text)
{
    UINT32 length = 0;

    while (text[length])
        length++;
    return length;
}

/* The step's command line: its program's name, then its arguments. */
static int build_command_line(const STEP *step)
{
    UINT32 program = length_of(step->program);
    UINT32 arguments = length_of(step->arguments), i;

    if (program + 1 + arguments >= COMMAND_LINE_MAX)
        return 0;
    for (i = 0; i < program; i++)
        command_line[i] = step->program[i];
    command_line[program] = ' ';
    for (i = 0; i <= arguments; i++)
        command_line[program + 1 + i] = step->arguments[i];
    return 1;
}

/* The view of step number: each file its arguments name, once, then its
   output, which starts empty at the output region's end. Returns the
   files' count, or 0 if they do not fit. */
static UINT32 build_view(UINT32 number, VIEW_FILE *files)
{
    const STEP *step = &steps[number];
    const char *word = step->arguments, *name;
    UINT32 count = 0, length, i;
    const UINT8 *data;
    UINT64 size;
    int named;

    for (;;) {
        for (length = 0; word[length] && word[length] != ' '; length++)
            ;
        named = file_named(word, length, number, &name, &data, &size);
        for (i = 0; named && i < count; i++)
            if (files[i].name == name)
                named = 0;
        if (named) {
            if (count == VIEW_FILES_MAX - 1)
                return 0;
            files[count].name = name;
            files[count].data = data;
            files[count].output = 0;
            files[count].size = size;
            files[count].exists = 1;
            count++;
        }
        if (!word[length])
            break;
        word += length + 1;
    }
    files[count].name = step->output;
    files[count].data = files[count].output = output_next();
    files[count].size = 0;
    files[count].exists = 0;
    return count + 1;
}

/* ------------------------------------------------------------------------
   The run. */

static void hex_digest(const UINT8 *digest, char *text)
{
    static const char digits[] = "0123456789abcdef";
    UINT32 i;

    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        text[2 * i] = digits[digest[i] >> 4];
        text[2 * i + 1] = digits[digest[i] & 0xf];
    }
    text[2 * SHA256_DIGEST_SIZE] = 0;
}

static int failed(UINT32 number, const char *why)
{
    say("nova-init: the chain failed at step ");
    say_number(number + 1);
    say(", ");
    say(steps[number].program);
    say(" ");
    say(steps[number].arguments);
    say(": ");
    say(why);
    say("\r\n");
    return 0;
}

/* Run step number with ntdll and check what it writes. */
static int run_step(UINT32 job, UINT32 number, const BOOT_FILE_VIEW *ntdll)
{
    const STEP *step = &steps[number];
    VIEW_FILE files[VIEW_FILES_MAX];
    VIEW_FILE *output;
    COMPAT_IMAGE program, library;
    SHA256_CONTEXT context;
    UINT8 digest[SHA256_DIGEST_SIZE];
    char text[2 * SHA256_DIGEST_SIZE + 1];
    UINT32 count, process, directory, state, exit_code;

    if (!build_command_line(step))
        return failed(number, "its command line is too long");
    say("nova-init: step ");
    say_number(number + 1);
    say(" of ");
    say_number(STEP_COUNT);
    say(": ");
    say(command_line);
    say("\r\n");
    if (!file_named(step->program, length_of(step->program), number,
                    &program.name, &program.data, &program.size))
        return failed(number, "its program is not there");
    library.name = NTDLL_FILE;
    library.data = ntdll->data;
    library.size = ntdll->size;
    count = build_view(number, files);
    if (!count)
        return failed(number, "it names too many files");
    process = compat_start(job, &program, &library, command_line, &directory);
    if (!process)
        return failed(number, "its process did not start");
    view_serve(directory, process, files, count);
    process_info(process, &state, &exit_code);
    handle_close(process);
    if (state != PROCESS_STATE_TERMINATED || exit_code != STATUS_SUCCESS) {
        say("nova-init: step ");
        say_number(number + 1);
        say(" ended with exit status ");
        say_hex(exit_code);
        say("\r\n");
        return failed(number, "it did not succeed");
    }
    output = &files[count - 1];
    if (!output->exists)
        return failed(number, "it wrote no output");
    output_seal(output->size);
    outputs[number].data = output->data;
    outputs[number].size = output->size;
    sha256_init(&context);
    sha256_update(&context, output->data, output->size);
    sha256_final(&context, digest);
    hex_digest(digest, text);
    say("nova-init: ");
    say(step->output);
    say(": ");
    say_number(output->size);
    say(" bytes, sha256 ");
    say(text);
    if (step->pin) {
        say(", nova-nix's ");
        say(step->link);
    }
    say("\r\n");
    if (step->pin && !word_is(text, 2 * SHA256_DIGEST_SIZE, step->pin))
        return failed(number, "its output is not the link nova-nix pins");
    return 1;
}

static UINT32 sources_present(void)
{
    UINT32 count = 0, i;

    for (i = 0; i < SOURCE_COUNT; i++)
        if (boot_file(sources[i].boot))
            count++;
    return count;
}

/* Refuse a boot volume that holds present of the chain's inputs but not
   all, naming those it lacks. */
static int incomplete(UINT32 present)
{
    const char *separator = " ";
    UINT32 i;

    say("nova-init: the chain failed: the boot volume holds ");
    say_number(present);
    say(" of its ");
    say_number(SOURCE_COUNT);
    say(" inputs and lacks");
    for (i = 0; i < SOURCE_COUNT; i++)
        if (!boot_file(sources[i].boot)) {
            say(separator);
            say(sources[i].boot);
            separator = ", ";
        }
    say("\r\n");
    return 0;
}

int chain_run(UINT32 job)
{
    const BOOT_FILE_VIEW *ntdll = boot_file(NTDLL_FILE);
    UINT32 present = sources_present(), i;

    if (!present) {
        say("nova-init: the bootstrap check is not on this disk\r\n");
        return 1;
    }
    if (present != SOURCE_COUNT)
        return incomplete(present);
    if (!ntdll) {
        say("nova-init: the chain failed: no ntdll.dll on the boot volume\r\n");
        return 0;
    }
    for (i = 0; i < STEP_COUNT; i++)
        if (!run_step(job, i, ntdll))
            return 0;
    say("nova-init: the chain built nova-nix's hex0, hex1, hex2, catm, M0, "
        "cc_x86, M2 and M1\r\n");
    return 1;
}
