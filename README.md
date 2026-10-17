# HPFSNT - HPFS / HPFS386 File System Driver for Windows NT

HPFSNT is a from-scratch driver for OS/2's **High Performance File System**,
HPFS386 included, on Windows NT 3.1, 3.5, 3.51, 4.0, 2000, XP and XP x64. It
mounts HPFS volumes read/write and **starts Windows NT from them**.

It stands in for Microsoft's `pinball.sys` under the same name: NT 3.1 - 3.51
shipped an HPFS driver and NT 4.0 dropped it, so on NT 3.1 - 3.51 HPFSNT
simply takes the old driver's place, and on NT 4.0 and later it brings HPFS
back. It is built on the same design and NT 3.1 - 4.0 kernel infrastructure as
[FATNT](https://github.com/ages2001/FATNT) and
[NTFS3NT](https://github.com/ages2001/NTFS3NT): one source tree, one universal
binary, and a kernel-free engine that the tools use too.

## At a glance

| | |
|---|---|
| **Version** | 1.0 |
| **File systems** | HPFS, HPFS386 (OS/2 LAN Server) |
| **Windows** | NT 3.1, 3.5, 3.51, 4.0, 2000, XP (x86); XP x64 / Server 2003 x64 (amd64) |
| **Access** | read/write, extended attributes, HPFS386 ACLs kept, directory limits |
| **Paging file** | yes |
| **Starting Windows from it** | NT 3.1 - 4.0 |
| **Tools** | `hpfsinst`, `hpfsfmt`, `hpfschk`, `hpfsea`, `hpfsdasd` |

## Tested

Under QEMU on real installs:

| System | Data volumes | Boot / system volume | Microsoft's `CHKDSK` agrees |
|---|:---:|:---:|:---:|
| Windows NT 3.1 | yes | yes | yes (`chkdsk`, `chkdsk /f`) |
| Windows NT 3.51 | yes | not tested (\*) | yes |
| Windows NT 4.0 | yes | yes (SP6 NTLDR) | - (4.0 has no HPFS `CHKDSK`) |
| Windows 2000 | yes | no (\*\*) | - |
| Windows XP x64 | yes (amd64) | no (\*\*) | - |

(\*) The NT 3.51 test machine starts with a later NTLDR that has no HPFS
support; 3.51's own NTLDR, like those of 3.1 and 4.0, reads HPFS.
(\*\*) The NTLDR of Windows 2000 and XP cannot read HPFS; data volumes work.
32-bit XP is expected to behave like 2000 but was not tried.

Every test ran a workload - big `xcopy`s, 30 MB files, moves, renames, EAs, and
a random run of 2000 create / extend / truncate / rename / delete operations
with read-back verification - and then checked the volume with `hpfschk` and,
where the system has one, with Microsoft's HPFS `CHKDSK`: no errors, and the
same counts of directories, files and free space. The boot tests ran NT 3.1 and
NT 4.0 SP6 from HPFS up to a working session - paging file on HPFS, command
prompt, DOS (NTVDM) programs, clean shutdown.

## Features

**HPFS**

- 254-character, case-preserving names in the volume's code page, with the
  OS/2 long-name attribute kept right.
- The dnode B-tree directories (splits, joins, balancing, spare dnodes when
  the disk is full), the fnode / anode allocation B+ trees, band bitmaps and
  the directory band, hotfix remapping.
- Files up to 2 GB, volumes up to 64 GB.
- The spareblock's dirty flag is set before the first change and cleared at
  shutdown, lock and dismount.

**Extended attributes**

- OS/2 EAs - inline, external, in anode trees, indirect values - with the full
  NT EA interface: `NtQueryEaFile` (lists, index, restart, single entry),
  `NtSetEaFile`, EAs given at create, `FILE_NEED_EA`.

**HPFS386**

- Volumes from OS/2 LAN Server / HPFS386 mount like any other.
- Access control lists stored in fnodes are kept intact through every change
  (EA rewrites, renames, moves).
- **Directory limits** (DASD limits) are enforced and can be set, changed and
  removed with `hpfsdasd`; OS/2 sees the same limits.

**Safe metadata**

HPFS mixes structures and file data in the same pages, so metadata is cached
sector by sector by the driver itself - never through a page-sized cache that
could write a stale data sector back. Changes reach the disk at the end of
each operation.

**The driver**

- Cache Manager integration (cached reads and writes, fast I/O, memory-mapped
  files and executables), paging I/O and a paging file on HPFS.
- Byte-range locks, share access, delete-on-close, write-through, lock and
  dismount for the tools.
- One i386 binary for NT 3.1 - XP: the executive-resource API is bound to the
  running kernel at load time, NT 3.1 sizes of `ERESOURCE` and `FILE_LOCK`
  are allowed for, the dispatch table stops at the IRP majors NT 3.1 has, and
  file system controls sent as device controls by NT 3.1's `DeviceIoControl`
  are accepted both ways.

## Installing

`bin\i386` holds the 32-bit driver and tools (NT 3.1 - XP), `bin\amd64` the
64-bit ones (XP x64 / Server 2003 x64). Copy `pinball.sys` and `hpfsinst.exe`
from the folder for your system to one folder, run `hpfsinst` from an
administrator account and restart.

```
hpfsinst             install
hpfsinst /BOOT       boot start, for starting NT 4.0 from an HPFS volume
hpfsinst /READONLY   mount HPFS volumes read-only
hpfsinst /U          uninstall
```

- **NT 3.1, 3.5, 3.51:** the system's `pinball.sys` is kept as `pinball.sav`
  and HPFSNT takes its place, for every HPFS volume including the boot volume.
- **NT 4.0, 2000, XP:** these have no HPFS driver; `hpfsinst` installs one as
  the `Pinball` service (system start).
- **XP x64:** use the files in `bin\amd64`; the 32-bit `hpfsinst` refuses to
  run there. The 32-bit `hpfschk`, `hpfsea` and `hpfsdasd` also work under
  WOW64.
- `hpfsinst /U` restores `pinball.sav`, or disables the service it created.

The one setting is `EnableWriteSupport` (`REG_DWORD`) under
`HKLM\SYSTEM\CurrentControlSet\Services\Pinball`: 1 or missing for
read/write, 0 for read-only.

## Starting Windows NT from HPFS

HPFSNT's boot loader, which `hpfsfmt` writes to sectors 0 - 15, reads the root
directory B-tree and NTLDR's allocation tree (any number of extents) with
INT 13h extensions or CHS, and loads NTLDR. The NTLDR of NT 3.1 - 4.0 reads
HPFS itself and loads the system; `pinball.sys` - HPFSNT - then runs the
system volume.

1. Format the partition with `hpfsfmt X:` (it writes the boot loader and sets
   the partition type to `07h`) and mark it active.
2. Copy the system onto it: NTLDR, NTDETECT.COM and BOOT.INI in the root, and
   the Windows directory.
3. NT 3.1 - 3.51: install HPFSNT on that system with `hpfsinst` (NT 3.1's
   loader loads `pinball.sys` for an HPFS boot volume by itself; add `/BOOT`
   on 3.5x if it does not). NT 4.0: `hpfsinst /BOOT`.

## Tools

```
hpfsfmt  X: [/V:label] [/HPFS386] [/NOBOOT] [/Y]
hpfschk  X: [/F] [/V]                       also an image file: hpfschk disk.img
hpfsea   FILE [/A] [NAME=VALUE | NAME= | /TEST]
hpfsdasd DIR [/L:kb [/T:%] [/D:%]]   |   hpfsdasd X: /INFO
```

All are freestanding console programs that run on NT 3.1 and later.

- **hpfsfmt** formats a volume, with an 11-character label, optionally marked
  as HPFS386, with the boot loader unless `/NOBOOT`.
- **hpfschk** checks a volume; `/F` locks it and repairs what can be repaired
  safely: entries whose fnode or allocation is damaged are removed,
  disagreeing sizes are made to agree, wrong parent links, `..` entries, entry
  flags and attributes, stale checksums, and the bitmaps (sectors in use are
  always marked used; lost space is freed only when the directory tree is
  sound). On NT 3.1 - 3.51 Microsoft's `CHKDSK` handles HPFS too and works
  with HPFSNT.
- **hpfsea** lists a file's EAs, sets one (raw, or as an OS/2 `EAT_ASCII`
  value with `/A`) or removes one; `/TEST` exercises the whole EA interface on
  a scratch file.
- **hpfsdasd** shows a directory's limit and usage, sets a limit in kilobytes
  (with warning threshold and delta), removes it (`/L:0`), or shows the
  volume's HPFS details (`/INFO`). The first limit on a volume turns
  enforcement on, the HPFS386 way.

## Limitations

- Names must be representable in the system's OEM code page (HPFS stores them
  that way); others are refused rather than stored altered.
- HPFS386 ACLs are preserved, not enforced; NT applies its own security.
- Opportunistic locks are not granted.
- The universal i386 binary holds change notification pending and completes
  it at cleanup (NT 3.1 has no `FsRtlNotifyFull` package), so File Manager and
  Explorer refresh on demand but never spin; the amd64 build delivers live
  notification.
- Windows 2000 and XP cannot start from HPFS (their NTLDR does not read it).

## Building

The tested binaries come from the GCC cross build, on Linux with
`gcc-mingw-w64-i686` (and `gcc-mingw-w64-x86-64` for amd64):

```
build/build_cross.sh [OUTDIR]      # i386: pinball.sys and all tools
build/build_amd64.sh [OUTDIR]      # amd64: the same for XP x64
build/build_linux.sh [OUTDIR]      # hpfstool: the engine against image files
```

`hpfstool IMAGE ...` formats, checks (`check -f` repairs), lists, copies in and
out, sets EAs, ACLs and directory limits, and runs a randomized engine stress
test against an in-memory model.

GCC has no `__try` / `__except`, so exceptions raised by the Cache Manager are
caught through a frame on the FS:[0] chain on i386 and through an unwind-table
language handler with `RtlUnwindEx` on amd64 (`src/hpfsseh.c`). The amd64
build is the Windows 2000 / XP profile of the same sources (real change
notification, Plug and Play removal).

There are also Visual C++ 4.x + NT 4.0 DDK builds: `build.bat` in `src\NT` (the
driver, with the free `ntifs.h`), `fmt\NT`, `chk\NT`, `inst\NT` and `tools\NT`.

## Source layout

| Path | What |
|---|---|
| `src/hpfsdisk.h` | on-disk structures |
| `src/hpfssup.c`, `hpfsalc.c`, `hpfsdnd.c`, `hpfsfil.c` | the engine: volume, bitmaps, allocation trees, dnode B-tree, files, EAs, ACLs, directory limits |
| `src/hpfsmeta.c` | the metadata cache: the engine's host in the driver |
| `src/hpfs*.c` | the driver: IRP handlers, FCBs, I/O, SEH for GCC |
| `boot/hpfsboot.S` | the boot loader |
| `fmt/`, `chk/`, `inst/`, `tools/` | the tools |
| `test/hpfsstrs.c` | the random workload used in the VMs |
| `bin/i386`, `bin/amd64` | the built driver and tools |

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).

`src/NT/ntifs.h` is the free `ntifs.h` (release 58) by Bo Branten, GPL v2 or
later, included unchanged.
