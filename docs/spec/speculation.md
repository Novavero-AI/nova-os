# Speculative execution (x86-64)

Which speculative-execution mitigations the kernel carries on its entries
from user mode (the system call of [syscalls.md S3](syscalls.md#s3-entry-and-exit),
interrupts and exceptions), on its exits to user mode, and when it switches
from one process to another; how it detects at boot which apply; and what
each costs.

Specifications are cited by the versions in [README.md](README.md): "SDM"
is the Intel manual's Volume 3, "APM" the AMD manual's Volume 2 and "APM
Vol. 3" its Volume 3. The vendors' guidance is pinned under
[Speculative execution](README.md#speculative-execution) and cited by
these names:

| Name | Document |
|---|---|
| Intel HFB | Hardware Features and Behavior Related to Speculative Execution, by section |
| Intel CPUID | CPUID Enumeration and Architectural MSRs |
| Intel retpoline | Retpoline: A Branch Target Injection Mitigation |
| Intel BHI | Branch History Injection and Intra-mode Branch Target Injection, the technical paper |
| Intel RSBU | Return Stack Buffer Underflow |
| Intel PBRSB | Post-barrier Return Stack Buffer Predictions |
| Intel RDCL | Rogue Data Cache Load |
| Intel L1TF | L1 Terminal Fault, the technical paper |
| Intel MDS | Microarchitectural Data Sampling, the technical paper |
| Intel TAA | Intel TSX Asynchronous Abort |
| Intel MMIO | Processor MMIO Stale Data Vulnerabilities, the technical paper |
| Intel GDS | Gather Data Sampling, the technical paper |
| Intel RFDS | Register File Data Sampling |
| Intel SWAPGS | Speculative Behavior of SWAPGS and Segment Registers, the technical paper |
| Intel ITS | Indirect Target Selection |
| Intel IBPDU | Indirect Branch Predictor Delayed Updates |
| Intel table | Affected Processors: Guidance for Security Issues on Intel Processors |
| AMD STMS | Software Techniques for Managing Speculation on AMD Processors, by mitigation |
| AMD BTC | Technical Guidance for Mitigating Branch Type Confusion, by section |
| AMD SRSO | Technical Update Regarding Speculative Return Stack Overflow, by section |
| AMD SRSO cloud | Speculative Return Stack Overflow Mitigation Options and Considerations for Public Cloud Users |
| AMD TSA | Technical Guidance for Mitigating Transient Scheduler Attacks, by section |

Intel's web pages have no section numbers and are cited by heading where
that helps.

## M1. Threat model

**Decided.**

Code at CPL 3 is untrusted by the kernel and by every other process. A
process holds only the handles it was given (DESIGN.md principle 7), so
each process is a security domain of its own, as is the kernel. The
measures below keep a process from reading kernel memory, or another
process's data, through speculative execution.

The kernel runs no code a process supplies: it has no interpreter or
compiler for user programs. Intel's guidance for that case, which it
gives for Linux's eBPF (Intel BHI, "Linux* Kernel: Disable Unprivileged
eBPF"; Intel ITS, "Operating Systems"), does not apply. nova-os runs no
virtual machines, so measures that protect a hypervisor from its guests
do not apply either: the L1D flush on VM entry (Intel L1TF), the
sequence after VM exit (Intel PBRSB), the guest/host case of Intel ITS,
and `BpSpecReduce` (AMD SRSO 3).

- **Why:** what a process can make the kernel run, and what shares a core
  with it, decide which attacks exist; fixing both keeps the measures to
  the entry, the exit and the process switch.
- **Cost:** an in-kernel runtime for user code, if one is ever wanted,
  brings back intra-mode branch target injection (Intel BHI,
  "Intra-mode BTI") and needs measures of its own.

## M2. Detection

**Decided.** At boot, before any user code runs, the kernel reads the
following on each CPU and derives from them the measures in M3.

| Where | Bit | Name | Meaning |
|---|---|---|---|
| CPUID.01H:ECX | 31 | | Running under a hypervisor (Intel BHI, footnote 9; APM Vol. 3 E.3.2) |
| CPUID.01H:ECX | 17 | PCID | CR4.PCIDE may be set (SDM 5.1.4) |
| CPUID.(EAX=07H,ECX=0):EBX | 7 | SMEP | CR4.SMEP may be set (SDM 5.1.4) |
| CPUID.(EAX=07H,ECX=0):EBX | 4, 11 | HLE, RTM | Intel TSX is present (Intel TAA, footnote 1) |
| CPUID.(EAX=07H,ECX=0):EDX | 10 | MD_CLEAR | VERW with a memory operand overwrites the buffers MDS samples (Intel MDS, "CPUID Enumeration") |
| CPUID.(EAX=07H,ECX=0):EDX | 26 | IBRS, IBPB | IA32_SPEC_CTRL (48H) bit 0 and IA32_PRED_CMD (49H) bit 0 exist (Intel CPUID) |
| CPUID.(EAX=07H,ECX=0):EDX | 29 | | IA32_ARCH_CAPABILITIES (10AH) exists |
| CPUID.(EAX=07H,ECX=2):EDX | 4 | BHI_CTRL | IA32_SPEC_CTRL bit 10, BHI_DIS_S, exists |
| IA32_ARCH_CAPABILITIES | 0 | RDCL_NO | Not susceptible to rogue data cache load (Intel CPUID) or to L1TF (Intel L1TF, "Affected Hardware") |
| IA32_ARCH_CAPABILITIES | 1 | IBRS_ALL | Enhanced IBRS |
| IA32_ARCH_CAPABILITIES | 5 | MDS_NO | Not susceptible to MDS |
| IA32_ARCH_CAPABILITIES | 7, 8 | TSX_CTRL, TAA_NO | IA32_TSX_CTRL (122H) exists; not susceptible to TAA |
| IA32_ARCH_CAPABILITIES | 13, 14, 15 | SBDR_SSDP_NO, FBSDP_NO, PSDP_NO | Not affected by the named MMIO stale-data behaviours |
| IA32_ARCH_CAPABILITIES | 17 | FB_CLEAR | VERW overwrites the fill buffers |
| IA32_ARCH_CAPABILITIES | 20 | BHI_NO | Branch history from CPL 3 does not select targets at CPL 0 to 2 |
| IA32_ARCH_CAPABILITIES | 28 | RFDS_CLEAR | Affected by RFDS, and VERW clears it |
| AMD Fn8000_0008_EBX | 12, 14, 15 | IBPB, IBRS, STIBP | PRED_CMD bit 0, SPEC_CTRL bits 0 and 1 exist (APM 3.2.9) |
| AMD Fn8000_0008_EBX | 16 | IbrsAlwaysOn | IBRS should be set once at boot |
| AMD Fn8000_0008_EBX | 29 | BTC_NO | Not affected by branch type confusion |
| AMD Fn8000_0008_EBX | 30 | IBPB_RET | IBPB also clears the return address predictor |
| AMD Fn8000_0021_EAX | 2 | LFenceAlwaysSerializing | LFENCE is always dispatch serializing |
| AMD Fn8000_0021_EAX | 5 | VERW_CLEAR | VERW with a memory operand helps mitigate TSA |
| AMD Fn8000_0021_EAX | 8 | AutomaticIBRS | EFER bit 21, AIBRSE, exists (APM 3.1.7) |
| AMD Fn8000_0021_EAX | 24 | ERAPS | Enhanced return address predictor security |
| AMD Fn8000_0021_EAX | 28 | IBPB_BRTYPE | IBPB clears all branch type predictions |
| AMD Fn8000_0021_EAX | 29, 30 | SRSO_NO, SRSO_USER_KERNEL_NO | Not affected by SRSO; not affected across user and kernel |
| AMD Fn8000_0021_ECX | 1, 2 | TSA_SQ_NO, TSA_L1_NO | Not vulnerable to TSA-SQ; to TSA-L1 |

The Intel bits are from Intel CPUID unless the row says otherwise; the AMD
bits from APM Vol. 3 E.4.7 and E.4.19.

1. **The enumeration is read once.** The kernel loads no microcode, and
   Intel asks software to evaluate the enumeration again only after
   loading an update (Intel CPUID). Measures that need microcode
   (MD_CLEAR, FB_CLEAR, RFDS_CLEAR, IBRS and IBPB on older processors,
   the GDS mitigation, the fixes of Intel ITS and Intel IBPDU, AMD TSA's
   VERW) exist only if the firmware, or the hypervisor, loaded it.
2. **Family and model only where a vendor says so:** "Zen" and "Zen2",
   whose IBPB flushes branch types without setting IBPB_BRTYPE (AMD SRSO
   5); Family 19h, which is not affected by BTC without setting BTC_NO
   (AMD BTC 8); and processors before Family 19h and Family 1Ah models
   00h to 4Fh and 60h to 7Fh, which are not affected by TSA without
   setting TSA_L1_NO or TSA_SQ_NO (AMD TSA 6). Under a hypervisor no
   such inference may remove a measure: AMD asks hypervisors to
   synthesize these bits (AMD BTC 8, AMD SRSO 5, AMD TSA 6), and the
   family and model a guest sees need not be the host's (Intel ITS,
   "Enumeration").
3. **Vendors.** Rules on Intel bits apply where CPUID.0 reports
   `GenuineIntel`, rules on AMD bits where it reports `AuthenticAMD`. A
   processor of another vendor is treated as an Intel processor, so every
   class no enumeration bit rules out is taken to apply.
4. **Report.** At boot the kernel writes to the debug log the measures in
   force and each class it cannot mitigate on this processor, for
   instance branch target injection where the microcode provides no IBRS.

- **Why:** the enumeration bits are the interface both vendors document;
  model tables are used only where a vendor directs it.
- **Cost:** a model list to keep for each inference above, and the debug
  log is the only report: no call returns the set to a process.
- **Not taken:** refusing to run user code where a class goes
  unmitigated. Old firmware would then stop nova-os outright for a class
  the user may judge acceptable, where the log lets the user decide.

## M3. The measures by path

**Decided.** The table lists what each class adds; the sections after it
give the conditions, sources and costs.

| Class | Applies when | Once at boot | Entry from user mode | Exit to user mode | Process switch |
|---|---|---|---|---|---|
| Bounds check bypass (M4) | Always | AMD: LFENCE made dispatch serializing | Call number masked | | |
| Conditional SWAPGS (M5) | Always | | Interrupts and exceptions: LFENCE after the SWAPGS test | | |
| Branch target injection (M6) | Always | Enhanced IBRS, Automatic IBRS or IBRS always-on | Otherwise IBRS set | Otherwise IBRS cleared | IBPB |
| Return predictions (M7) | SMEP absent; AMD without IBPB_RET or ERAPS | | Without SMEP: 32-CALL overwrite | | AMD: 32-CALL overwrite after IBPB |
| Branch history injection (M8) | Intel without BHI_NO | BHI_DIS_S | Without BHI_CTRL: BHB-clearing sequence | | |
| Branch type confusion (M9) | AMD without BTC_NO | | IBPB | | |
| Speculative return stack overflow (M10) | AMD without SRSO_NO or SRSO_USER_KERNEL_NO | | IBPB | | IBPB (M6) |
| Rogue data cache load (M11) | Intel without RDCL_NO | PCIDs | Kernel page tables | User page tables | Both tables |
| L1 terminal fault (M12) | Intel without RDCL_NO; applied on all | Not-present entries zero, page 0 unused | | | |
| Data sampling (M13) | MDS, TAA, MMIO, RFDS, TSA as M13 lists | TSX disabled where TAA applies | | VERW | |
| Speculative store bypass (M14) | | SSBD left clear | | | |
| Gather data sampling (M15) | Intel without GDS_NO | Microcode mitigation left on | | | |

Entry from user mode (SYSCALL, and an interrupt or exception at CPL 3)
adds, in this order, after S3's entry step 2 (`SWAPGS`) and before its
step 5 (`STI`):

1. The kernel's page tables (M11), before any memory access outside the
   entry region.
2. S3's steps 3 and 4.
3. IA32_SPEC_CTRL with IBRS set (M6), where neither IBRS is set once at
   boot nor IBPB runs at step 4.
4. IBPB (M9, M10).
5. The 32-CALL overwrite (M7).
6. The BHB-clearing sequence (M8).

No RET and no indirect branch runs before steps 3 and 4, other than the
balanced CALLs and RETs of steps 5 and 6. The call number is masked
before the call table is read (M4).

Exit to user mode adds, within S3's exit:

1. After step 1 (`CLI`): IA32_SPEC_CTRL with IBRS clear, where the entry
   set it.
2. Within steps 2 to 5: the user's page tables (M11), after the last
   memory access outside the entry region.
3. After step 6 (`SWAPGS`): VERW (M13), the last instruction before
   `SYSRETQ` or `IRETQ`.

NMI, double fault and machine check can arrive at CPL 3 or inside S3's
windows (S3), so with either set of page tables loaded and either IBRS
value. Where M11 or M6's rule 4 applies they save CR3 and
IA32_SPEC_CTRL, load the kernel's values, and restore both on return.
Each stops the kernel today, so none returns and none restores.

On the entry and the exit, a measure only some processors need is
assembled in place behind an unconditional jump over it, which the kernel
replaces with a NOP at boot where the measure applies, before any entry
from user mode. A branch on
a flag would decide the same, but it can be mispredicted past the
measure, as the conditional `SWAPGS` of M5 is.

A process switch, when a CPU changes to a thread of a different process
from the last user thread it ran, adds before loading the new process's
CR3: IBPB (M6), then, on AMD processors with neither IBPB_RET nor ERAPS,
the 32-CALL overwrite (M7).

## M4. Bounds check bypass

A conditional branch that checks a caller's value is mispredicted, and
code past the check runs speculatively with the out-of-range value
(Intel HFB 3.2.1; AMD STMS, variant 1).

**Applies to:** every processor. No enumeration rules it out, and AMD's
"mitigation recommendation is software only solutions" (AMD STMS,
variant 1).

**Decided.**

1. **Indices are masked.** An index the kernel derives from a call's
   arguments is masked after its range check and before the load it
   selects: the call number before the call table is read (S3, entry
   step 6), a handle value before the handle table is read, and every
   such index after. The mask is all ones when the index is in range and
   zero otherwise, computed with `CMP` and `SBB` and applied with `AND`,
   so a mispredicted check reads entry 0, inside the array, never memory
   beyond it. For a handle the mask applies to the index field
   ([objects.md O3](objects.md#o3-handles)), and for a section the page
   index an offset gives. Intel lists `SBB` among the instructions for
   bounds clipping (Intel HFB 3.2.4.1).
2. **User pointers are selected.** The copy routines of S6, after
   checking that a range lies in [`USER_BASE`, `USER_TOP`), select the
   pointer with `CMOV`, which yields 0 when the check failed, so a
   mispredicted check reads from address 0, which is never mapped (S6),
   not from the kernel. Intel: "CMOVcc is sufficient to mitigate bounds
   check bypass vulnerabilities even in cases where bounds clipping is not
   applicable" (Intel HFB 3.2.4.2; AMD STMS V1-2).
3. **LFENCE where neither fits**, and after the conditional `SWAPGS`
   (M5). On AMD processors that do not enumerate LfenceAlwaysSerializing,
   the kernel sets MSR C001_1029[1] at boot so that LFENCE is dispatch
   serializing (AMD STMS G-2; APM Vol. 3 E.4.19).

`tools/check-masks.c` checks the mask and the select against the
comparisons they stand in for.

- **Why:** masking and `CMOV` cost an instruction or two and do not stall
  the pipeline; Intel recommends them first and LFENCE "as a last
  resort" (Intel HFB 3.2.4, 3.2.4.3).
- **Cost:** every array the kernel indexes with a caller's value must be
  found and masked by hand; no compiler pass does it (M17). No cycle
  figure is given for masking; LFENCE "can have a significantly higher
  performance impact" (Intel HFB 3.2.4.3).
- **Not taken:** LFENCE after every check, the costlier barrier; a
  compiler that inserts barriers (Intel HFB 3.2.4.5), which TinyCC is
  not.

## M5. Conditional SWAPGS

An entry that decides whether to execute `SWAPGS` mispredicts the
decision and runs GS-relative loads with the other GS base, the user's
in the kernel (Intel SWAPGS, "Speculatively Missed SWAPGS" and
"Speculatively Executed Extra SWAPGS").

**Applies to:** every processor; the cost is one LFENCE, so the kernel
does not consult Intel's list.

**Decided.** The interrupt and exception entries execute `SWAPGS` only
when the saved CS has RPL 3 (S3). On that path an LFENCE follows the
`SWAPGS`; on the other an LFENCE comes before the first GS-relative
access. Where the kernel's page tables are loaded right after `SWAPGS`
(M11), that CR3 write is serializing and takes the LFENCE's place (Intel
SWAPGS, "Mitigation"; Intel HFB 6.0). The `SYSCALL` entry executes
`SWAPGS` unconditionally, and NMI, double fault and machine check never
use GS (S3), so neither needs a fence.

- **Why:** both mispredictions Intel describes exist in S3's interrupt
  entry, and the fence is the remedy it gives.
- **Cost:** one LFENCE per interrupt or exception.
- **Not taken:** an entry stub per privilege level, so that no branch
  decides the `SWAPGS`: the IDT gate cannot tell where the event came
  from.

## M6. Branch target injection

User code trains the indirect branch predictor so that an indirect `JMP`
or `CALL` in the kernel speculatively runs code at a target the attacker
chose (Intel HFB 3.1; AMD STMS, variant 2).

**Applies to:** every processor that predicts indirect branches. The
controls are IBRS and IBPB (Intel CPUID.(EAX=07H,ECX=0):EDX[26]; AMD
Fn8000_0008_EBX[14] and [12]), enhanced IBRS (IA32_ARCH_CAPABILITIES[1]),
Automatic IBRS (AMD Fn8000_0021_EAX[8]) and IBRS always-on (AMD
Fn8000_0008_EBX[16]).

**Decided:** user to kernel, the first rule that applies.

1. **Enhanced IBRS:** IA32_SPEC_CTRL.IBRS set once at boot and never
   cleared. "On processors with enhanced IBRS support, Intel recommends
   that IBRS be set to 1 and left set" (Intel HFB 3.1.2.2.3).
2. **Automatic IBRS:** EFER.AIBRSE set once at boot, which gives code at
   CPL 0 IBRS protection "even when SPEC_CTRL[IBRS] is not set" (APM
   3.1.7).
3. **IBRS always-on:** SPEC_CTRL.IBRS set once at boot, as
   Fn8000_0008_EBX[16] recommends (APM 3.2.9).
4. **IBRS:** the entry writes IA32_SPEC_CTRL with IBRS set, even if it
   already is, and the exit writes it with IBRS clear. Both vendors
   require the write after each transition (Intel HFB 3.1.2.2.2; APM
   3.2.9), and Intel advises that on these processors "IBRS should not
   be left set during application execution" (Intel HFB 3.1.2.2.3). The
   two values written differ only in IBRS.
   Where IBPB runs on every entry (M9, M10) the writes are skipped: the
   barrier already keeps every prediction made before it from
   influencing later indirect branches (APM 3.2.9).
5. **None of these:** the class is unmitigated and logged (M2).

**Decided:** process to process. IBPB when a CPU switches to a thread of
a different process from the last user thread it ran, and not when it
returns to the same process. IBRS does not separate two programs in the
same predictor mode; IBPB does (Intel HFB 3.1.2.2.2, 3.1.2.4), and AMD
names "switching from one user context to another" as its use (APM
3.2.9). The same IBPB serves SRSO between processes (M10) and the fast
store forwarding predictor (Intel HFB 4.5.4).

STIBP is not set: one logical processor runs per core (M16), and IBRS
and enhanced IBRS isolate sibling threads where they are set (Intel HFB
3.1.2.3; APM 3.2.9).

- **Why:** where the processor isolates predictor modes in hardware, one
  write at boot does it; per-transition writes only where the processor
  offers nothing better.
- **Cost:** nothing per call under rules 1 to 3. Under rule 4, two
  WRMSRs per entry and exit pair, each of which waits for every older
  instruction and holds every younger one (Intel CPUID, "IA32_SPEC_CTRL
  MSR"); the guidance gives no cycle figure. IBPB per process switch:
  about 10,000 cycles on "Zen" and 2,500 on "Zen 2" (AMD BTC 6.1.2), with
  no figure for Intel processors in the guidance read.
- **Not taken:** retpoline. TinyCC compiles a C indirect call to
  `call *%r11` and cannot emit a retpoline instead (M17), and retpoline
  "requires that all code in a program (or OS kernel) is compiled with a
  retpoline-enabled compiler" (Intel retpoline, "Recompilation"). It may
  also not be fully effective on Goldmont Plus and Tremont (Intel
  retpoline, "Processor Models") and is incompatible with CET shadow
  stacks (Intel BHI, "Retpoline"). AMD names retpoline its preferred
  variant 2 solution (AMD STMS, conclusion); it is out of reach without
  a TinyCC change. Also not taken: SBPB at process switches on AMD
  processors where SRSO is ruled out (AMD SRSO 4), a second barrier for
  one vendor's newest parts.

## M7. Return predictions

A `RET` is predicted from the return stack buffer (RSB, Intel) or return
address predictor (AMD), which `CALL` instructions fill. User code can
steer a kernel `RET` through entries it left there before entering
(Intel HFB 3.1.2.2.2; APM 3.2.9); on some processors an empty RSB makes
`RET` use other predictors (Intel retpoline, "Empty RSB Mitigation on
Skylake-generation"; Intel RSBU, which Intel also calls "Retbleed"); and
entries a process leaves can steer the next process's returns.

**Applies to:** leftover entries, every processor. The empty-RSB case:
Intel processors that enumerate RSBA (IA32_ARCH_CAPABILITIES[2], Intel
CPUID) or are listed in Intel retpoline, Table 5; the measures below do
not depend on which.

**Decided.**

1. **Entry.** No overwrite where SMEP is enabled (S6). Intel needs none
   after a transition from user mode with SMEP (Intel HFB 3.1.2.2.2,
   3.1.2.2.3), and AMD none when SMEP is enabled, "the kernel and user
   virtual address spaces do not overlap, and at least one unmapped 4K
   page is separating them" (APM 3.2.9): user mappings end one page below
   the top of the lower half (S5), and the kernel is in the upper half.
   Without SMEP, every entry from user mode executes 32 `CALL`s with a
   non-zero displacement before its first `RET` (Intel HFB 3.1.2.2.2,
   footnote 5; AMD STMS V2-3). On a processor with enhanced IBRS but no
   SMEP that sequence "may not suffice" and SMEP is the only remedy Intel
   names (Intel HFB 3.1.2.2.3), so the kernel logs the class as
   unmitigated.
2. **Empty RSB.** IBRS on entry (M6, rule 4) covers it on processors
   without enhanced IBRS: enabling IBRS, enhanced IBRS included, mitigates
   the attack (Intel RSBU, "Guidance for RSBU"). RRSBA_DIS_S is not set:
   it serves kernels that use retpoline (Intel HFB 3.1.4.1; Intel BHI,
   "Retpoline"), and this one does not.
3. **Process switch.** On Intel the IBPB of M6 suffices: "RSB entries
   created before an IBPB command cannot control the predicted targets
   of returns executed after the command" (Intel HFB 3.1.2.4, footnote
   6). On AMD the IBPB clears the return address predictor only where
   IBPB_RET is set; elsewhere, unless ERAPS is set, the switch executes
   32 `CALL`s after the IBPB. With ERAPS the switch's CR3 load clears it
   (APM 3.2.9).
4. **Post-barrier RSB predictions.** On processors with enhanced IBRS
   that do not enumerate PBRSB_NO (IA32_ARCH_CAPABILITIES[24]), the first
   unbalanced `RET` after an IBPB may be predicted from the last `CALL`
   before it (Intel PBRSB).
   The kernel issues IBPB after its own `CALL`s, the case Intel says
   needs nothing more (Intel PBRSB, "Indirect Branch Prediction Barrier
   (IBPB)").

- **Why:** the address-space layout and SMEP already give what the
  overwrite would; the overwrite is kept for the cases they do not cover.
- **Cost:** none on entry where SMEP is enabled. Otherwise 32 `CALL`s and
  a stack adjustment per entry, and per process switch on AMD without
  IBPB_RET or ERAPS; the guidance gives no cycle figure.
- **Not taken:** the overwrite on every entry regardless of SMEP; RSB
  stuffing against underflow, which serves kernels that use retpoline
  (Intel retpoline, "Empty RSB Mitigation on Skylake-generation").

## M8. Branch history injection

User code fills the branch history before entering the kernel, so that a
kernel indirect branch selects a predictor entry made in kernel mode
whose target is a gadget. Enhanced IBRS separates predictor entries by
mode but, on many processors, not the branch history (Intel BHI, "Branch
History and the Branch History Buffer (BHB)").

**Applies to:** Intel processors that do not enumerate BHI_NO
(IA32_ARCH_CAPABILITIES[20]). No AMD document read covers this class; the
rules apply to Intel processors only.

**Decided**, by Intel's rules for operating systems (Intel BHI,
"Guidelines for Applying Additional Hardening Options"):

1. BHI_NO: nothing.
2. BHI_CTRL (CPUID.(EAX=07H,ECX=2):EDX[4]): IA32_SPEC_CTRL.BHI_DIS_S set
   once at boot, with IBRS.
3. Enhanced IBRS without BHI_CTRL: Intel's short BHB-clearing sequence
   (Intel BHI, "Software BHB-clearing Sequence", Listing 1) on every
   entry from user mode, before the first indirect branch.
4. No enhanced IBRS: nothing on bare metal. Under a hypervisor that
   enumerates IBRS but not enhanced IBRS, the short sequence on every
   entry, as Intel directs for a guest that relies on IBRS, since the
   host may have enhanced IBRS.

Two related Intel issues need microcode and no kernel change here.
Indirect Target Selection: the microcode makes IBPB behave as documented,
the issue leaves enhanced IBRS's separation of user and kernel targets in
place, and its remaining operating-system case is intra-mode injection
through in-kernel runtimes (Intel ITS, "Mitigation" and "Operating
Systems"), which M1 excludes. Indirect branch predictor delayed updates
(Branch Privilege Injection): the microcode makes the enhanced-IBRS
mitigation "effective as expected" (Intel IBPDU).

- **Why:** Intel orders the hardware control first and the sequence
  where it is absent (Intel HFB 3.1.3.2).
- **Cost:** BHI_DIS_S, nothing per call; Intel says future processors
  "may lower the performance overhead" without giving a figure. The short
  sequence: nested calls and loops of taken jumps, then LFENCE, per entry;
  no figure.
- **Not taken:** Intel's long and TSX sequences, for kernels that leave
  BHI_DIS_S clear where it exists; IPRED_DIS_S, aimed at intra-mode
  injection from in-kernel runtimes; the virtual MSRs a guest uses to tell
  its hypervisor which sequence it relies on (Intel BHI, "Software
  Mitigations in Migration Pools"), since hypervisors "are expected to
  default to assuming that guests are using the short software sequence".

## M9. Branch type confusion

The branch predictor predicts a branch of the wrong type at an
instruction, for instance an indirect jump where the bytes hold a `RET`
or no branch at all, and speculation proceeds at the predicted target
(AMD BTC 1, 2). The `RET` case is BTC-RET, CVE-2022-29900 (AMD BTC 3).

**Applies to:** AMD Family 15h and Family 17h ("Zen", "Zen+", "Zen 2"),
not Family 19h (AMD BTC, Appendix). The kernel takes an AMD processor to
be affected unless it enumerates BTC_NO (Fn8000_0008_EBX[29]) or is
bare-metal Family 19h (M2); later families without BTC_NO count as
affected, since AMD BTC predates them.

**Decided:** IBPB on every entry from user mode, before the first `RET`
or indirect branch. IBPB "effectively mitigates all forms of BTC for
scenarios like user-to-supervisor or VM-to-hypervisor attacks" (AMD BTC
6.1.2). Without IBPB the class is unmitigated and logged.

- **Why:** it is the one mitigation for every case of BTC (AMD BTC, Table
  4), and the other mitigation for BTC-RET needs compiler support TinyCC
  lacks (below).
- **Cost:** about 10,000 cycles per entry on "Zen" and 2,500 on "Zen 2"
  (AMD BTC 6.1.2), the costliest measure in this document.
- **Not taken:** Jmp2Ret, in which every `RET` reachable in privileged
  code becomes a jump to one return thunk that the entry retrains (AMD
  BTC 6.1.1). TinyCC ends every function with `ret` and cannot jump to a
  thunk instead (M17). DE_CFG2[1] (AMD BTC 6.2.1) covers only BTC-NOBR on
  "Zen 2", which the IBPB covers as well.

## M10. Speculative return stack overflow

Code at CPL 3 influences "the branch type used to predict branches in
CPL0" (AMD SRSO 3), CVE-2023-20569 (AMD SRSO cloud).

**Applies to:** AMD processors that set neither SRSO_NO
(Fn8000_0021_EAX[29]) nor SRSO_USER_KERNEL_NO ([30]). Both are 0 on every
AMD processor through "Zen4" (AMD SRSO 5). The researchers' proof of
concept ran on "Zen", "Zen2" and "Zen4", and "Zen3" is "believed to be
vulnerable in theory" (AMD SRSO cloud).

**Decided.**

1. **Entry:** IBPB on every entry from user mode, shared with M9 where
   both apply. Where IBPB clears branch types it "may be used when
   transitioning between address spaces and/or privilege domains to
   mitigate SRSO" (AMD SRSO 4). That property is IBPB_BRTYPE
   (Fn8000_0021_EAX[28]), or holds on bare-metal "Zen" and "Zen2" without
   the bit (AMD SRSO 5). On "Zen3" and "Zen4" it holds from a microcode
   revision listed in AMD-SB-7005 (AMD SRSO 5), which could not be
   obtained ([README.md](README.md#not-pinned)); on those processors
   without the bit the kernel issues the IBPB and logs SRSO as
   unconfirmed.
2. **Process switch:** the IBPB of M6. "Mitigation of SRSO across
   user/user or VM/VM boundaries requires the use of IBPB unless SRSO_NO
   is 1" (AMD SRSO 3).

- **Why:** IBPB that clears branch types is the one SRSO mitigation whose
  definition was obtained, and it needs nothing from the compiler.
- **Cost:** an IBPB per entry; AMD BTC's figures for "Zen" and "Zen 2"
  apply there, and the sources read give none for "Zen3" or "Zen4". AMD
  judges the risk "low in typical production environments" and finds it
  "reasonable to disable SafeRET" for cloud users (AMD SRSO cloud); M1
  treats every process as untrusted.
- **Not taken:** Safe-RET, which AMD SRSO cloud names but none of the
  documents obtained defines; leaving the class unmitigated on AMD's
  assessment of the risk; `BpSpecReduce`, which serves guest and host only
  (AMD SRSO 3).

## M11. Rogue data cache load and page-table isolation

User code loads from a supervisor-only kernel address; the load runs
speculatively and on some implementations passes the data to dependent
instructions before the fault (Intel RDCL, "Overview").

**Applies to:** Intel processors that do not enumerate RDCL_NO
(IA32_ARCH_CAPABILITIES[0]), including those without the MSR. Intel still
lists affected processors among those it supports: Haswell, Broadwell and
Skylake server processors and, at some steppings, Skylake, Kaby Lake,
Amber Lake, Whiskey Lake and Coffee Lake client processors (Intel table,
2018-2021, "Rogue Data Cache Load (Meltdown)"). Not AMD: "No AMD
processor has been designed with this behavior", and AMD asks software to
check the vendor to avoid this mitigation (AMD STMS, variant 3). Enhanced
IBRS does not enter the decision; none of the guidance read relates it to
this class.

**Decided:** page-table isolation where the class applies. Each process
has two top-level tables. The kernel's maps the user half and the kernel.
The user's maps the user half and the entry region: what the processor
and the entry code touch before the entry's CR3 write and after the
exit's, namely the entry and exit code, the GDT, IDT and TSS, the stacks
that RSP0 and the IST entries name, the per-CPU entry block, and VERW's
operand (M13). The entry region is supervisor-only. The entry loads the
kernel's table after `SWAPGS` and before any other memory access; the
exit loads the user's after its last access outside the entry region.
Where PCID is enumerated the kernel sets CR4.PCIDE and gives the two
tables of a process different PCIDs, so that loading one need not
invalidate the other's cached translations (SDM 5.10.1, 5.10.4.1). Where
the class does not apply, each process has one table.

This follows Intel's remedy: "the operating system software may ensure
that privileged pages are not mapped when executing user code", with two
CR3 values per process and PCID to reduce the cost (Intel RDCL,
"Mitigation").

- **Why:** the kernel maps all of physical memory in its direct map
  ([boot-protocol.md](boot-protocol.md#paging)), so on an affected
  processor any process could read all of memory.
- **Cost:** two CR3 writes per entry and exit pair and a second top-level
  table per process. Without PCID each write invalidates every TLB entry
  that is not for a global page (SDM 5.10.4.1). Intel gives no figure.
- **Not taken:** refusing user code on affected processors, which Intel
  still supports; shrinking the direct map, which reduces what is
  exposed but leaves kernel data mapped.

## M12. L1 terminal fault

A load through a paging entry that is not present, or has a reserved bit
set, speculatively reads the L1D line at the physical address the entry's
frame bits name (Intel L1TF, "How L1TF Works").

**Applies to:** Intel processors that do not enumerate RDCL_NO;
"Processors that have the RDCL_NO bit set to one (1) ... are not
susceptible" (Intel L1TF, "Affected Hardware"). No AMD document read
covers it. The measure costs nothing and is applied on every processor.

**Decided.** Every paging entry the kernel marks not present is all
zero, at every level, so its frame is physical page 0 and its PS bit is
clear. Physical page 0 holds no data of the kernel or of any process: the
page allocator never returns it (`kernel/mm/page.c`). Intel: an OS "may
use an all-zero paging structure entry to represent linear addresses with
no physical mapping ... while ensuring that the 4 KB page frame starting
at physical address 0 contains no secrets" (Intel L1TF, "Mitigating
Operating Systems (OS)").

The L1D flush and the measures for hyperthreads protect a hypervisor
from guests (Intel L1TF, "Virtual Machine Monitors (VMMs)" and "Appendix
Intel Hyper-Threading Technology"), which nova-os does not have (M1).

- **Why:** nova-os writes no metadata into not-present entries, so the
  all-zero form costs nothing.
- **Cost:** a later design that keeps metadata in not-present entries
  (paging to disk) must point them at addresses above all cacheable
  memory instead (Intel L1TF, "Mitigation by Referring to Invalid
  Memory").
- **Not taken:** setting the high address bits in every not-present
  entry, which serves entries that carry metadata.

## M13. Data sampling and VERW

These classes let user code sample data the kernel or another process
left in buffers the processor shares between them:

- **MDS:** store buffers, fill buffers and load ports, read by faulting
  or assisting loads (Intel MDS, "How Microarchitectural Data Sampling
  (MDS) Works").
- **TAA:** the same buffers, read inside an aborting TSX transaction
  (Intel TAA, "Overview").
- **MMIO stale data:** uncore buffers reached through memory-mapped I/O
  (Intel MMIO). nova-os maps device memory into user-mode drivers
  ([ipc.md I6](ipc.md#i6-device-memory-and-io-ports)), so this class
  reaches it.
- **RFDS:** stale values in the register files of Intel Atom cores
  (Intel RFDS, "Introduction").
- **TSA:** load timing after a "false completion" in the L1 data cache or
  store queue of AMD Family 19h (AMD TSA 2, 3).

**Applies to**, where the processor provides the VERW behaviour named:

| Class | Affected when | VERW clears it when |
|---|---|---|
| MDS | MDS_NO = 0 | MD_CLEAR (Intel MDS, "Processor Support for Buffer Overwriting (MD_CLEAR)") |
| TAA | Intel TSX present, TAA_NO = 0, and TSX not disabled (Intel TAA, "Mitigation") | MD_CLEAR (Intel TAA, "OS Developers") |
| MMIO stale data | SBDR_SSDP_NO, FBSDP_NO or PSDP_NO = 0 | FB_CLEAR, or MD_CLEAR on processors affected by MDS (Intel MMIO, "Definitions") |
| RFDS | RFDS_CLEAR = 1 | RFDS_CLEAR (Intel RFDS, "Enumeration") |
| TSA | AMD processors that do not set both TSA_L1_NO and TSA_SQ_NO, other than those M2 exempts (AMD TSA 6) | VERW_CLEAR, or on bare metal microcode at least the revision AMD TSA's appendix lists (AMD TSA 6) |

**Decided.**

1. **VERW on every exit to user mode** where any row applies: VERW with a
   memory operand naming the kernel's data selector, 0x10, as the last
   instruction before `SYSRETQ` or `IRETQ`. The memory form is required
   (Intel MDS, "VERW Buffer Overwriting Details"; AMD TSA 5), a writable
   data segment gives the lowest latency (Intel MDS), and at CPL 0 (AMD
   TSA 5: VERW elsewhere "will not clear"). Coming last, it runs after
   every general-purpose register holds the user's value or zero (S4), as
   RFDS needs: register values present at the VERW "may be inferred
   later" (Intel RFDS, "Mitigation"). The privilege change of `SYSRETQ`
   or `IRETQ` is the speculation barrier VERW needs after it (Intel MDS,
   "VERW Fall-Through Speculation"). VERW writes ZF, but neither
   instruction returns the current flags to user mode: `SYSRETQ` loads
   RFLAGS from R11 and `IRETQ` from the frame.
2. **VERW before `HLT`** in the idle loop under the same condition (AMD
   TSA 5; Intel MDS, "OS").
3. **TSX disabled** where TSX_CTRL = 1 and TAA_NO = 0: the kernel sets
   IA32_TSX_CTRL bits 0 (RTM_DISABLE) and 1 (TSX_CPUID_CLEAR) at boot
   (Intel CPUID, "IA32_TSX_CTRL MSR"), so TAA no longer calls for VERW on
   processors with MDS_NO = 1 (Intel TAA, "OS Developers").

- **Why:** one VERW on the way out clears what every class here samples,
  and both vendors place it at the transition to less trusted code.
- **Cost:** one VERW per exit; Intel expects "minimal performance impact
  ... for the majority of workloads" for RFDS (Intel RFDS) and AMD says
  the TSA mitigation "may affect system performance" (AMD TSA 5); neither
  gives a cycle figure. TSX is unavailable to programs on processors that
  need item 3.
- **Not taken:** Intel's model-specific software sequences for
  processors without MD_CLEAR (Intel MDS, "Software Sequences to
  Overwrite Buffers"); the L1D flush, which AMD says may not be effective
  for TSA-L1 (AMD TSA 5) and Intel says does not clear RFDS (Intel RFDS,
  footnote 3); VERW only on exits to processes that map device memory
  (Intel MMIO), a refinement for later.

## M14. Speculative store bypass

A load runs before an older store to the same address has its address
resolved and reads the stale value (Intel HFB 4.2; APM 3.2.9).

**Applies to:** Intel processors without SSB_NO
(IA32_ARCH_CAPABILITIES[4], Intel CPUID); AMD processors without
SsbdNotRequired (Fn8000_0008_EBX[26], APM 3.2.9).

**Decided:** SSBD stays clear in the kernel and in user mode, and the
entry and exit do nothing. Intel recommends SSBD for "applications and/or
execution runtimes relying on language-based security mechanisms" and is
"not aware of any practical exploit for OSes or other applications that
do not rely on language-based security" (Intel HFB 4.3.2.2). The kernel
runs no code a process supplies (M1), and processes are separated by
address spaces.

- **Why:** the class needs attacker-chosen code in the victim's domain,
  which M1 rules out for the kernel.
- **Cost:** a process that runs untrusted code in its own address space
  (a script engine) is unprotected until it can ask for SSBD; Intel asks
  that "Operating systems should provide an API through which a process
  can request it be protected by SSBD mitigation" (Intel HFB 4.3.2.2).
  That call is added with the first such program.
- **Not taken:** SSBD always set, which "may reduce performance" (Intel
  HFB 4.3.2.2) for no attack the guidance knows of.

## M15. Gather data sampling

A gather instruction transiently forwards stale data from vector
registers last used by the same or the sibling thread (Intel GDS,
"Background and Issue Description").

**Applies to:** Intel processors in Intel table's Gather Data Sampling
column; GDS_NO (IA32_ARCH_CAPABILITIES[26]) rules it out, and GDS_CTRL
([25]) marks the microcode that mitigates it (Intel GDS, "MSR Control and
Enumeration"). User code can use gathers, since S4 enables AVX state.

**Decided:** the kernel never sets IA32_MCU_OPT_CTRL.GDS_MITG_DIS, so
the microcode mitigation, on by default, stays on; nothing on entry or
exit. "No software changes are required to enable the mitigation" (Intel
GDS, introduction), and it covers sibling threads (Intel GDS, "Mitigation
Options").

- **Why:** the microcode does the work; the kernel only has to not turn
  it off.
- **Cost:** "specific workloads may show performance impacts of up to
  50%" (Intel GDS, "Mitigation Options"), paid by gather-heavy programs.
- **Not taken:** clearing AVX from XCR0 on processors that might be
  affected but lack the microcode, since "the attack is not possible
  where Intel AVX is not supported" (Intel GDS, "MSR Control and
  Enumeration"): it takes AVX from every program and needs a model list
  to aim it.

## M16. Sibling threads

**Decided:** the kernel runs at most one logical processor per core until
it has a design for sibling threads. Today it runs on one processor
([boot-protocol.md](boot-protocol.md#other-processors)).

A design that runs siblings must meet what the guidance asks of them:

- MDS: group scheduling with synchronized entry and exit through
  interprocessor interrupts, or SMT off (Intel MDS, "Mitigations for
  Environments Utilizing Simultaneous Multithreading (SMT)").
- Indirect branches: STIBP wherever IBRS is not set (Intel HFB 3.1.2.3;
  APM 3.2.9); for BTC, STIBP or SMT off (AMD BTC 6.1.1, 6.1.2); and IBPB
  that "In some implementations ... only flushes branch prediction
  information related to the current thread" (AMD SRSO 4).
- IBRS and STIBP cleared before `HLT` on processors without enhanced IBRS
  (Intel HFB 3.1.2.2.2, 3.1.2.3).

Not needed for siblings: L1TF, whose cross-thread case concerns virtual
machines (M12); GDS, whose microcode covers siblings (M15); RFDS, whose
affected cores do not support Hyper-Threading (Intel RFDS); TSA, which
does not leak between SMT threads except a store queue left by an idle
sibling (AMD TSA 3.1, 3.2), covered by M13 item 2.

- **Why:** a kernel without core scheduling cannot keep two security
  domains off one core, and one thread per core removes every
  cross-thread case at once.
- **Cost:** on processors with SMT, half the logical processors stay
  idle.
- **Not taken:** siblings with STIBP set everywhere, which covers branch
  prediction but not the fill buffers and load ports MDS samples across
  threads (Intel MDS, "Cross-Thread Impacts of MFBDS", "Cross-Thread
  Impacts of MLPDS").

## M17. What TinyCC rules out

TinyCC compiles the kernel (DESIGN.md, Compiler). At the revision nova-nix
builds it from, `cb41cbfe717e4c00d7bb70035cda5ee5f0ff9341`, its x86-64
code generator emits an indirect call as `call *%r11` (`x86_64-gen.c`,
`gcall_or_jmp`) and ends every function with `leave` and `ret`
(`gfunc_epilog`), and its source has no option for retpolines, return
thunks, padding after branches or clearing registers before a return.
So:

1. **No retpoline** (M6): IBRS and its kin carry branch target
   injection.
2. **No return thunk:** Jmp2Ret is unavailable, which leaves IBPB on
   every entry on processors affected by BTC (M9), the costliest measure
   here.
3. **No barrier after a compiled branch:** AMD advises INT3 after `RET`
   and `JMP` and LFENCE after `CALL` against sequential speculation past
   the branch (AMD STMS G-5), and Intel advises a barrier after `RET` on
   Skylake-generation processors with RSBA (Intel RSBU, "Speculative
   Execution of Instructions after RET"). The kernel's assembly follows
   each `RET` and `JMP` with INT3 and each indirect `CALL` with LFENCE.
   Direct `CALL`s, whose LFENCE would run on every return, and compiled
   code go without: an accepted residual.
4. **No register clearing before `RET`**, which AMD lists as defense in
   depth against BTC-RET (AMD BTC 7.1). The exit clears the volatile
   registers at the system-call boundary (S4).

The masks, `CMOV` selects, fences, MSR writes, VERW, the 32-CALL overwrite
and the BHB-clearing sequence are written in the kernel's assembly
(`kernel/x86_64/entry.S`, `syscall.S`, `switch.S`, sharing the macros of
`x86_64.h`) or in GCC-style inline assembly, which TinyCC accepts
(`include/cpu.h`).

- **Not taken:** changing TinyCC's code generator to call a register
  thunk for indirect calls and to end functions with a jump to a return
  thunk. It would make retpoline and Jmp2Ret possible, at the cost of a
  compiler change carried outside TinyCC's own source.
