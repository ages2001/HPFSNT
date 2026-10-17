/*
 * HPFSNT - HPFS / HPFS386 on-disk format
 *
 * Needs only the basic NT types (UCHAR, USHORT, ULONG, BOOLEAN), so the
 * engine in hpfssup.c also builds outside the kernel (format, check and
 * image tools, the test harness).
 *
 * Every structure is little endian. Sector numbers are 32-bit and relative
 * to the start of the partition; a sector is always 512 bytes.
 */

#ifndef _HPFSDISK_H_
#define _HPFSDISK_H_

#pragma pack(1)

#define HPFS_SECTOR_SIZE        512
#define HPFS_SECTOR_SHIFT       9

/* Fixed sectors */
#define HPFS_BOOT_SECTORS       16          /* 0..15: boot sector + loader */
#define HPFS_SUPER_SECTOR       16
#define HPFS_SPARE_SECTOR       17

/* Bitmap bands: one 2 KB bitmap (4 sectors) per 16384 sectors (8 MB) */
#define HPFS_BAND_SECTORS       16384
#define HPFS_BAND_SHIFT         14
#define HPFS_BITMAP_SECTORS     4

/* Signatures */
#define HPFS_SUPER_MAGIC        0xF995E849
#define HPFS_SUPER_MAGIC1       0xFA53E9C5
#define HPFS_SPARE_MAGIC        0xF9911849
#define HPFS_SPARE_MAGIC1       0xFA5229C5
#define HPFS_FNODE_MAGIC        0xF7E40AAE
#define HPFS_ANODE_MAGIC        0x37E40AAE
#define HPFS_DNODE_MAGIC        0x77E40AAE
#define HPFS_CPDIR_MAGIC        0x494521F7
#define HPFS_CPDATA_MAGIC       0x894521F7

/* ------------------------------------------------------------------ */
/* Boot sector (sector 0): a DOS 4 BPB with the "HPFS    " tag         */
/* ------------------------------------------------------------------ */

typedef struct _HPFS_BOOT_SECTOR {
    UCHAR       Jump[3];
    UCHAR       Oem[8];
    USHORT      BytesPerSector;         /* 512 */
    UCHAR       SectorsPerCluster;      /* 0 */
    USHORT      ReservedSectors;        /* 1 */
    UCHAR       NumberOfFats;           /* 0 */
    USHORT      RootEntries;            /* 512 (unused) */
    USHORT      TotalSectors16;
    UCHAR       Media;                  /* 0xF8 */
    USHORT      FatSize;                /* 0 */
    USHORT      SectorsPerTrack;
    USHORT      Heads;
    ULONG       HiddenSectors;
    ULONG       TotalSectors32;
    UCHAR       DriveNumber;            /* 0x80 */
    UCHAR       Reserved1;
    UCHAR       BootSignature;          /* 0x29 */
    ULONG       SerialNumber;
    UCHAR       VolumeLabel[11];
    UCHAR       FileSystemType[8];      /* "HPFS    " */
    UCHAR       BootCode[448];
    USHORT      Signature;              /* 0xAA55 */
} HPFS_BOOT_SECTOR, *PHPFS_BOOT_SECTOR;

/* ------------------------------------------------------------------ */
/* Superblock (sector 16)                                               */
/* ------------------------------------------------------------------ */

typedef struct _HPFS_SUPERBLOCK {
    ULONG       Magic;                  /* HPFS_SUPER_MAGIC */
    ULONG       Magic1;                 /* HPFS_SUPER_MAGIC1 */
    UCHAR       Version;                /* 2 (3: volume > 4 GB, OS/2 Warp) */
    UCHAR       FuncVersion;            /* 2 (3: HPFS386 extensions present) */
    USHORT      Zero1;
    ULONG       RootFnode;
    ULONG       TotalSectors;           /* sectors in the file system */
    ULONG       BadSectors;
    ULONG       BitmapList;             /* 4+ sectors: one bitmap sector per band */
    ULONG       Zero2;
    ULONG       BadBlockList;           /* 4-sector chained list of bad sectors */
    ULONG       Zero3;
    ULONG       LastChkdsk;             /* seconds since 1970 */
    ULONG       LastOptimize;
    ULONG       DirBandSectors;         /* sectors in the directory band */
    ULONG       DirBandStart;
    ULONG       DirBandEnd;             /* last sector of the band */
    ULONG       DirBandBitmap;          /* 4 sectors, one bit per dnode, 1 = free */
    UCHAR       VolumeName[32];         /* unused by OS/2; we keep it zero */
    ULONG       UserIdTable;            /* 8 sectors, HPFS386 user/group table */
    ULONG       Zero6[103];
} HPFS_SUPERBLOCK, *PHPFS_SUPERBLOCK;

/* ------------------------------------------------------------------ */
/* Spareblock (sector 17)                                               */
/* ------------------------------------------------------------------ */

/* Flags (byte 0) */
#define HPFS_SPARE_DIRTY        0x01    /* not cleanly dismounted */
#define HPFS_SPARE_SPARE_USED   0x02    /* a spare dnode was used */
#define HPFS_SPARE_HOTFIX_USED  0x04    /* hotfix sectors are in use */
#define HPFS_SPARE_BAD_SECTOR   0x08    /* a bad sector was found */
#define HPFS_SPARE_BAD_BITMAP   0x10    /* a bitmap is unreliable */
#define HPFS_SPARE_FAST_FORMAT  0x20
#define HPFS_SPARE_OLD_WROTE    0x40    /* an old (pre-1.2) OS/2 wrote to it */
#define HPFS_SPARE_OLD_WROTE1   0x80
/* Flags2 (byte 1): HPFS386 */
#define HPFS_SPARE2_INSTALL_DASD 0x01   /* DASD limits installed */
#define HPFS_SPARE2_RESYNC_DASD  0x02   /* usage must be recounted */
#define HPFS_SPARE2_DASD_ACTIVE  0x04   /* limits are enforced */
#define HPFS_SPARE2_MULTIMEDIA   0x08
#define HPFS_SPARE2_DCE_ACLS     0x10
#define HPFS_SPARE2_DASD_DIRTY   0x20

typedef struct _HPFS_SPAREBLOCK {
    ULONG       Magic;                  /* HPFS_SPARE_MAGIC */
    ULONG       Magic1;                 /* HPFS_SPARE_MAGIC1 */
    UCHAR       Flags;
    UCHAR       Flags2;
    UCHAR       MmContiguity;
    UCHAR       Unused;
    ULONG       HotfixMap;              /* 4 sectors: bad[n], replacement[n], fnode[n] */
    ULONG       HotfixUsed;
    ULONG       HotfixMax;
    ULONG       SpareDnodesFree;
    ULONG       SpareDnodes;
    ULONG       CodePageDir;
    ULONG       CodePages;
    ULONG       SuperChecksum;
    ULONG       SpareChecksum;          /* computed with this field zero */
    ULONG       Zero1[15];
    ULONG       SpareDnode[101];        /* SpareDnodes entries used */
} HPFS_SPAREBLOCK, *PHPFS_SPAREBLOCK;

/* ------------------------------------------------------------------ */
/* Code pages                                                           */
/* ------------------------------------------------------------------ */

typedef struct _HPFS_CPDIR_ENTRY {
    USHORT      Index;                  /* code page index used in dirents */
    USHORT      CodePage;               /* e.g. 850 */
    ULONG       Checksum;               /* checksum of the data entry */
    ULONG       DataSector;             /* sector holding the case table */
    USHORT      DataIndex;              /* entry in that sector (0..2) */
    USHORT      Unknown;
} HPFS_CPDIR_ENTRY, *PHPFS_CPDIR_ENTRY;

typedef struct _HPFS_CPDIR {
    ULONG       Magic;                  /* HPFS_CPDIR_MAGIC */
    ULONG       Count;
    ULONG       Zero[2];
    HPFS_CPDIR_ENTRY Entry[31];
} HPFS_CPDIR, *PHPFS_CPDIR;

typedef struct _HPFS_CPDATA_ENTRY {
    USHORT      Index;
    USHORT      CodePage;
    USHORT      Unknown;                /* DBCS range count; 0 for SBCS */
    UCHAR       UpperMap[128];          /* uppercase of bytes 0x80..0xFF */
    USHORT      Zero;                   /* DBCS range terminator */
} HPFS_CPDATA_ENTRY, *PHPFS_CPDATA_ENTRY;

typedef struct _HPFS_CPDATA {
    ULONG       Magic;                  /* HPFS_CPDATA_MAGIC */
    ULONG       Count;                  /* entries in this sector (1..3) */
    ULONG       Checksum[3];            /* checksum of each entry */
    USHORT      Offset[3];              /* byte offset of each entry */
    /* entries follow */
} HPFS_CPDATA, *PHPFS_CPDATA;

#define HPFS_CPDATA_ENTRY_SIZE  136     /* 6 + 128 + 2; also what the checksum covers */

/* ------------------------------------------------------------------ */
/* Allocation B+ tree (fnode and anode)                                 */
/* ------------------------------------------------------------------ */

#define HPFS_BT_INTERNAL        0x80    /* entries are subtree pointers */
#define HPFS_BT_BINARY_SEARCH   0x40    /* hint: >= some entries, binary search */
#define HPFS_BT_FNODE_PARENT    0x20    /* this anode's parent is an fnode */

typedef struct _HPFS_BTREE_HEADER {
    UCHAR       Flags;
    UCHAR       Fill[3];
    UCHAR       FreeCount;
    UCHAR       UsedCount;
    USHORT      FirstFree;              /* byte offset of the first free slot, from the header */
} HPFS_BTREE_HEADER, *PHPFS_BTREE_HEADER;

typedef struct _HPFS_EXTENT {           /* leaf entry */
    ULONG       FileSector;             /* first sector of the run in the file */
    ULONG       Length;                 /* sectors */
    ULONG       DiskSector;
} HPFS_EXTENT, *PHPFS_EXTENT;

typedef struct _HPFS_BTREE_NODE {       /* internal entry */
    ULONG       FileSector;             /* first file sector NOT in the subtree; last = 0xFFFFFFFF */
    ULONG       Down;                   /* anode sector */
} HPFS_BTREE_NODE, *PHPFS_BTREE_NODE;

#define HPFS_FNODE_LEAVES       8
#define HPFS_FNODE_NODES        12
#define HPFS_ANODE_LEAVES       40
#define HPFS_ANODE_NODES        60

typedef struct _HPFS_ANODE {
    ULONG       Magic;                  /* HPFS_ANODE_MAGIC */
    ULONG       Self;
    ULONG       Up;                     /* parent anode or fnode */
    HPFS_BTREE_HEADER Header;
    union {
        HPFS_EXTENT     Extent[HPFS_ANODE_LEAVES];
        HPFS_BTREE_NODE Node[HPFS_ANODE_NODES];
    } u;
    ULONG       Fill[3];
} HPFS_ANODE, *PHPFS_ANODE;

/* ------------------------------------------------------------------ */
/* Fnode                                                                */
/* ------------------------------------------------------------------ */

/* Fnode Flags (16 bits) */
#define HPFS_FNODE_EA_ANODE     0x0002  /* EaSector is an anode (ALSEC) */
#define HPFS_FNODE_DIR          0x0100

#define HPFS_FNODE_EA_OFFSET    0xC4    /* start of the in-fnode EA/ACL area */
#define HPFS_FNODE_EA_SPACE     (HPFS_SECTOR_SIZE - HPFS_FNODE_EA_OFFSET)   /* 316 */

typedef struct _HPFS_FNODE {
    ULONG       Magic;                  /* HPFS_FNODE_MAGIC */
    ULONG       Zero1[2];               /* history (unused) */
    UCHAR       NameLength;             /* full length of the name */
    UCHAR       Name[15];               /* first 15 bytes of the name */
    ULONG       Parent;                 /* fnode of the containing directory */
    ULONG       AclSizeExternal;
    ULONG       AclSector;
    USHORT      AclSizeInternal;        /* bytes of ACL at EaOffset */
    UCHAR       AclAnode;               /* AclSector is an anode */
    UCHAR       Zero2;
    ULONG       EaSizeExternal;         /* bytes of EAs at EaSector */
    ULONG       EaSector;
    USHORT      EaSizeInternal;         /* bytes of EAs after the internal ACL */
    USHORT      Flags;
    HPFS_BTREE_HEADER Header;
    union {
        HPFS_EXTENT     Extent[HPFS_FNODE_LEAVES];
        HPFS_BTREE_NODE Node[HPFS_FNODE_NODES];
    } u;
    ULONG       FileSize;               /* valid data length for files */
    ULONG       NeedEaCount;            /* EAs with the NEEDEA flag */
    UCHAR       UserId[16];             /* HPFS386: owner */
    USHORT      EaOffset;               /* 0xC4: start of the internal ACL+EA area */
    UCHAR       DasdThreshold;          /* HPFS386 DASD limits (directories) */
    UCHAR       DasdDelta;
    ULONG       DasdLimit;              /* sectors, 0 = none */
    ULONG       DasdUsage;
    UCHAR       Ea[HPFS_FNODE_EA_SPACE];
} HPFS_FNODE, *PHPFS_FNODE;

/* ------------------------------------------------------------------ */
/* Dnode and directory entries                                          */
/* ------------------------------------------------------------------ */

#define HPFS_DNODE_SIZE         2048
#define HPFS_DNODE_SECTORS      4
#define HPFS_DNODE_HEADER       20

#define HPFS_DNODE_ROOT         0x01    /* in RootFlag: the top dnode of a directory */

typedef struct _HPFS_DNODE {
    ULONG       Magic;                  /* HPFS_DNODE_MAGIC */
    ULONG       FirstFree;              /* byte offset of the first unused byte */
    UCHAR       RootFlag;               /* bit 0 = root dnode; bits 1-7: change count */
    UCHAR       Zero[3];
    ULONG       Up;                     /* parent dnode, or the directory fnode for a root */
    ULONG       Self;
    UCHAR       Entries[HPFS_DNODE_SIZE - HPFS_DNODE_HEADER];
} HPFS_DNODE, *PHPFS_DNODE;

/* Dirent Flags */
#define HPFS_DE_SPECIAL         0x01    /* "\1\1" (..) and the end entry */
#define HPFS_DE_ACL             0x02
#define HPFS_DE_DOWN            0x04    /* a down pointer ends the entry */
#define HPFS_DE_END             0x08    /* last entry of the dnode */
#define HPFS_DE_EA              0x10    /* the file has EAs */
#define HPFS_DE_XPERM           0x20    /* HPFS386 extended permissions */
#define HPFS_DE_EXPLICIT_ACL    0x40    /* HPFS386 */
#define HPFS_DE_NEEDEA          0x80

/* Dirent attributes (DOS attributes plus one) */
#define HPFS_ATTR_READONLY      0x01
#define HPFS_ATTR_HIDDEN        0x02
#define HPFS_ATTR_SYSTEM        0x04
#define HPFS_ATTR_DIRECTORY     0x10
#define HPFS_ATTR_ARCHIVE       0x20
#define HPFS_ATTR_LONGNAME      0x40    /* name is not a valid 8.3 name */

typedef struct _HPFS_DIRENT {
    USHORT      Length;                 /* whole entry, multiple of 4 */
    UCHAR       Flags;
    UCHAR       Attributes;
    ULONG       Fnode;
    ULONG       WriteTime;              /* seconds since 1970, local time */
    ULONG       FileSize;
    ULONG       AccessTime;
    ULONG       CreateTime;
    ULONG       EaSize;                 /* bytes of external EAs */
    UCHAR       AclCount;               /* HPFS386: bits 0-2 count of ACEs inline */
    UCHAR       CodePage;               /* code page index; bit 7: DBCS name */
    UCHAR       NameLength;
    UCHAR       Name[1];                /* NameLength bytes, then pad to 4, then [ULONG Down] */
} HPFS_DIRENT, *PHPFS_DIRENT;

#define HPFS_DIRENT_FIXED       31      /* bytes before the name */
#define HPFS_MAX_NAME           254

#define HpfsDirentSize(NameLen, Down) \
    ((((HPFS_DIRENT_FIXED + (NameLen)) + 3) & ~3) + ((Down) ? 4 : 0))

#define HpfsDirentDown(De) \
    (*(ULONG UNALIGNED_PTR *)((PUCHAR)(De) + (De)->Length - 4))

#define HpfsNextDirent(De) \
    ((PHPFS_DIRENT)((PUCHAR)(De) + (De)->Length))

#ifndef UNALIGNED_PTR
#define UNALIGNED_PTR
#endif

/* ------------------------------------------------------------------ */
/* Extended attributes (FEA format, as in OS/2 FEA2 without the link)   */
/* ------------------------------------------------------------------ */

#define HPFS_EA_NEEDEA          0x80
#define HPFS_EA_INDIRECT        0x01    /* value is a run descriptor (large EA) */
#define HPFS_EA_ANODE           0x02    /* ... whose sector is an anode */

typedef struct _HPFS_EA {
    UCHAR       Flags;
    UCHAR       NameLength;             /* without the terminating 0 */
    UCHAR       ValueLengthLo;          /* USHORT, but unaligned */
    UCHAR       ValueLengthHi;
    /* Name[NameLength], 0, Value[ValueLength] */
} HPFS_EA, *PHPFS_EA;

#define HpfsEaValueLength(Ea)   ((ULONG)(Ea)->ValueLengthLo | ((ULONG)(Ea)->ValueLengthHi << 8))
#define HpfsEaSize(Ea)          (4 + (Ea)->NameLength + 1 + HpfsEaValueLength(Ea))

/* Indirect EA value: the value lives in its own sectors */
typedef struct _HPFS_EA_INDIRECT_VALUE {
    ULONG       Length;
    ULONG       Sector;
} HPFS_EA_INDIRECT_VALUE;

/* ------------------------------------------------------------------ */
/* Hotfix map (4 sectors at HotfixMap)                                  */
/*   ULONG Bad[HotfixMax]; ULONG Spare[HotfixMax]; ULONG Fnode[HotfixMax] */
/* ------------------------------------------------------------------ */

#define HPFS_MAX_HOTFIX         169     /* 3 * 169 * 4 <= 2048 */

#pragma pack()

#endif /* _HPFSDISK_H_ */
