/*
 * HPFSNT - the shared HPFS engine. See hpfssup.h.
 *
 * Builds three ways: into the driver (HPFS_KERNEL, with ntifs.h), into the
 * Windows tools and into the Linux tools/tests (hpfsusr.h).
 */

#ifdef HPFS_KERNEL
#include "hpfs.h"
#else
#include "hpfsusr.h"
#include "hpfssup.h"
#endif

VOID
HpfsZero(PVOID P, ULONG N)
{
    PUCHAR B = (PUCHAR)P;
    while (N--) *B++ = 0;
}

VOID
HpfsCopy(PVOID D, const VOID *S, ULONG N)
{
    PUCHAR A = (PUCHAR)D;
    const UCHAR *B = (const UCHAR *)S;
    if (A < B) {
        while (N--) *A++ = *B++;
    } else if (A > B) {
        A += N; B += N;
        while (N--) *--A = *--B;
    }
}

/* ------------------------------------------------------------------ */
/* Checksums                                                           */
/* ------------------------------------------------------------------ */

/* x = rol7(x + byte) over the data: the superblock / spareblock / code page checksum */
ULONG
HpfsChecksum(const VOID *Data, ULONG Length)
{
    const UCHAR *P = (const UCHAR *)Data;
    ULONG X = 0;
    ULONG I;

    for (I = 0; I < Length; I++) {
        X += P[I];
        X = (X << 7) | (X >> 25);
    }
    return X;
}

VOID
HpfsUpdateChecksums(PHPFS_SUPERBLOCK Super, PHPFS_SPAREBLOCK Spare)
{
    Spare->SuperChecksum = HpfsChecksum(Super, HPFS_SECTOR_SIZE);
    Spare->SpareChecksum = 0;
    Spare->SpareChecksum = HpfsChecksum(Spare, HPFS_SECTOR_SIZE);
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

VOID
HpfsSetCaseTable(PHPFS_VOL Vol, const UCHAR *UpperMap128)
{
    ULONG I;

    for (I = 0; I < 128; I++) {
        Vol->Upcase[I] = (UCHAR)((I >= 'a' && I <= 'z') ? I - 32 : I);
    }
    for (I = 0; I < 128; I++) {
        Vol->Upcase[128 + I] = UpperMap128 ? UpperMap128[I] : (UCHAR)(128 + I);
    }
}

LONG
HpfsCompareNames(PHPFS_VOL Vol, const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen)
{
    ULONG N = MIN_UL(ALen, BLen);
    ULONG I;

    for (I = 0; I < N; I++) {
        UCHAR X = Vol->Upcase[A[I]];
        UCHAR Y = Vol->Upcase[B[I]];
        if (X != Y) {
            return (LONG)X - (LONG)Y;
        }
    }
    return (LONG)ALen - (LONG)BLen;
}

BOOLEAN
HpfsIsLegalName(const UCHAR *Name, ULONG Length)
{
    ULONG I;

    if (Length == 0 || Length > HPFS_MAX_NAME) {
        return FALSE;
    }
    for (I = 0; I < Length; I++) {
        UCHAR C = Name[I];
        if (C < 0x20 || C == '"' || C == '*' || C == '/' || C == ':' || C == '<' ||
            C == '>' || C == '?' || C == '\\' || C == '|') {
            return FALSE;
        }
    }
    /* "." and ".." are not names; a name may not end in a dot or space */
    if (Name[Length - 1] == '.' || Name[Length - 1] == ' ') {
        return FALSE;
    }
    return TRUE;
}

BOOLEAN
HpfsIsDos83(const UCHAR *Name, ULONG Length)
{
    ULONG I, Base = 0, Ext = 0;
    BOOLEAN Dot = FALSE;

    if (Length == 0 || Length > 12) {
        return FALSE;
    }
    for (I = 0; I < Length; I++) {
        UCHAR C = Name[I];
        if (C == '.') {
            if (Dot || I == 0) return FALSE;
            Dot = TRUE;
            continue;
        }
        if (C <= ' ' || C == '+' || C == ',' || C == ';' || C == '=' || C == '[' || C == ']') {
            return FALSE;
        }
        if (Dot) Ext++; else Base++;
    }
    return (BOOLEAN)(Base >= 1 && Base <= 8 && Ext <= 3 && !(Dot && Ext == 0));
}

/* ------------------------------------------------------------------ */
/* Volume                                                              */
/* ------------------------------------------------------------------ */

BOOLEAN
HpfsIsBootSector(const VOID *Sector)
{
    const HPFS_BOOT_SECTOR *B = (const HPFS_BOOT_SECTOR *)Sector;
    static const UCHAR Tag[8] = { 'H', 'P', 'F', 'S', ' ', ' ', ' ', ' ' };
    ULONG I;

    if (B->BytesPerSector != HPFS_SECTOR_SIZE) {
        return FALSE;
    }
    for (I = 0; I < 8; I++) {
        if (B->FileSystemType[I] != Tag[I]) {
            return FALSE;
        }
    }
    return TRUE;
}

static HPFS_STATUS
HpfsLoadCodePage(PHPFS_VOL Vol, ULONG DirSector, ULONG Count)
{
    HPFS_BUF Dir, Data;
    PHPFS_CPDIR CpDir;
    PHPFS_CPDATA CpData;
    PHPFS_CPDATA_ENTRY Ent;
    HPFS_STATUS Status;
    ULONG Off;

    HpfsSetCaseTable(Vol, NULL);
    Vol->CodePage = 0;
    Vol->CodePageIndex = 0;

    if (Count == 0 || DirSector == 0 || DirSector >= Vol->TotalSectors) {
        return HPFS_OK;         /* no table: ASCII-only case folding */
    }

    Status = HpfsHostMap(Vol, DirSector, 1, HPFS_MAP_READ, &Dir);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    CpDir = (PHPFS_CPDIR)Dir.Data;
    if (CpDir->Magic != HPFS_CPDIR_MAGIC || CpDir->Count == 0 ||
        CpDir->Entry[0].DataSector >= Vol->TotalSectors || CpDir->Entry[0].DataIndex > 2) {
        HpfsHostUnmap(Vol, &Dir);
        return HPFS_OK;
    }
    Vol->CodePage = CpDir->Entry[0].CodePage;
    Vol->CodePageIndex = 0;     /* entries refer to code pages by their slot in the directory */

    Status = HpfsHostMap(Vol, CpDir->Entry[0].DataSector, 1, HPFS_MAP_READ, &Data);
    if (NT_SUCCESS(Status)) {
        CpData = (PHPFS_CPDATA)Data.Data;
        Off = CpData->Offset[CpDir->Entry[0].DataIndex];
        if (CpData->Magic == HPFS_CPDATA_MAGIC && Off >= sizeof(HPFS_CPDATA) &&
            Off + HPFS_CPDATA_ENTRY_SIZE <= HPFS_SECTOR_SIZE) {
            Ent = (PHPFS_CPDATA_ENTRY)(Data.Data + Off);
            HpfsSetCaseTable(Vol, Ent->UpperMap);
        }
        HpfsHostUnmap(Vol, &Data);
    }
    HpfsHostUnmap(Vol, &Dir);
    return HPFS_OK;
}

static HPFS_STATUS
HpfsLoadHotfixes(PHPFS_VOL Vol, ULONG MapSector, ULONG Used, ULONG Max)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    PULONG Map;
    ULONG I;

    Vol->HotfixCount = 0;
    if (Used == 0) {
        return HPFS_OK;
    }
    if (Max > HPFS_MAX_HOTFIX || Used > Max || MapSector + 4 > Vol->TotalSectors) {
        return HPFS_DISK_CORRUPT;
    }
    Status = HpfsHostMap(Vol, MapSector, 4, HPFS_MAP_READ, &Buf);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Map = (PULONG)Buf.Data;
    for (I = 0; I < Used; I++) {
        if (Map[I] != 0 && Map[Max + I] != 0 && Map[Max + I] < Vol->TotalSectors) {
            Vol->HotfixBad[Vol->HotfixCount] = Map[I];
            Vol->HotfixSpare[Vol->HotfixCount] = Map[Max + I];
            Vol->HotfixCount++;
        }
    }
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

ULONG
HpfsHotfix(PHPFS_VOL Vol, ULONG Sector)
{
    ULONG I;

    for (I = 0; I < Vol->HotfixCount; I++) {
        if (Vol->HotfixBad[I] == Sector) {
            return Vol->HotfixSpare[I];
        }
    }
    return Sector;
}

HPFS_STATUS
HpfsMount(PHPFS_VOL Vol, BOOLEAN ReadOnly)
{
    HPFS_BUF Boot, Sb, Sp, List;
    PHPFS_SUPERBLOCK Super;
    PHPFS_SPAREBLOCK Spare;
    PHPFS_BOOT_SECTOR B;
    HPFS_STATUS Status;
    ULONG ListSectors;
    ULONG I;

    Vol->BandBitmap = NULL;
    Vol->BandFree = NULL;
    Vol->ReadOnly = ReadOnly;
    Vol->MarkedDirty = FALSE;
    Vol->FreeKnown = FALSE;

    if (Vol->TotalSectors == 0) {
        Vol->TotalSectors = 18;     /* enough to read the fixed sectors */
    }

    Status = HpfsHostMap(Vol, 0, 1, HPFS_MAP_READ, &Boot);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    B = (PHPFS_BOOT_SECTOR)Boot.Data;
    if (!HpfsIsBootSector(B)) {
        HpfsHostUnmap(Vol, &Boot);
        return HPFS_UNRECOGNIZED;
    }
    Vol->SerialNumber = B->SerialNumber;
    for (I = 0; I < 11; I++) {
        Vol->Label[I] = B->VolumeLabel[I];
    }
    Vol->Label[11] = 0;
    Vol->LabelLength = 11;
    while (Vol->LabelLength > 0 &&
           (Vol->Label[Vol->LabelLength - 1] == ' ' || Vol->Label[Vol->LabelLength - 1] == 0)) {
        Vol->LabelLength--;
    }
    Vol->Label[Vol->LabelLength] = 0;
    if (Vol->LabelLength == 7 && Vol->Label[0] == 'N' && Vol->Label[1] == 'O' && Vol->Label[2] == ' ' &&
        Vol->Label[3] == 'N' && Vol->Label[4] == 'A' && Vol->Label[5] == 'M' && Vol->Label[6] == 'E') {
        Vol->LabelLength = 0;
        Vol->Label[0] = 0;
    }
    HpfsHostUnmap(Vol, &Boot);

    Status = HpfsHostMap(Vol, HPFS_SUPER_SECTOR, 1, HPFS_MAP_READ, &Sb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = HpfsHostMap(Vol, HPFS_SPARE_SECTOR, 1, HPFS_MAP_READ, &Sp);
    if (!NT_SUCCESS(Status)) {
        HpfsHostUnmap(Vol, &Sb);
        return Status;
    }
    Super = (PHPFS_SUPERBLOCK)Sb.Data;
    Spare = (PHPFS_SPAREBLOCK)Sp.Data;

    Status = HPFS_UNRECOGNIZED;
    if (Super->Magic != HPFS_SUPER_MAGIC || Super->Magic1 != HPFS_SUPER_MAGIC1 ||
        Spare->Magic != HPFS_SPARE_MAGIC || Spare->Magic1 != HPFS_SPARE_MAGIC1) {
        goto Done;
    }
    if (Super->Version < 2 || Super->Version > 3 ||
        Super->FuncVersion < 2 || Super->FuncVersion > 4) {
        goto Done;
    }

    Status = HPFS_DISK_CORRUPT;
    Vol->TotalSectors = Super->TotalSectors;
    if (Vol->TotalSectors < 128 || Vol->TotalSectors > HPFS_MAX_SECTORS) {
        goto Done;
    }
    Vol->Version = Super->Version;
    Vol->FuncVersion = Super->FuncVersion;
    Vol->RootFnode = Super->RootFnode;
    Vol->BitmapListSector = Super->BitmapList;
    Vol->DirBandStart = Super->DirBandStart;
    Vol->DirBandEnd = Super->DirBandEnd;
    Vol->DirBandBitmap = Super->DirBandBitmap;
    Vol->DirBandDnodes = Super->DirBandSectors / HPFS_DNODE_SECTORS;
    Vol->DirBandFree = HPFS_UNKNOWN;
    Vol->DirBandHint = 0;
    Vol->SpareFlags = Spare->Flags;
    Vol->SpareFlags2 = Spare->Flags2;
    Vol->SpareDnodes = Spare->SpareDnodes;
    Vol->SpareDnodesFree = Spare->SpareDnodesFree;
    Vol->Hpfs386 = (BOOLEAN)(Super->FuncVersion >= 3 ||
                             (Spare->Flags2 & (HPFS_SPARE2_INSTALL_DASD | HPFS_SPARE2_DASD_ACTIVE |
                                               HPFS_SPARE2_DCE_ACLS)) != 0);
    Vol->DasdActive = (BOOLEAN)((Spare->Flags2 & HPFS_SPARE2_DASD_ACTIVE) != 0);

    if (Vol->RootFnode >= Vol->TotalSectors || Vol->BitmapListSector >= Vol->TotalSectors ||
        (Vol->DirBandDnodes != 0 &&
         (Vol->DirBandStart >= Vol->TotalSectors || Vol->DirBandEnd >= Vol->TotalSectors ||
          Vol->DirBandBitmap + 4 > Vol->TotalSectors || Vol->DirBandDnodes > 16384))) {
        goto Done;
    }

    Vol->BandCount = (Vol->TotalSectors + HPFS_BAND_SECTORS - 1) >> HPFS_BAND_SHIFT;
    ListSectors = (Vol->BandCount * 4 + HPFS_SECTOR_SIZE - 1) / HPFS_SECTOR_SIZE;
    ListSectors = (ListSectors + 3) & ~3UL;
    if (Vol->BitmapListSector + ListSectors > Vol->TotalSectors) {
        goto Done;
    }

    Vol->BandBitmap = (PULONG)HpfsHostAlloc(Vol->BandCount * sizeof(ULONG));
    Vol->BandFree = (PULONG)HpfsHostAlloc(Vol->BandCount * sizeof(ULONG));
    if (Vol->BandBitmap == NULL || Vol->BandFree == NULL) {
        Status = HPFS_NO_MEMORY;
        goto Done;
    }

    Status = HpfsHostMap(Vol, Vol->BitmapListSector, ListSectors, HPFS_MAP_READ, &List);
    if (!NT_SUCCESS(Status)) {
        goto Done;
    }
    Status = HPFS_OK;
    for (I = 0; I < Vol->BandCount; I++) {
        Vol->BandBitmap[I] = ((PULONG)List.Data)[I];
        Vol->BandFree[I] = HPFS_UNKNOWN;
        if (Vol->BandBitmap[I] == 0 || Vol->BandBitmap[I] + 4 > Vol->TotalSectors) {
            Status = HPFS_DISK_CORRUPT;
        }
    }
    HpfsHostUnmap(Vol, &List);
    if (!NT_SUCCESS(Status)) {
        goto Done;
    }

    Status = HpfsLoadHotfixes(Vol, Spare->HotfixMap, Spare->HotfixUsed, Spare->HotfixMax);
    if (!NT_SUCCESS(Status)) {
        goto Done;
    }

    Status = HpfsLoadCodePage(Vol, Spare->CodePageDir, Spare->CodePages);

    Vol->DataHint = Vol->RootFnode;

Done:
    HpfsHostUnmap(Vol, &Sp);
    HpfsHostUnmap(Vol, &Sb);
    if (!NT_SUCCESS(Status)) {
        HpfsUnmountFree(Vol);
    }
    return Status;
}

VOID
HpfsUnmountFree(PHPFS_VOL Vol)
{
    if (Vol->BandBitmap != NULL) {
        HpfsHostFree(Vol->BandBitmap);
        Vol->BandBitmap = NULL;
    }
    if (Vol->BandFree != NULL) {
        HpfsHostFree(Vol->BandFree);
        Vol->BandFree = NULL;
    }
}

HPFS_STATUS
HpfsMarkDirty(PHPFS_VOL Vol, BOOLEAN Dirty)
{
    HPFS_BUF Sb, Sp;
    HPFS_STATUS Status;
    PHPFS_SPAREBLOCK Spare;

    if (Vol->ReadOnly) {
        return HPFS_OK;
    }
    Status = HpfsHostMap(Vol, HPFS_SUPER_SECTOR, 1, HPFS_MAP_READ, &Sb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = HpfsHostMap(Vol, HPFS_SPARE_SECTOR, 1, HPFS_MAP_WRITE, &Sp);
    if (!NT_SUCCESS(Status)) {
        HpfsHostUnmap(Vol, &Sb);
        return Status;
    }
    Spare = (PHPFS_SPAREBLOCK)Sp.Data;
    if (Dirty) {
        Spare->Flags |= HPFS_SPARE_DIRTY;
    } else {
        Spare->Flags &= ~HPFS_SPARE_DIRTY;
    }
    HpfsUpdateChecksums((PHPFS_SUPERBLOCK)Sb.Data, Spare);
    Vol->SpareFlags = Spare->Flags;
    HpfsSetDirty(Vol, &Sp);
    HpfsHostUnmap(Vol, &Sp);
    HpfsHostUnmap(Vol, &Sb);
    Vol->MarkedDirty = Dirty;
    return HPFS_OK;
}

HPFS_STATUS
HpfsSetLabel(PHPFS_VOL Vol, const UCHAR *Label, ULONG Length)
{
    HPFS_BUF Boot;
    HPFS_STATUS Status;
    PHPFS_BOOT_SECTOR B;
    ULONG I;

    if (Length > 11) {
        return HPFS_BAD_NAME;
    }
    Status = HpfsHostMap(Vol, 0, 1, HPFS_MAP_WRITE, &Boot);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    B = (PHPFS_BOOT_SECTOR)Boot.Data;
    for (I = 0; I < 11; I++) {
        B->VolumeLabel[I] = (UCHAR)(I < Length ? Vol->Upcase[Label[I]] : (Length ? ' ' : 0));
        Vol->Label[I] = B->VolumeLabel[I];
    }
    if (Length == 0) {
        static const UCHAR NoName[11] = { 'N','O',' ','N','A','M','E',' ',' ',' ',' ' };
        for (I = 0; I < 11; I++) B->VolumeLabel[I] = NoName[I];
    }
    Vol->LabelLength = Length;
    HpfsSetDirty(Vol, &Boot);
    HpfsHostUnmap(Vol, &Boot);
    return HPFS_OK;
}

/* ------------------------------------------------------------------ */
/* Bitmaps                                                             */
/* ------------------------------------------------------------------ */

static ULONG
HpfsBandSize(PHPFS_VOL Vol, ULONG Band)
{
    ULONG Start = Band << HPFS_BAND_SHIFT;
    return MIN_UL(HPFS_BAND_SECTORS, Vol->TotalSectors - Start);
}

static ULONG
HpfsPopCount(const UCHAR *Bits, ULONG NBits)
{
    static const UCHAR Nib[16] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };
    ULONG N = 0, I;

    for (I = 0; I < NBits / 8; I++) {
        N += Nib[Bits[I] & 15] + Nib[Bits[I] >> 4];
    }
    for (I = NBits & ~7UL; I < NBits; I++) {
        N += (Bits[I >> 3] >> (I & 7)) & 1;
    }
    return N;
}

static HPFS_STATUS
HpfsBandFreeCount(PHPFS_VOL Vol, ULONG Band, PULONG Free)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;

    if (Vol->BandFree[Band] != HPFS_UNKNOWN) {
        *Free = Vol->BandFree[Band];
        return HPFS_OK;
    }
    Status = HpfsHostMap(Vol, Vol->BandBitmap[Band], HPFS_BITMAP_SECTORS, HPFS_MAP_READ, &Buf);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Vol->BandFree[Band] = HpfsPopCount(Buf.Data, HpfsBandSize(Vol, Band));
    HpfsHostUnmap(Vol, &Buf);
    *Free = Vol->BandFree[Band];
    return HPFS_OK;
}

HPFS_STATUS
HpfsCountFree(PHPFS_VOL Vol, PULONG Free)
{
    HPFS_STATUS Status;
    ULONG Band, N, Total = 0;

    if (Vol->FreeKnown) {
        *Free = Vol->FreeSectors;
        return HPFS_OK;
    }
    for (Band = 0; Band < Vol->BandCount; Band++) {
        Status = HpfsBandFreeCount(Vol, Band, &N);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Total += N;
    }
    Vol->FreeSectors = Total;
    Vol->FreeKnown = TRUE;
    *Free = Total;
    return HPFS_OK;
}

static VOID
HpfsAdjustFree(PHPFS_VOL Vol, ULONG Band, LONG Delta)
{
    if (Vol->BandFree[Band] != HPFS_UNKNOWN) {
        Vol->BandFree[Band] += (ULONG)Delta;
    }
    if (Vol->FreeKnown) {
        Vol->FreeSectors += (ULONG)Delta;
    }
}

/*
 * Marks [First, First+Count) used (Used = TRUE) or free in the band bitmaps.
 * With Check, fails (changing nothing) if any sector is not in the
 * expected state.
 */
static HPFS_STATUS
HpfsMarkSectors(PHPFS_VOL Vol, ULONG First, ULONG Count, BOOLEAN Used, BOOLEAN Check)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    ULONG S, End, Band, Bit, N, I;
    PUCHAR Bits;

    if (Count == 0) {
        return HPFS_OK;
    }
    if (First >= Vol->TotalSectors || Count > Vol->TotalSectors - First) {
        return HPFS_DISK_CORRUPT;
    }
    End = First + Count;

    if (Check) {
        for (S = First; S < End; S += N) {
            Band = S >> HPFS_BAND_SHIFT;
            Bit = S & (HPFS_BAND_SECTORS - 1);
            N = MIN_UL(End - S, HPFS_BAND_SECTORS - Bit);
            Status = HpfsHostMap(Vol, Vol->BandBitmap[Band], HPFS_BITMAP_SECTORS, HPFS_MAP_READ, &Buf);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
            Bits = Buf.Data;
            for (I = Bit; I < Bit + N; I++) {
                ULONG IsFree = (Bits[I >> 3] >> (I & 7)) & 1;
                if (IsFree != (ULONG)(Used ? 1 : 0)) {
                    HpfsHostUnmap(Vol, &Buf);
                    return HPFS_DISK_CORRUPT;
                }
            }
            HpfsHostUnmap(Vol, &Buf);
        }
    }

    for (S = First; S < End; S += N) {
        Band = S >> HPFS_BAND_SHIFT;
        Bit = S & (HPFS_BAND_SECTORS - 1);
        N = MIN_UL(End - S, HPFS_BAND_SECTORS - Bit);
        Status = HpfsHostMap(Vol, Vol->BandBitmap[Band], HPFS_BITMAP_SECTORS, HPFS_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Bits = Buf.Data;
        I = Bit;
        while (I < Bit + N) {
            if ((I & 7) == 0 && I + 8 <= Bit + N) {
                ULONG Was = HpfsPopCount(&Bits[I >> 3], 8);
                Bits[I >> 3] = Used ? 0x00 : 0xFF;
                HpfsAdjustFree(Vol, Band, Used ? -(LONG)Was : (LONG)(8 - Was));
                I += 8;
            } else {
                ULONG Mask = 1UL << (I & 7);
                ULONG IsFree = Bits[I >> 3] & Mask;
                if (Used && IsFree) {
                    Bits[I >> 3] &= (UCHAR)~Mask;
                    HpfsAdjustFree(Vol, Band, -1);
                } else if (!Used && !IsFree) {
                    Bits[I >> 3] |= (UCHAR)Mask;
                    HpfsAdjustFree(Vol, Band, 1);
                }
                I++;
            }
        }
        HpfsSetDirty(Vol, &Buf);
        HpfsHostUnmap(Vol, &Buf);
    }
    return HPFS_OK;
}

BOOLEAN
HpfsSectorsUsed(PHPFS_VOL Vol, ULONG First, ULONG Count)
{
    HPFS_BUF Buf;
    ULONG S, End, Band, Bit, N, I;

    if (First >= Vol->TotalSectors || Count > Vol->TotalSectors - First) {
        return FALSE;
    }
    End = First + Count;
    for (S = First; S < End; S += N) {
        Band = S >> HPFS_BAND_SHIFT;
        Bit = S & (HPFS_BAND_SECTORS - 1);
        N = MIN_UL(End - S, HPFS_BAND_SECTORS - Bit);
        if (!NT_SUCCESS(HpfsHostMap(Vol, Vol->BandBitmap[Band], HPFS_BITMAP_SECTORS, HPFS_MAP_READ, &Buf))) {
            return FALSE;
        }
        for (I = Bit; I < Bit + N; I++) {
            if ((Buf.Data[I >> 3] >> (I & 7)) & 1) {
                HpfsHostUnmap(Vol, &Buf);
                return FALSE;
            }
        }
        HpfsHostUnmap(Vol, &Buf);
    }
    return TRUE;
}

/*
 * Scans [From, To) for free runs. Stops at the first run of Count sectors
 * (returns TRUE); otherwise remembers the longest run in *Best, *BestLen.
 */
static HPFS_STATUS
HpfsScanFree(PHPFS_VOL Vol, ULONG From, ULONG To, ULONG Count, ULONG Align,
             PULONG Best, PULONG BestLen, PBOOLEAN Found)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    ULONG S = From, Band, Bit, Lim, Free;
    ULONG RunStart = 0, RunLen = 0;
    PUCHAR Bits;

    *Found = FALSE;
    while (S < To) {
        Band = S >> HPFS_BAND_SHIFT;
        Bit = S & (HPFS_BAND_SECTORS - 1);
        Lim = MIN_UL(HPFS_BAND_SECTORS, To - (Band << HPFS_BAND_SHIFT));

        Status = HpfsBandFreeCount(Vol, Band, &Free);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        if (Free == 0) {
            RunLen = 0;
            S = (Band + 1) << HPFS_BAND_SHIFT;
            continue;
        }
        Status = HpfsHostMap(Vol, Vol->BandBitmap[Band], HPFS_BITMAP_SECTORS, HPFS_MAP_READ, &Buf);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Bits = Buf.Data;
        while (Bit < Lim) {
            if ((Bit & 7) == 0 && Bit + 8 <= Lim) {
                if (Bits[Bit >> 3] == 0) {
                    RunLen = 0;
                    Bit += 8;
                    continue;
                }
                if (Bits[Bit >> 3] == 0xFF && RunLen != 0) {
                    RunLen += 8;
                    Bit += 8;
                    goto Check;
                }
            }
            if ((Bits[Bit >> 3] >> (Bit & 7)) & 1) {
                if (RunLen == 0) {
                    ULONG Abs = (Band << HPFS_BAND_SHIFT) + Bit;
                    if (Abs % Align) {
                        Bit++;
                        continue;
                    }
                    RunStart = Abs;
                }
                RunLen++;
            } else {
                RunLen = 0;
            }
            Bit++;
Check:
            if (RunLen >= Count) {
                HpfsHostUnmap(Vol, &Buf);
                *Best = RunStart;
                *BestLen = Count;
                *Found = TRUE;
                return HPFS_OK;
            }
            if (RunLen > *BestLen) {
                *Best = RunStart;
                *BestLen = RunLen;
            }
        }
        HpfsHostUnmap(Vol, &Buf);
        S = (Band + 1) << HPFS_BAND_SHIFT;
    }
    return HPFS_OK;
}

/*
 * Allocates a run: the first run of Count sectors at or after Near, then
 * (unless FORWARD) from the start of the volume. With PARTIAL and no such
 * run, the longest free run seen (up to Count).
 */
HPFS_STATUS
HpfsAllocSectors(PHPFS_VOL Vol, ULONG Near, ULONG Count, ULONG Flags, PULONG First, PULONG Got)
{
    HPFS_STATUS Status;
    ULONG Best = 0, BestLen = 0;
    ULONG Align = (Flags & HPFS_ALLOC_ALIGN4) ? 4 : 1;
    BOOLEAN Found;

    if (Vol->ReadOnly) {
        return HPFS_READ_ONLY;
    }
    if (Count == 0) {
        *First = 0;
        if (Got) *Got = 0;
        return HPFS_OK;
    }
    if (Near >= Vol->TotalSectors) {
        Near = 0;
    }

    Status = HpfsScanFree(Vol, Near, Vol->TotalSectors, Count, Align, &Best, &BestLen, &Found);
    if (NT_SUCCESS(Status) && !Found && Near != 0 && !(Flags & HPFS_ALLOC_FORWARD)) {
        Status = HpfsScanFree(Vol, 0, MIN_UL(Near + Count, Vol->TotalSectors), Count, Align,
                              &Best, &BestLen, &Found);
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (!Found) {
        if (!(Flags & HPFS_ALLOC_PARTIAL) || BestLen == 0) {
            return HPFS_DISK_FULL;
        }
        if (BestLen > Count) {
            BestLen = Count;
        }
    }

    Status = HpfsMarkSectors(Vol, Best, BestLen, TRUE, TRUE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    *First = Best;
    if (Got) {
        *Got = BestLen;
    }
    return HPFS_OK;
}

HPFS_STATUS
HpfsFreeSectors(PHPFS_VOL Vol, ULONG First, ULONG Count)
{
    HPFS_STATUS Status;

    if (Vol->ReadOnly) {
        return HPFS_READ_ONLY;
    }
    Status = HpfsMarkSectors(Vol, First, Count, FALSE, FALSE);
    if (NT_SUCCESS(Status)) {
        HpfsHostFreed(Vol, First, Count);
    }
    return Status;
}

/*
 * Spare dnodes (listed in the spareblock, entries [0, SpareDnodesFree) are
 * free): used when the disk is full, refilled by the next freed dnode.
 */
static HPFS_STATUS
SpareDnodeOp(PHPFS_VOL Vol, BOOLEAN Take, PULONG Dnode)
{
    HPFS_BUF Sb, Sp;
    HPFS_STATUS Status;
    PHPFS_SPAREBLOCK Spare;

    Status = HpfsHostMap(Vol, HPFS_SUPER_SECTOR, 1, HPFS_MAP_READ, &Sb);
    if (!NT_SUCCESS(Status)) return Status;
    Status = HpfsHostMap(Vol, HPFS_SPARE_SECTOR, 1, HPFS_MAP_WRITE, &Sp);
    if (!NT_SUCCESS(Status)) {
        HpfsHostUnmap(Vol, &Sb);
        return Status;
    }
    Spare = (PHPFS_SPAREBLOCK)Sp.Data;
    Status = HPFS_OK;
    if (Spare->SpareDnodes > 101 || Spare->SpareDnodesFree > Spare->SpareDnodes) {
        Status = HPFS_DISK_CORRUPT;
    } else if (Take) {
        if (Spare->SpareDnodesFree == 0) {
            Status = HPFS_DISK_FULL;
        } else {
            Spare->SpareDnodesFree--;
            *Dnode = Spare->SpareDnode[Spare->SpareDnodesFree];
            Spare->Flags |= HPFS_SPARE_SPARE_USED;
            if (*Dnode < HPFS_BOOT_SECTORS + 2 || *Dnode + 4 > Vol->TotalSectors) {
                Status = HPFS_DISK_CORRUPT;
            }
        }
    } else {
        if (Spare->SpareDnodesFree >= Spare->SpareDnodes) {
            Status = HPFS_DISK_FULL;        /* the list is full: caller frees normally */
        } else {
            Spare->SpareDnode[Spare->SpareDnodesFree++] = *Dnode;
            if (Spare->SpareDnodesFree == Spare->SpareDnodes) {
                Spare->Flags &= ~HPFS_SPARE_SPARE_USED;
            }
        }
    }
    if (NT_SUCCESS(Status)) {
        Vol->SpareDnodesFree = Spare->SpareDnodesFree;
        Vol->SpareFlags = Spare->Flags;
        HpfsUpdateChecksums((PHPFS_SUPERBLOCK)Sb.Data, Spare);
        HpfsSetDirty(Vol, &Sp);
    }
    HpfsHostUnmap(Vol, &Sp);
    HpfsHostUnmap(Vol, &Sb);
    return Status;
}

HPFS_STATUS
HpfsTakeSpareDnode(PHPFS_VOL Vol, PULONG Dnode)
{
    if (Vol->SpareDnodesFree == 0) return HPFS_DISK_FULL;
    return SpareDnodeOp(Vol, TRUE, Dnode);
}

/* Directory band: one bit per dnode, 1 = free */
HPFS_STATUS
HpfsAllocDnode(PHPFS_VOL Vol, ULONG Near, PULONG Dnode)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    ULONG I, N, Start;

    if (Vol->ReadOnly) {
        return HPFS_READ_ONLY;
    }

    if (Vol->DirBandDnodes != 0 && Vol->DirBandFree != 0) {
        Status = HpfsHostMap(Vol, Vol->DirBandBitmap, 4, HPFS_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        N = Vol->DirBandDnodes;
        Start = Vol->DirBandHint < N ? Vol->DirBandHint : 0;
        for (I = 0; I < N; I++) {
            ULONG J = (Start + I) % N;
            if ((Buf.Data[J >> 3] >> (J & 7)) & 1) {
                Buf.Data[J >> 3] &= (UCHAR)~(1 << (J & 7));
                HpfsSetDirty(Vol, &Buf);
                HpfsHostUnmap(Vol, &Buf);
                Vol->DirBandHint = J + 1;
                if (Vol->DirBandFree != HPFS_UNKNOWN) {
                    Vol->DirBandFree--;
                }
                *Dnode = Vol->DirBandStart + J * HPFS_DNODE_SECTORS;
                return HPFS_OK;
            }
        }
        HpfsHostUnmap(Vol, &Buf);
        Vol->DirBandFree = 0;
    }

    /* The band is full: four aligned sectors from the general pool */
    return HpfsAllocSectors(Vol, Near, HPFS_DNODE_SECTORS, HPFS_ALLOC_ALIGN4, Dnode, NULL);
}

HPFS_STATUS
HpfsFreeDnode(PHPFS_VOL Vol, ULONG Dnode)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    ULONG J;

    if (Vol->SpareDnodesFree < Vol->SpareDnodes) {
        /* refill the spare list instead (the dnode stays allocated) */
        HpfsHostFreed(Vol, Dnode, HPFS_DNODE_SECTORS);
        if (NT_SUCCESS(SpareDnodeOp(Vol, FALSE, &Dnode))) {
            return HPFS_OK;
        }
    }

    if (Vol->DirBandDnodes != 0 && Dnode >= Vol->DirBandStart &&
        Dnode < Vol->DirBandStart + Vol->DirBandDnodes * HPFS_DNODE_SECTORS) {

        J = (Dnode - Vol->DirBandStart) / HPFS_DNODE_SECTORS;
        Status = HpfsHostMap(Vol, Vol->DirBandBitmap, 4, HPFS_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Buf.Data[J >> 3] |= (UCHAR)(1 << (J & 7));
        HpfsSetDirty(Vol, &Buf);
        HpfsHostUnmap(Vol, &Buf);
        if (Vol->DirBandFree != HPFS_UNKNOWN) {
            Vol->DirBandFree++;
        }
        HpfsHostFreed(Vol, Dnode, HPFS_DNODE_SECTORS);
        return HPFS_OK;
    }
    return HpfsFreeSectors(Vol, Dnode, HPFS_DNODE_SECTORS);
}

