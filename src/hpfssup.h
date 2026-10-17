/*
 * HPFSNT - the shared, kernel-free HPFS engine (hpfssup.c)
 *
 * Everything that understands the on-disk format lives here: superblock
 * and spareblock, band bitmaps and the directory band, code page case
 * tables and name ordering, the directory dnode B-tree, the allocation
 * B+ tree of fnodes and anodes, extended attributes, HPFS386 ACL areas and
 * DASD limits, hotfixes. The same file builds into the driver, the format,
 * check and image tools and the Linux test harness.
 *
 * Disk access goes through a small host interface (HpfsHost*, below) that
 * maps runs of sectors into memory: in the driver that is the Cache
 * Manager on the volume stream, in user mode a simple sector cache. The
 * engine never touches a buffer after unmapping it.
 *
 * Callers serialize: one thread inside the engine per volume at a time
 * for anything that changes metadata.
 */

#ifndef _HPFSSUP_H_
#define _HPFSSUP_H_

#include "hpfsdisk.h"

#ifndef HPFS_STATUS_DEFINED
#define HPFS_STATUS_DEFINED
typedef LONG HPFS_STATUS;
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((HPFS_STATUS)(s) >= 0)
#endif

/* Status values the engine returns (NTSTATUS codes) */
#define HPFS_OK                 ((HPFS_STATUS)0x00000000L)
#define HPFS_NO_MORE_ENTRIES    ((HPFS_STATUS)0x8000001AL)  /* STATUS_NO_MORE_ENTRIES */
#define HPFS_NOT_FOUND          ((HPFS_STATUS)0xC0000034L)  /* STATUS_OBJECT_NAME_NOT_FOUND */
#define HPFS_EXISTS             ((HPFS_STATUS)0xC0000035L)  /* STATUS_OBJECT_NAME_COLLISION */
#define HPFS_DISK_FULL          ((HPFS_STATUS)0xC000007FL)  /* STATUS_DISK_FULL */
#define HPFS_NO_MEMORY          ((HPFS_STATUS)0xC000009AL)  /* STATUS_INSUFFICIENT_RESOURCES */
#define HPFS_CORRUPT            ((HPFS_STATUS)0xC0000102L)  /* STATUS_FILE_CORRUPT_ERROR */
#ifdef HPFS_TRACE_CORRUPT
HPFS_STATUS HpfsTraceCorrupt(const char *File, int Line);
#define HPFS_DISK_CORRUPT       HpfsTraceCorrupt(__FILE__, __LINE__)
#else
#define HPFS_DISK_CORRUPT       ((HPFS_STATUS)0xC0000032L)  /* STATUS_DISK_CORRUPT_ERROR */
#endif
#define HPFS_BAD_NAME           ((HPFS_STATUS)0xC0000033L)  /* STATUS_OBJECT_NAME_INVALID */
#define HPFS_NOT_EMPTY          ((HPFS_STATUS)0xC0000101L)  /* STATUS_DIRECTORY_NOT_EMPTY */
#define HPFS_EA_TOO_LARGE       ((HPFS_STATUS)0xC0000050L)  /* STATUS_EA_TOO_LARGE */
#define HPFS_BAD_EA             ((HPFS_STATUS)0x80000013L)  /* STATUS_EA_LIST_INCONSISTENT */
#define HPFS_QUOTA              ((HPFS_STATUS)0xC0000044L)  /* STATUS_QUOTA_EXCEEDED */
#define HPFS_IO_ERROR           ((HPFS_STATUS)0xC0000185L)  /* STATUS_IO_DEVICE_ERROR */
#define HPFS_READ_ONLY          ((HPFS_STATUS)0xC00000A2L)  /* STATUS_MEDIA_WRITE_PROTECTED */
#define HPFS_UNRECOGNIZED       ((HPFS_STATUS)0xC000014FL)  /* STATUS_UNRECOGNIZED_VOLUME */
#define HPFS_TOO_LARGE          ((HPFS_STATUS)0xC0000904L)  /* STATUS_FILE_TOO_LARGE */

/* Largest file HPFS can describe: 2 GB - 1 (32-bit signed sizes in OS/2) */
#define HPFS_MAX_FILE_SIZE      0x7FFFFFFFUL
#define HPFS_MAX_EA_LIST        65536UL     /* OS/2 limit on a file's EA list */

/* A mapped run of sectors */
typedef struct _HPFS_BUF {
    PUCHAR      Data;
    ULONG       Sector;
    ULONG       Count;
    PVOID       Host;                   /* host-private (BCB / cache slot) */
    BOOLEAN     Dirty;
} HPFS_BUF, *PHPFS_BUF;

#define HPFS_MAP_READ           0x00
#define HPFS_MAP_WRITE          0x01    /* the caller will change the data */
#define HPFS_MAP_NEW            0x03    /* write, and the old contents do not matter (zeroed) */

/* Allocation flags */
#define HPFS_ALLOC_CONTIG       0x00    /* all Count sectors in one run or fail */
#define HPFS_ALLOC_PARTIAL      0x01    /* the first free run, up to Count sectors */
#define HPFS_ALLOC_FORWARD      0x02    /* search only forward from Near */
#define HPFS_ALLOC_ALIGN4       0x04    /* the run starts on a multiple of 4 */

#define HPFS_MAX_BANDS          8192    /* 64 GB */
#define HPFS_MAX_SECTORS        (HPFS_MAX_BANDS * HPFS_BAND_SECTORS)

/* Per-volume engine state */
typedef struct _HPFS_VOL {
    PVOID       HostContext;            /* the host's volume (VCB, image) */

    ULONG       TotalSectors;
    ULONG       RootFnode;
    ULONG       BandCount;
    ULONG       BitmapListSector;
    PULONG      BandBitmap;             /* [BandCount] bitmap sector of each band */
    PULONG      BandFree;               /* [BandCount] free sectors, or ~0 = not counted */
    ULONG       FreeSectors;            /* valid when FreeKnown */
    BOOLEAN     FreeKnown;

    ULONG       DirBandStart;
    ULONG       DirBandEnd;
    ULONG       DirBandDnodes;
    ULONG       DirBandBitmap;
    ULONG       DirBandFree;            /* free dnodes in the band, ~0 = not counted */
    ULONG       DirBandHint;

    ULONG       DataHint;               /* where the last data allocation ended */
    ULONG       SpareDnodes;            /* spare dnodes listed in the spareblock */
    ULONG       SpareDnodesFree;

    UCHAR       Version;
    UCHAR       FuncVersion;
    UCHAR       SpareFlags;
    UCHAR       SpareFlags2;
    ULONG       SerialNumber;
    UCHAR       Label[12];              /* from the boot sector, 0-terminated */
    ULONG       LabelLength;

    /* Code page */
    USHORT      CodePage;               /* code page of index 0 (first entry) */
    UCHAR       CodePageIndex;
    UCHAR       Upcase[256];            /* byte -> uppercase byte */

    /* Hotfixes */
    ULONG       HotfixCount;
    ULONG       HotfixBad[HPFS_MAX_HOTFIX];
    ULONG       HotfixSpare[HPFS_MAX_HOTFIX];

    BOOLEAN     ReadOnly;
    BOOLEAN     MarkedDirty;            /* we set the dirty bit on disk */
    BOOLEAN     Hpfs386;                /* HPFS386 structures present */
    BOOLEAN     DasdActive;             /* enforce DASD limits */
} HPFS_VOL, *PHPFS_VOL;

/* ------------------------------------------------------------------ */
/* Host interface (implemented by the driver, the tools, the test)     */
/* ------------------------------------------------------------------ */

HPFS_STATUS HpfsHostMap(PHPFS_VOL Vol, ULONG Sector, ULONG Count, ULONG Mode, PHPFS_BUF Buf);
VOID        HpfsHostSetDirty(PHPFS_VOL Vol, PHPFS_BUF Buf);
VOID        HpfsHostUnmap(PHPFS_VOL Vol, PHPFS_BUF Buf);
PVOID       HpfsHostAlloc(ULONG Bytes);
VOID        HpfsHostFree(PVOID P);
ULONG       HpfsHostNow(VOID);         /* local time, seconds since 1970 */
/* Sectors freed by the engine: the host drops cached copies (may be a no-op) */
VOID        HpfsHostFreed(PHPFS_VOL Vol, ULONG Sector, ULONG Count);

#define HpfsSetDirty(Vol, Buf)  HpfsHostSetDirty((Vol), (Buf))

/* ------------------------------------------------------------------ */
/* Basics                                                              */
/* ------------------------------------------------------------------ */

ULONG   HpfsChecksum(const VOID *Data, ULONG Length);
VOID    HpfsUpdateChecksums(PHPFS_SUPERBLOCK Super, PHPFS_SPAREBLOCK Spare);

/* Build Vol->Upcase from an HPFS case table (128 bytes for 0x80..0xFF) */
VOID    HpfsSetCaseTable(PHPFS_VOL Vol, const UCHAR *UpperMap128);

/* Compare two names in HPFS order: <0, 0 (equal ignoring case), >0 */
LONG    HpfsCompareNames(PHPFS_VOL Vol, const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen);

/* TRUE if the (OEM) name is a valid DOS 8.3 name (in any case); entries with
   other names carry HPFS_ATTR_LONGNAME, as OS/2 and NT's CHKDSK expect */
BOOLEAN HpfsIsDos83(const UCHAR *Name, ULONG Length);

/* TRUE if the (OEM) name is a legal HPFS name */
BOOLEAN HpfsIsLegalName(const UCHAR *Name, ULONG Length);

/* ------------------------------------------------------------------ */
/* Volume                                                              */
/* ------------------------------------------------------------------ */

/* Checks a boot sector; returns TRUE if it describes an HPFS volume */
BOOLEAN HpfsIsBootSector(const VOID *Sector);

/*
 * Reads the superblock, spareblock, bitmap list, code page and hotfix
 * table into Vol (HostContext must be set). Does not write anything.
 */
HPFS_STATUS HpfsMount(PHPFS_VOL Vol, BOOLEAN ReadOnly);
VOID        HpfsUnmountFree(PHPFS_VOL Vol);

/* Sets (Dirty) or clears the spareblock dirty bit and rewrites the checksums */
HPFS_STATUS HpfsMarkDirty(PHPFS_VOL Vol, BOOLEAN Dirty);

/* Counts free sectors (whole bitmap pass the first time) */
HPFS_STATUS HpfsCountFree(PHPFS_VOL Vol, PULONG Free);

/* Maps a data sector through the hotfix table */
ULONG       HpfsHotfix(PHPFS_VOL Vol, ULONG Sector);

/* Sets the volume label in the boot sector (and Vol) */
HPFS_STATUS HpfsSetLabel(PHPFS_VOL Vol, const UCHAR *Label, ULONG Length);

/* ------------------------------------------------------------------ */
/* Space                                                               */
/* ------------------------------------------------------------------ */

HPFS_STATUS HpfsAllocSectors(PHPFS_VOL Vol, ULONG Near, ULONG Count, ULONG Flags,
                             PULONG First, PULONG Got);
HPFS_STATUS HpfsFreeSectors(PHPFS_VOL Vol, ULONG First, ULONG Count);
/* TRUE if all Count sectors from First are marked used */
BOOLEAN     HpfsSectorsUsed(PHPFS_VOL Vol, ULONG First, ULONG Count);

HPFS_STATUS HpfsAllocDnode(PHPFS_VOL Vol, ULONG Near, PULONG Dnode);
HPFS_STATUS HpfsFreeDnode(PHPFS_VOL Vol, ULONG Dnode);
HPFS_STATUS HpfsTakeSpareDnode(PHPFS_VOL Vol, PULONG Dnode);

/* ------------------------------------------------------------------ */
/* Allocation tree of an fnode (file data, or EA/ACL runs via anodes)  */
/* ------------------------------------------------------------------ */

typedef struct _HPFS_RUN {
    ULONG       FileSector;
    ULONG       Length;
    ULONG       DiskSector;
} HPFS_RUN, *PHPFS_RUN;

/* Look up file sector Vsn: *Lsn and the run length from there (0 = beyond the end) */
HPFS_STATUS HpfsMapFileSector(PHPFS_VOL Vol, ULONG Fnode, ULONG Vsn, PULONG Lsn, PULONG RunLength);

/* All runs of a file in order (caller frees *Runs with HpfsHostFree) */
HPFS_STATUS HpfsGetRuns(PHPFS_VOL Vol, ULONG Fnode, PHPFS_RUN *Runs, PULONG Count, PULONG Sectors);

/* Allocated sectors of a file */
HPFS_STATUS HpfsGetAllocation(PHPFS_VOL Vol, ULONG Fnode, PULONG Sectors);

/*
 * Grows the allocation of a file to NewSectors (appending runs near the
 * last one). The new sectors are not zeroed.
 */
HPFS_STATUS HpfsGrowAllocation(PHPFS_VOL Vol, ULONG Fnode, ULONG NewSectors);

/* Shrinks the allocation to NewSectors, freeing the rest */
HPFS_STATUS HpfsTruncateAllocation(PHPFS_VOL Vol, ULONG Fnode, ULONG NewSectors);

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

/* A directory entry as the engine hands it out */
typedef struct _HPFS_ENTRY {
    ULONG       Fnode;
    UCHAR       Flags;
    UCHAR       Attributes;
    UCHAR       CodePage;
    UCHAR       AclCount;
    ULONG       WriteTime;
    ULONG       AccessTime;
    ULONG       CreateTime;
    ULONG       FileSize;
    ULONG       EaSize;
    ULONG       NameLength;
    UCHAR       Name[HPFS_MAX_NAME + 1];
    ULONG       Dnode;                  /* where it was found */
} HPFS_ENTRY, *PHPFS_ENTRY;

/* The root dnode of a directory fnode */
HPFS_STATUS HpfsDirRoot(PHPFS_VOL Vol, ULONG DirFnode, PULONG Dnode);

/* Finds Name in the directory */
HPFS_STATUS HpfsLookup(PHPFS_VOL Vol, ULONG DirFnode, const UCHAR *Name, ULONG Length, PHPFS_ENTRY Entry);

/*
 * Enumeration: the first entry whose name sorts after After (After == NULL:
 * the first entry). The ".." entry is skipped. HPFS_NO_MORE_ENTRIES at the end.
 */
HPFS_STATUS HpfsNextEntry(PHPFS_VOL Vol, ULONG DirFnode, const UCHAR *After, ULONG AfterLength,
                          PHPFS_ENTRY Entry);

/* TRUE when the directory has no entries besides ".." */
HPFS_STATUS HpfsDirIsEmpty(PHPFS_VOL Vol, ULONG DirFnode, PBOOLEAN Empty);

/* Adds an entry (Entry->Name, all fields as given) */
HPFS_STATUS HpfsDirInsert(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_ENTRY Entry);

/* Removes the entry Name */
HPFS_STATUS HpfsDirRemove(PHPFS_VOL Vol, ULONG DirFnode, const UCHAR *Name, ULONG Length);

/* Rewrites the fixed fields (times, size, attributes, flags, EA size) of entry Name */
HPFS_STATUS HpfsDirUpdate(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_ENTRY Entry);

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

/*
 * Creates a file or directory: a new fnode near the parent, a dnode for a
 * directory, and the entry in the parent. Entry gives name, attributes and
 * times; Entry->Fnode is filled in.
 */
HPFS_STATUS HpfsCreate(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_ENTRY Entry, BOOLEAN Directory);

/*
 * Deletes a file or (empty) directory: frees data, EAs, ACLs, anodes,
 * dnodes and the fnode, and removes the entry from the parent.
 */
HPFS_STATUS HpfsDelete(PHPFS_VOL Vol, ULONG DirFnode, const UCHAR *Name, ULONG Length);

/* Moves/renames entry OldName of OldDir to NewName in NewDir */
HPFS_STATUS HpfsRename(PHPFS_VOL Vol, ULONG OldDir, const UCHAR *OldName, ULONG OldLength,
                       ULONG NewDir, const UCHAR *NewName, ULONG NewLength);

/* Sets the size in the fnode (the directory entry is updated separately) */
HPFS_STATUS HpfsSetFnodeSize(PHPFS_VOL Vol, ULONG Fnode, ULONG Size);
/* Fnode name and parent; ".." of a directory; times from ".." */
HPFS_STATUS HpfsSetFnodeName(PHPFS_VOL Vol, ULONG Fnode, const UCHAR *Name, ULONG Length, ULONG Parent);
HPFS_STATUS HpfsSetDotDot(PHPFS_VOL Vol, ULONG DirFnode, ULONG Parent);
HPFS_STATUS HpfsGetDotDot(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_ENTRY Entry);
HPFS_STATUS HpfsGetFnodeInfo(PHPFS_VOL Vol, ULONG Fnode, PULONG Size, PUSHORT Flags, PULONG Parent);

/* ------------------------------------------------------------------ */
/* Extended attributes                                                 */
/* ------------------------------------------------------------------ */

/*
 * Reads the EA list of a file (concatenated HPFS_EA records, large values
 * resolved inline). *List is allocated (HpfsHostFree), *Length its size.
 */
HPFS_STATUS HpfsReadEas(PHPFS_VOL Vol, ULONG Fnode, PUCHAR *List, PULONG Length);

/*
 * Replaces the EA list of a file. Stores it inside the fnode when it fits,
 * else in a run of sectors (or an anode tree). Updates the fnode's EA
 * fields and need-EA count; returns the external size and need-EA flag for
 * the directory entry.
 */
HPFS_STATUS HpfsWriteEas(PHPFS_VOL Vol, ULONG Fnode, const UCHAR *List, ULONG Length,
                         PULONG ExternalSize, PBOOLEAN NeedEa);

/* The EA list as stored (indirect values not resolved) */
HPFS_STATUS HpfsReadEasRaw(PHPFS_VOL Vol, ULONG Fnode, PUCHAR *List, PULONG Length);

/* Total EA list size of a file (bytes, as OS/2 counts it) */
HPFS_STATUS HpfsEaListSize(PHPFS_VOL Vol, ULONG Fnode, PULONG Size);

/* ------------------------------------------------------------------ */
/* HPFS386                                                             */
/* ------------------------------------------------------------------ */

/* Raw ACL area of an fnode (inline + external); *Acl allocated */
HPFS_STATUS HpfsReadAcl(PHPFS_VOL Vol, ULONG Fnode, PUCHAR *Acl, PULONG Length);
HPFS_STATUS HpfsWriteAcl(PHPFS_VOL Vol, ULONG Fnode, const UCHAR *Acl, ULONG Length);

/*
 * DASD limits: charges Delta sectors (signed) to every directory with a
 * limit on the way from Dir to the root. Fails with HPFS_QUOTA (and
 * changes nothing) when a limit would be exceeded. No-op unless the volume
 * has DASD limits active.
 */
HPFS_STATUS HpfsDasdCharge(PHPFS_VOL Vol, ULONG DirFnode, LONG Delta);

/* A directory's DASD limit (sectors; 0 = none), its usage, warning threshold and step (percent) */
typedef struct _HPFS_DASD {
    ULONG       Limit;
    ULONG       Usage;
    ULONG       Threshold;
    ULONG       Delta;
    ULONG       Active;                 /* the volume enforces limits */
} HPFS_DASD, *PHPFS_DASD;

HPFS_STATUS HpfsDasdQuery(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_DASD Dasd);
HPFS_STATUS HpfsDasdSet(PHPFS_VOL Vol, ULONG DirFnode, ULONG Limit, ULONG Threshold, ULONG Delta);

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

/* HPFS stores seconds since 1970-01-01 00:00 (local time) */
#define HPFS_EPOCH_DELTA        11644473600ULL      /* 1601 -> 1970, seconds */

/* ------------------------------------------------------------------ */
/* Engine-internal (shared between the hpfs*.c engine files)           */
/* ------------------------------------------------------------------ */

#define MIN_UL(a, b)    ((a) < (b) ? (a) : (b))
#define HPFS_UNKNOWN    0xFFFFFFFFUL

VOID        HpfsZero(PVOID P, ULONG N);
VOID        HpfsCopy(PVOID D, const VOID *S, ULONG N);

/* Builds the allocation tree under Root (an fnode, or an anode used as an
   EA/ACL tree root) from Runs; frees no old anodes (see HpfsFreeTreeAnodes) */
HPFS_STATUS HpfsBuildTree(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode,
                          PHPFS_RUN Runs, ULONG Count, ULONG Hint);
HPFS_STATUS HpfsTreeRuns(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode,
                         PHPFS_RUN *Runs, PULONG Count, PULONG Sectors);
HPFS_STATUS HpfsFreeTreeAnodes(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode);
HPFS_STATUS HpfsTreeAppend(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, ULONG FileSector, ULONG Length, ULONG Disk);
HPFS_STATUS HpfsTreeTruncate(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, ULONG NewSectors);

HPFS_STATUS HpfsMapFnode(PHPFS_VOL Vol, ULONG Fnode, ULONG Mode, PHPFS_BUF Buf, PHPFS_FNODE *F);

#endif /* _HPFSSUP_H_ */
