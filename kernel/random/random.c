/* The kernel's random number generator: one fast-key-erasure generator
   (generator.c), seeded at boot from every source the machine offers,
   hashed together so that no single source is trusted alone:

   - a seed from each EFI_RNG_PROTOCOL instance, which nova-boot asks for
     before ExitBootServices (docs/spec/boot-protocol.md);
   - words from RDSEED, or from RDRAND where RDSEED is absent or gives
     none (kernel/x86_64/entropy.c);
   - the timestamp counter at TIMING_SAMPLES timer interrupts.

   Everything goes into one SHA-256 hash, whose digest is injected once.
   The post behind generator.c warns that a cycle count hashed into the
   state on its own can be guessed by an attacker who knew the state, and
   has material accumulated "into a separate hash before injecting that
   hash into the RNG".

   The generator counts as seeded only when the firmware or the processor
   gave material. Interrupt timing is never enough alone: where the APIC
   timer and the timestamp counter both run from the core crystal clock
   (SDM Vol. 3A 13.5.4, Vol. 3B 20.17.4), the counter's value at each tick
   is close to predictable. An unseeded generator says so in the log and
   refuses every read; where the processor has RDSEED or RDRAND
   but its words failed at boot, the reseed below can still seed it.

   Threads share one generator. Each piece of a read runs with interrupts
   disabled, which on the one processor the kernel runs on keeps it from
   being interrupted or preempted; with more processors it needs a
   lock.

   A virtual machine restored from a snapshot repeats the generator's
   output until its next reseed. The post's answer, a kernel in a virtual
   machine reading the hypervisor's generator, needs a driver the kernel
   does not have. */

#include "random.h"
#include "generator.h"
#include "sha256.h"
#include "wipe.h"
#include "cpu.h"
#include "efi.h"
#include "../selftest.h"
#include "../x86_64/arch.h"
#include "../x86_64/entropy.h"
#include "../x86_64/serial.h"

/* Words drawn from the processor at boot and at each reseed: 256 bits,
   the generator's key size. */
#define CPU_WORDS (GENERATOR_KEY_SIZE / sizeof(UINT64))

/* No entropy is credited to interrupt timing, so its sample count is set
   by the boot time it costs: one timer tick each, 64 ms at TIMER_HZ. */
#define TIMING_SAMPLES 64

/* A reseed injects fresh words from the processor. Reseeding bounds how
   long a state an attacker has read keeps predicting output; a minute
   keeps that window short, while a reseed costs four RDSEED reads from
   a generator that every core shares (SDM Vol. 1 7.3.17.2) and three
   SHA-256 blocks. It happens at the first read after the interval, not
   on a timer. */
#define RESEED_TICKS (60 * TIMER_HZ)

/* The most bytes one piece of a read generates with interrupts disabled:
   one refill's output. */
#define READ_PIECE GENERATOR_BUFFER_SIZE

/* Output bytes logged at boot, so that two boots can be told apart. */
#define SHOWN_BYTES 16

/* The stack wipe_stack clears: over twice the 960 bytes that
   random_read's callees reach in TinyCC's build, and under the 4096-byte
   frame at which TinyCC calls __chkstk, which no runtime here provides. */
#define WIPE_STACK_SIZE 2048

static GENERATOR generator;
static int seeded;
static int has_rdseed, has_rdrand;
static UINT64 seeded_tick; /* timer_ticks() at the last injection */

/* Clear the stack below the caller's frame. The functions the caller has
   just called kept their locals there, and TinyCC spilled their
   arguments and temporaries there, state words and processor words among
   them, where wipe cannot name them. TinyCC never inlines a function, so
   this one's array lies where theirs did. */
static void wipe_stack(void)
{
    UINT8 below[WIPE_STACK_SIZE];

    wipe(below, sizeof(below));
}

/* ------------------------------------------------------------------------
   Sources */

/* Up to BOOT_RANDOM_SEEDS seeds; nova-boot asks no more instances than
   that, and a count beyond it is not trusted to index the array. Every
   seed is wiped from BOOT_INFO, used or not. Returns the seeds used, and
   in *count the instances that were asked. */
static UINT64 add_firmware_seeds(SHA256_CONTEXT *pool, BOOT_INFO *info,
                                 UINT64 *count)
{
    UINT64 i, used = 0;

    *count = info->random_seed_count < BOOT_RANDOM_SEEDS
                 ? info->random_seed_count
                 : BOOT_RANDOM_SEEDS;

    for (i = 0; i < *count; i++) {
        const BOOT_RANDOM_SEED *seed = &info->random_seeds[i];

        if (seed->status == EFI_SUCCESS) {
            sha256_update(pool, seed->bytes, sizeof(seed->bytes));
            used++;
        } else {
            serial_write("nova: random firmware seed unavailable instance=");
            serial_number(i);
            serial_write(" status=");
            serial_hex(seed->status);
            serial_write("\r\n");
        }
    }
    wipe(info->random_seeds, sizeof(info->random_seeds));
    return used;
}

/* Words that repeat come from an instruction that reports success
   without delivering random data, so such a draw does not count, as
   EDK II's BaseRngLib refuses an RDRAND whose values do not change
   (MdePkg/Library/BaseRngLib/Rand/RdRand.c, TestRdRand). */
static int repeats(const UINT64 *words, UINT64 count)
{
    UINT64 i;

    for (i = 1; i < count; i++)
        if (words[i] == words[i - 1])
            return 1;
    return 0;
}

static int draw_from(CPU_RANDOM instruction, UINT64 *words)
{
    const char *problem = 0;

    if (!cpu_random_read(instruction, words, CPU_WORDS))
        problem = "no value within its retries";
    else if (repeats(words, CPU_WORDS))
        problem = "a value repeated";
    if (!problem)
        return 1;
    serial_write("nova: random cpu source rejected instruction=");
    serial_write(cpu_random_name(instruction));
    serial_write(" reason=");
    serial_value(problem);
    serial_write("\r\n");
    return 0;
}

/* Fill words from RDSEED, or from RDRAND where RDSEED is absent or fails.
   Returns the instruction that filled them, or CPU_RANDOM_NONE. */
static CPU_RANDOM draw_cpu(UINT64 *words)
{
    if (has_rdseed && draw_from(CPU_RANDOM_RDSEED, words))
        return CPU_RANDOM_RDSEED;
    if (has_rdrand && draw_from(CPU_RANDOM_RDRAND, words))
        return CPU_RANDOM_RDRAND;
    return CPU_RANDOM_NONE;
}

/* The counter just after each of TIMING_SAMPLES timer interrupts. */
static void add_timing(SHA256_CONTEXT *pool)
{
    UINT64 i, sample;

    for (i = 0; i < TIMING_SAMPLES; i++) {
        timer_sleep_until(timer_ticks() + 1);
        sample = cycle_counter();
        sha256_update(pool, &sample, sizeof(sample));
    }
    wipe(&sample, sizeof(sample));
}

/* ------------------------------------------------------------------------
   Seeding */

/* Without the processor's instruction nothing worth injecting is left:
   the firmware's protocol went with boot services, and a cycle count
   alone is what the post says an attacker who knew the state can guess.
   Words from the processor seed a generator that was not seeded. */
static void reseed(void)
{
    SHA256_CONTEXT pool;
    UINT64 words[CPU_WORDS], now;
    UINT8 digest[SHA256_DIGEST_SIZE];
    CPU_RANDOM cpu;

    seeded_tick = timer_ticks();
    cpu = draw_cpu(words);
    if (cpu == CPU_RANDOM_NONE) {
        wipe(words, sizeof(words));
        return;
    }
    now = cycle_counter();
    sha256_init(&pool);
    sha256_update(&pool, words, sizeof(words));
    sha256_update(&pool, &now, sizeof(now));
    sha256_final(&pool, digest);
    generator_inject(&generator, digest);
    wipe(words, sizeof(words));
    wipe(digest, sizeof(digest));
    if (!seeded) {
        seeded = 1;
        serial_write("nova: random seeded late cpu=");
        serial_write(cpu_random_name(cpu));
        serial_write("\r\n");
    }
}

/* The generator's first SHOWN_BYTES, as a key of the seeding line:
   drawn for the log and never used, so that two boots can be compared;
   output reveals nothing of the key, and these bytes leave the buffer
   like any others. */
static void show_first_bytes(void)
{
    static const char hex[] = "0123456789abcdef";
    UINT8 bytes[SHOWN_BYTES];
    char text[2 * SHOWN_BYTES + 1];
    int i;

    random_read(bytes, sizeof(bytes));
    for (i = 0; i < SHOWN_BYTES; i++) {
        text[2 * i] = hex[bytes[i] >> 4];
        text[2 * i + 1] = hex[bytes[i] & 0xf];
    }
    text[2 * SHOWN_BYTES] = 0;
    serial_write(" first=");
    serial_write(text);
}

/* The sources the boot drew from: the firmware's seeds used and the
   instances asked, the processor's instruction, and the interrupt
   timings; then, if they seeded the generator, its first bytes, else
   whether a reseed from the processor can still seed it. */
static void report_seeding(UINT64 firmware, UINT64 instances,
                           CPU_RANDOM cpu)
{
    serial_write(seeded ? "nova: random seeded firmware="
                        : "nova: random unseeded firmware=");
    serial_number(firmware);
    serial_write(" instances=");
    serial_number(instances);
    serial_write(" cpu=");
    serial_write(cpu != CPU_RANDOM_NONE ? cpu_random_name(cpu) : "none");
    serial_write(" timings=");
    serial_number(TIMING_SAMPLES);
    if (seeded)
        show_first_bytes();
    else
        serial_write(has_rdseed || has_rdrand ? " retry=minute"
                                              : " retry=none");
    serial_write("\r\n");
}

void random_init(BOOT_INFO *info)
{
    SHA256_CONTEXT pool;
    UINT64 words[CPU_WORDS], firmware, instances;
    UINT8 digest[SHA256_DIGEST_SIZE];
    CPU_RANDOM cpu;
    const char *failure = random_self_test();

    if (failure)
        selftest_halt(SELFTEST_RANDOM, failure);
    selftest_passed(SELFTEST_RANDOM);

    has_rdseed = cpu_random_supported(CPU_RANDOM_RDSEED);
    has_rdrand = cpu_random_supported(CPU_RANDOM_RDRAND);

    generator_init(&generator);
    sha256_init(&pool);
    firmware = add_firmware_seeds(&pool, info, &instances);
    cpu = draw_cpu(words);
    if (cpu != CPU_RANDOM_NONE)
        sha256_update(&pool, words, sizeof(words));
    wipe(words, sizeof(words));
    add_timing(&pool);
    sha256_final(&pool, digest);
    generator_inject(&generator, digest);
    wipe(digest, sizeof(digest));
    wipe_stack();
    seeded_tick = timer_ticks();
    seeded = firmware > 0 || cpu != CPU_RANDOM_NONE;
    report_seeding(firmware, instances, cpu);
}

/* ------------------------------------------------------------------------
   Reading */

int random_read(VOID *buffer, UINT64 count)
{
    UINT8 *out = buffer;
    int ready;

    do {
        UINT64 piece = count < READ_PIECE ? count : READ_PIECE;
        UINT64 flags = save_and_disable_interrupts();

        if (timer_ticks() - seeded_tick >= RESEED_TICKS)
            reseed();
        ready = seeded;
        if (ready)
            generator_read(&generator, out, piece);
        restore_interrupts(flags);
        wipe_stack();
        out += piece;
        count -= piece;
    } while (ready && count > 0);
    return ready;
}
