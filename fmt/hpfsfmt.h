/* HPFSNT - formatter core interface (fmt/hpfsfmtc.c) */
#ifndef _HPFSFMT_H_
#define _HPFSFMT_H_

typedef struct _HPFS_FORMAT_OPTIONS {
    ULONG           Sectors;            /* partition length */
    ULONG           HiddenSectors;      /* partition start (for the boot sector) */
    ULONG           SectorsPerTrack;    /* geometry for the BPB, 0 = 63 */
    ULONG           Heads;              /* 0 = 255 */
    ULONG           Serial;             /* 0 = derive one */
    const char     *Label;
    USHORT          CodePage;           /* 0 = 850 */
    const UCHAR    *CaseTable;          /* 128 bytes, NULL = country 001 table */
    BOOLEAN         Hpfs386;            /* mark HPFS386 (function version 3) */
    const UCHAR    *BootCode;           /* sectors 0..15 image, NULL = not bootable */
    ULONG           BootCodeSize;
} HPFS_FORMAT_OPTIONS;

int HpfsFormatVolume(PHPFS_IMAGE Img, const HPFS_FORMAT_OPTIONS *Opt, char *Error, unsigned ErrorSize);
const UCHAR *HpfsDefaultCaseTable(VOID);

#endif
