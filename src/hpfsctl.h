/*
 * HPFSNT - private file system controls (driver and tools)
 *
 *   HPFS_FSCTL_QUERY_DASD   on a directory handle: its HPFS386 DASD limit
 *                           and usage (output HPFS_DASD_INFO)
 *   HPFS_FSCTL_SET_DASD     on a directory handle opened for writing: set or
 *                           remove (Limit 0) the limit (input HPFS_DASD_INFO)
 *   HPFS_FSCTL_QUERY_VOLUME on any handle of the volume (output
 *                           HPFS_VOLUME_INFO)
 */

#ifndef _HPFSCTL_H_
#define _HPFSCTL_H_

#define HPFS_FSCTL_QUERY_DASD   CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8A0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define HPFS_FSCTL_SET_DASD     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8A1, METHOD_BUFFERED, FILE_WRITE_DATA)
#define HPFS_FSCTL_QUERY_VOLUME CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8A2, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _HPFS_DASD_INFO {
    ULONG   Limit;          /* sectors, 0 = none */
    ULONG   Usage;          /* sectors */
    ULONG   Threshold;      /* percent: warn above */
    ULONG   Delta;          /* percent: warn again every */
    ULONG   Active;         /* the volume enforces limits */
} HPFS_DASD_INFO, *PHPFS_DASD_INFO;

typedef struct _HPFS_VOLUME_INFO {
    ULONG   TotalSectors;
    ULONG   FreeSectors;
    ULONG   Version;        /* superblock version / function version */
    ULONG   FuncVersion;
    ULONG   SpareFlags;
    ULONG   SpareFlags2;
    ULONG   Hpfs386;
    ULONG   DasdActive;
    ULONG   CodePage;
    ULONG   HotfixCount;
    ULONG   SpareDnodes;
    ULONG   SpareDnodesFree;
    ULONG   DirBandDnodes;
    ULONG   ReadOnly;
} HPFS_VOLUME_INFO, *PHPFS_VOLUME_INFO;

#endif
