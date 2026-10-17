/*
 * HPFSNT - the formatter core: lays out an empty HPFS volume.
 *
 * Layout (all runs of 4 sectors start on a multiple of 4):
 *
 *   0..15      boot sector and boot loader (boot/hpfsboot.S)
 *   16, 17     superblock, spareblock
 *   20..23     hotfix map; then the hotfix spare sectors
 *   bitmaps    one 2 KB bitmap per 8 MB band: at the end of even bands
 *              and the start of odd bands, so they pair up across the
 *              band boundary (a partial last band has it at its start)
 *   middle     in the middle band: directory band bitmap, root fnode,
 *              root dnode, the directory band, spare dnodes, bitmap
 *              list, bad block list, HPFS386 user id table, code page
 *              data and directory
 *
 * Keeping the directory band and the root in the middle of the volume
 * keeps directory lookups close to the data, as OS/2 does.
 */

#include "hpfsusr.h"
#include "hpfssup.h"
#include "hpfsimg.h"
#include "hpfsfmt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The standard country 001 case table for code pages 437 and 850 (bytes 0x80..0xFF) */
static const UCHAR UsUpper[128] = {
    0x80,0x9A,0x45,0x41,0x8E,0x41,0x8F,0x80,0x45,0x45,0x45,0x49,0x49,0x49,0x8E,0x8F,
    0x90,0x92,0x92,0x4F,0x99,0x4F,0x55,0x55,0x59,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
    0x41,0x49,0x4F,0x55,0xA5,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF,
};

const UCHAR *
HpfsDefaultCaseTable(VOID)
{
    return UsUpper;
}

static VOID
Put32(PUCHAR P, ULONG V)
{
    P[0] = (UCHAR)V; P[1] = (UCHAR)(V >> 8); P[2] = (UCHAR)(V >> 16); P[3] = (UCHAR)(V >> 24);
}

typedef struct _FMT {
    PHPFS_IMAGE Img;
    ULONG       Total;
    ULONG       Bands;
    PUCHAR      Bitmaps;            /* Bands * 2048 */
    PULONG      BandBitmap;
} FMT;

static VOID
MarkUsed(FMT *F, ULONG First, ULONG Count)
{
    ULONG S;
    for (S = First; S < First + Count; S++) {
        if (S >= F->Bands * HPFS_BAND_SECTORS) break;
        F->Bitmaps[S >> 3] &= (UCHAR)~(1 << (S & 7));
    }
}

static int
WriteZero(FMT *F, ULONG Sector, ULONG Count)
{
    static UCHAR Zero[64 * 512];
    while (Count) {
        ULONG N = Count > 64 ? 64 : Count;
        if (ImgWrite(F->Img, Sector, N, Zero) != 0) return -1;
        Sector += N;
        Count -= N;
    }
    return 0;
}

int
HpfsFormatVolume(PHPFS_IMAGE Img, const HPFS_FORMAT_OPTIONS *Opt, char *Error, unsigned ErrorSize)
{
    FMT F;
    UCHAR Sec[2048];
    ULONG Mid, MidStart, MidEnd, P, I;
    ULONG HotfixMax, HotfixSpares, Lower;
    ULONG DirBmp, RootFnode, RootDnode, DirBand, DirBandSectors, SpareDn, SpareCount;
    ULONG BmList, BmListSectors, BadList, UidTable, CpData, CpDir;
    ULONG Now = HpfsHostNow();
    PHPFS_SUPERBLOCK Super;
    PHPFS_SPAREBLOCK Spare;
    PHPFS_FNODE Fn;
    PHPFS_DNODE Dn;
    PHPFS_DIRENT De;
    PHPFS_BOOT_SECTOR Boot;
    const UCHAR *Case = Opt->CaseTable ? Opt->CaseTable : UsUpper;
    int Rc = -1;

    memset(&F, 0, sizeof(F));
    F.Img = Img;
    F.Total = Opt->Sectors & ~3UL;
    if (F.Total < 1024) {
        snprintf(Error, ErrorSize, "volume too small for HPFS (%lu sectors, need 1024)", (unsigned long)F.Total);
        return -1;
    }
    if (F.Total > HPFS_MAX_SECTORS) {
        snprintf(Error, ErrorSize, "volume too large for HPFS (limit 64 GB)");
        return -1;
    }
    F.Bands = (F.Total + HPFS_BAND_SECTORS - 1) / HPFS_BAND_SECTORS;
    F.Bitmaps = (PUCHAR)malloc(F.Bands * 2048);
    F.BandBitmap = (PULONG)calloc(F.Bands, sizeof(ULONG));
    if (F.Bitmaps == NULL || F.BandBitmap == NULL) {
        snprintf(Error, ErrorSize, "out of memory");
        goto Out;
    }
    memset(F.Bitmaps, 0xFF, F.Bands * 2048);
    MarkUsed(&F, F.Total, F.Bands * HPFS_BAND_SECTORS - F.Total);

    /* Fixed area: boot, super, spare, hotfix map and spares */
    HotfixMax = F.Total >= 65536 ? 100 : (F.Total / 1024 < 4 ? 4 : F.Total / 1024);
    if (HotfixMax > 100) HotfixMax = 100;
    HotfixSpares = 24;
    Lower = (HotfixSpares + HotfixMax + 3) & ~3UL;
    MarkUsed(&F, 0, Lower);

    /* Band bitmaps */
    for (I = 0; I < F.Bands; I++) {
        ULONG Start = I * HPFS_BAND_SECTORS;
        ULONG End = Start + HPFS_BAND_SECTORS;
        if (I == 0 && End > F.Total) {
            F.BandBitmap[I] = Lower;
        } else if ((I & 1) == 0 && End <= F.Total) {
            F.BandBitmap[I] = End - 4;
        } else {
            F.BandBitmap[I] = (I == 0) ? Lower : Start;
        }
        MarkUsed(&F, F.BandBitmap[I], 4);
    }

    /* The middle area */
    Mid = F.Bands / 2;
    MidStart = Mid * HPFS_BAND_SECTORS;
    MidEnd = MidStart + HPFS_BAND_SECTORS;
    if (MidEnd > F.Total) MidEnd = F.Total;
    if (F.BandBitmap[Mid] == MidStart || (Mid == 0 && F.BandBitmap[0] == Lower)) {
        P = F.BandBitmap[Mid] + 4;
    } else {
        P = MidStart;
    }
    if (Mid == 0 && P < Lower + 4) P = Lower + 4;
    if (F.BandBitmap[Mid] == MidEnd - 4 && MidEnd - 4 > P) MidEnd -= 4;

    BmListSectors = ((F.Bands * 4 + 511) / 512 + 3) & ~3UL;
    SpareCount = 20;

    DirBandSectors = (F.Total / 50) & ~3UL;
    {
        ULONG Fixed = 4 + 4 + SpareCount * 4 + BmListSectors + 4 + 8 + 3;
        ULONG Avail = (MidEnd > P + Fixed) ? MidEnd - P - Fixed : 0;
        if (DirBandSectors > Avail) DirBandSectors = Avail & ~3UL;
        if (DirBandSectors > 16384 * 4) DirBandSectors = 16384 * 4;
        if (DirBandSectors < 16) {
            snprintf(Error, ErrorSize, "no room for the directory band");
            goto Out;
        }
    }

    DirBmp = P;            P += 4;
    RootDnode = P;         P += 4;
    DirBand = P;           P += DirBandSectors;
    SpareDn = P;           P += SpareCount * 4;
    BmList = P;            P += BmListSectors;
    BadList = P;           P += 4;
    UidTable = P;          P += 8;
    CpData = P;            P += 1;
    CpDir = P;             P += 1;
    RootFnode = P;         P += 1;
    MarkUsed(&F, DirBmp, P - DirBmp);

    /* Clear the metadata areas */
    if (WriteZero(&F, 0, Lower) != 0 || WriteZero(&F, DirBmp, P - DirBmp) != 0) {
        snprintf(Error, ErrorSize, "write error");
        goto Out;
    }

    /* Bitmaps */
    for (I = 0; I < F.Bands; I++) {
        if (ImgWrite(Img, F.BandBitmap[I], 4, F.Bitmaps + I * 2048) != 0) {
            snprintf(Error, ErrorSize, "write error (bitmap)");
            goto Out;
        }
    }

    /* Bitmap list */
    {
        PUCHAR L = (PUCHAR)calloc(BmListSectors, 512);
        for (I = 0; I < F.Bands; I++) Put32(L + I * 4, F.BandBitmap[I]);
        Rc = ImgWrite(Img, BmList, BmListSectors, L);
        free(L);
        if (Rc != 0) { snprintf(Error, ErrorSize, "write error"); Rc = -1; goto Out; }
        Rc = -1;
    }

    /* Directory band bitmap: one bit per dnode, 1 = free */
    memset(Sec, 0, sizeof(Sec));
    for (I = 0; I < DirBandSectors / 4; I++) Sec[I >> 3] |= (UCHAR)(1 << (I & 7));
    if (ImgWrite(Img, DirBmp, 4, Sec) != 0) goto WErr;

    /* Root fnode */
    memset(Sec, 0, 512);
    Fn = (PHPFS_FNODE)Sec;
    Fn->Magic = HPFS_FNODE_MAGIC;
    Fn->Parent = RootFnode;
    Fn->Flags = HPFS_FNODE_DIR;
    Fn->Header.FreeCount = HPFS_FNODE_LEAVES - 1;
    Fn->Header.UsedCount = 1;
    Fn->Header.FirstFree = sizeof(HPFS_BTREE_HEADER) + sizeof(HPFS_EXTENT);
    Fn->u.Extent[0].DiskSector = RootDnode;
    Fn->EaOffset = HPFS_FNODE_EA_OFFSET;
    if (ImgWrite(Img, RootFnode, 1, Sec) != 0) goto WErr;

    /* Root dnode: ".." (pointing at the root itself) and the end entry */
    memset(Sec, 0, 2048);
    Dn = (PHPFS_DNODE)Sec;
    Dn->Magic = HPFS_DNODE_MAGIC;
    Dn->RootFlag = HPFS_DNODE_ROOT;
    Dn->Up = RootFnode;
    Dn->Self = RootDnode;
    De = (PHPFS_DIRENT)Dn->Entries;
    De->Length = 36;
    De->Flags = HPFS_DE_SPECIAL;
    De->Attributes = HPFS_ATTR_DIRECTORY;
    De->Fnode = RootFnode;
    De->WriteTime = De->AccessTime = De->CreateTime = Now;
    De->NameLength = 2;
    De->Name[0] = 1; De->Name[1] = 1;
    De = (PHPFS_DIRENT)((PUCHAR)De + 36);
    De->Length = 32;
    De->Flags = HPFS_DE_SPECIAL | HPFS_DE_END;
    De->WriteTime = De->AccessTime = De->CreateTime = Now;
    De->NameLength = 1;
    De->Name[0] = 0xFF;
    Dn->FirstFree = HPFS_DNODE_HEADER + 36 + 32;
    if (ImgWrite(Img, RootDnode, 4, Sec) != 0) goto WErr;

    /* Code page data and directory */
    memset(Sec, 0, 512);
    {
        PHPFS_CPDATA D = (PHPFS_CPDATA)Sec;
        PHPFS_CPDATA_ENTRY E = (PHPFS_CPDATA_ENTRY)(Sec + 0x1A);
        PHPFS_CPDIR C;
        ULONG Ck;
        D->Magic = HPFS_CPDATA_MAGIC;
        D->Count = 1;
        D->Offset[0] = 0x1A;
        E->Index = 1;
        E->CodePage = Opt->CodePage ? Opt->CodePage : 850;
        E->Unknown = 0;
        memcpy(E->UpperMap, Case, 128);
        E->Zero = 0;
        Ck = HpfsChecksum(E, HPFS_CPDATA_ENTRY_SIZE);
        D->Checksum[0] = Ck;
        if (ImgWrite(Img, CpData, 1, Sec) != 0) goto WErr;

        memset(Sec, 0, 512);
        C = (PHPFS_CPDIR)Sec;
        C->Magic = HPFS_CPDIR_MAGIC;
        C->Count = 1;
        C->Entry[0].Index = 1;
        C->Entry[0].CodePage = Opt->CodePage ? Opt->CodePage : 850;
        C->Entry[0].Checksum = Ck;
        C->Entry[0].DataSector = CpData;
        C->Entry[0].DataIndex = 0;
        if (ImgWrite(Img, CpDir, 1, Sec) != 0) goto WErr;
    }

    /* Hotfix map: no hotfixes yet, the spares listed */
    memset(Sec, 0, 2048);
    for (I = 0; I < HotfixMax; I++) Put32(Sec + (HotfixMax + I) * 4, HotfixSpares + I);
    if (ImgWrite(Img, 20, 4, Sec) != 0) goto WErr;

    /* Superblock and spareblock */
    {
        UCHAR SbBuf[512], SpBuf[512];
        memset(SbBuf, 0, 512);
        memset(SpBuf, 0, 512);
        Super = (PHPFS_SUPERBLOCK)SbBuf;
        Spare = (PHPFS_SPAREBLOCK)SpBuf;
        Super->Magic = HPFS_SUPER_MAGIC;
        Super->Magic1 = HPFS_SUPER_MAGIC1;
        Super->Version = 2;
        Super->FuncVersion = Opt->Hpfs386 ? 3 : 2;
        Super->RootFnode = RootFnode;
        Super->TotalSectors = F.Total;
        Super->BitmapList = BmList;
        Super->BadBlockList = BadList;
        Super->DirBandSectors = DirBandSectors;
        Super->DirBandStart = DirBand;
        Super->DirBandEnd = DirBand + DirBandSectors - 1;
        Super->DirBandBitmap = DirBmp;
        Super->UserIdTable = UidTable;

        Spare->Magic = HPFS_SPARE_MAGIC;
        Spare->Magic1 = HPFS_SPARE_MAGIC1;
        Spare->Flags = 0;
        Spare->Flags2 = 0;
        Spare->HotfixMap = 20;
        Spare->HotfixUsed = 0;
        Spare->HotfixMax = HotfixMax;
        Spare->SpareDnodesFree = SpareCount;
        Spare->SpareDnodes = SpareCount;
        Spare->CodePageDir = CpDir;
        Spare->CodePages = 1;
        for (I = 0; I < SpareCount; I++) Spare->SpareDnode[I] = SpareDn + I * 4;
        HpfsUpdateChecksums(Super, Spare);
        if (ImgWrite(Img, HPFS_SUPER_SECTOR, 1, SbBuf) != 0) goto WErr;
        if (ImgWrite(Img, HPFS_SPARE_SECTOR, 1, SpBuf) != 0) goto WErr;
    }

    /* Boot sector (+ loader in sectors 1..15) */
    {
        UCHAR BootBuf[16 * 512];
        memset(BootBuf, 0, sizeof(BootBuf));
        if (Opt->BootCode && Opt->BootCodeSize <= sizeof(BootBuf)) {
            memcpy(BootBuf, Opt->BootCode, Opt->BootCodeSize);
        } else {
            /* not bootable: jmp to a halt */
            BootBuf[0] = 0xEB; BootBuf[1] = 0x3C; BootBuf[2] = 0x90;
            BootBuf[62] = 0xFA; BootBuf[63] = 0xF4; BootBuf[64] = 0xEB; BootBuf[65] = 0xFD;
        }
        Boot = (PHPFS_BOOT_SECTOR)BootBuf;
        memcpy(Boot->Oem, "HPFSNT10", 8);
        Boot->BytesPerSector = 512;
        Boot->SectorsPerCluster = 0;
        Boot->ReservedSectors = 1;
        Boot->NumberOfFats = 0;
        Boot->RootEntries = 512;
        Boot->TotalSectors16 = (USHORT)(Opt->Sectors < 65536 ? Opt->Sectors : 0);
        Boot->Media = 0xF8;
        Boot->FatSize = 0;
        Boot->SectorsPerTrack = (USHORT)(Opt->SectorsPerTrack ? Opt->SectorsPerTrack : 63);
        Boot->Heads = (USHORT)(Opt->Heads ? Opt->Heads : 255);
        Boot->HiddenSectors = Opt->HiddenSectors;
        Boot->TotalSectors32 = Opt->Sectors < 65536 ? 0 : Opt->Sectors;
        Boot->DriveNumber = 0x80;
        Boot->BootSignature = 0x29;
        Boot->SerialNumber = Opt->Serial ? Opt->Serial : (Now * 2654435761u) ^ F.Total;
        memset(Boot->VolumeLabel, ' ', 11);
        if (Opt->Label && Opt->Label[0]) {
            for (I = 0; I < 11 && Opt->Label[I]; I++) {
                UCHAR C = (UCHAR)Opt->Label[I];
                Boot->VolumeLabel[I] = (C >= 'a' && C <= 'z') ? C - 32 : (C >= 0x80 ? Case[C - 0x80] : C);
            }
        } else {
            memcpy(Boot->VolumeLabel, "NO NAME    ", 11);
        }
        memcpy(Boot->FileSystemType, "HPFS    ", 8);
        Boot->Signature = 0xAA55;
        if (ImgWrite(Img, 0, 16, BootBuf) != 0) goto WErr;
    }

    Rc = 0;
    goto Out;
WErr:
    snprintf(Error, ErrorSize, "write error");
    Rc = -1;
Out:
    free(F.Bitmaps);
    free(F.BandBitmap);
    return Rc;
}
