/*
 * HPFSNT - files: fnodes, create/delete/rename, extended attributes,
 * HPFS386 ACL areas and DASD limits.
 */

#ifdef HPFS_KERNEL
#include "hpfs.h"
#else
#include "hpfsusr.h"
#include "hpfssup.h"
#endif

HPFS_STATUS HpfsNewDirDnode(PHPFS_VOL Vol, ULONG DirFnode, ULONG ParentFnode, ULONG Now, PULONG Dnode);
HPFS_STATUS HpfsFreeDirDnodes(PHPFS_VOL Vol, ULONG Root);

/* ------------------------------------------------------------------ */
/* Fnode fields                                                        */
/* ------------------------------------------------------------------ */

HPFS_STATUS
HpfsSetFnodeSize(PHPFS_VOL Vol, ULONG Fnode, ULONG Size)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;

    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_WRITE, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    if (F->FileSize != Size) {
        F->FileSize = Size;
        HpfsSetDirty(Vol, &Buf);
    }
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

HPFS_STATUS
HpfsGetFnodeInfo(PHPFS_VOL Vol, ULONG Fnode, PULONG Size, PUSHORT Flags, PULONG Parent)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;

    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_READ, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    if (Size) *Size = F->FileSize;
    if (Flags) *Flags = F->Flags;
    if (Parent) *Parent = F->Parent;
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

HPFS_STATUS
HpfsSetFnodeName(PHPFS_VOL Vol, ULONG Fnode, const UCHAR *Name, ULONG Length, ULONG Parent)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;
    ULONG I;

    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_WRITE, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    F->NameLength = (UCHAR)Length;
    for (I = 0; I < 15; I++) {
        F->Name[I] = (UCHAR)(I < Length ? Name[I] : 0);
    }
    F->Parent = Parent;
    HpfsSetDirty(Vol, &Buf);
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

/* The ".." entry of a directory (first entry of its leftmost leaf) */
HPFS_STATUS
HpfsSetDotDot(PHPFS_VOL Vol, ULONG DirFnode, ULONG Parent)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    PHPFS_DNODE D;
    PHPFS_DIRENT E;
    ULONG Dn, Depth;

    Status = HpfsDirRoot(Vol, DirFnode, &Dn);
    if (!NT_SUCCESS(Status)) return Status;
    for (Depth = 0; Depth < 32; Depth++) {
        Status = HpfsHostMap(Vol, Dn, HPFS_DNODE_SECTORS, HPFS_MAP_WRITE, &Buf);
        if (!NT_SUCCESS(Status)) return Status;
        D = (PHPFS_DNODE)Buf.Data;
        E = (PHPFS_DIRENT)D->Entries;
        if (D->Magic != HPFS_DNODE_MAGIC || E->Length < 32 || E->Length > 300) {
            HpfsHostUnmap(Vol, &Buf);
            return HPFS_DISK_CORRUPT;
        }
        if (E->Flags & HPFS_DE_DOWN) {
            Dn = HpfsDirentDown(E);
            HpfsHostUnmap(Vol, &Buf);
            continue;
        }
        if ((E->Flags & HPFS_DE_SPECIAL) && !(E->Flags & HPFS_DE_END) && E->NameLength == 2) {
            E->Fnode = Parent;
            HpfsSetDirty(Vol, &Buf);
            HpfsHostUnmap(Vol, &Buf);
            return HPFS_OK;
        }
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_DISK_CORRUPT;
    }
    return HPFS_DISK_CORRUPT;
}

/* The times of a directory as its ".." entry carries them (the root has no other entry) */
HPFS_STATUS
HpfsGetDotDot(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_ENTRY Entry)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    PHPFS_DNODE D;
    PHPFS_DIRENT E;
    ULONG Dn, Depth;

    Status = HpfsDirRoot(Vol, DirFnode, &Dn);
    if (!NT_SUCCESS(Status)) return Status;
    for (Depth = 0; Depth < 32; Depth++) {
        Status = HpfsHostMap(Vol, Dn, HPFS_DNODE_SECTORS, HPFS_MAP_READ, &Buf);
        if (!NT_SUCCESS(Status)) return Status;
        D = (PHPFS_DNODE)Buf.Data;
        E = (PHPFS_DIRENT)D->Entries;
        if (D->Magic != HPFS_DNODE_MAGIC || E->Length < 32 || E->Length > 300) {
            HpfsHostUnmap(Vol, &Buf);
            return HPFS_DISK_CORRUPT;
        }
        if (E->Flags & HPFS_DE_DOWN) {
            Dn = HpfsDirentDown(E);
            HpfsHostUnmap(Vol, &Buf);
            continue;
        }
        Status = HPFS_DISK_CORRUPT;
        if ((E->Flags & HPFS_DE_SPECIAL) && !(E->Flags & HPFS_DE_END) && E->NameLength == 2) {
            HpfsZero(Entry, sizeof(HPFS_ENTRY));
            Entry->Fnode = E->Fnode;
            Entry->Flags = E->Flags;
            Entry->Attributes = E->Attributes;
            Entry->WriteTime = E->WriteTime;
            Entry->AccessTime = E->AccessTime;
            Entry->CreateTime = E->CreateTime;
            Entry->NameLength = 2;
            Entry->Name[0] = Entry->Name[1] = '.';
            Status = HPFS_OK;
        }
        HpfsHostUnmap(Vol, &Buf);
        return Status;
    }
    return HPFS_DISK_CORRUPT;
}

/* ------------------------------------------------------------------ */
/* Raw runs (EA / ACL data, large EA values)                           */
/* ------------------------------------------------------------------ */

/* Reads Length bytes stored at Sector (a plain run) or under an anode tree */
static HPFS_STATUS
ReadStore(PHPFS_VOL Vol, ULONG Sector, BOOLEAN Anode, ULONG Length, PUCHAR Out)
{
    HPFS_STATUS Status;
    HPFS_BUF Buf;
    ULONG Done = 0, Chunk, Lsn, Run, Vsn;

    if (Length == 0) return HPFS_OK;
    if (!Anode) {
        ULONG Sectors = (Length + HPFS_SECTOR_SIZE - 1) / HPFS_SECTOR_SIZE;
        if (Sector >= Vol->TotalSectors || Sectors > Vol->TotalSectors - Sector) {
            return HPFS_DISK_CORRUPT;
        }
        while (Done < Length) {
            Vsn = Done / HPFS_SECTOR_SIZE;
            Chunk = MIN_UL(Length - Done, 16 * HPFS_SECTOR_SIZE);
            Status = HpfsHostMap(Vol, Sector + Vsn, (Chunk + HPFS_SECTOR_SIZE - 1) / HPFS_SECTOR_SIZE,
                                 HPFS_MAP_READ, &Buf);
            if (!NT_SUCCESS(Status)) return Status;
            HpfsCopy(Out + Done, Buf.Data, Chunk);
            HpfsHostUnmap(Vol, &Buf);
            Done += Chunk;
        }
        return HPFS_OK;
    }
    while (Done < Length) {
        Vsn = Done / HPFS_SECTOR_SIZE;
        {
            /* look the sector up in the anode tree */
            PHPFS_RUN Runs;
            ULONG Count, I, Total;
            Status = HpfsTreeRuns(Vol, Sector, FALSE, &Runs, &Count, &Total);
            if (!NT_SUCCESS(Status)) return Status;
            if ((ULONGLONG)Total * HPFS_SECTOR_SIZE < Length) {
                if (Runs) HpfsHostFree(Runs);
                return HPFS_DISK_CORRUPT;
            }
            for (I = 0; I < Count && Done < Length; I++) {
                ULONG S;
                for (S = 0; S < Runs[I].Length && Done < Length; S++) {
                    Lsn = Runs[I].DiskSector + S;
                    Chunk = MIN_UL(Length - Done, HPFS_SECTOR_SIZE);
                    Status = HpfsHostMap(Vol, Lsn, 1, HPFS_MAP_READ, &Buf);
                    if (!NT_SUCCESS(Status)) {
                        HpfsHostFree(Runs);
                        return Status;
                    }
                    HpfsCopy(Out + Done, Buf.Data, Chunk);
                    HpfsHostUnmap(Vol, &Buf);
                    Done += Chunk;
                }
            }
            if (Runs) HpfsHostFree(Runs);
            (void)Run;
            return Done >= Length ? HPFS_OK : HPFS_DISK_CORRUPT;
        }
    }
    return HPFS_OK;
}

/* Frees a store written by WriteStore (or found on disk) */
static HPFS_STATUS
FreeStore(PHPFS_VOL Vol, ULONG Sector, BOOLEAN Anode, ULONG Length)
{
    HPFS_STATUS Status;

    if (Length == 0 || Sector == 0) return HPFS_OK;
    if (!Anode) {
        return HpfsFreeSectors(Vol, Sector, (Length + HPFS_SECTOR_SIZE - 1) / HPFS_SECTOR_SIZE);
    }
    Status = HpfsTreeTruncate(Vol, Sector, FALSE, 0);       /* frees the runs and inner anodes */
    if (NT_SUCCESS(Status)) {
        Status = HpfsFreeSectors(Vol, Sector, 1);          /* the root anode */
    }
    return Status;
}

/*
 * Stores Length bytes in a new run near Near (or, when no single run is
 * free, under a new anode). Returns the sector and whether it is an anode.
 */
static HPFS_STATUS
WriteStore(PHPFS_VOL Vol, ULONG Near, ULONG Up, const UCHAR *Data, ULONG Length,
           PULONG Sector, PBOOLEAN Anode)
{
    HPFS_STATUS Status;
    HPFS_BUF Buf;
    ULONG Sectors = (Length + HPFS_SECTOR_SIZE - 1) / HPFS_SECTOR_SIZE;
    ULONG First, Got, I, Have, Done;

    Status = HpfsAllocSectors(Vol, Near, Sectors, HPFS_ALLOC_CONTIG, &First, &Got);
    if (NT_SUCCESS(Status)) {
        for (I = 0; I < Sectors; I++) {
            ULONG Off = I * HPFS_SECTOR_SIZE;
            Status = HpfsHostMap(Vol, First + I, 1, HPFS_MAP_NEW, &Buf);
            if (!NT_SUCCESS(Status)) {
                HpfsFreeSectors(Vol, First, Sectors);
                return Status;
            }
            HpfsZero(Buf.Data, HPFS_SECTOR_SIZE);
            HpfsCopy(Buf.Data, Data + Off, MIN_UL(HPFS_SECTOR_SIZE, Length - Off));
            HpfsSetDirty(Vol, &Buf);
            HpfsHostUnmap(Vol, &Buf);
        }
        *Sector = First;
        *Anode = FALSE;
        return HPFS_OK;
    }
    if (Status != HPFS_DISK_FULL) return Status;

    /* Fragmented: an anode root with as many runs as it takes */
    Status = HpfsAllocSectors(Vol, Near, 1, HPFS_ALLOC_CONTIG, &First, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    {
        PHPFS_ANODE A;
        Status = HpfsHostMap(Vol, First, 1, HPFS_MAP_NEW, &Buf);
        if (!NT_SUCCESS(Status)) {
            HpfsFreeSectors(Vol, First, 1);
            return Status;
        }
        A = (PHPFS_ANODE)Buf.Data;
        HpfsZero(A, HPFS_SECTOR_SIZE);
        A->Magic = HPFS_ANODE_MAGIC;
        A->Self = First;
        A->Up = Up;
        A->Header.Flags = HPFS_BT_FNODE_PARENT;
        A->Header.FreeCount = HPFS_ANODE_LEAVES;
        A->Header.UsedCount = 0;
        A->Header.FirstFree = sizeof(HPFS_BTREE_HEADER);
        HpfsSetDirty(Vol, &Buf);
        HpfsHostUnmap(Vol, &Buf);
    }
    *Sector = First;
    *Anode = TRUE;
    Have = 0;
    Done = 0;
    while (Have < Sectors) {
        ULONG Run;
        Status = HpfsAllocSectors(Vol, Near, Sectors - Have, HPFS_ALLOC_PARTIAL, &Run, &Got);
        if (!NT_SUCCESS(Status)) break;
        Status = HpfsTreeAppend(Vol, First, FALSE, Have, Got, Run);
        if (!NT_SUCCESS(Status)) {
            HpfsFreeSectors(Vol, Run, Got);
            break;
        }
        for (I = 0; I < Got; I++) {
            ULONG Off = (Have + I) * HPFS_SECTOR_SIZE;
            Status = HpfsHostMap(Vol, Run + I, 1, HPFS_MAP_NEW, &Buf);
            if (!NT_SUCCESS(Status)) break;
            HpfsZero(Buf.Data, HPFS_SECTOR_SIZE);
            HpfsCopy(Buf.Data, Data + Off, MIN_UL(HPFS_SECTOR_SIZE, Length - Off));
            HpfsSetDirty(Vol, &Buf);
            HpfsHostUnmap(Vol, &Buf);
        }
        if (!NT_SUCCESS(Status)) break;
        Have += Got;
        Near = Run + Got;
    }
    if (!NT_SUCCESS(Status)) {
        FreeStore(Vol, First, TRUE, Have * HPFS_SECTOR_SIZE);
        return Status;
    }
    (void)Done;
    return HPFS_OK;
}

/* ------------------------------------------------------------------ */
/* Extended attributes                                                 */
/* ------------------------------------------------------------------ */

/* Walks an EA list: TRUE if well formed */
static BOOLEAN
EaListValid(const UCHAR *List, ULONG Length)
{
    ULONG Off = 0;
    while (Off < Length) {
        const HPFS_EA *E = (const HPFS_EA *)(List + Off);
        if (Off + 4 > Length || Off + HpfsEaSize(E) > Length) {
            return FALSE;
        }
        Off += HpfsEaSize(E);
    }
    return TRUE;
}

/* Frees the separately stored values of indirect EAs in a raw list */
static HPFS_STATUS
FreeIndirectValues(PHPFS_VOL Vol, const UCHAR *List, ULONG Length)
{
    ULONG Off = 0;
    HPFS_STATUS Status = HPFS_OK;

    while (Off + 4 <= Length) {
        const HPFS_EA *E = (const HPFS_EA *)(List + Off);
        ULONG Size = HpfsEaSize(E);
        if (Off + Size > Length) break;
        if ((E->Flags & HPFS_EA_INDIRECT) && HpfsEaValueLength(E) == 8) {
            const UCHAR *V = (const UCHAR *)E + 4 + E->NameLength + 1;
            ULONG Len = V[0] | (V[1] << 8) | ((ULONG)V[2] << 16) | ((ULONG)V[3] << 24);
            ULONG Sec = V[4] | (V[5] << 8) | ((ULONG)V[6] << 16) | ((ULONG)V[7] << 24);
            HPFS_STATUS S2 = FreeStore(Vol, Sec, (BOOLEAN)((E->Flags & HPFS_EA_ANODE) != 0), Len);
            if (!NT_SUCCESS(S2)) Status = S2;
        }
        Off += Size;
    }
    return Status;
}

/* Reads the raw (as stored) EA list: inline part, then the external part */
static HPFS_STATUS ReadRawEas(PHPFS_VOL Vol, ULONG Fnode, PUCHAR *List, PULONG Length);

HPFS_STATUS
HpfsReadEasRaw(PHPFS_VOL Vol, ULONG Fnode, PUCHAR *List, PULONG Length)
{
    return ReadRawEas(Vol, Fnode, List, Length);
}

static HPFS_STATUS
ReadRawEas(PHPFS_VOL Vol, ULONG Fnode, PUCHAR *List, PULONG Length)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;
    ULONG In, Ext, ExtSector, Off;
    BOOLEAN ExtAnode;
    PUCHAR L;

    *List = NULL;
    *Length = 0;
    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_READ, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;

    In = F->EaSizeInternal;
    Off = F->EaOffset + F->AclSizeInternal;
    Ext = F->EaSizeExternal;
    ExtSector = F->EaSector;
    ExtAnode = (BOOLEAN)((F->Flags & HPFS_FNODE_EA_ANODE) != 0);
    if (Off < HPFS_FNODE_EA_OFFSET || Off + In > HPFS_SECTOR_SIZE || Ext > HPFS_MAX_EA_LIST * 2) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_DISK_CORRUPT;
    }
    if (In + Ext == 0) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_OK;
    }
    L = (PUCHAR)HpfsHostAlloc(In + Ext + 1);
    if (L == NULL) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_NO_MEMORY;
    }
    HpfsCopy(L, (PUCHAR)F + Off, In);
    HpfsHostUnmap(Vol, &Buf);

    Status = ReadStore(Vol, ExtSector, ExtAnode, Ext, L + In);
    if (!NT_SUCCESS(Status)) {
        HpfsHostFree(L);
        return Status;
    }
    if (!EaListValid(L, In + Ext)) {
        HpfsHostFree(L);
        return HPFS_DISK_CORRUPT;
    }
    *List = L;
    *Length = In + Ext;
    return HPFS_OK;
}

HPFS_STATUS
HpfsReadEas(PHPFS_VOL Vol, ULONG Fnode, PUCHAR *List, PULONG Length)
{
    HPFS_STATUS Status;
    PUCHAR Raw, Out;
    ULONG RawLen, Off, OutLen, Total;

    Status = ReadRawEas(Vol, Fnode, &Raw, &RawLen);
    if (!NT_SUCCESS(Status) || Raw == NULL) {
        *List = NULL;
        *Length = 0;
        return Status;
    }

    /* Resolve indirect values: first the size of the result */
    Total = 0;
    for (Off = 0; Off < RawLen; ) {
        PHPFS_EA E = (PHPFS_EA)(Raw + Off);
        ULONG VLen = HpfsEaValueLength(E);
        if ((E->Flags & HPFS_EA_INDIRECT) && VLen == 8) {
            PUCHAR V = Raw + Off + 4 + E->NameLength + 1;
            VLen = V[0] | (V[1] << 8) | ((ULONG)V[2] << 16) | ((ULONG)V[3] << 24);
            if (VLen > 0xFFFF) {
                HpfsHostFree(Raw);
                return HPFS_DISK_CORRUPT;
            }
        }
        Total += 4 + E->NameLength + 1 + VLen;
        Off += HpfsEaSize(E);
    }
    if (Total == RawLen) {
        BOOLEAN AnyIndirect = FALSE;
        for (Off = 0; Off < RawLen; Off += HpfsEaSize((PHPFS_EA)(Raw + Off))) {
            if (((PHPFS_EA)(Raw + Off))->Flags & HPFS_EA_INDIRECT) AnyIndirect = TRUE;
        }
        if (!AnyIndirect) {
            *List = Raw;
            *Length = RawLen;
            return HPFS_OK;
        }
    }
    Out = (PUCHAR)HpfsHostAlloc(Total + 1);
    if (Out == NULL) {
        HpfsHostFree(Raw);
        return HPFS_NO_MEMORY;
    }
    OutLen = 0;
    for (Off = 0; Off < RawLen; ) {
        PHPFS_EA E = (PHPFS_EA)(Raw + Off);
        PHPFS_EA D = (PHPFS_EA)(Out + OutLen);
        ULONG VLen = HpfsEaValueLength(E);
        PUCHAR V = Raw + Off + 4 + E->NameLength + 1;
        HpfsCopy(D, E, 4 + E->NameLength + 1);
        if ((E->Flags & HPFS_EA_INDIRECT) && VLen == 8) {
            ULONG Len = V[0] | (V[1] << 8) | ((ULONG)V[2] << 16) | ((ULONG)V[3] << 24);
            ULONG Sec = V[4] | (V[5] << 8) | ((ULONG)V[6] << 16) | ((ULONG)V[7] << 24);
            Status = ReadStore(Vol, Sec, (BOOLEAN)((E->Flags & HPFS_EA_ANODE) != 0), Len,
                               Out + OutLen + 4 + E->NameLength + 1);
            if (!NT_SUCCESS(Status)) {
                HpfsHostFree(Raw);
                HpfsHostFree(Out);
                return Status;
            }
            D->Flags &= ~(HPFS_EA_INDIRECT | HPFS_EA_ANODE);
            D->ValueLengthLo = (UCHAR)Len;
            D->ValueLengthHi = (UCHAR)(Len >> 8);
            VLen = Len;
        } else {
            HpfsCopy(Out + OutLen + 4 + E->NameLength + 1, V, VLen);
        }
        OutLen += 4 + E->NameLength + 1 + VLen;
        Off += HpfsEaSize(E);
    }
    HpfsHostFree(Raw);
    *List = Out;
    *Length = OutLen;
    return HPFS_OK;
}

HPFS_STATUS
HpfsEaListSize(PHPFS_VOL Vol, ULONG Fnode, PULONG Size)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;

    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_READ, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    *Size = F->EaSizeInternal + F->EaSizeExternal;
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

/*
 * Lays out the fnode's inline area (ACL first, then EAs) and the external
 * EA store. Acl/AclLen: the inline ACL to keep (copied by the caller).
 */
static HPFS_STATUS
PlaceEas(PHPFS_VOL Vol, ULONG Fnode, const UCHAR *List, ULONG Length,
         PULONG ExternalSize, PBOOLEAN NeedEa)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;
    PUCHAR OldRaw;
    ULONG OldLen, OldSector, OldExt, Space, NeedCount = 0, Off, NewSector = 0;
    BOOLEAN OldAnode, NewAnode = FALSE;
    BOOLEAN Inline;

    if (Length > HPFS_MAX_EA_LIST) return HPFS_EA_TOO_LARGE;
    if (!EaListValid(List, Length)) return HPFS_BAD_EA;
    for (Off = 0; Off < Length; Off += HpfsEaSize((const HPFS_EA *)(List + Off))) {
        if (((const HPFS_EA *)(List + Off))->Flags & HPFS_EA_NEEDEA) NeedCount++;
    }

    /* The old list: its indirect values and external store go */
    Status = ReadRawEas(Vol, Fnode, &OldRaw, &OldLen);
    if (!NT_SUCCESS(Status)) return Status;

    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_READ, &Buf, &F);
    if (!NT_SUCCESS(Status)) {
        if (OldRaw) HpfsHostFree(OldRaw);
        return Status;
    }
    OldSector = F->EaSector;
    OldExt = F->EaSizeExternal;
    OldAnode = (BOOLEAN)((F->Flags & HPFS_FNODE_EA_ANODE) != 0);
    Space = HPFS_FNODE_EA_SPACE - F->AclSizeInternal;
    HpfsHostUnmap(Vol, &Buf);

    Inline = (BOOLEAN)(Length <= Space);
    if (!Inline) {
        Status = WriteStore(Vol, Fnode + 1, Fnode, List, Length, &NewSector, &NewAnode);
        if (!NT_SUCCESS(Status)) {
            if (OldRaw) HpfsHostFree(OldRaw);
            return Status;
        }
    }

    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_WRITE, &Buf, &F);
    if (!NT_SUCCESS(Status)) {
        if (!Inline) FreeStore(Vol, NewSector, NewAnode, Length);
        if (OldRaw) HpfsHostFree(OldRaw);
        return Status;
    }
    if (F->EaOffset == 0) F->EaOffset = HPFS_FNODE_EA_OFFSET;
    HpfsZero((PUCHAR)F + F->EaOffset + F->AclSizeInternal,
             HPFS_SECTOR_SIZE - (F->EaOffset + F->AclSizeInternal));
    if (Inline) {
        HpfsCopy((PUCHAR)F + F->EaOffset + F->AclSizeInternal, List, Length);
        F->EaSizeInternal = (USHORT)Length;
        F->EaSizeExternal = 0;
        F->EaSector = 0;
        F->Flags &= ~HPFS_FNODE_EA_ANODE;
    } else {
        F->EaSizeInternal = 0;
        F->EaSizeExternal = Length;
        F->EaSector = NewSector;
        if (NewAnode) F->Flags |= HPFS_FNODE_EA_ANODE;
        else F->Flags &= ~HPFS_FNODE_EA_ANODE;
    }
    F->NeedEaCount = NeedCount;
    HpfsSetDirty(Vol, &Buf);
    HpfsHostUnmap(Vol, &Buf);

    if (OldRaw) {
        FreeIndirectValues(Vol, OldRaw, OldLen);
        HpfsHostFree(OldRaw);
    }
    FreeStore(Vol, OldSector, OldAnode, OldExt);

    *ExternalSize = Length;
    *NeedEa = (BOOLEAN)(NeedCount != 0);
    return HPFS_OK;
}

HPFS_STATUS
HpfsWriteEas(PHPFS_VOL Vol, ULONG Fnode, const UCHAR *List, ULONG Length,
             PULONG ExternalSize, PBOOLEAN NeedEa)
{
    if (Vol->ReadOnly) return HPFS_READ_ONLY;
    return PlaceEas(Vol, Fnode, List, Length, ExternalSize, NeedEa);
}

/* ------------------------------------------------------------------ */
/* HPFS386 ACLs                                                        */
/* ------------------------------------------------------------------ */

HPFS_STATUS
HpfsReadAcl(PHPFS_VOL Vol, ULONG Fnode, PUCHAR *Acl, PULONG Length)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;
    ULONG In, Ext, Sec;
    BOOLEAN An;
    PUCHAR L;

    *Acl = NULL;
    *Length = 0;
    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_READ, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    In = F->AclSizeInternal;
    Ext = F->AclSizeExternal;
    Sec = F->AclSector;
    An = (BOOLEAN)(F->AclAnode != 0);
    if (F->EaOffset + In > HPFS_SECTOR_SIZE || Ext > 0x100000) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_DISK_CORRUPT;
    }
    if (In + Ext == 0) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_OK;
    }
    L = (PUCHAR)HpfsHostAlloc(In + Ext);
    if (L == NULL) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_NO_MEMORY;
    }
    HpfsCopy(L, (PUCHAR)F + F->EaOffset, In);
    HpfsHostUnmap(Vol, &Buf);
    Status = ReadStore(Vol, Sec, An, Ext, L + In);
    if (!NT_SUCCESS(Status)) {
        HpfsHostFree(L);
        return Status;
    }
    *Acl = L;
    *Length = In + Ext;
    return HPFS_OK;
}

HPFS_STATUS
HpfsWriteAcl(PHPFS_VOL Vol, ULONG Fnode, const UCHAR *Acl, ULONG Length)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;
    PUCHAR Eas;
    ULONG EaLen, OldSec, OldExt, NewSec = 0, Ext;
    BOOLEAN OldAn, NewAn = FALSE, Need;
    BOOLEAN Inline;

    if (Vol->ReadOnly) return HPFS_READ_ONLY;

    /* EAs share the inline area: take them out, place the ACL, put them back */
    Status = HpfsReadEas(Vol, Fnode, &Eas, &EaLen);
    if (!NT_SUCCESS(Status)) return Status;
    Status = PlaceEas(Vol, Fnode, (const UCHAR *)"", 0, &Ext, &Need);
    if (!NT_SUCCESS(Status)) goto Out;

    Inline = (BOOLEAN)(Length <= 128);      /* keep room for EAs inline */
    if (!Inline) {
        Status = WriteStore(Vol, Fnode + 1, Fnode, Acl, Length, &NewSec, &NewAn);
        if (!NT_SUCCESS(Status)) goto Out;
    }
    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_WRITE, &Buf, &F);
    if (!NT_SUCCESS(Status)) {
        if (!Inline) FreeStore(Vol, NewSec, NewAn, Length);
        goto Out;
    }
    OldSec = F->AclSector;
    OldExt = F->AclSizeExternal;
    OldAn = (BOOLEAN)(F->AclAnode != 0);
    if (F->EaOffset == 0) F->EaOffset = HPFS_FNODE_EA_OFFSET;
    HpfsZero((PUCHAR)F + F->EaOffset, HPFS_SECTOR_SIZE - F->EaOffset);
    if (Inline) {
        HpfsCopy((PUCHAR)F + F->EaOffset, Acl, Length);
        F->AclSizeInternal = (USHORT)Length;
        F->AclSizeExternal = 0;
        F->AclSector = 0;
        F->AclAnode = 0;
    } else {
        F->AclSizeInternal = 0;
        F->AclSizeExternal = Length;
        F->AclSector = NewSec;
        F->AclAnode = (UCHAR)(NewAn ? 1 : 0);
    }
    HpfsSetDirty(Vol, &Buf);
    HpfsHostUnmap(Vol, &Buf);
    FreeStore(Vol, OldSec, OldAn, OldExt);

    Status = PlaceEas(Vol, Fnode, Eas ? Eas : (const UCHAR *)"", EaLen, &Ext, &Need);
Out:
    if (Eas) HpfsHostFree(Eas);
    return Status;
}

/* ------------------------------------------------------------------ */
/* HPFS386 DASD limits                                                 */
/* ------------------------------------------------------------------ */

HPFS_STATUS
HpfsDasdCharge(PHPFS_VOL Vol, ULONG DirFnode, LONG Delta)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;
    ULONG Dir, Depth, Parent;
    ULONG Chain[64];
    ULONG Count = 0, I;

    if (!Vol->DasdActive || Delta == 0) {
        return HPFS_OK;
    }

    /* First pass: check every limit on the way up */
    for (Dir = DirFnode, Depth = 0; Depth < 64; Depth++) {
        Status = HpfsMapFnode(Vol, Dir, HPFS_MAP_READ, &Buf, &F);
        if (!NT_SUCCESS(Status)) return Status;
        if (F->DasdLimit != 0) {
            if (Delta > 0 && F->DasdUsage + (ULONG)Delta > F->DasdLimit) {
                HpfsHostUnmap(Vol, &Buf);
                return HPFS_QUOTA;
            }
            Chain[Count++] = Dir;
        }
        Parent = F->Parent;
        HpfsHostUnmap(Vol, &Buf);
        if (Dir == Vol->RootFnode || Parent == Dir || Parent == 0) break;
        Dir = Parent;
    }

    /* Second pass: charge */
    for (I = 0; I < Count; I++) {
        Status = HpfsMapFnode(Vol, Chain[I], HPFS_MAP_WRITE, &Buf, &F);
        if (!NT_SUCCESS(Status)) return Status;
        if (Delta < 0 && (ULONG)(-Delta) > F->DasdUsage) {
            F->DasdUsage = 0;
        } else {
            F->DasdUsage += (ULONG)Delta;
        }
        HpfsSetDirty(Vol, &Buf);
        HpfsHostUnmap(Vol, &Buf);
    }
    return HPFS_OK;
}

/* ------------------------------------------------------------------ */
/* Create / delete / rename                                            */
/* ------------------------------------------------------------------ */

HPFS_STATUS
HpfsCreate(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_ENTRY Entry, BOOLEAN Directory)
{
    HPFS_STATUS Status;
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    ULONG Fnode, Dnode = 0, I;
    HPFS_ENTRY *Old;

    if (Vol->ReadOnly) return HPFS_READ_ONLY;
    if (!HpfsIsLegalName(Entry->Name, Entry->NameLength)) return HPFS_BAD_NAME;

    Old = (HPFS_ENTRY *)HpfsHostAlloc(sizeof(HPFS_ENTRY));
    if (Old == NULL) return HPFS_NO_MEMORY;
    Status = HpfsLookup(Vol, DirFnode, Entry->Name, Entry->NameLength, Old);
    HpfsHostFree(Old);
    if (NT_SUCCESS(Status)) return HPFS_EXISTS;
    if (Status != HPFS_NOT_FOUND) return Status;

    Status = HpfsDasdCharge(Vol, DirFnode, Directory ? 1 + HPFS_DNODE_SECTORS : 1);
    if (!NT_SUCCESS(Status)) return Status;

    Status = HpfsAllocSectors(Vol, DirFnode + 1, 1, HPFS_ALLOC_CONTIG, &Fnode, NULL);
    if (!NT_SUCCESS(Status)) goto Uncharge;

    Status = HpfsHostMap(Vol, Fnode, 1, HPFS_MAP_NEW, &Buf);
    if (!NT_SUCCESS(Status)) goto FreeFnode;
    F = (PHPFS_FNODE)Buf.Data;
    HpfsZero(F, HPFS_SECTOR_SIZE);
    F->Magic = HPFS_FNODE_MAGIC;
    F->NameLength = (UCHAR)Entry->NameLength;
    for (I = 0; I < 15 && I < Entry->NameLength; I++) F->Name[I] = Entry->Name[I];
    F->Parent = DirFnode;
    F->EaOffset = HPFS_FNODE_EA_OFFSET;
    F->Header.FreeCount = HPFS_FNODE_LEAVES;
    F->Header.UsedCount = 0;
    F->Header.FirstFree = sizeof(HPFS_BTREE_HEADER);
    if (Directory) {
        F->Flags = HPFS_FNODE_DIR;
        F->Header.FreeCount = HPFS_FNODE_LEAVES - 1;
        F->Header.UsedCount = 1;
        F->Header.FirstFree = sizeof(HPFS_BTREE_HEADER) + sizeof(HPFS_EXTENT);
    }
    HpfsSetDirty(Vol, &Buf);
    HpfsHostUnmap(Vol, &Buf);

    if (Directory) {
        Status = HpfsNewDirDnode(Vol, Fnode, DirFnode, Entry->WriteTime, &Dnode);
        if (!NT_SUCCESS(Status)) goto FreeFnode;
        Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_WRITE, &Buf, &F);
        if (!NT_SUCCESS(Status)) goto FreeDnode;
        F->u.Extent[0].FileSector = 0;
        F->u.Extent[0].Length = 0;
        F->u.Extent[0].DiskSector = Dnode;
        HpfsSetDirty(Vol, &Buf);
        HpfsHostUnmap(Vol, &Buf);
        Entry->Attributes |= HPFS_ATTR_DIRECTORY;
    } else {
        Entry->Attributes &= ~HPFS_ATTR_DIRECTORY;
    }
    Entry->Attributes &= (HPFS_ATTR_READONLY | HPFS_ATTR_HIDDEN | HPFS_ATTR_SYSTEM |
                          HPFS_ATTR_DIRECTORY | HPFS_ATTR_ARCHIVE);
    if (!HpfsIsDos83(Entry->Name, Entry->NameLength)) {
        Entry->Attributes |= HPFS_ATTR_LONGNAME;
    }
    Entry->Fnode = Fnode;
    Entry->FileSize = 0;
    Entry->EaSize = 0;
    Entry->CodePage = Vol->CodePageIndex;
    Entry->Flags &= ~(HPFS_DE_EA | HPFS_DE_NEEDEA | HPFS_DE_ACL);

    Status = HpfsDirInsert(Vol, DirFnode, Entry);
    if (NT_SUCCESS(Status)) {
        return HPFS_OK;
    }

FreeDnode:
    if (Dnode) HpfsFreeDnode(Vol, Dnode);
FreeFnode:
    HpfsFreeSectors(Vol, Fnode, 1);
Uncharge:
    HpfsDasdCharge(Vol, DirFnode, -(LONG)(Directory ? 1 + HPFS_DNODE_SECTORS : 1));
    return Status;
}

/* Frees everything an fnode owns, then the fnode */
static HPFS_STATUS
FreeFnodeAll(PHPFS_VOL Vol, ULONG Fnode, PULONG Freed)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;
    PUCHAR Raw;
    ULONG RawLen, EaSec, EaExt, AclSec, AclExt, Alloc = 0;
    BOOLEAN EaAn, AclAn, Dir;

    *Freed = 1;
    Status = HpfsMapFnode(Vol, Fnode, HPFS_MAP_READ, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    EaSec = F->EaSector; EaExt = F->EaSizeExternal; EaAn = (BOOLEAN)((F->Flags & HPFS_FNODE_EA_ANODE) != 0);
    AclSec = F->AclSector; AclExt = F->AclSizeExternal; AclAn = (BOOLEAN)(F->AclAnode != 0);
    Dir = (BOOLEAN)((F->Flags & HPFS_FNODE_DIR) != 0);
    HpfsHostUnmap(Vol, &Buf);

    if (!Dir) {
        Status = HpfsGetAllocation(Vol, Fnode, &Alloc);
        if (NT_SUCCESS(Status)) Status = HpfsTruncateAllocation(Vol, Fnode, 0);
        if (!NT_SUCCESS(Status)) return Status;
        *Freed += Alloc;
    }
    if (NT_SUCCESS(ReadRawEas(Vol, Fnode, &Raw, &RawLen)) && Raw != NULL) {
        FreeIndirectValues(Vol, Raw, RawLen);
        HpfsHostFree(Raw);
    }
    FreeStore(Vol, EaSec, EaAn, EaExt);
    FreeStore(Vol, AclSec, AclAn, AclExt);

    /* Clear the magic so a stale fnode is never taken for a live one */
    if (NT_SUCCESS(HpfsHostMap(Vol, Fnode, 1, HPFS_MAP_WRITE, &Buf))) {
        ((PHPFS_FNODE)Buf.Data)->Magic = 0;
        HpfsSetDirty(Vol, &Buf);
        HpfsHostUnmap(Vol, &Buf);
    }
    return HpfsFreeSectors(Vol, Fnode, 1);
}

HPFS_STATUS
HpfsDelete(PHPFS_VOL Vol, ULONG DirFnode, const UCHAR *Name, ULONG Length)
{
    HPFS_STATUS Status;
    HPFS_ENTRY *E;
    ULONG Root = 0, Freed;
    BOOLEAN Empty, Dir;

    if (Vol->ReadOnly) return HPFS_READ_ONLY;
    E = (HPFS_ENTRY *)HpfsHostAlloc(sizeof(HPFS_ENTRY));
    if (E == NULL) return HPFS_NO_MEMORY;

    Status = HpfsLookup(Vol, DirFnode, Name, Length, E);
    if (!NT_SUCCESS(Status)) goto Out;
    Dir = (BOOLEAN)((E->Attributes & HPFS_ATTR_DIRECTORY) != 0);
    if (Dir) {
        Status = HpfsDirIsEmpty(Vol, E->Fnode, &Empty);
        if (NT_SUCCESS(Status) && !Empty) Status = HPFS_NOT_EMPTY;
        if (NT_SUCCESS(Status)) Status = HpfsDirRoot(Vol, E->Fnode, &Root);
        if (!NT_SUCCESS(Status)) goto Out;
    }

    Status = HpfsDirRemove(Vol, DirFnode, E->Name, E->NameLength);
    if (!NT_SUCCESS(Status)) goto Out;

    if (Dir) {
        Status = HpfsFreeDirDnodes(Vol, Root);
        if (!NT_SUCCESS(Status)) goto Out;
    }
    Status = FreeFnodeAll(Vol, E->Fnode, &Freed);
    if (NT_SUCCESS(Status)) {
        HpfsDasdCharge(Vol, DirFnode, -(LONG)(Freed + (Dir ? HPFS_DNODE_SECTORS : 0)));
    }
Out:
    HpfsHostFree(E);
    return Status;
}

/* TRUE if Dir is Ancestor or lies below it */
static HPFS_STATUS
IsBelow(PHPFS_VOL Vol, ULONG Dir, ULONG Ancestor, PBOOLEAN Below)
{
    HPFS_STATUS Status;
    ULONG Parent, Depth;

    *Below = FALSE;
    for (Depth = 0; Depth < 256; Depth++) {
        if (Dir == Ancestor) {
            *Below = TRUE;
            return HPFS_OK;
        }
        if (Dir == Vol->RootFnode) return HPFS_OK;
        Status = HpfsGetFnodeInfo(Vol, Dir, NULL, NULL, &Parent);
        if (!NT_SUCCESS(Status)) return Status;
        if (Parent == Dir) return HPFS_OK;
        Dir = Parent;
    }
    return HPFS_DISK_CORRUPT;
}

HPFS_STATUS
HpfsRename(PHPFS_VOL Vol, ULONG OldDir, const UCHAR *OldName, ULONG OldLength,
           ULONG NewDir, const UCHAR *NewName, ULONG NewLength)
{
    HPFS_STATUS Status;
    HPFS_ENTRY *E, *X;
    BOOLEAN Dir, Below, SameName;
    ULONG I;

    if (Vol->ReadOnly) return HPFS_READ_ONLY;
    if (!HpfsIsLegalName(NewName, NewLength)) return HPFS_BAD_NAME;

    E = (HPFS_ENTRY *)HpfsHostAlloc(2 * sizeof(HPFS_ENTRY));
    if (E == NULL) return HPFS_NO_MEMORY;
    X = E + 1;

    Status = HpfsLookup(Vol, OldDir, OldName, OldLength, E);
    if (!NT_SUCCESS(Status)) goto Out;
    Dir = (BOOLEAN)((E->Attributes & HPFS_ATTR_DIRECTORY) != 0);

    SameName = (BOOLEAN)(OldDir == NewDir &&
                         HpfsCompareNames(Vol, OldName, OldLength, NewName, NewLength) == 0);
    if (!SameName) {
        Status = HpfsLookup(Vol, NewDir, NewName, NewLength, X);
        if (NT_SUCCESS(Status)) {
            Status = HPFS_EXISTS;
            goto Out;
        }
        if (Status != HPFS_NOT_FOUND) goto Out;
    }
    if (Dir && NewDir != OldDir) {
        Status = IsBelow(Vol, NewDir, E->Fnode, &Below);
        if (!NT_SUCCESS(Status)) goto Out;
        if (Below) {
            Status = HPFS_BAD_NAME;
            goto Out;
        }
    }

    if (SameName && OldLength == NewLength) {
        /* Only the case changes: rewrite the name in place */
        for (I = 0; I < NewLength; I++) E->Name[I] = NewName[I];
        Status = HpfsDirUpdate(Vol, OldDir, E);
        if (NT_SUCCESS(Status)) {
            Status = HpfsSetFnodeName(Vol, E->Fnode, NewName, NewLength, NewDir);
        }
        goto Out;
    }

    Status = HpfsDirRemove(Vol, OldDir, E->Name, E->NameLength);
    if (!NT_SUCCESS(Status)) goto Out;

    HpfsCopy(X, E, sizeof(HPFS_ENTRY));
    HpfsCopy(X->Name, NewName, NewLength);
    X->NameLength = NewLength;
    X->Attributes &= ~HPFS_ATTR_LONGNAME;
    if (!HpfsIsDos83(NewName, NewLength)) X->Attributes |= HPFS_ATTR_LONGNAME;
    Status = HpfsDirInsert(Vol, NewDir, X);
    if (!NT_SUCCESS(Status)) {
        /* put the old entry back */
        HpfsDirInsert(Vol, OldDir, E);
        goto Out;
    }
    Status = HpfsSetFnodeName(Vol, E->Fnode, NewName, NewLength, NewDir);
    if (NT_SUCCESS(Status) && Dir && NewDir != OldDir) {
        Status = HpfsSetDotDot(Vol, E->Fnode, NewDir);
    }
Out:
    HpfsHostFree(E);
    return Status;
}

/* ------------------------------------------------------------------ */
/* HPFS386 DASD limits: setting and reading them                       */
/* ------------------------------------------------------------------ */

/* Dnodes of the subtree under dnode Dn */
static HPFS_STATUS
CountDnodes(PHPFS_VOL Vol, ULONG Dn, ULONG Depth, PULONG Count)
{
    HPFS_BUF Buf;
    PHPFS_DNODE D;
    PHPFS_DIRENT E;
    ULONG Off, Downs[HPFS_DNODE_SIZE / 32], N = 0, I;
    HPFS_STATUS Status;

    if (Depth > 32) return HPFS_DISK_CORRUPT;
    Status = HpfsHostMap(Vol, Dn, HPFS_DNODE_SECTORS, HPFS_MAP_READ, &Buf);
    if (!NT_SUCCESS(Status)) return Status;
    D = (PHPFS_DNODE)Buf.Data;
    if (D->Magic != HPFS_DNODE_MAGIC || D->FirstFree > HPFS_DNODE_SIZE) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_DISK_CORRUPT;
    }
    for (Off = HPFS_DNODE_HEADER; Off + 32 <= D->FirstFree; Off += E->Length) {
        E = (PHPFS_DIRENT)(Buf.Data + Off);
        if (E->Length < 32) break;
        if (E->Flags & HPFS_DE_DOWN) Downs[N++] = HpfsDirentDown(E);
        if (E->Flags & HPFS_DE_END) break;
    }
    HpfsHostUnmap(Vol, &Buf);
    (*Count)++;
    for (I = 0; I < N; I++) {
        Status = CountDnodes(Vol, Downs[I], Depth + 1, Count);
        if (!NT_SUCCESS(Status)) return Status;
    }
    return HPFS_OK;
}

/* Sectors a directory and everything in it occupy (fnodes, dnodes, data, EA and ACL stores) */
static HPFS_STATUS
DirUsage(PHPFS_VOL Vol, ULONG DirFnode, ULONG Depth, PULONG Total)
{
    HPFS_STATUS Status;
    HPFS_ENTRY *E;
    ULONG Dn, Dnodes = 0, Alloc, PrevLength = 0;
    PUCHAR Prev;
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    BOOLEAN First = TRUE;

    if (Depth > 64) return HPFS_DISK_CORRUPT;
    Status = HpfsDirRoot(Vol, DirFnode, &Dn);
    if (!NT_SUCCESS(Status)) return Status;
    Status = CountDnodes(Vol, Dn, 0, &Dnodes);
    if (!NT_SUCCESS(Status)) return Status;
    *Total += Dnodes * HPFS_DNODE_SECTORS;

    /* E and, behind it, the name the walk resumes after */
    E = (HPFS_ENTRY *)HpfsHostAlloc(sizeof(HPFS_ENTRY) + HPFS_MAX_NAME + 1);
    if (E == NULL) return HPFS_NO_MEMORY;
    Prev = (PUCHAR)(E + 1);
    for (;;) {
        Status = HpfsNextEntry(Vol, DirFnode, First ? NULL : Prev, First ? 0 : PrevLength, E);
        if (Status == HPFS_NO_MORE_ENTRIES) { Status = HPFS_OK; break; }
        if (!NT_SUCCESS(Status)) break;
        First = FALSE;
        HpfsCopy(Prev, E->Name, E->NameLength);
        PrevLength = E->NameLength;
        *Total += 1;
        Status = HpfsMapFnode(Vol, E->Fnode, HPFS_MAP_READ, &Buf, &F);
        if (!NT_SUCCESS(Status)) break;
        *Total += (F->EaSizeExternal + HPFS_SECTOR_SIZE - 1) / HPFS_SECTOR_SIZE;
        *Total += (F->AclSizeExternal + HPFS_SECTOR_SIZE - 1) / HPFS_SECTOR_SIZE;
        HpfsHostUnmap(Vol, &Buf);
        if (E->Attributes & HPFS_ATTR_DIRECTORY) {
            Status = DirUsage(Vol, E->Fnode, Depth + 1, Total);
        } else {
            Status = HpfsGetAllocation(Vol, E->Fnode, &Alloc);
            *Total += Alloc;
        }
        if (!NT_SUCCESS(Status)) break;
    }
    HpfsHostFree(E);
    return Status;
}

HPFS_STATUS
HpfsDasdQuery(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_DASD Dasd)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;

    Status = HpfsMapFnode(Vol, DirFnode, HPFS_MAP_READ, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    if (!(F->Flags & HPFS_FNODE_DIR)) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_BAD_NAME;
    }
    Dasd->Limit = F->DasdLimit;
    Dasd->Usage = F->DasdUsage;
    Dasd->Threshold = F->DasdThreshold;
    Dasd->Delta = F->DasdDelta;
    Dasd->Active = Vol->DasdActive;
    HpfsHostUnmap(Vol, &Buf);
    if (Dasd->Limit == 0 || !Vol->DasdActive) {
        /* No limit kept: count what is there */
        Dasd->Usage = 0;
        Status = DirUsage(Vol, DirFnode, 0, &Dasd->Usage);
    }
    return Status;
}

/*
 * Sets (Limit != 0) or removes the DASD limit of a directory. Its usage is
 * counted afresh. The first limit on a volume switches enforcement on in
 * the spareblock (HPFS386 "DASD limits installed and active").
 */
HPFS_STATUS
HpfsDasdSet(PHPFS_VOL Vol, ULONG DirFnode, ULONG Limit, ULONG Threshold, ULONG Delta)
{
    HPFS_BUF Buf, Sb, Sp;
    PHPFS_FNODE F;
    PHPFS_SPAREBLOCK Spare;
    HPFS_STATUS Status;
    ULONG Usage = 0;

    if (Vol->ReadOnly) return HPFS_READ_ONLY;
    Status = DirUsage(Vol, DirFnode, 0, &Usage);
    if (!NT_SUCCESS(Status)) return Status;

    Status = HpfsMapFnode(Vol, DirFnode, HPFS_MAP_WRITE, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    if (!(F->Flags & HPFS_FNODE_DIR)) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_BAD_NAME;
    }
    F->DasdLimit = Limit;
    F->DasdUsage = Limit ? Usage : 0;
    F->DasdThreshold = (UCHAR)(Limit ? Threshold : 0);
    F->DasdDelta = (UCHAR)(Limit ? Delta : 0);
    HpfsSetDirty(Vol, &Buf);
    HpfsHostUnmap(Vol, &Buf);

    if (Limit != 0 && !Vol->DasdActive) {
        Status = HpfsHostMap(Vol, HPFS_SUPER_SECTOR, 1, HPFS_MAP_READ, &Sb);
        if (!NT_SUCCESS(Status)) return Status;
        Status = HpfsHostMap(Vol, HPFS_SPARE_SECTOR, 1, HPFS_MAP_WRITE, &Sp);
        if (!NT_SUCCESS(Status)) {
            HpfsHostUnmap(Vol, &Sb);
            return Status;
        }
        Spare = (PHPFS_SPAREBLOCK)Sp.Data;
        Spare->Flags2 |= HPFS_SPARE2_INSTALL_DASD | HPFS_SPARE2_DASD_ACTIVE;
        HpfsUpdateChecksums((PHPFS_SUPERBLOCK)Sb.Data, Spare);
        Vol->SpareFlags2 = Spare->Flags2;
        HpfsSetDirty(Vol, &Sp);
        HpfsHostUnmap(Vol, &Sp);
        HpfsHostUnmap(Vol, &Sb);
        Vol->DasdActive = TRUE;
        Vol->Hpfs386 = TRUE;
    }
    return HPFS_OK;
}
