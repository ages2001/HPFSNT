/*
 * HPFSNT - the allocation B+ tree: fnodes and anodes (ALSECs).
 *
 * A file's runs hang off its fnode: up to 8 extents directly, or up to 12
 * pointers to anodes, which hold 40 extents or 60 pointers each. Internal
 * entries carry the first file sector NOT in their subtree; the last entry
 * of every internal node carries 0xFFFFFFFF. The same tree, rooted in an
 * anode, describes EA and ACL data that does not fit in one run.
 *
 * Files grow at the end, so appending is the hot path: it extends the last
 * extent or fills the last leaf in place. Anything that would split a node
 * (and every truncation of a non-trivial tree) rebuilds the tree from the
 * run list, which keeps the code small and the result always balanced.
 */

#ifdef HPFS_KERNEL
#include "hpfs.h"
#else
#include "hpfsusr.h"
#include "hpfssup.h"
#endif

#define FNODE_HDR_OFF   0x38
#define ANODE_HDR_OFF   0x0C

typedef struct _TREE_ROOT {
    HPFS_BUF            Buf;
    PHPFS_BTREE_HEADER  Hdr;
    PUCHAR              Array;
    ULONG               LeafCap;
    ULONG               NodeCap;
} TREE_ROOT;

HPFS_STATUS
HpfsMapFnode(PHPFS_VOL Vol, ULONG Fnode, ULONG Mode, PHPFS_BUF Buf, PHPFS_FNODE *F)
{
    HPFS_STATUS Status;

    if (Fnode < HPFS_BOOT_SECTORS + 2 || Fnode >= Vol->TotalSectors) {
        return HPFS_DISK_CORRUPT;
    }
    Status = HpfsHostMap(Vol, Fnode, 1, Mode, Buf);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    *F = (PHPFS_FNODE)Buf->Data;
    if ((*F)->Magic != HPFS_FNODE_MAGIC) {
        HpfsHostUnmap(Vol, Buf);
        return HPFS_DISK_CORRUPT;
    }
    return HPFS_OK;
}

static HPFS_STATUS
MapAnode(PHPFS_VOL Vol, ULONG Sector, ULONG Mode, PHPFS_BUF Buf, PHPFS_ANODE *A)
{
    HPFS_STATUS Status;

    if (Sector < HPFS_BOOT_SECTORS + 2 || Sector >= Vol->TotalSectors) {
        return HPFS_DISK_CORRUPT;
    }
    Status = HpfsHostMap(Vol, Sector, 1, Mode, Buf);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    *A = (PHPFS_ANODE)Buf->Data;
    if (Mode != HPFS_MAP_NEW && (*A)->Magic != HPFS_ANODE_MAGIC) {
        HpfsHostUnmap(Vol, Buf);
        return HPFS_DISK_CORRUPT;
    }
    return HPFS_OK;
}

static HPFS_STATUS
MapRoot(PHPFS_VOL Vol, ULONG Root, BOOLEAN IsFnode, ULONG Mode, TREE_ROOT *R)
{
    HPFS_STATUS Status;

    if (IsFnode) {
        PHPFS_FNODE F;
        Status = HpfsMapFnode(Vol, Root, Mode, &R->Buf, &F);
        if (!NT_SUCCESS(Status)) return Status;
        R->Hdr = &F->Header;
        R->Array = (PUCHAR)&F->u;
        R->LeafCap = HPFS_FNODE_LEAVES;
        R->NodeCap = HPFS_FNODE_NODES;
    } else {
        PHPFS_ANODE A;
        Status = MapAnode(Vol, Root, Mode, &R->Buf, &A);
        if (!NT_SUCCESS(Status)) return Status;
        R->Hdr = &A->Header;
        R->Array = (PUCHAR)&A->u;
        R->LeafCap = HPFS_ANODE_LEAVES;
        R->NodeCap = HPFS_ANODE_NODES;
    }
    return HPFS_OK;
}

static BOOLEAN
HeaderSane(PHPFS_BTREE_HEADER H, ULONG LeafCap, ULONG NodeCap)
{
    ULONG Cap = (H->Flags & HPFS_BT_INTERNAL) ? NodeCap : LeafCap;
    return (BOOLEAN)(H->UsedCount <= Cap);
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

static HPFS_STATUS
TreeLookup(PHPFS_VOL Vol, ULONG Root, BOOLEAN IsFnode, ULONG Vsn, PULONG Lsn, PULONG RunLength)
{
    HPFS_BUF Buf;
    PHPFS_BTREE_HEADER H;
    PUCHAR Arr;
    HPFS_STATUS Status;
    ULONG Sector = Root, LeafCap, NodeCap, I, Depth;
    BOOLEAN Fnode = IsFnode;

    *Lsn = 0;
    *RunLength = 0;

    for (Depth = 0; Depth < 16; Depth++) {
        if (Fnode) {
            PHPFS_FNODE F;
            Status = HpfsMapFnode(Vol, Sector, HPFS_MAP_READ, &Buf, &F);
            if (!NT_SUCCESS(Status)) return Status;
            H = &F->Header; Arr = (PUCHAR)&F->u;
            LeafCap = HPFS_FNODE_LEAVES; NodeCap = HPFS_FNODE_NODES;
        } else {
            PHPFS_ANODE A;
            Status = MapAnode(Vol, Sector, HPFS_MAP_READ, &Buf, &A);
            if (!NT_SUCCESS(Status)) return Status;
            H = &A->Header; Arr = (PUCHAR)&A->u;
            LeafCap = HPFS_ANODE_LEAVES; NodeCap = HPFS_ANODE_NODES;
        }
        if (!HeaderSane(H, LeafCap, NodeCap)) {
            HpfsHostUnmap(Vol, &Buf);
            return HPFS_DISK_CORRUPT;
        }
        if (H->Flags & HPFS_BT_INTERNAL) {
            PHPFS_BTREE_NODE N = (PHPFS_BTREE_NODE)Arr;
            ULONG Down = 0;
            for (I = 0; I < H->UsedCount; I++) {
                if (N[I].FileSector > Vsn || I + 1 == H->UsedCount) {
                    Down = N[I].Down;
                    break;
                }
            }
            HpfsHostUnmap(Vol, &Buf);
            if (Down == 0) {
                return HPFS_OK;         /* empty internal node: nothing mapped */
            }
            Sector = Down;
            Fnode = FALSE;
            continue;
        }
        {
            PHPFS_EXTENT E = (PHPFS_EXTENT)Arr;
            for (I = 0; I < H->UsedCount; I++) {
                if (Vsn >= E[I].FileSector && Vsn - E[I].FileSector < E[I].Length) {
                    *Lsn = E[I].DiskSector + (Vsn - E[I].FileSector);
                    *RunLength = E[I].Length - (Vsn - E[I].FileSector);
                    break;
                }
            }
        }
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_OK;
    }
    return HPFS_DISK_CORRUPT;
}

HPFS_STATUS
HpfsMapFileSector(PHPFS_VOL Vol, ULONG Fnode, ULONG Vsn, PULONG Lsn, PULONG RunLength)
{
    return TreeLookup(Vol, Fnode, TRUE, Vsn, Lsn, RunLength);
}

/* ------------------------------------------------------------------ */
/* Collecting runs / anodes                                            */
/* ------------------------------------------------------------------ */

typedef struct _RUN_LIST {
    PHPFS_RUN   Runs;
    ULONG       Count;
    ULONG       Max;
    ULONG       Sectors;
    PULONG      Anodes;
    ULONG       AnodeCount;
    ULONG       AnodeMax;
} RUN_LIST;

static HPFS_STATUS
ListAddRun(RUN_LIST *L, ULONG FileSector, ULONG Length, ULONG Disk)
{
    if (L->Count == L->Max) {
        ULONG NewMax = L->Max ? L->Max * 2 : 16;
        PHPFS_RUN N = (PHPFS_RUN)HpfsHostAlloc(NewMax * sizeof(HPFS_RUN));
        if (N == NULL) return HPFS_NO_MEMORY;
        if (L->Runs) {
            HpfsCopy(N, L->Runs, L->Count * sizeof(HPFS_RUN));
            HpfsHostFree(L->Runs);
        }
        L->Runs = N;
        L->Max = NewMax;
    }
    L->Runs[L->Count].FileSector = FileSector;
    L->Runs[L->Count].Length = Length;
    L->Runs[L->Count].DiskSector = Disk;
    L->Count++;
    L->Sectors += Length;
    return HPFS_OK;
}

static HPFS_STATUS
ListAddAnode(RUN_LIST *L, ULONG Sector)
{
    if (L->AnodeCount == L->AnodeMax) {
        ULONG NewMax = L->AnodeMax ? L->AnodeMax * 2 : 16;
        PULONG N = (PULONG)HpfsHostAlloc(NewMax * sizeof(ULONG));
        if (N == NULL) return HPFS_NO_MEMORY;
        if (L->Anodes) {
            HpfsCopy(N, L->Anodes, L->AnodeCount * sizeof(ULONG));
            HpfsHostFree(L->Anodes);
        }
        L->Anodes = N;
        L->AnodeMax = NewMax;
    }
    L->Anodes[L->AnodeCount++] = Sector;
    return HPFS_OK;
}

static VOID
ListFree(RUN_LIST *L)
{
    if (L->Runs) HpfsHostFree(L->Runs);
    if (L->Anodes) HpfsHostFree(L->Anodes);
    L->Runs = NULL;
    L->Anodes = NULL;
}

static HPFS_STATUS
Collect(PHPFS_VOL Vol, ULONG Sector, BOOLEAN IsFnode, RUN_LIST *L, ULONG Depth, BOOLEAN WantRuns)
{
    TREE_ROOT R;
    HPFS_STATUS Status;
    ULONG I, N;
    ULONG Down[HPFS_ANODE_NODES];

    if (Depth > 16) {
        return HPFS_DISK_CORRUPT;
    }
    Status = MapRoot(Vol, Sector, IsFnode, HPFS_MAP_READ, &R);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (!HeaderSane(R.Hdr, R.LeafCap, R.NodeCap)) {
        HpfsHostUnmap(Vol, &R.Buf);
        return HPFS_DISK_CORRUPT;
    }
    N = R.Hdr->UsedCount;
    if (R.Hdr->Flags & HPFS_BT_INTERNAL) {
        PHPFS_BTREE_NODE Nd = (PHPFS_BTREE_NODE)R.Array;
        for (I = 0; I < N; I++) {
            Down[I] = Nd[I].Down;
        }
        HpfsHostUnmap(Vol, &R.Buf);
        for (I = 0; I < N; I++) {
            Status = ListAddAnode(L, Down[I]);
            if (!NT_SUCCESS(Status)) return Status;
            Status = Collect(Vol, Down[I], FALSE, L, Depth + 1, WantRuns);
            if (!NT_SUCCESS(Status)) return Status;
        }
        return HPFS_OK;
    }
    if (WantRuns) {
        PHPFS_EXTENT E = (PHPFS_EXTENT)R.Array;
        for (I = 0; I < N; I++) {
            if (E[I].FileSector != L->Sectors) {
                HpfsHostUnmap(Vol, &R.Buf);
                return HPFS_DISK_CORRUPT;       /* holes or overlaps */
            }
            Status = ListAddRun(L, E[I].FileSector, E[I].Length, E[I].DiskSector);
            if (!NT_SUCCESS(Status)) {
                HpfsHostUnmap(Vol, &R.Buf);
                return Status;
            }
        }
    }
    HpfsHostUnmap(Vol, &R.Buf);
    return HPFS_OK;
}

HPFS_STATUS
HpfsTreeRuns(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, PHPFS_RUN *Runs, PULONG Count, PULONG Sectors)
{
    RUN_LIST L;
    HPFS_STATUS Status;

    HpfsZero(&L, sizeof(L));
    Status = Collect(Vol, Root, RootIsFnode, &L, 0, TRUE);
    if (L.Anodes) HpfsHostFree(L.Anodes);
    if (!NT_SUCCESS(Status)) {
        if (L.Runs) HpfsHostFree(L.Runs);
        return Status;
    }
    *Runs = L.Runs;
    *Count = L.Count;
    if (Sectors) *Sectors = L.Sectors;
    return HPFS_OK;
}

HPFS_STATUS
HpfsGetRuns(PHPFS_VOL Vol, ULONG Fnode, PHPFS_RUN *Runs, PULONG Count, PULONG Sectors)
{
    return HpfsTreeRuns(Vol, Fnode, TRUE, Runs, Count, Sectors);
}

HPFS_STATUS
HpfsFreeTreeAnodes(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode)
{
    RUN_LIST L;
    HPFS_STATUS Status;
    ULONG I;

    HpfsZero(&L, sizeof(L));
    Status = Collect(Vol, Root, RootIsFnode, &L, 0, FALSE);
    if (NT_SUCCESS(Status)) {
        for (I = 0; I < L.AnodeCount && NT_SUCCESS(Status); I++) {
            Status = HpfsFreeSectors(Vol, L.Anodes[I], 1);
        }
    }
    ListFree(&L);
    return Status;
}

/* Allocated sectors: the end of the last extent (cheap: rightmost path only) */
HPFS_STATUS
HpfsGetAllocation(PHPFS_VOL Vol, ULONG Fnode, PULONG Sectors)
{
    TREE_ROOT R;
    HPFS_STATUS Status;
    ULONG Sector = Fnode, Depth;
    BOOLEAN IsFnode = TRUE;

    *Sectors = 0;
    for (Depth = 0; Depth < 16; Depth++) {
        Status = MapRoot(Vol, Sector, IsFnode, HPFS_MAP_READ, &R);
        if (!NT_SUCCESS(Status)) return Status;
        if (!HeaderSane(R.Hdr, R.LeafCap, R.NodeCap)) {
            HpfsHostUnmap(Vol, &R.Buf);
            return HPFS_DISK_CORRUPT;
        }
        if (R.Hdr->UsedCount == 0) {
            HpfsHostUnmap(Vol, &R.Buf);
            return HPFS_OK;
        }
        if (R.Hdr->Flags & HPFS_BT_INTERNAL) {
            Sector = ((PHPFS_BTREE_NODE)R.Array)[R.Hdr->UsedCount - 1].Down;
            HpfsHostUnmap(Vol, &R.Buf);
            IsFnode = FALSE;
            continue;
        }
        {
            PHPFS_EXTENT E = &((PHPFS_EXTENT)R.Array)[R.Hdr->UsedCount - 1];
            *Sectors = E->FileSector + E->Length;
        }
        HpfsHostUnmap(Vol, &R.Buf);
        return HPFS_OK;
    }
    return HPFS_DISK_CORRUPT;
}

/* ------------------------------------------------------------------ */
/* Building                                                            */
/* ------------------------------------------------------------------ */

static VOID
SetHeader(PHPFS_BTREE_HEADER H, BOOLEAN Internal, ULONG Used, ULONG Cap, BOOLEAN FnodeParent)
{
    ULONG EntrySize = Internal ? sizeof(HPFS_BTREE_NODE) : sizeof(HPFS_EXTENT);

    H->Flags = (UCHAR)((Internal ? HPFS_BT_INTERNAL : 0) | (FnodeParent ? HPFS_BT_FNODE_PARENT : 0));
    H->Fill[0] = H->Fill[1] = H->Fill[2] = 0;
    H->UsedCount = (UCHAR)Used;
    H->FreeCount = (UCHAR)(Cap - Used);
    H->FirstFree = (USHORT)(sizeof(HPFS_BTREE_HEADER) + Used * EntrySize);
}

typedef struct _LEVEL_ENT {
    ULONG   End;        /* first file sector after the subtree */
    ULONG   Sector;
} LEVEL_ENT;

/* Writes a new anode at Sector */
static HPFS_STATUS
WriteAnode(PHPFS_VOL Vol, ULONG Sector, ULONG Up, BOOLEAN FnodeParent, BOOLEAN Internal,
           const VOID *Entries, ULONG Used)
{
    HPFS_BUF Buf;
    PHPFS_ANODE A;
    HPFS_STATUS Status;

    Status = MapAnode(Vol, Sector, HPFS_MAP_NEW, &Buf, &A);
    if (!NT_SUCCESS(Status)) return Status;
    HpfsZero(A, HPFS_SECTOR_SIZE);
    A->Magic = HPFS_ANODE_MAGIC;
    A->Self = Sector;
    A->Up = Up;
    SetHeader(&A->Header, Internal, Used, Internal ? HPFS_ANODE_NODES : HPFS_ANODE_LEAVES, FnodeParent);
    HpfsCopy(&A->u, Entries, Used * (Internal ? sizeof(HPFS_BTREE_NODE) : sizeof(HPFS_EXTENT)));
    HpfsSetDirty(Vol, &Buf);
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

/* Fixes the Up pointer (and the fnode-parent flag) of an anode */
static HPFS_STATUS
SetAnodeUp(PHPFS_VOL Vol, ULONG Sector, ULONG Up, BOOLEAN FnodeParent)
{
    HPFS_BUF Buf;
    PHPFS_ANODE A;
    HPFS_STATUS Status;

    Status = MapAnode(Vol, Sector, HPFS_MAP_WRITE, &Buf, &A);
    if (!NT_SUCCESS(Status)) return Status;
    A->Up = Up;
    if (FnodeParent) A->Header.Flags |= HPFS_BT_FNODE_PARENT;
    else A->Header.Flags &= ~HPFS_BT_FNODE_PARENT;
    HpfsSetDirty(Vol, &Buf);
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

HPFS_STATUS
HpfsBuildTree(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, PHPFS_RUN Runs, ULONG Count, ULONG Hint)
{
    TREE_ROOT R;
    HPFS_STATUS Status;
    LEVEL_ENT *Level = NULL, *Next = NULL;
    ULONG LevelCount, NextCount, I, J, Chunk;
    ULONG RootLeafCap = RootIsFnode ? HPFS_FNODE_LEAVES : HPFS_ANODE_LEAVES;
    ULONG RootNodeCap = RootIsFnode ? HPFS_FNODE_NODES : HPFS_ANODE_NODES;
    HPFS_BTREE_NODE Nodes[HPFS_ANODE_NODES];
    PULONG Allocated = NULL;
    ULONG AllocCount = 0, AllocMax;

    if (Count <= RootLeafCap) {
        Status = MapRoot(Vol, Root, RootIsFnode, HPFS_MAP_WRITE, &R);
        if (!NT_SUCCESS(Status)) return Status;
        HpfsZero(R.Array, (RootIsFnode ? HPFS_FNODE_LEAVES : HPFS_ANODE_LEAVES) * sizeof(HPFS_EXTENT));
        HpfsCopy(R.Array, Runs, Count * sizeof(HPFS_EXTENT));
        SetHeader(R.Hdr, FALSE, Count, RootLeafCap, (BOOLEAN)(!RootIsFnode && (R.Hdr->Flags & HPFS_BT_FNODE_PARENT)));
        HpfsSetDirty(Vol, &R.Buf);
        HpfsHostUnmap(Vol, &R.Buf);
        return HPFS_OK;
    }

    /* Leaves: 40 extents per anode */
    LevelCount = (Count + HPFS_ANODE_LEAVES - 1) / HPFS_ANODE_LEAVES;
    AllocMax = LevelCount * 2 + 8;
    Level = (LEVEL_ENT *)HpfsHostAlloc(LevelCount * sizeof(LEVEL_ENT));
    Next = (LEVEL_ENT *)HpfsHostAlloc(LevelCount * sizeof(LEVEL_ENT));
    Allocated = (PULONG)HpfsHostAlloc(AllocMax * sizeof(ULONG));
    if (Level == NULL || Next == NULL || Allocated == NULL) {
        Status = HPFS_NO_MEMORY;
        goto Fail;
    }

    for (I = 0; I < LevelCount; I++) {
        Status = HpfsAllocSectors(Vol, Hint, 1, HPFS_ALLOC_CONTIG, &Level[I].Sector, NULL);
        if (!NT_SUCCESS(Status)) goto Fail;
        Allocated[AllocCount++] = Level[I].Sector;
        Hint = Level[I].Sector + 1;
        Chunk = MIN_UL(HPFS_ANODE_LEAVES, Count - I * HPFS_ANODE_LEAVES);
        Level[I].End = Runs[I * HPFS_ANODE_LEAVES + Chunk - 1].FileSector +
                       Runs[I * HPFS_ANODE_LEAVES + Chunk - 1].Length;
        /* Up is fixed below once the parent is known */
        Status = WriteAnode(Vol, Level[I].Sector, 0, FALSE, FALSE, &Runs[I * HPFS_ANODE_LEAVES], Chunk);
        if (!NT_SUCCESS(Status)) goto Fail;
    }

    /* Internal levels until the root can hold them */
    while (LevelCount > RootNodeCap) {
        NextCount = (LevelCount + HPFS_ANODE_NODES - 1) / HPFS_ANODE_NODES;
        for (I = 0; I < NextCount; I++) {
            ULONG Base = I * HPFS_ANODE_NODES;
            Chunk = MIN_UL(HPFS_ANODE_NODES, LevelCount - Base);
            Status = HpfsAllocSectors(Vol, Hint, 1, HPFS_ALLOC_CONTIG, &Next[I].Sector, NULL);
            if (!NT_SUCCESS(Status)) goto Fail;
            if (AllocCount < AllocMax) Allocated[AllocCount++] = Next[I].Sector;
            Hint = Next[I].Sector + 1;
            for (J = 0; J < Chunk; J++) {
                Nodes[J].FileSector = (J + 1 == Chunk) ? 0xFFFFFFFF : Level[Base + J].End;
                Nodes[J].Down = Level[Base + J].Sector;
                Status = SetAnodeUp(Vol, Level[Base + J].Sector, Next[I].Sector, FALSE);
                if (!NT_SUCCESS(Status)) goto Fail;
            }
            Next[I].End = Level[Base + Chunk - 1].End;
            Status = WriteAnode(Vol, Next[I].Sector, 0, FALSE, TRUE, Nodes, Chunk);
            if (!NT_SUCCESS(Status)) goto Fail;
        }
        {
            LEVEL_ENT *T = Level; Level = Next; Next = T;
        }
        LevelCount = NextCount;
    }

    /* The root */
    for (J = 0; J < LevelCount; J++) {
        Nodes[J].FileSector = (J + 1 == LevelCount) ? 0xFFFFFFFF : Level[J].End;
        Nodes[J].Down = Level[J].Sector;
        Status = SetAnodeUp(Vol, Level[J].Sector, Root, RootIsFnode);
        if (!NT_SUCCESS(Status)) goto Fail;
    }
    Status = MapRoot(Vol, Root, RootIsFnode, HPFS_MAP_WRITE, &R);
    if (!NT_SUCCESS(Status)) goto Fail;
    HpfsZero(R.Array, (RootIsFnode ? HPFS_FNODE_LEAVES : HPFS_ANODE_LEAVES) * sizeof(HPFS_EXTENT));
    HpfsCopy(R.Array, Nodes, LevelCount * sizeof(HPFS_BTREE_NODE));
    SetHeader(R.Hdr, TRUE, LevelCount, RootNodeCap, (BOOLEAN)(!RootIsFnode && (R.Hdr->Flags & HPFS_BT_FNODE_PARENT)));
    HpfsSetDirty(Vol, &R.Buf);
    HpfsHostUnmap(Vol, &R.Buf);

    HpfsHostFree(Level);
    HpfsHostFree(Next);
    HpfsHostFree(Allocated);
    return HPFS_OK;

Fail:
    if (Allocated != NULL) {
        for (I = 0; I < AllocCount; I++) {
            HpfsFreeSectors(Vol, Allocated[I], 1);
        }
        HpfsHostFree(Allocated);
    }
    if (Level) HpfsHostFree(Level);
    if (Next) HpfsHostFree(Next);
    return Status;
}

/* ------------------------------------------------------------------ */
/* Growing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Appends one run in place when possible: merges with the last extent or
 * fills the last leaf. *Done = FALSE when the tree must be rebuilt.
 */
static HPFS_STATUS
AppendInPlace(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, ULONG FileSector, ULONG Length,
              ULONG Disk, PBOOLEAN Done)
{
    TREE_ROOT R;
    HPFS_STATUS Status;
    ULONG Sector = Root, Depth;
    BOOLEAN IsFnode = RootIsFnode;

    *Done = FALSE;
    for (Depth = 0; Depth < 16; Depth++) {
        Status = MapRoot(Vol, Sector, IsFnode, HPFS_MAP_WRITE, &R);
        if (!NT_SUCCESS(Status)) return Status;
        if (!HeaderSane(R.Hdr, R.LeafCap, R.NodeCap)) {
            HpfsHostUnmap(Vol, &R.Buf);
            return HPFS_DISK_CORRUPT;
        }
        if (R.Hdr->Flags & HPFS_BT_INTERNAL) {
            if (R.Hdr->UsedCount == 0) {
                HpfsHostUnmap(Vol, &R.Buf);
                return HPFS_OK;         /* odd; let the rebuild handle it */
            }
            Sector = ((PHPFS_BTREE_NODE)R.Array)[R.Hdr->UsedCount - 1].Down;
            HpfsHostUnmap(Vol, &R.Buf);
            IsFnode = FALSE;
            continue;
        }
        {
            PHPFS_EXTENT E = (PHPFS_EXTENT)R.Array;
            ULONG N = R.Hdr->UsedCount;
            if (N > 0 && E[N - 1].DiskSector + E[N - 1].Length == Disk &&
                E[N - 1].FileSector + E[N - 1].Length == FileSector) {
                E[N - 1].Length += Length;
                *Done = TRUE;
            } else if (N < R.LeafCap &&
                       (N == 0 ? FileSector == 0 : E[N - 1].FileSector + E[N - 1].Length == FileSector)) {
                E[N].FileSector = FileSector;
                E[N].Length = Length;
                E[N].DiskSector = Disk;
                SetHeader(R.Hdr, FALSE, N + 1, R.LeafCap, (BOOLEAN)((R.Hdr->Flags & HPFS_BT_FNODE_PARENT) != 0));
                *Done = TRUE;
            }
        }
        if (*Done) {
            HpfsSetDirty(Vol, &R.Buf);
        }
        HpfsHostUnmap(Vol, &R.Buf);
        return HPFS_OK;
    }
    return HPFS_DISK_CORRUPT;
}

/* Appends a run to the tree under Root (rebuilding it when a node is full) */
static HPFS_STATUS
TreeAppend(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, ULONG FileSector, ULONG Length, ULONG Disk)
{
    HPFS_STATUS Status;
    BOOLEAN Done;
    PHPFS_RUN Runs, Bigger;
    ULONG Count, Sectors;

    Status = AppendInPlace(Vol, Root, RootIsFnode, FileSector, Length, Disk, &Done);
    if (!NT_SUCCESS(Status) || Done) {
        return Status;
    }

    Status = HpfsTreeRuns(Vol, Root, RootIsFnode, &Runs, &Count, &Sectors);
    if (!NT_SUCCESS(Status)) return Status;
    if (Sectors != FileSector) {
        HpfsHostFree(Runs);
        return HPFS_DISK_CORRUPT;
    }
    Bigger = (PHPFS_RUN)HpfsHostAlloc((Count + 1) * sizeof(HPFS_RUN));
    if (Bigger == NULL) {
        if (Runs) HpfsHostFree(Runs);
        return HPFS_NO_MEMORY;
    }
    if (Count) HpfsCopy(Bigger, Runs, Count * sizeof(HPFS_RUN));
    if (Runs) HpfsHostFree(Runs);
    Bigger[Count].FileSector = FileSector;
    Bigger[Count].Length = Length;
    Bigger[Count].DiskSector = Disk;

    Status = HpfsFreeTreeAnodes(Vol, Root, RootIsFnode);
    if (NT_SUCCESS(Status)) {
        Status = HpfsBuildTree(Vol, Root, RootIsFnode, Bigger, Count + 1, Disk);
    }
    HpfsHostFree(Bigger);
    return Status;
}

static HPFS_STATUS TreeTruncate(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, ULONG NewSectors);

HPFS_STATUS
HpfsGrowAllocation(PHPFS_VOL Vol, ULONG Fnode, ULONG NewSectors)
{
    HPFS_STATUS Status;
    ULONG Have, Have0, Need, First, Got, Near, LastLsn, RunLen;

    if (Vol->ReadOnly) {
        return HPFS_READ_ONLY;
    }
    Status = HpfsGetAllocation(Vol, Fnode, &Have);
    if (!NT_SUCCESS(Status) || NewSectors <= Have) {
        return Status;
    }
    Have0 = Have;

    /* Continue right after the last run, else near the fnode */
    Near = Fnode + 1;
    if (Have != 0) {
        Status = HpfsMapFileSector(Vol, Fnode, Have - 1, &LastLsn, &RunLen);
        if (!NT_SUCCESS(Status)) return Status;
        if (LastLsn != 0) Near = LastLsn + 1;
    }

    while (Have < NewSectors) {
        Need = NewSectors - Have;
        Status = HpfsAllocSectors(Vol, Near, Need, HPFS_ALLOC_PARTIAL, &First, &Got);
        if (!NT_SUCCESS(Status)) break;
        Status = TreeAppend(Vol, Fnode, TRUE, Have, Got, First);
        if (!NT_SUCCESS(Status)) {
            HpfsFreeSectors(Vol, First, Got);
            break;
        }
        Have += Got;
        Near = First + Got;
    }
    if (!NT_SUCCESS(Status) && Have > Have0) {
        /* All or nothing: give back what this call added */
        TreeTruncate(Vol, Fnode, TRUE, Have0);
    }
    Vol->DataHint = Near;
    return Status;
}

/* ------------------------------------------------------------------ */
/* Truncating                                                          */
/* ------------------------------------------------------------------ */

static HPFS_STATUS
TreeTruncate(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, ULONG NewSectors)
{
    HPFS_STATUS Status;
    PHPFS_RUN Runs;
    ULONG Count, Sectors, I, Keep;

    Status = HpfsTreeRuns(Vol, Root, RootIsFnode, &Runs, &Count, &Sectors);
    if (!NT_SUCCESS(Status)) return Status;
    if (NewSectors >= Sectors) {
        if (Runs) HpfsHostFree(Runs);
        return HPFS_OK;
    }

    /* Free whole runs past the end and the tail of the one that straddles it */
    Keep = 0;
    for (I = 0; I < Count; I++) {
        PHPFS_RUN Rn = &Runs[I];
        if (Rn->FileSector >= NewSectors) {
            Status = HpfsFreeSectors(Vol, Rn->DiskSector, Rn->Length);
        } else if (Rn->FileSector + Rn->Length > NewSectors) {
            ULONG Cut = NewSectors - Rn->FileSector;
            Status = HpfsFreeSectors(Vol, Rn->DiskSector + Cut, Rn->Length - Cut);
            Rn->Length = Cut;
            Keep = I + 1;
        } else {
            Keep = I + 1;
        }
        if (!NT_SUCCESS(Status)) break;
    }

    if (NT_SUCCESS(Status)) {
        Status = HpfsFreeTreeAnodes(Vol, Root, RootIsFnode);
    }
    if (NT_SUCCESS(Status)) {
        Status = HpfsBuildTree(Vol, Root, RootIsFnode, Runs, Keep, Keep ? Runs[0].DiskSector : Root);
    }
    if (Runs) HpfsHostFree(Runs);
    return Status;
}

HPFS_STATUS
HpfsTruncateAllocation(PHPFS_VOL Vol, ULONG Fnode, ULONG NewSectors)
{
    if (Vol->ReadOnly) {
        return HPFS_READ_ONLY;
    }
    return TreeTruncate(Vol, Fnode, TRUE, NewSectors);
}

HPFS_STATUS
HpfsTreeTruncate(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, ULONG NewSectors)
{
    return TreeTruncate(Vol, Root, RootIsFnode, NewSectors);
}

/* Used by the EA/ACL code: append to a tree rooted in an anode */
HPFS_STATUS
HpfsTreeAppend(PHPFS_VOL Vol, ULONG Root, BOOLEAN RootIsFnode, ULONG FileSector, ULONG Length, ULONG Disk)
{
    return TreeAppend(Vol, Root, RootIsFnode, FileSector, Length, Disk);
}
