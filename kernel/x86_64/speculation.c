/* The speculative-execution measures (docs/spec/speculation.md). At boot
   the kernel reads the enumeration M2 lists, decides each class's measure
   by the rules of M4 to M16 in an order that lets each rule see the ones
   it depends on, sets the measures set once at boot, enables the entry
   and exit's measure sites for the rest, and writes one line to the log:
   the processor, then for each class what it gets and why. A class the
   processor gives no means to mitigate is logged UNMITIGATED, and one
   whose mitigation the guidance obtained cannot confirm, unconfirmed.

   The vendors' guidance is cited by speculation.md's names. */

#include "speculation.h"
#include "cpu.h"
#include "page_table.h"
#include "paging.h"
#include "serial.h"
#include "x86_64.h"

/* ------------------------------------------------------------------------
   The enumeration (M2). */

/* CPUID.0's vendor string, in EBX, EDX and ECX, four characters each in
   memory order. */
#define CPUID_VENDOR 0
#define INTEL_EBX 0x756e6547 /* "Genu" */
#define INTEL_EDX 0x49656e69 /* "ineI" */
#define INTEL_ECX 0x6c65746e /* "ntel" */
#define AMD_EBX 0x68747541   /* "Auth" */
#define AMD_EDX 0x69746e65   /* "enti" */
#define AMD_ECX 0x444d4163   /* "cAMD" */

/* CPUID.01H: the signature in EAX, whose family and model AMD defines
   as the base fields plus, where the base family is 0Fh, the extended
   ones (APM Vol. 3 E.3.2); the hypervisor bit in ECX. */
#define CPUID_FEATURES 1
#define SIGNATURE_STEPPING(eax) ((eax) & 0xf)
#define SIGNATURE_MODEL(eax) ((eax) >> 4 & 0xf)
#define SIGNATURE_FAMILY(eax) ((eax) >> 8 & 0xf)
#define SIGNATURE_EXTENDED_MODEL(eax) ((eax) >> 16 & 0xf)
#define SIGNATURE_EXTENDED_FAMILY(eax) ((eax) >> 20 & 0xff)
#define FAMILY_EXTENDED 0xf
#define FEATURES_ECX_HYPERVISOR (1u << 31)

/* CPUID.(EAX=07H,ECX=0): its EAX gives the highest subleaf. */
#define CPUID_STRUCTURED 7
#define STRUCTURED_EBX_HLE (1u << 4)
#define STRUCTURED_EBX_SMEP (1u << 7)
#define STRUCTURED_EBX_RTM (1u << 11)
#define STRUCTURED_EDX_MD_CLEAR (1u << 10)
#define STRUCTURED_EDX_IBRS_IBPB (1u << 26)
#define STRUCTURED_EDX_ARCH_CAPABILITIES (1u << 29)
#define STRUCTURED_SUBLEAF_2 2
#define STRUCTURED_2_EDX_BHI_CTRL (1u << 4)

/* AMD's Fn8000_0008_EBX and Fn8000_0021_EAX and ECX (APM Vol. 3 E.4.7,
   E.4.19; AMD TSA 6). CPUID.80000000H:EAX gives the highest extended
   leaf. */
#define CPUID_EXTENDED_MAX 0x80000000u
#define CPUID_AMD_8 0x80000008u
#define AMD_8_EBX_IBPB (1u << 12)
#define AMD_8_EBX_IBRS (1u << 14)
#define AMD_8_EBX_IBRS_ALWAYS_ON (1u << 16)
#define AMD_8_EBX_BTC_NO (1u << 29)
#define AMD_8_EBX_IBPB_RET (1u << 30)
#define CPUID_AMD_21 0x80000021u
#define AMD_21_EAX_LFENCE_SERIALIZING (1u << 2)
#define AMD_21_EAX_VERW_CLEAR (1u << 5)
#define AMD_21_EAX_AUTOMATIC_IBRS (1u << 8)
#define AMD_21_EAX_ERAPS (1u << 24)
#define AMD_21_EAX_IBPB_BRTYPE (1u << 28)
#define AMD_21_EAX_SRSO_NO (1u << 29)
#define AMD_21_EAX_SRSO_USER_KERNEL_NO (1u << 30)
#define AMD_21_ECX_TSA_SQ_NO (1u << 1)
#define AMD_21_ECX_TSA_L1_NO (1u << 2)

/* IA32_ARCH_CAPABILITIES (Intel CPUID; Intel GDS, "MSR Control and
   Enumeration"). */
#define IA32_ARCH_CAPABILITIES 0x10au
#define ARCH_RDCL_NO (1ULL << 0)
#define ARCH_IBRS_ALL (1ULL << 1)
#define ARCH_MDS_NO (1ULL << 5)
#define ARCH_TSX_CTRL (1ULL << 7)
#define ARCH_TAA_NO (1ULL << 8)
#define ARCH_SBDR_SSDP_NO (1ULL << 13)
#define ARCH_FBSDP_NO (1ULL << 14)
#define ARCH_PSDP_NO (1ULL << 15)
#define ARCH_FB_CLEAR (1ULL << 17)
#define ARCH_BHI_NO (1ULL << 20)
#define ARCH_GDS_CTRL (1ULL << 25)
#define ARCH_GDS_NO (1ULL << 26)
#define ARCH_RFDS_CLEAR (1ULL << 28)
#define ARCH_MMIO_NO (ARCH_SBDR_SSDP_NO | ARCH_FBSDP_NO | ARCH_PSDP_NO)

/* The controls the measures set: IA32_SPEC_CTRL's IBRS and BHI_DIS_S
   (Intel CPUID; Intel BHI), IA32_TSX_CTRL's RTM_DISABLE and
   TSX_CPUID_CLEAR (Intel CPUID, "IA32_TSX_CTRL MSR"), EFER's AIBRSE
   (APM 3.1.7), and AMD's DE_CFG, whose bit 1 makes LFENCE dispatch
   serializing (AMD STMS G-2; PPR 57238, MSRC001_1029), and PATCH_LEVEL,
   the loaded microcode's revision (PPR 57238, MSR0000_008B). */
#define SPEC_CTRL_IBRS (1ULL << 0)
#define SPEC_CTRL_BHI_DIS_S (1ULL << 10)
#define IA32_TSX_CTRL 0x122u
#define TSX_CTRL_RTM_DISABLE (1ULL << 0)
#define TSX_CTRL_CPUID_CLEAR (1ULL << 1)
#define EFER_AUTOMATIC_IBRS (1ULL << 21)
#define AMD_DE_CFG 0xc0011029u
#define DE_CFG_LFENCE_SERIALIZING (1ULL << 1)
#define AMD_PATCH_LEVEL 0x8bu
#define PATCH_LEVEL_MASK 0xffffffffULL

/* IA32_MCU_OPT_CTRL's GDS_MITG_DIS: set, the microcode's GDS mitigation
   is off (Intel GDS, "MSR Control and Enumeration"). */
#define IA32_MCU_OPT_CTRL 0x123u
#define MCU_OPT_CTRL_GDS_MITG_DIS (1ULL << 4)

/* The AMD families M2 names: "Zen" and "Zen 2" are Family 17h, "Zen 3"
   and "Zen 4" Family 19h (AMD SRSO 5; AMD BTC, Appendix). Family 0Fh
   and 11h make LFENCE serializing without DE_CFG, which they lack (AMD
   STMS G-2). Family 1Ah models 00h to 4Fh and 60h to 7Fh are not
   affected by TSA (AMD TSA 6). */
#define AMD_FAMILY_0F 0x0f
#define AMD_FAMILY_11 0x11
#define AMD_FAMILY_17 0x17
#define AMD_FAMILY_19 0x19
#define AMD_FAMILY_1A 0x1a
#define TSA_1A_MODELS_LOW_END 0x4f
#define TSA_1A_MODELS_HIGH_START 0x60
#define TSA_1A_MODELS_HIGH_END 0x7f

typedef enum { VENDOR_INTEL, VENDOR_AMD, VENDOR_OTHER } VENDOR;

typedef struct {
    VENDOR vendor;
    UINT32 signature;
    UINT32 family; /* AMD's definition, for the inferences M2 allows */
    UINT32 model;
    int hypervisor;
    UINT32 features_ecx;
    UINT32 structured_ebx;
    UINT32 structured_edx;
    UINT32 structured_2_edx;
    UINT32 amd_8_ebx;
    UINT32 amd_21_eax;
    UINT32 amd_21_ecx;
    int arch_present; /* IA32_ARCH_CAPABILITIES enumerated and read */
    UINT64 arch;
} ENUMERATION;

static ENUMERATION cpu;

/* Rules on Intel bits apply where CPUID.0 reports GenuineIntel, and a
   processor of another vendor is treated as Intel's (M2, rule 3). */
static int intel_rules(void)
{
    return cpu.vendor != VENDOR_AMD;
}

static int amd(void)
{
    return cpu.vendor == VENDOR_AMD;
}

/* Family and model inferences hold only on bare metal (M2, rule 2). */
static int bare_metal_family(UINT32 family)
{
    return amd() && !cpu.hypervisor && cpu.family == family;
}

static int arch(UINT64 bit)
{
    return (cpu.arch & bit) != 0;
}

/* An unreadable IA32_ARCH_CAPABILITIES reads as 0, every "not affected"
   bit clear (M11: "including those without the MSR"). */
static void enumerate(void)
{
    UINT32 a, b, c, d, max, extended_max;

    cpuid(CPUID_VENDOR, &max, &b, &c, &d);
    if (b == INTEL_EBX && d == INTEL_EDX && c == INTEL_ECX)
        cpu.vendor = VENDOR_INTEL;
    else if (b == AMD_EBX && d == AMD_EDX && c == AMD_ECX)
        cpu.vendor = VENDOR_AMD;
    else
        cpu.vendor = VENDOR_OTHER;
    cpuid(CPUID_FEATURES, &a, &b, &c, &d);
    cpu.signature = a;
    cpu.features_ecx = c;
    cpu.hypervisor = (c & FEATURES_ECX_HYPERVISOR) != 0;
    cpu.family = SIGNATURE_FAMILY(a);
    cpu.model = SIGNATURE_MODEL(a);
    if (cpu.family == FAMILY_EXTENDED) {
        cpu.family += SIGNATURE_EXTENDED_FAMILY(a);
        cpu.model |= SIGNATURE_EXTENDED_MODEL(a) << 4;
    }
    if (max >= CPUID_STRUCTURED) {
        cpuid(CPUID_STRUCTURED, &a, &b, &c, &d);
        cpu.structured_ebx = b;
        cpu.structured_edx = d;
        if (a >= STRUCTURED_SUBLEAF_2) {
            cpuid_subleaf(CPUID_STRUCTURED, STRUCTURED_SUBLEAF_2, &a, &b, &c,
                          &d);
            cpu.structured_2_edx = d;
        }
    }
    cpuid(CPUID_EXTENDED_MAX, &extended_max, &b, &c, &d);
    if (amd() && extended_max >= CPUID_AMD_8) {
        cpuid(CPUID_AMD_8, &a, &b, &c, &d);
        cpu.amd_8_ebx = b;
    }
    if (amd() && extended_max >= CPUID_AMD_21) {
        cpuid(CPUID_AMD_21, &a, &b, &c, &d);
        cpu.amd_21_eax = a;
        cpu.amd_21_ecx = c;
    }
    cpu.arch = 0;
    cpu.arch_present =
        intel_rules() &&
        (cpu.structured_edx & STRUCTURED_EDX_ARCH_CAPABILITIES) &&
        msr_read_checked(IA32_ARCH_CAPABILITIES, &cpu.arch);
}

/* ------------------------------------------------------------------------
   The log line. */

static void report(const char *class_name, const char *measure,
                   const char *reason)
{
    serial_write("; ");
    serial_write(class_name);
    serial_write(" ");
    serial_write(measure);
    if (reason) {
        serial_write(" (");
        serial_write(reason);
        serial_write(")");
    }
}

static void report_processor(void)
{
    serial_write("nova: speculation: ");
    serial_write(cpu.vendor == VENDOR_INTEL ? "GenuineIntel"
                 : amd()                    ? "AuthenticAMD"
                                            : "other vendor, Intel's rules");
    serial_write(" signature ");
    serial_hex(cpu.signature);
    if (amd()) {
        serial_write(" family ");
        serial_hex(cpu.family);
        serial_write(" model ");
        serial_hex(cpu.model);
    }
    serial_write(cpu.hypervisor ? ", hypervisor" : ", bare metal");
    if (cpu.arch_present) {
        serial_write(", arch capabilities ");
        serial_hex(cpu.arch);
    } else if (intel_rules()) {
        serial_write(cpu.structured_edx & STRUCTURED_EDX_ARCH_CAPABILITIES
                         ? ", arch capabilities refused"
                         : ", no arch capabilities");
    }
}

/* ------------------------------------------------------------------------
   The decisions, in an order that lets each see the ones it depends on. */

/* The measure sites to enable, the IA32_SPEC_CTRL values the entry and
   exit write (entry.S, syscall.S), and the process switch's work. */
static UINT64 measures;
UINT64 spec_ctrl_kernel;
UINT64 spec_ctrl_user;
static int spec_ctrl_present;
static int ibrs_present;
static int bhi_dis_present;
static int ibpb_present;
static int switch_ibpb;
static int switch_rsb_overwrite;

/* IBRS, BHI_DIS_S and IBPB, from Intel's or AMD's enumeration, each
   confirmed by a write that completed: a hypervisor can enumerate a
   control without emulating its MSR, or refuse a bit of it. IA32_SPEC_CTRL
   is left 0, which also clears SSBD whatever firmware left (M14), until
   speculation_init writes the kernel's value. The IBPB the probe issues
   is harmless. */
static void probe_controls(int ibrs_enumerated, int ibpb_enumerated)
{
    int bhi_ctrl = intel_rules() &&
                   (cpu.structured_2_edx & STRUCTURED_2_EDX_BHI_CTRL) != 0;

    spec_ctrl_present =
        (ibrs_enumerated || bhi_ctrl) && msr_write_checked(IA32_SPEC_CTRL, 0);
    ibrs_present = spec_ctrl_present && ibrs_enumerated &&
                   msr_write_checked(IA32_SPEC_CTRL, SPEC_CTRL_IBRS);
    bhi_dis_present = spec_ctrl_present && bhi_ctrl &&
                      msr_write_checked(IA32_SPEC_CTRL, SPEC_CTRL_BHI_DIS_S);
    if (spec_ctrl_present)
        wrmsr(IA32_SPEC_CTRL, 0);
    ibpb_present =
        ibpb_enumerated && msr_write_checked(IA32_PRED_CMD, PRED_CMD_IBPB);
}

/* M4: masks and CMOV are in the code (cpu.h); LFENCE must be dispatch
   serializing, which AMD processors make it where they do not say it
   always is, and where a bare-metal family's LFENCE is serializing
   without DE_CFG (AMD STMS G-2). */
static void decide_bounds_check(void)
{
    UINT64 de_cfg = 0;

    if (!amd()) {
        report("bcb", "masks", 0);
    } else if (cpu.amd_21_eax & AMD_21_EAX_LFENCE_SERIALIZING) {
        report("bcb", "masks", "lfence serializing, LfenceAlwaysSerializing");
    } else if (bare_metal_family(AMD_FAMILY_0F) ||
               bare_metal_family(AMD_FAMILY_11)) {
        report("bcb", "masks", "lfence serializing by family, no DE_CFG");
    } else if (msr_read_checked(AMD_DE_CFG, &de_cfg) &&
               msr_write_checked(AMD_DE_CFG,
                                 de_cfg | DE_CFG_LFENCE_SERIALIZING) &&
               msr_read_checked(AMD_DE_CFG, &de_cfg) &&
               (de_cfg & DE_CFG_LFENCE_SERIALIZING)) {
        report("bcb", "masks", "lfence serializing, DE_CFG[1] set");
    } else {
        report("bcb", "masks",
               "lfence serializing unconfirmed, DE_CFG refused");
    }
}

/* M5. */
static void decide_swapgs(void)
{
    report("swapgs", "fenced", 0);
}

/* M9 and M10: IBPB on every entry, shared, on AMD processors affected by
   either. Returns whether it runs. */
static int decide_amd_entry_ibpb(void)
{
    int btc, srso;

    if (!amd()) {
        report("btc", "n/a", "not AMD");
        report("srso", "n/a", "not AMD");
        return 0;
    }
    btc = !(cpu.amd_8_ebx & AMD_8_EBX_BTC_NO) &&
          !bare_metal_family(AMD_FAMILY_19);
    srso = !(cpu.amd_21_eax &
             (AMD_21_EAX_SRSO_NO | AMD_21_EAX_SRSO_USER_KERNEL_NO));
    if (!btc)
        report("btc", "not affected",
               cpu.amd_8_ebx & AMD_8_EBX_BTC_NO ? "BTC_NO" : "family 19h");
    else
        report("btc", ibpb_present ? "ibpb on entry" : "UNMITIGATED",
               ibpb_present ? 0 : "no IBPB");
    if (cpu.amd_21_eax & AMD_21_EAX_SRSO_NO)
        report("srso", "not affected", "SRSO_NO");
    else if (!srso)
        report("srso", "not affected user to kernel", "SRSO_USER_KERNEL_NO");
    else if (!ibpb_present)
        report("srso", "UNMITIGATED", "no IBPB");
    else if (cpu.amd_21_eax & AMD_21_EAX_IBPB_BRTYPE)
        report("srso", "ibpb on entry", "IBPB_BRTYPE");
    else if (bare_metal_family(AMD_FAMILY_17))
        report("srso", "ibpb on entry", "family 17h");
    else
        report("srso", "ibpb on entry, unconfirmed",
               "no IBPB_BRTYPE, microcode list not obtained");
    return (btc || srso) && ibpb_present;
}

/* M6: user to kernel, the first rule that applies, then process to
   process. IBPB on every entry covers user to kernel on its own, IBRS or
   not (rule 4). */
static void decide_branch_target(int entry_ibpb)
{
    int eibrs = intel_rules() && arch(ARCH_IBRS_ALL) && ibrs_present;

    if (eibrs) {
        spec_ctrl_kernel |= SPEC_CTRL_IBRS;
        report("bti", "enhanced ibrs", 0);
    } else if (amd() && (cpu.amd_21_eax & AMD_21_EAX_AUTOMATIC_IBRS) &&
               msr_write_checked(IA32_EFER,
                                 rdmsr(IA32_EFER) | EFER_AUTOMATIC_IBRS)) {
        report("bti", "automatic ibrs", 0);
    } else if (amd() && (cpu.amd_8_ebx & AMD_8_EBX_IBRS_ALWAYS_ON) &&
               ibrs_present) {
        spec_ctrl_kernel |= SPEC_CTRL_IBRS;
        report("bti", "ibrs always on", 0);
    } else if (entry_ibpb) {
        report("bti", "ibpb on entry",
               ibrs_present ? "ibrs writes skipped" : 0);
    } else if (ibrs_present) {
        spec_ctrl_kernel |= SPEC_CTRL_IBRS;
        measures |= MEASURE_IBRS;
        report("bti", "ibrs on entry", "no enhanced ibrs");
    } else {
        report("bti", "UNMITIGATED", "no IBRS");
    }
    switch_ibpb = ibpb_present;
    report("process switch", ibpb_present ? "ibpb" : "UNMITIGATED",
           ibpb_present ? 0 : "no IBPB");
}

/* M7. */
static void decide_return_stack(void)
{
    int smep = (cpu.structured_ebx & STRUCTURED_EBX_SMEP) != 0;

    if (smep) {
        report("rsb", "smep", 0);
    } else {
        measures |= MEASURE_RSB;
        report("rsb", "32 calls on entry",
               intel_rules() && arch(ARCH_IBRS_ALL)
                   ? "no SMEP; UNMITIGATED with enhanced ibrs"
                   : "no SMEP");
    }
    if (amd() && !(cpu.amd_8_ebx & AMD_8_EBX_IBPB_RET) &&
        !(cpu.amd_21_eax & AMD_21_EAX_ERAPS)) {
        switch_rsb_overwrite = 1;
        report("rsb switch", "32 calls", "no IBPB_RET or ERAPS");
    }
}

/* M8, Intel's rules for operating systems. */
static void decide_branch_history(void)
{
    int eibrs = arch(ARCH_IBRS_ALL);
    int ibrs = (cpu.structured_edx & STRUCTURED_EDX_IBRS_IBPB) != 0;

    if (!intel_rules()) {
        report("bhi", "n/a", "AMD");
    } else if (arch(ARCH_BHI_NO)) {
        report("bhi", "not affected", "BHI_NO");
    } else if (bhi_dis_present) {
        spec_ctrl_kernel |= SPEC_CTRL_BHI_DIS_S;
        report("bhi", "bhi_dis_s", 0);
    } else if (eibrs) {
        measures |= MEASURE_BHB;
        report("bhi", "bhb clear on entry", "enhanced ibrs, no BHI_CTRL");
    } else if (!cpu.hypervisor) {
        report("bhi", "none needed", "no enhanced ibrs, bare metal");
    } else if (ibrs) {
        measures |= MEASURE_BHB;
        report("bhi", "bhb clear on entry", "ibrs under a hypervisor");
    } else {
        report("bhi", "none needed", "no ibrs under a hypervisor");
    }
}

/* M11 and M12. The kernel has no page-table isolation. */
static void decide_isolation(void)
{
    if (!intel_rules() || arch(ARCH_RDCL_NO)) {
        report("rdcl", "not affected", amd() ? "AMD" : "RDCL_NO");
        report("l1tf", "zero non-present entries", "not affected");
        return;
    }
    report("rdcl", "UNMITIGATED", "no page-table isolation");
    report("l1tf", "zero non-present entries", "affected");
}

/* The microcode revision from which VERW clears what TSA samples on bare
   metal, by Family 19h model and stepping (AMD TSA, Appendix). */
typedef struct {
    UINT8 model;
    UINT8 stepping;
    UINT32 revision;
} TSA_MICROCODE;

static const TSA_MICROCODE tsa_microcode[] = {
    {0x01, 0x1, 0x0a0011d7}, {0x01, 0x2, 0x0a00123b},
    {0x08, 0x2, 0x0a00820d}, {0x11, 0x1, 0x0a10114c},
    {0x11, 0x2, 0x0a10124c}, {0x18, 0x1, 0x0a108109},
    {0x21, 0x0, 0x0a20102e}, {0x21, 0x2, 0x0a201211},
    {0x44, 0x1, 0x0a404108}, {0x50, 0x0, 0x0a500012},
    {0x61, 0x2, 0x0a60120a}, {0x74, 0x1, 0x0a704108},
    {0x75, 0x2, 0x0a705208}, {0x78, 0x0, 0x0a708008},
    {0x7c, 0x0, 0x0a70c008}, {0xa0, 0x2, 0x0aa00216},
};

#define TSA_MICROCODE_COUNT (sizeof(tsa_microcode) / sizeof(tsa_microcode[0]))

static int tsa_microcode_present(void)
{
    UINT64 level = 0;
    UINT32 i;

    if (!cpu.hypervisor && cpu.family == AMD_FAMILY_19 &&
        msr_read_checked(AMD_PATCH_LEVEL, &level))
        for (i = 0; i < TSA_MICROCODE_COUNT; i++)
            if (tsa_microcode[i].model == cpu.model &&
                tsa_microcode[i].stepping == SIGNATURE_STEPPING(cpu.signature))
                return (level & PATCH_LEVEL_MASK) >= tsa_microcode[i].revision;
    return 0;
}

static int tsa_exempt(void)
{
    return !cpu.hypervisor &&
           (cpu.family < AMD_FAMILY_19 ||
            (cpu.family == AMD_FAMILY_1A &&
             (cpu.model <= TSA_1A_MODELS_LOW_END ||
              (cpu.model >= TSA_1A_MODELS_HIGH_START &&
               cpu.model <= TSA_1A_MODELS_HIGH_END))));
}

/* One row of M13's table: affected, and whether VERW clears it. Returns
   1 where it calls for VERW. */
static int data_sampling_row(const char *class_name, int affected,
                             int cleared, const char *missing)
{
    if (!affected) {
        report(class_name, "not affected", 0);
        return 0;
    }
    report(class_name, cleared ? "verw" : "UNMITIGATED", cleared ? 0 : missing);
    return cleared;
}

/* M13: TSX disabled where TAA applies and the processor can disable it,
   then VERW on every exit and before HLT where any row calls for it. TSX
   is present where CPUID reports HLE or RTM, and where TSX_CTRL exists,
   which TSX_CPUID_CLEAR can leave set with RTM still running (Intel
   CPUID, "IA32_TSX_CTRL MSR"). */
static void decide_data_sampling(void)
{
    int md_clear = (cpu.structured_edx & STRUCTURED_EDX_MD_CLEAR) != 0;
    int tsx =
        (cpu.structured_ebx & (STRUCTURED_EBX_HLE | STRUCTURED_EBX_RTM)) != 0 ||
        arch(ARCH_TSX_CTRL);
    int mds = !arch(ARCH_MDS_NO);
    int verw = 0;

    if (intel_rules()) {
        if (arch(ARCH_TSX_CTRL) && !arch(ARCH_TAA_NO) &&
            msr_write_checked(IA32_TSX_CTRL,
                              TSX_CTRL_RTM_DISABLE | TSX_CTRL_CPUID_CLEAR)) {
            tsx = 0;
            report("tsx", "disabled", "TAA_NO clear");
        }
        verw |= data_sampling_row("mds", mds, md_clear, "no MD_CLEAR");
        verw |= data_sampling_row("taa", tsx && !arch(ARCH_TAA_NO), md_clear,
                                  "no MD_CLEAR");
        verw |= data_sampling_row(
            "mmio", (cpu.arch & ARCH_MMIO_NO) != ARCH_MMIO_NO,
            arch(ARCH_FB_CLEAR) || (md_clear && mds), "no FB_CLEAR");
        verw |= data_sampling_row("rfds", arch(ARCH_RFDS_CLEAR), 1, 0);
    } else {
        verw |= data_sampling_row(
            "tsa",
            (cpu.amd_21_ecx & (AMD_21_ECX_TSA_L1_NO | AMD_21_ECX_TSA_SQ_NO)) !=
                    (AMD_21_ECX_TSA_L1_NO | AMD_21_ECX_TSA_SQ_NO) &&
                !tsa_exempt(),
            (cpu.amd_21_eax & AMD_21_EAX_VERW_CLEAR) || tsa_microcode_present(),
            "no VERW_CLEAR or listed microcode");
    }
    if (verw)
        measures |= MEASURE_VERW;
    report("verw", verw ? "on exit and before hlt" : "off", 0);
}

/* M14 to M16, which take nothing on the entry or exit. The kernel never
   sets GDS_MITG_DIS, but firmware can have. */
static void decide_rest(void)
{
    UINT64 mcu_opt_ctrl = 0;

    report("ssb", "ssbd clear", 0);
    if (!intel_rules())
        report("gds", "n/a", "AMD");
    else if (arch(ARCH_GDS_NO))
        report("gds", "not affected", "GDS_NO");
    else if (!arch(ARCH_GDS_CTRL))
        report("gds", "UNMITIGATED", "no GDS_CTRL");
    else if (!msr_read_checked(IA32_MCU_OPT_CTRL, &mcu_opt_ctrl))
        report("gds", "unconfirmed", "IA32_MCU_OPT_CTRL refused");
    else if (mcu_opt_ctrl & MCU_OPT_CTRL_GDS_MITG_DIS)
        report("gds", "UNMITIGATED", "GDS_MITG_DIS set by firmware");
    else
        report("gds", "microcode mitigation left on", 0);
    report("smt", "bootstrap processor only", 0);
}

/* ------------------------------------------------------------------------
   The measure sites (x86_64.h, MEASURE). */

#define JMP_REL32 0xe9
#define SITE_SIZE 5

/* The five-byte NOP, 0F 1F 44 00 00 (SDM Vol. 2B, NOP, Table 4-12). */
static const UINT8 nop5[SITE_SIZE] = {0x0f, 0x1f, 0x44, 0x00, 0x00};

/* Enable each site whose measure applies, through the direct map, since
   the image's text is mapped read-only. A site that does not start with
   the JMP MEASURE assembles means the table and the code disagree, and
   the kernel stops rather than run without a measure. CPUID, which is
   serializing, then makes sure no stale copy of the old bytes is run
   (SDM Vol. 3A 10.1.3). */
static void enable_sites(const MEASURE_SITE *first, const MEASURE_SITE *end)
{
    const MEASURE_SITE *site;

    for (site = first; site < end; site++) {
        if (!(site->measure & measures))
            continue;
        if (*(const UINT8 *)site->site != JMP_REL32) {
            serial_write("\r\nnova: speculation: measure site ");
            serial_hex(site->site);
            serial_write(" is not a jump, halted\r\n");
            halt_forever();
        }
        paging_write_text(site->site, nop5, SITE_SIZE);
    }
}

static void serialize(void)
{
    UINT32 a, b, c, d;

    cpuid(CPUID_VENDOR, &a, &b, &c, &d);
}

void speculation_init(void)
{
    int entry_ibpb;

    enumerate();
    report_processor();
    probe_controls(intel_rules()
                       ? (cpu.structured_edx & STRUCTURED_EDX_IBRS_IBPB) != 0
                       : (cpu.amd_8_ebx & AMD_8_EBX_IBRS) != 0,
                   intel_rules()
                       ? (cpu.structured_edx & STRUCTURED_EDX_IBRS_IBPB) != 0
                       : (cpu.amd_8_ebx & AMD_8_EBX_IBPB) != 0);
    decide_bounds_check();
    decide_swapgs();
    entry_ibpb = decide_amd_entry_ibpb();
    if (entry_ibpb)
        measures |= MEASURE_IBPB;
    decide_branch_target(entry_ibpb);
    decide_return_stack();
    decide_branch_history();
    decide_isolation();
    decide_data_sampling();
    decide_rest();
    if (spec_ctrl_present &&
        !msr_write_checked(IA32_SPEC_CTRL, spec_ctrl_kernel)) {
        measures &= ~MEASURE_IBRS;
        spec_ctrl_kernel = 0;
        report("spec_ctrl", "UNMITIGATED",
               "the processor refused IBRS and BHI_DIS_S together");
    }
    serial_write("\r\n");
    spec_ctrl_user = spec_ctrl_kernel & ~SPEC_CTRL_IBRS;
    enable_sites(entry_measures, entry_measures_end);
    enable_sites(syscall_measures, syscall_measures_end);
    serialize();
}

void speculation_process_switch(void)
{
    if (switch_ibpb)
        wrmsr(IA32_PRED_CMD, PRED_CMD_IBPB);
    if (switch_rsb_overwrite)
        rsb_overwrite();
}

/* VERW comes before the STI, whose one-instruction shadow must cover the
   HLT (cpu.h, wait_for_interrupt). */
void speculation_wait_for_interrupt(void)
{
    if (measures & MEASURE_VERW)
        __asm__ volatile("verw %0\n\t"
                         "sti\n\t"
                         "hlt\n\t"
                         "cli"
                         :
                         : "m"(verw_selector)
                         : "cc", "memory");
    else
        wait_for_interrupt();
}
