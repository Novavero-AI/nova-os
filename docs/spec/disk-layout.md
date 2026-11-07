# Disk layout

The boot disks nova-os's image tool writes, the system's and the test
disk: what the firmware needs to find and start nova-boot, and what
nova-boot needs to find the kernel and the first process. The two differ
only in the files of `\NOVA\`.
Specifications are cited by the versions in [README.md](README.md):
"UEFI 5.3.2" is section 5.3.2 of UEFI 2.11, "FAT" the FAT32 File System
Specification 1.03.

## Blocks and container

The disk is a sequence of 512-byte logical blocks. It is written either
raw or inside a VHDX file ([MS-VHDX] 8.0) whose `LogicalSectorSize` is 512
(2.6.2.4) and whose `VirtualDiskSize` is the disk's size in bytes, a
multiple of 512 (2.6.2.2). A Hyper-V generation 2 VM boots from a VHDX
attached to its SCSI controller.

## Partitioning

The disk is partitioned with a GUID Partition Table (UEFI 5.3). With `N`
blocks in all, the last is block `N - 1`:

| Blocks | Contents |
|---|---|
| 0 | Protective MBR (UEFI 5.2.3) |
| 1 | Primary GPT header (UEFI 5.3.2) |
| 2 to 33 | Primary partition entry array: 128 entries of 128 bytes |
| 34 to 2047 | Unused, zero |
| 2048 to `E` | The EFI System Partition |
| `E + 1` to `N - 34` | Unused, zero |
| `N - 33` to `N - 2` | Backup partition entry array |
| `N - 1` | Backup GPT header |

The protective MBR's first partition record has `OSType` 0xEE,
`StartingLBA` 1 and `SizeInLBA` `N - 1` (0xFFFFFFFF when `N - 1` does not
fit in 32 bits), with the remaining fields as UEFI Table 5.4 gives them; the
other three records are zero, bytes 510 and 511 hold 0x55 and 0xAA, and
bytes 440 to 445 are zero (UEFI Table 5.3).

Both GPT headers have `Signature` "EFI PART", `Revision` 0x00010000,
`HeaderSize` 92, `FirstUsableLBA` 34, `LastUsableLBA` `N - 34`,
`NumberOfPartitionEntries` 128 and `SizeOfPartitionEntry` 128. The primary
has `MyLBA` 1, `AlternateLBA` `N - 1` and `PartitionEntryLBA` 2; the backup
has `MyLBA` `N - 1`, `AlternateLBA` 1 and `PartitionEntryLBA` `N - 33`. The
rest of each header block is zero (UEFI Table 5.5). The entry array is the
16,384-byte minimum (UEFI 5.3.1). `HeaderCRC32` covers `HeaderSize` bytes
with the field itself zero, and `PartitionEntryArrayCRC32` covers the whole
array; both use UEFI's CRC32 (polynomial 0x04C11DB7, UEFI 4.2).

The first entry describes the EFI System Partition:

- `PartitionTypeGUID` C12A7328-F81F-11D2-BA4B-00A0C93EC93B (UEFI Table 5.7);
- `StartingLBA` 2048, a 1 MiB boundary, which suits most common physical
  block sizes (UEFI 5.3.1); `EndingLBA` `E`, inclusive;
- `Attributes` 0.

The other 127 entries are zero. GUIDs are stored with their first three
fields little-endian (UEFI Appendix A).

## The EFI System Partition

The partition holds a FAT32 file system (UEFI 13.3; FAT):

- `BPB_BytsPerSec` 512, the disk's block size.
- At least 65,525 clusters: FAT's type follows from the cluster count
  alone ("FAT Type Determination"), and a volume with fewer clusters is
  FAT16 or FAT12 whatever its boot sector says. Microsoft's sizing table
  rejects FAT32 below 66,600 sectors (about 32.5 MiB) and uses one sector
  per cluster up to 532,480 sectors ("FAT Volume Initialization").
- A cluster of at most 32 KiB (`BPB_SecPerClus`).
- `BPB_NumFATs` 2, `BPB_RootClus` 2, `BPB_FSInfo` 1, `BPB_BkBootSec` 6,
  `BS_FilSysType` "FAT32   ".
- `BPB_RsvdSecCnt` and `BPB_FATSz32` chosen so that the data region starts
  on a physical block boundary (UEFI 13.3.1.1).

## Files

Both disks hold:

```
\EFI\BOOT\BOOTX64.EFI    nova-boot
\NOVA\KERNEL.EXE         the kernel
\NOVA\INIT.EXE           the first user process
\NOVA\NTDLL.DLL          ntdll for 32-bit processes
\NOVA\VIRTBLK.EXE        the virtio block driver
\NOVA\VIRTINP.EXE        the virtio input driver
\NOVA\CONSOLE.EXE        the console, the input driver's client
\NOVA\STORE.IMG          the store image
\NOVA\FS.EXE             the file system service, for a data disk
\NOVA\FSTEST.EXE         the data disk's check
```

The test disk adds the inputs of the bootstrap check, the chain the first
process runs from the seed:

```
\NOVA\HEX0SEED.EXE       stage0-pe32's seed, hex0-seed.exe
\NOVA\...                the other inputs of the seed's chain
```

`\EFI\BOOT\BOOTX64.EFI` is the file the firmware starts when a boot option
names only the device, on fixed and removable media alike (UEFI 3.1.2,
3.5.1.1), and when no boot option works (UEFI 3.4.3). UEFI Table 3.4 gives
the x64 name as `BOOTx64.EFI` with `Machine` 0x8664. An EFI System
Partition on a hard disk has the directory `\EFI` in its root, and `BOOT`
under it holds the images used to recover booting (UEFI 13.3.1.3).

`\NOVA\KERNEL.EXE` is the file nova-boot opens as `\nova\kernel.exe`, and
the others in `\NOVA` are the boot files it reads beside it
([boot-protocol.md](boot-protocol.md#boot-files)), `\NOVA\STORE.IMG`
among them as the store image
([store-image.md](store-image.md)). None is a UEFI image,
and UEFI sets no location for files an application reads (UEFI
13.3.1.3). The seed and the other inputs of its chain are the files of
stage0-pe32 and of the trees it builds from, unmodified, under 8.3 names
the README's disk-image block gives them; the first process gives them
to the chain's programs under their own names
([compat.md](compat.md#c4-files)). The first process runs the chain
when `\NOVA\` holds all of them, and refuses a disk that holds some
but not all.

Every name fits 8.3 and is stored as a short directory entry only, in
uppercase (UEFI 13.3.1.2), with no long-name entries. `\NOVA\`'s entries
follow its dot entries in the bytewise order of their `DIR_Name`s, and
a directory takes as many clusters as its entries fill:

| Path | `DIR_Name` |
|---|---|
| `\EFI` | `EFI` followed by 8 spaces |
| `\EFI\BOOT` | `BOOT` followed by 7 spaces |
| `\EFI\BOOT\BOOTX64.EFI` | `BOOTX64 EFI` |
| `\NOVA` | `NOVA` followed by 7 spaces |
| `\NOVA\KERNEL.EXE` | `KERNEL  EXE` |
| `\NOVA\INIT.EXE` | `INIT    EXE` |
| `\NOVA\NTDLL.DLL` | `NTDLL   DLL` |
| `\NOVA\VIRTBLK.EXE` | `VIRTBLK EXE` |
| `\NOVA\VIRTINP.EXE` | `VIRTINP EXE` |
| `\NOVA\CONSOLE.EXE` | `CONSOLE EXE` |
| `\NOVA\STORE.IMG` | `STORE   IMG` |
| `\NOVA\FS.EXE` | `FS      EXE` |
| `\NOVA\FSTEST.EXE` | `FSTEST  EXE` |
| `\NOVA\HEX0SEED.EXE` | `HEX0SEEDEXE` |
| `\NOVA\` *name*`.`*ext* | *name* padded to 8 bytes with spaces, then *ext* padded to 3 |

FAT matches names without regard to case ("Name Matching In Short & Long
Names"), so `\nova\kernel.exe`, `\nova\init.exe` and `BOOTx64.EFI`
find these entries.
`DIR_NTRes` is 0. Each directory other than the root starts with its dot
and dotdot entries; the root has neither (FAT, "FAT Directory Structure").

Every date and time field is 1980-01-01 00:00:00 (date 0x0021, time 0),
the start of FAT's date range, so that the image's bytes do not depend on
when it was built.

## Undecided

- **Identifiers.** UEFI requires a unique `DiskGUID` and unique partition
  GUIDs, and a boot option that names the partition by hard-drive device
  path is matched on its `UniquePartitionGUID` (UEFI 3.1.2, 5.3.3). A
  reproducible image fixes them, along with FAT's `BS_VolID` and VHDX's
  `VirtualDiskId`; how they are derived is open.
- **Sizes.** The disk's size, the partition's size and the cluster size.
- **Installation.** A vendor directory under `\EFI` and a `Boot####`
  variable for an installed system (UEFI 3.1, 13.3.1.3).
- **4,096-byte blocks.** VHDX and GPT allow them (UEFI 5.3.1 sets
  `FirstUsableLBA` at least 6 for them).
- **The system partition.** A pool, the partition the native file system
  keeps its volumes in, has the type GUID [pool-format.md](pool-format.md)
  gives, and a data disk holds one; where the system's own pool lies, on
  this disk or another, is open.
- **ARM64.** `\EFI\BOOT\BOOTAA64.EFI` (UEFI Table 3.4).
