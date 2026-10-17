/*
 * HPFSNT - directories: the dnode B-tree.
 *
 * A directory is a B-tree of 2 KB dnodes. Each dnode holds directory
 * entries in name order and ends with a special end entry. An entry with
 * a down pointer roots the subtree of names that sort before it; the end
 * entry's down pointer roots the names after the last entry. Keys live in
 * internal nodes too (a classic B-tree, not a B+ tree). Every dnode knows
 * its parent (Up); the top dnode's Up is the directory fnode.
 *
 * The code works on whole dnodes copied into a NODE (an entry pointer
 * array over a 4 KB arena), so a dnode may overflow in memory and is then
 * split before it is written. Deletion takes the in-order predecessor for
 * an internal entry; a dnode that loses its last entry is removed by
 * taking its separator out of the parent and inserting that entry again
 * from the top, which keeps every dnode non-empty without any merging
 * arithmetic.
 */

#ifdef HPFS_KERNEL
#include "hpfs.h"
#else
#include "hpfsusr.h"
#include "hpfssup.h"
#endif

#define DN_PAYLOAD      (HPFS_DNODE_SIZE - HPFS_DNODE_HEADER)       /* 2028 */
#define NODE_ARENA      8192
#define NODE_MAX_ENT    300

typedef struct _NODE {
    ULONG       Sector;
    ULONG       Up;
    UCHAR       RootFlag;
    ULONG       Count;                  /* entries including the end entry */
    ULONG       ArenaUsed;
    PUCHAR      Ent[NODE_MAX_ENT];
    UCHAR       Arena[NODE_ARENA];
} NODE, *PNODE;

#define DE(p)           ((PHPFS_DIRENT)(p))
#define IS_END(p)       ((DE(p)->Flags & HPFS_DE_END) != 0)
#define HAS_DOWN(p)     ((DE(p)->Flags & HPFS_DE_DOWN) != 0)

static ULONG
GetDown(PUCHAR P)
{
    PUCHAR Q = P + DE(P)->Length - 4;
    return (ULONG)Q[0] | ((ULONG)Q[1] << 8) | ((ULONG)Q[2] << 16) | ((ULONG)Q[3] << 24);
}

static PNODE
NewNode(VOID)
{
    PNODE N = (PNODE)HpfsHostAlloc(sizeof(NODE));
    if (N != NULL) {
        N->Count = 0;
        N->ArenaUsed = 0;
    }
    return N;
}

static ULONG
NodeBytes(PNODE N)
{
    ULONG I, B = HPFS_DNODE_HEADER;
    for (I = 0; I < N->Count; I++) {
        B += DE(N->Ent[I])->Length;
    }
    return B;
}

/* Copies an entry into the node's arena, setting (Down != 0) or removing its down pointer */
static PUCHAR
CopyEntry(PNODE N, const UCHAR *Src, BOOLEAN SetDown, ULONG Down)
{
    const HPFS_DIRENT *S = (const HPFS_DIRENT *)Src;
    ULONG Base = S->Length - ((S->Flags & HPFS_DE_DOWN) ? 4 : 0);
    ULONG NewLen = Base + (SetDown ? 4 : 0);
    PUCHAR D;

    if (N->ArenaUsed + NewLen > NODE_ARENA) {
        /* compact: rebuild the arena from the live entries */
        UCHAR *Tmp = (UCHAR *)HpfsHostAlloc(NODE_ARENA);
        ULONG I, Used = 0;
        if (Tmp == NULL) return NULL;
        for (I = 0; I < N->Count; I++) {
            ULONG L = DE(N->Ent[I])->Length;
            HpfsCopy(Tmp + Used, N->Ent[I], L);
            N->Ent[I] = (PUCHAR)(ULONG_PTR)Used;
            Used += L;
        }
        HpfsCopy(N->Arena, Tmp, Used);
        for (I = 0; I < N->Count; I++) {
            N->Ent[I] = N->Arena + (ULONG)(ULONG_PTR)N->Ent[I];
        }
        HpfsHostFree(Tmp);
        N->ArenaUsed = Used;
        if (N->ArenaUsed + NewLen > NODE_ARENA) return NULL;
    }
    D = N->Arena + N->ArenaUsed;
    HpfsCopy(D, Src, Base);
    DE(D)->Length = (USHORT)NewLen;
    if (SetDown) {
        DE(D)->Flags |= HPFS_DE_DOWN;
        D[Base] = (UCHAR)Down; D[Base + 1] = (UCHAR)(Down >> 8);
        D[Base + 2] = (UCHAR)(Down >> 16); D[Base + 3] = (UCHAR)(Down >> 24);
    } else {
        DE(D)->Flags &= ~HPFS_DE_DOWN;
    }
    N->ArenaUsed += NewLen;
    return D;
}

static HPFS_STATUS
NodeInsertAt(PNODE N, ULONG Index, const UCHAR *Src, BOOLEAN SetDown, ULONG Down)
{
    PUCHAR D;
    ULONG I;

    if (N->Count >= NODE_MAX_ENT) return HPFS_DISK_CORRUPT;
    D = CopyEntry(N, Src, SetDown, Down);
    if (D == NULL) return HPFS_NO_MEMORY;
    for (I = N->Count; I > Index; I--) {
        N->Ent[I] = N->Ent[I - 1];
    }
    N->Ent[Index] = D;
    N->Count++;
    return HPFS_OK;
}

static VOID
NodeRemoveAt(PNODE N, ULONG Index)
{
    ULONG I;
    for (I = Index; I + 1 < N->Count; I++) {
        N->Ent[I] = N->Ent[I + 1];
    }
    N->Count--;
}

/* Replaces entry Index with a copy whose down pointer is changed */
static HPFS_STATUS
NodeSetDown(PNODE N, ULONG Index, BOOLEAN SetDown, ULONG Down)
{
    PUCHAR D = CopyEntry(N, N->Ent[Index], SetDown, Down);
    if (D == NULL) return HPFS_NO_MEMORY;
    N->Ent[Index] = D;
    return HPFS_OK;
}

static HPFS_STATUS
LoadNode(PHPFS_VOL Vol, ULONG Sector, PNODE N)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    PHPFS_DNODE D;
    ULONG Off, Len;

    if (Sector < HPFS_BOOT_SECTORS + 2 || Sector + HPFS_DNODE_SECTORS > Vol->TotalSectors) {
        return HPFS_DISK_CORRUPT;
    }
    Status = HpfsHostMap(Vol, Sector, HPFS_DNODE_SECTORS, HPFS_MAP_READ, &Buf);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    D = (PHPFS_DNODE)Buf.Data;
    Status = HPFS_DISK_CORRUPT;
    if (D->Magic != HPFS_DNODE_MAGIC || D->Self != Sector ||
        D->FirstFree > HPFS_DNODE_SIZE || D->FirstFree < HPFS_DNODE_HEADER + 32) {
        goto Out;
    }
    N->Sector = Sector;
    N->Up = D->Up;
    N->RootFlag = D->RootFlag;
    N->Count = 0;
    HpfsCopy(N->Arena, Buf.Data + HPFS_DNODE_HEADER, D->FirstFree - HPFS_DNODE_HEADER);
    N->ArenaUsed = D->FirstFree - HPFS_DNODE_HEADER;
    Off = 0;
    for (;;) {
        PHPFS_DIRENT E;
        if (Off + HPFS_DIRENT_FIXED > N->ArenaUsed || N->Count >= NODE_MAX_ENT) goto Out;
        E = (PHPFS_DIRENT)(N->Arena + Off);
        Len = E->Length;
        if (Len < 32 || (Len & 3) || Off + Len > N->ArenaUsed ||
            (ULONG)(HPFS_DIRENT_FIXED + E->NameLength + ((E->Flags & HPFS_DE_DOWN) ? 4 : 0)) > Len) {
            goto Out;
        }
        N->Ent[N->Count++] = (PUCHAR)E;
        Off += Len;
        if (E->Flags & HPFS_DE_END) break;
    }
    if (Off != N->ArenaUsed) goto Out;
    Status = HPFS_OK;
Out:
    HpfsHostUnmap(Vol, &Buf);
    return Status;
}

static HPFS_STATUS
StoreNode(PHPFS_VOL Vol, PNODE N)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    PHPFS_DNODE D;
    ULONG I, Off;

    if (NodeBytes(N) > HPFS_DNODE_SIZE) {
        return HPFS_DISK_CORRUPT;       /* caller must split first */
    }
    Status = HpfsHostMap(Vol, N->Sector, HPFS_DNODE_SECTORS, HPFS_MAP_NEW, &Buf);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    D = (PHPFS_DNODE)Buf.Data;
    HpfsZero(D, HPFS_DNODE_SIZE);
    D->Magic = HPFS_DNODE_MAGIC;
    D->RootFlag = N->RootFlag;
    D->Up = N->Up;
    D->Self = N->Sector;
    Off = HPFS_DNODE_HEADER;
    for (I = 0; I < N->Count; I++) {
        ULONG L = DE(N->Ent[I])->Length;
        HpfsCopy(Buf.Data + Off, N->Ent[I], L);
        Off += L;
    }
    D->FirstFree = Off;
    HpfsSetDirty(Vol, &Buf);
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

/* Sets the Up field of a dnode */
static HPFS_STATUS
SetUp(PHPFS_VOL Vol, ULONG Dnode, ULONG Up)
{
    HPFS_BUF Buf;
    HPFS_STATUS Status;
    PHPFS_DNODE D;

    if (Dnode < HPFS_BOOT_SECTORS + 2 || Dnode + HPFS_DNODE_SECTORS > Vol->TotalSectors) {
        return HPFS_DISK_CORRUPT;
    }
    Status = HpfsHostMap(Vol, Dnode, HPFS_DNODE_SECTORS, HPFS_MAP_WRITE, &Buf);
    if (!NT_SUCCESS(Status)) return Status;
    D = (PHPFS_DNODE)Buf.Data;
    if (D->Magic != HPFS_DNODE_MAGIC || D->Self != Dnode) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_DISK_CORRUPT;
    }
    if (D->Up != Up) {
        D->Up = Up;
        HpfsSetDirty(Vol, &Buf);
    }
    HpfsHostUnmap(Vol, &Buf);
    return HPFS_OK;
}

/* Up of every child of entries [From, To) of N := N->Sector */
static HPFS_STATUS
AdoptChildren(PHPFS_VOL Vol, PNODE N, ULONG From, ULONG To)
{
    HPFS_STATUS Status;
    ULONG I;

    for (I = From; I < To; I++) {
        if (HAS_DOWN(N->Ent[I])) {
            Status = SetUp(Vol, GetDown(N->Ent[I]), N->Sector);
            if (!NT_SUCCESS(Status)) return Status;
        }
    }
    return HPFS_OK;
}

static LONG
CompareEntry(PHPFS_VOL Vol, const UCHAR *Name, ULONG Length, PUCHAR E)
{
    if (IS_END(E)) {
        return -1;
    }
    return HpfsCompareNames(Vol, Name, Length, DE(E)->Name, DE(E)->NameLength);
}

static VOID
ToEntry(PUCHAR E, ULONG Dnode, PHPFS_ENTRY Out)
{
    PHPFS_DIRENT D = DE(E);
    Out->Fnode = D->Fnode;
    Out->Flags = D->Flags;
    Out->Attributes = D->Attributes;
    Out->CodePage = D->CodePage;
    Out->AclCount = D->AclCount;
    Out->WriteTime = D->WriteTime;
    Out->AccessTime = D->AccessTime;
    Out->CreateTime = D->CreateTime;
    Out->FileSize = D->FileSize;
    Out->EaSize = D->EaSize;
    Out->NameLength = D->NameLength;
    HpfsCopy(Out->Name, D->Name, D->NameLength);
    Out->Name[D->NameLength] = 0;
    Out->Dnode = Dnode;
}

/* Builds a dirent (no down pointer) from an HPFS_ENTRY into Buf (>= 300 bytes) */
static VOID
FromEntry(PHPFS_ENTRY In, PUCHAR Buf)
{
    PHPFS_DIRENT D = DE(Buf);
    ULONG Len = HpfsDirentSize(In->NameLength, 0);

    HpfsZero(Buf, Len);
    D->Length = (USHORT)Len;
    D->Flags = (UCHAR)(In->Flags & ~(HPFS_DE_DOWN | HPFS_DE_END | HPFS_DE_SPECIAL));
    D->Attributes = In->Attributes;
    D->Fnode = In->Fnode;
    D->WriteTime = In->WriteTime;
    D->FileSize = In->FileSize;
    D->AccessTime = In->AccessTime;
    D->CreateTime = In->CreateTime;
    D->EaSize = In->EaSize;
    D->AclCount = In->AclCount;
    D->CodePage = In->CodePage;
    D->NameLength = (UCHAR)In->NameLength;
    HpfsCopy(D->Name, In->Name, In->NameLength);
}

HPFS_STATUS
HpfsDirRoot(PHPFS_VOL Vol, ULONG DirFnode, PULONG Dnode)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    HPFS_STATUS Status;

    Status = HpfsMapFnode(Vol, DirFnode, HPFS_MAP_READ, &Buf, &F);
    if (!NT_SUCCESS(Status)) return Status;
    if (!(F->Flags & HPFS_FNODE_DIR)) {
        HpfsHostUnmap(Vol, &Buf);
        return HPFS_DISK_CORRUPT;
    }
    *Dnode = F->u.Extent[0].DiskSector;
    HpfsHostUnmap(Vol, &Buf);
    if (*Dnode < HPFS_BOOT_SECTORS + 2 || *Dnode + HPFS_DNODE_SECTORS > Vol->TotalSectors) {
        return HPFS_DISK_CORRUPT;
    }
    return HPFS_OK;
}

/* ------------------------------------------------------------------ */
/* Lookup and enumeration                                              */
/* ------------------------------------------------------------------ */

HPFS_STATUS
HpfsLookup(PHPFS_VOL Vol, ULONG DirFnode, const UCHAR *Name, ULONG Length, PHPFS_ENTRY Entry)
{
    PNODE N;
    HPFS_STATUS Status;
    ULONG Dn, I, Depth;
    LONG C;

    Status = HpfsDirRoot(Vol, DirFnode, &Dn);
    if (!NT_SUCCESS(Status)) return Status;
    N = NewNode();
    if (N == NULL) return HPFS_NO_MEMORY;

    for (Depth = 0; Depth < 32; Depth++) {
        Status = LoadNode(Vol, Dn, N);
        if (!NT_SUCCESS(Status)) break;
        Status = HPFS_NOT_FOUND;
        for (I = 0; I < N->Count; I++) {
            C = CompareEntry(Vol, Name, Length, N->Ent[I]);
            if (C == 0 && !(DE(N->Ent[I])->Flags & HPFS_DE_SPECIAL)) {
                ToEntry(N->Ent[I], Dn, Entry);
                HpfsHostFree(N);
                return HPFS_OK;
            }
            if (C < 0) break;
        }
        if (I == N->Count || !HAS_DOWN(N->Ent[I])) {
            break;
        }
        Dn = GetDown(N->Ent[I]);
    }
    if (Depth == 32) Status = HPFS_DISK_CORRUPT;
    HpfsHostFree(N);
    return Status;
}

HPFS_STATUS
HpfsNextEntry(PHPFS_VOL Vol, ULONG DirFnode, const UCHAR *After, ULONG AfterLength, PHPFS_ENTRY Entry)
{
    static const UCHAR DotDot[2] = { 1, 1 };
    PNODE N;
    HPFS_STATUS Status;
    ULONG Dn, I, Depth;
    BOOLEAN Have;

    if (After == NULL) {
        After = DotDot;          /* nothing real sorts at or before "\1\1" */
        AfterLength = 2;
    }
    Status = HpfsDirRoot(Vol, DirFnode, &Dn);
    if (!NT_SUCCESS(Status)) return Status;
    N = NewNode();
    if (N == NULL) return HPFS_NO_MEMORY;

    Have = FALSE;
    for (Depth = 0; Depth < 32; Depth++) {
        Status = LoadNode(Vol, Dn, N);
        if (!NT_SUCCESS(Status)) {
            HpfsHostFree(N);
            return Status;
        }
        for (I = 0; I < N->Count; I++) {
            if (IS_END(N->Ent[I]) || CompareEntry(Vol, After, AfterLength, N->Ent[I]) < 0) {
                break;
            }
        }
        if (I == N->Count) {
            HpfsHostFree(N);
            return HPFS_DISK_CORRUPT;
        }
        if (!IS_END(N->Ent[I])) {
            ToEntry(N->Ent[I], Dn, Entry);   /* best so far: the smallest > After on this path */
            Have = TRUE;
        }
        if (!HAS_DOWN(N->Ent[I])) {
            break;
        }
        Dn = GetDown(N->Ent[I]);
    }
    HpfsHostFree(N);
    if (Depth == 32) return HPFS_DISK_CORRUPT;
    return Have ? HPFS_OK : HPFS_NO_MORE_ENTRIES;
}

HPFS_STATUS
HpfsDirIsEmpty(PHPFS_VOL Vol, ULONG DirFnode, PBOOLEAN Empty)
{
    HPFS_ENTRY *E;
    HPFS_STATUS Status;

    E = (HPFS_ENTRY *)HpfsHostAlloc(sizeof(HPFS_ENTRY));
    if (E == NULL) return HPFS_NO_MEMORY;
    Status = HpfsNextEntry(Vol, DirFnode, NULL, 0, E);
    HpfsHostFree(E);
    if (Status == HPFS_NO_MORE_ENTRIES) {
        *Empty = TRUE;
        return HPFS_OK;
    }
    *Empty = FALSE;
    return Status;
}

/* ------------------------------------------------------------------ */
/* Insertion                                                           */
/* ------------------------------------------------------------------ */

/* Index in Parent of the entry whose down pointer is Child */
static HPFS_STATUS
FindChild(PNODE Parent, ULONG Child, PULONG Index)
{
    ULONG I;
    for (I = 0; I < Parent->Count; I++) {
        if (HAS_DOWN(Parent->Ent[I]) && GetDown(Parent->Ent[I]) == Child) {
            *Index = I;
            return HPFS_OK;
        }
    }
    return HPFS_DISK_CORRUPT;
}

/* ------------------------------------------------------------------ */
/* Dnode reservations: an insertion takes all the dnodes its splits    */
/* could need before it changes anything, so a full disk fails it      */
/* cleanly. When the disk is full the spare dnodes listed in the        */
/* spareblock are used (as OS/2 does), so deleting still works.         */
/* ------------------------------------------------------------------ */

#define POOL_MAX 40

typedef struct _POOL {
    ULONG   Count;
    ULONG   Sector[POOL_MAX];
    BOOLEAN Spare[POOL_MAX];
} POOL;

static VOID
PoolRelease(PHPFS_VOL Vol, POOL *P)
{
    while (P->Count) {
        P->Count--;
        HpfsFreeDnode(Vol, P->Sector[P->Count]);    /* refills the spare list first */
    }
}

static HPFS_STATUS
PoolReserve(PHPFS_VOL Vol, ULONG Near, ULONG Count, POOL *P)
{
    HPFS_STATUS Status;

    P->Count = 0;
    if (Count > POOL_MAX) Count = POOL_MAX;
    while (P->Count < Count) {
        Status = HpfsAllocDnode(Vol, Near, &P->Sector[P->Count]);
        if (NT_SUCCESS(Status)) {
            P->Spare[P->Count++] = FALSE;
            continue;
        }
        if (Status == HPFS_DISK_FULL) {
            Status = HpfsTakeSpareDnode(Vol, &P->Sector[P->Count]);
            if (NT_SUCCESS(Status)) {
                P->Spare[P->Count++] = TRUE;
                continue;
            }
        }
        PoolRelease(Vol, P);
        return Status;
    }
    return HPFS_OK;
}

static HPFS_STATUS
PoolTake(POOL *P, PULONG Dnode)
{
    if (P->Count == 0) return HPFS_DISK_CORRUPT;     /* reservation too small: a bug */
    P->Count--;
    *Dnode = P->Sector[P->Count];
    return HPFS_OK;
}

/* Picks the median of an overflowing node: about half of the bytes on each side */
static ULONG
PickMedian(PNODE N)
{
    ULONG Total = NodeBytes(N) - HPFS_DNODE_HEADER;
    ULONG Acc = 0, M;

    for (M = 0; M + 1 < N->Count; M++) {
        Acc += DE(N->Ent[M])->Length;
        if (Acc * 2 >= Total) break;
    }
    if (M < 1) M = 1;
    if (M > N->Count - 3) M = N->Count - 3;
    return M;
}

/*
 * N (loaded, modified, maybe too big) is written back, splitting it and its
 * ancestors as needed. DirFnode is the directory (for a new root's Up).
 * Consumes nothing; N is scratch afterwards.
 */
static HPFS_STATUS
WriteWithSplit(PHPFS_VOL Vol, ULONG DirFnode, PNODE N, POOL *Pool)
{
    HPFS_STATUS Status;
    PNODE L = NULL, R = NULL;
    ULONG M, I, Near;
    BOOLEAN Internal;
    UCHAR Median[300];
    ULONG MedianDown = 0;
    BOOLEAN MedianHasDown;

    (void)DirFnode;
    L = NewNode();
    R = NewNode();
    if (L == NULL || R == NULL) {
        Status = HPFS_NO_MEMORY;
        goto Out;
    }

    while (NodeBytes(N) > HPFS_DNODE_SIZE) {
        if (N->Count < 4) {
            Status = HPFS_DISK_CORRUPT;
            goto Out;
        }
        M = PickMedian(N);
        Internal = (BOOLEAN)HAS_DOWN(N->Ent[N->Count - 1]);
        MedianHasDown = (BOOLEAN)HAS_DOWN(N->Ent[M]);
        if (MedianHasDown) MedianDown = GetDown(N->Ent[M]);
        HpfsCopy(Median, N->Ent[M], DE(N->Ent[M])->Length);
        Near = N->Sector;

        /* Left part: entries [0, M) + an end entry carrying the median's subtree */
        L->Count = 0; L->ArenaUsed = 0; L->RootFlag = 0;
        Status = PoolTake(Pool, &L->Sector);
        if (!NT_SUCCESS(Status)) goto Out;
        for (I = 0; I < M; I++) {
            Status = NodeInsertAt(L, L->Count, N->Ent[I], HAS_DOWN(N->Ent[I]),
                                  HAS_DOWN(N->Ent[I]) ? GetDown(N->Ent[I]) : 0);
            if (!NT_SUCCESS(Status)) goto Out;
        }
        Status = NodeInsertAt(L, L->Count, N->Ent[N->Count - 1], MedianHasDown, MedianDown);
        if (!NT_SUCCESS(Status)) goto Out;
        (void)Internal;

        if (N->RootFlag & HPFS_DNODE_ROOT) {
            /* The root keeps its sector: both halves move to new dnodes */
            R->Count = 0; R->ArenaUsed = 0; R->RootFlag = 0;
            Status = PoolTake(Pool, &R->Sector);
            if (!NT_SUCCESS(Status)) goto Out;
            for (I = M + 1; I < N->Count; I++) {
                Status = NodeInsertAt(R, R->Count, N->Ent[I], HAS_DOWN(N->Ent[I]),
                                      HAS_DOWN(N->Ent[I]) ? GetDown(N->Ent[I]) : 0);
                if (!NT_SUCCESS(Status)) goto Out;
            }
            L->Up = N->Sector;
            R->Up = N->Sector;
            Status = StoreNode(Vol, L);
            if (NT_SUCCESS(Status)) Status = StoreNode(Vol, R);
            if (NT_SUCCESS(Status)) Status = AdoptChildren(Vol, L, 0, L->Count);
            if (NT_SUCCESS(Status)) Status = AdoptChildren(Vol, R, 0, R->Count);
            if (!NT_SUCCESS(Status)) goto Out;

            /* Root: [median -> L, end -> R] */
            {
                UCHAR End[HPFS_DIRENT_FIXED + 1 + 8];
                HpfsCopy(End, N->Ent[N->Count - 1], 32);   /* end entry: fixed part + "\xff" */
                DE(End)->Length = 32;
                DE(End)->Flags &= ~HPFS_DE_DOWN;
                N->Count = 0;
                N->ArenaUsed = 0;
                DE(Median)->Flags &= ~HPFS_DE_DOWN;
                DE(Median)->Length = (USHORT)HpfsDirentSize(DE(Median)->NameLength, 0);
                Status = NodeInsertAt(N, 0, Median, TRUE, L->Sector);
                if (NT_SUCCESS(Status)) Status = NodeInsertAt(N, 1, End, TRUE, R->Sector);
                if (NT_SUCCESS(Status)) Status = StoreNode(Vol, N);
            }
            goto Out;
        }

        /* Not the root: N keeps the right part, L goes to the parent's left of N */
        L->Up = N->Up;
        for (I = 0; I <= M; I++) {
            NodeRemoveAt(N, 0);
        }
        Status = StoreNode(Vol, L);
        if (NT_SUCCESS(Status)) Status = StoreNode(Vol, N);
        if (NT_SUCCESS(Status)) Status = AdoptChildren(Vol, L, 0, L->Count);
        if (!NT_SUCCESS(Status)) goto Out;

        /* Insert median -> L into the parent, right before the pointer to N */
        {
            ULONG Child = N->Sector, Idx;
            Status = LoadNode(Vol, N->Up, N);
            if (!NT_SUCCESS(Status)) goto Out;
            Status = FindChild(N, Child, &Idx);
            if (!NT_SUCCESS(Status)) goto Out;
            DE(Median)->Flags &= ~HPFS_DE_DOWN;
            DE(Median)->Length = (USHORT)HpfsDirentSize(DE(Median)->NameLength, 0);
            Status = NodeInsertAt(N, Idx, Median, TRUE, L->Sector);
            if (!NT_SUCCESS(Status)) goto Out;
        }
        /* loop: the parent may now overflow */
    }
    Status = StoreNode(Vol, N);

Out:
    if (L) HpfsHostFree(L);
    if (R) HpfsHostFree(R);
    return Status;
}

/* Inserts a ready dirent (no down pointer) into the directory */
static HPFS_STATUS
InsertDirent(PHPFS_VOL Vol, ULONG DirFnode, PUCHAR Dirent)
{
    PNODE N;
    HPFS_STATUS Status;
    ULONG Dn, I, Depth;
    LONG C;

    Status = HpfsDirRoot(Vol, DirFnode, &Dn);
    if (!NT_SUCCESS(Status)) return Status;
    N = NewNode();
    if (N == NULL) return HPFS_NO_MEMORY;

    for (Depth = 0; Depth < 32; Depth++) {
        Status = LoadNode(Vol, Dn, N);
        if (!NT_SUCCESS(Status)) goto Out;
        for (I = 0; I < N->Count; I++) {
            C = CompareEntry(Vol, DE(Dirent)->Name, DE(Dirent)->NameLength, N->Ent[I]);
            if (C == 0) {
                Status = HPFS_EXISTS;
                goto Out;
            }
            if (C < 0) break;
        }
        if (I == N->Count) {
            Status = HPFS_DISK_CORRUPT;
            goto Out;
        }
        if (HAS_DOWN(N->Ent[I])) {
            Dn = GetDown(N->Ent[I]);
            continue;
        }
        Status = NodeInsertAt(N, I, Dirent, FALSE, 0);
        if (NT_SUCCESS(Status)) {
            if (NodeBytes(N) <= HPFS_DNODE_SIZE) {
                Status = StoreNode(Vol, N);
            } else {
                POOL Pool;
                Status = PoolReserve(Vol, N->Sector, Depth + 2, &Pool);
                if (NT_SUCCESS(Status)) {
                    Status = WriteWithSplit(Vol, DirFnode, N, &Pool);
                    PoolRelease(Vol, &Pool);
                }
            }
        }
        goto Out;
    }
    Status = HPFS_DISK_CORRUPT;
Out:
    HpfsHostFree(N);
    return Status;
}

HPFS_STATUS
HpfsDirInsert(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_ENTRY Entry)
{
    UCHAR Buf[300];

    if (Entry->NameLength == 0 || Entry->NameLength > HPFS_MAX_NAME) {
        return HPFS_BAD_NAME;
    }
    FromEntry(Entry, Buf);
    return InsertDirent(Vol, DirFnode, Buf);
}

/* ------------------------------------------------------------------ */
/* Removal                                                             */
/* ------------------------------------------------------------------ */

static ULONG
RealCount(PNODE N)
{
    return N->Count - 1;    /* all but the end entry */
}

/*
 * One step of removing empty dnodes. If dnode Sector has no entries left
 * besides its end entry, takes it out of the tree. An entry that has to
 * leave its place for that is copied to Pending (Length 0 = none) for the
 * caller to insert again; *Next gets a dnode that may have become empty in
 * turn (0 = none).
 */
static HPFS_STATUS
FixOne(PHPFS_VOL Vol, ULONG Sector, PNODE X, PNODE P, PUCHAR Pending, PULONG Next)
{
    HPFS_STATUS Status;
    ULONG Idx, Child;

    DE(Pending)->Length = 0;
    *Next = 0;

    Status = LoadNode(Vol, Sector, X);
    if (!NT_SUCCESS(Status)) return Status;
    if (RealCount(X) > 0) {
        return HPFS_OK;
    }

    if (X->RootFlag & HPFS_DNODE_ROOT) {
        if (!HAS_DOWN(X->Ent[0])) {
            return HPFS_OK;
        }
        /* Pull the only child up into the root */
        Child = GetDown(X->Ent[0]);
        Status = LoadNode(Vol, Child, P);
        if (!NT_SUCCESS(Status)) return Status;
        P->Sector = X->Sector;
        P->Up = X->Up;
        P->RootFlag = X->RootFlag;
        Status = StoreNode(Vol, P);
        if (NT_SUCCESS(Status)) Status = AdoptChildren(Vol, P, 0, P->Count);
        if (NT_SUCCESS(Status)) Status = HpfsFreeDnode(Vol, Child);
        *Next = X->Sector;
        return Status;
    }

    Status = LoadNode(Vol, X->Up, P);
    if (!NT_SUCCESS(Status)) return Status;
    Status = FindChild(P, X->Sector, &Idx);
    if (!NT_SUCCESS(Status)) return Status;

    if (HAS_DOWN(X->Ent[0])) {
        /* One child left: the parent points at it directly */
        Child = GetDown(X->Ent[0]);
        Status = NodeSetDown(P, Idx, TRUE, Child);
        if (NT_SUCCESS(Status)) Status = StoreNode(Vol, P);
        if (NT_SUCCESS(Status)) Status = SetUp(Vol, Child, P->Sector);
        if (NT_SUCCESS(Status)) Status = HpfsFreeDnode(Vol, X->Sector);
        return Status;
    }

    /* An empty leaf */
    if (!IS_END(P->Ent[Idx])) {
        /* The left child of separator S: S leaves the parent */
        HpfsCopy(Pending, P->Ent[Idx], DE(P->Ent[Idx])->Length);
        NodeRemoveAt(P, Idx);
    } else if (Idx > 0) {
        /* The rightmost child: the last separator S leaves, its left
           subtree becomes the rightmost one */
        PUCHAR S = P->Ent[Idx - 1];
        ULONG W = HAS_DOWN(S) ? GetDown(S) : 0;
        HpfsCopy(Pending, S, DE(S)->Length);
        NodeRemoveAt(P, Idx - 1);
        Status = NodeSetDown(P, P->Count - 1, (BOOLEAN)(W != 0), W);
        if (!NT_SUCCESS(Status)) return Status;
    } else {
        /* The parent's only child: the parent becomes a leaf */
        Status = NodeSetDown(P, 0, FALSE, 0);
        if (!NT_SUCCESS(Status)) return Status;
    }
    if (DE(Pending)->Length != 0) {
        DE(Pending)->Flags &= ~HPFS_DE_DOWN;
        DE(Pending)->Length = (USHORT)HpfsDirentSize(DE(Pending)->NameLength, 0);
    }
    Status = StoreNode(Vol, P);
    if (NT_SUCCESS(Status)) Status = HpfsFreeDnode(Vol, X->Sector);
    if (NT_SUCCESS(Status) && RealCount(P) == 0) {
        *Next = P->Sector;
    }
    return Status;
}

#define MAX_PENDING 40

/* Removes empty dnodes from Sector upwards, then inserts the displaced entries */
static HPFS_STATUS
FixEmpty(PHPFS_VOL Vol, ULONG DirFnode, ULONG Sector, PNODE X, PNODE P)
{
    HPFS_STATUS Status = HPFS_OK;
    PUCHAR Pending;
    ULONG Count = 0, I, Next, Rounds;

    Pending = (PUCHAR)HpfsHostAlloc(MAX_PENDING * 300);
    if (Pending == NULL) return HPFS_NO_MEMORY;

    for (Rounds = 0; Sector != 0 && Rounds < 64; Rounds++) {
        if (Count == MAX_PENDING) {
            Status = HPFS_DISK_CORRUPT;
            break;
        }
        Status = FixOne(Vol, Sector, X, P, Pending + Count * 300, &Next);
        if (!NT_SUCCESS(Status)) break;
        if (DE(Pending + Count * 300)->Length != 0) {
            Count++;
        }
        Sector = Next;
    }
    for (I = 0; I < Count && NT_SUCCESS(Status); I++) {
        Status = InsertDirent(Vol, DirFnode, Pending + I * 300);
    }
    HpfsHostFree(Pending);
    return Status;
}

HPFS_STATUS
HpfsDirRemove(PHPFS_VOL Vol, ULONG DirFnode, const UCHAR *Name, ULONG Length)
{
    PNODE N = NULL, Lf = NULL, P = NULL;
    HPFS_STATUS Status;
    ULONG Dn, I, Depth, LeafSector = 0;
    LONG C = 1;
    UCHAR Pred[300];
    ULONG NDepth = 0;
    POOL Pool;
    BOOLEAN Reserved = FALSE;

    Status = HpfsDirRoot(Vol, DirFnode, &Dn);
    if (!NT_SUCCESS(Status)) return Status;
    N = NewNode();
    Lf = NewNode();
    P = NewNode();
    if (N == NULL || Lf == NULL || P == NULL) {
        Status = HPFS_NO_MEMORY;
        goto Out;
    }

    /* Find the entry */
    for (Depth = 0; ; Depth++) {
        if (Depth == 32) {
            Status = HPFS_DISK_CORRUPT;
            goto Out;
        }
        Status = LoadNode(Vol, Dn, N);
        if (!NT_SUCCESS(Status)) goto Out;
        for (I = 0; I < N->Count; I++) {
            C = CompareEntry(Vol, Name, Length, N->Ent[I]);
            if (C <= 0) break;
        }
        if (I < N->Count && C == 0 && !(DE(N->Ent[I])->Flags & HPFS_DE_SPECIAL)) {
            NDepth = Depth;
            break;
        }
        if (I == N->Count || !HAS_DOWN(N->Ent[I])) {
            Status = HPFS_NOT_FOUND;
            goto Out;
        }
        Dn = GetDown(N->Ent[I]);
    }

    if (!HAS_DOWN(N->Ent[I])) {
        /* A leaf entry */
        NodeRemoveAt(N, I);
        Status = StoreNode(Vol, N);
        LeafSector = N->Sector;
    } else {
        /* Internal: replace with the in-order predecessor from the left subtree */
        ULONG Left = GetDown(N->Ent[I]);
        ULONG Ld = Left;
        for (Depth = 0; ; Depth++) {
            if (Depth == 32) {
                Status = HPFS_DISK_CORRUPT;
                goto Out;
            }
            Status = LoadNode(Vol, Ld, Lf);
            if (!NT_SUCCESS(Status)) goto Out;
            if (!HAS_DOWN(Lf->Ent[Lf->Count - 1])) break;
            Ld = GetDown(Lf->Ent[Lf->Count - 1]);
        }
        if (RealCount(Lf) == 0) {
            Status = HPFS_DISK_CORRUPT;
            goto Out;
        }
        if (!(DE(Lf->Ent[Lf->Count - 2])->Flags & HPFS_DE_SPECIAL)) {
            HpfsCopy(Pred, Lf->Ent[Lf->Count - 2], DE(Lf->Ent[Lf->Count - 2])->Length);
            NodeRemoveAt(Lf, Lf->Count - 2);
        } else {
            /* The predecessor would be "..", which stays first in the leftmost
               leaf: take the in-order successor from the right subtree instead */
            if (I + 1 >= N->Count || !HAS_DOWN(N->Ent[I + 1])) {
                Status = HPFS_DISK_CORRUPT;
                goto Out;
            }
            Ld = GetDown(N->Ent[I + 1]);
            for (Depth = 0; ; Depth++) {
                if (Depth == 32) {
                    Status = HPFS_DISK_CORRUPT;
                    goto Out;
                }
                Status = LoadNode(Vol, Ld, Lf);
                if (!NT_SUCCESS(Status)) goto Out;
                if (!HAS_DOWN(Lf->Ent[0])) break;
                Ld = GetDown(Lf->Ent[0]);
            }
            if (RealCount(Lf) == 0 || (DE(Lf->Ent[0])->Flags & HPFS_DE_SPECIAL)) {
                Status = HPFS_DISK_CORRUPT;
                goto Out;
            }
            HpfsCopy(Pred, Lf->Ent[0], DE(Lf->Ent[0])->Length);
            NodeRemoveAt(Lf, 0);
        }

        /* Reserve dnodes before anything changes if N will overflow */
        if (NodeBytes(N) - DE(N->Ent[I])->Length +
            HpfsDirentSize(DE(Pred)->NameLength, 1) > HPFS_DNODE_SIZE) {
            Status = PoolReserve(Vol, N->Sector, NDepth + 2, &Pool);
            if (!NT_SUCCESS(Status)) goto Out;
            Reserved = TRUE;
        }
        Status = StoreNode(Vol, Lf);
        if (!NT_SUCCESS(Status)) goto Out;
        LeafSector = Lf->Sector;

        /* The leaf might be N's own child chain; reload N (unchanged by the store above
           unless Lf == N, which cannot be: Lf is below N) */
        NodeRemoveAt(N, I);
        Status = NodeInsertAt(N, I, Pred, TRUE, Left);
        if (!NT_SUCCESS(Status)) goto Out;
        if (!Reserved) {
            Status = StoreNode(Vol, N);
        } else {
            Status = WriteWithSplit(Vol, DirFnode, N, &Pool);
        }
    }
    if (!NT_SUCCESS(Status)) goto Out;

    /* The leaf that lost an entry may be empty now; the leaf's Up is current
       even if the split above moved it (AdoptChildren keeps it) */
    Status = FixEmpty(Vol, DirFnode, LeafSector, N, P);

Out:
    if (Reserved) PoolRelease(Vol, &Pool);
    if (N) HpfsHostFree(N);
    if (Lf) HpfsHostFree(Lf);
    if (P) HpfsHostFree(P);
    return Status;
}

HPFS_STATUS
HpfsDirUpdate(PHPFS_VOL Vol, ULONG DirFnode, PHPFS_ENTRY Entry)
{
    PNODE N;
    HPFS_STATUS Status;
    ULONG Dn, I, Depth;
    LONG C;

    Status = HpfsDirRoot(Vol, DirFnode, &Dn);
    if (!NT_SUCCESS(Status)) return Status;
    N = NewNode();
    if (N == NULL) return HPFS_NO_MEMORY;

    for (Depth = 0; Depth < 32; Depth++) {
        Status = LoadNode(Vol, Dn, N);
        if (!NT_SUCCESS(Status)) break;
        for (I = 0; I < N->Count; I++) {
            C = CompareEntry(Vol, Entry->Name, Entry->NameLength, N->Ent[I]);
            if (C <= 0) break;
        }
        if (I < N->Count && C == 0 && !(DE(N->Ent[I])->Flags & HPFS_DE_SPECIAL)) {
            PHPFS_DIRENT D = DE(N->Ent[I]);
            D->Attributes = Entry->Attributes;
            D->Flags = (UCHAR)((D->Flags & (HPFS_DE_DOWN | HPFS_DE_END | HPFS_DE_SPECIAL)) |
                               (Entry->Flags & ~(HPFS_DE_DOWN | HPFS_DE_END | HPFS_DE_SPECIAL)));
            D->WriteTime = Entry->WriteTime;
            D->AccessTime = Entry->AccessTime;
            D->CreateTime = Entry->CreateTime;
            D->FileSize = Entry->FileSize;
            D->EaSize = Entry->EaSize;
            D->AclCount = Entry->AclCount;
            /* the name may change case (same length, same order) */
            if (D->NameLength == Entry->NameLength) {
                HpfsCopy(D->Name, Entry->Name, Entry->NameLength);
            }
            Status = StoreNode(Vol, N);
            HpfsHostFree(N);
            return Status;
        }
        if (I == N->Count || !HAS_DOWN(N->Ent[I])) {
            Status = HPFS_NOT_FOUND;
            break;
        }
        Dn = GetDown(N->Ent[I]);
    }
    if (Depth == 32) Status = HPFS_DISK_CORRUPT;
    HpfsHostFree(N);
    return Status;
}

/* ------------------------------------------------------------------ */
/* A new, empty directory: its root dnode                              */
/* ------------------------------------------------------------------ */

HPFS_STATUS
HpfsNewDirDnode(PHPFS_VOL Vol, ULONG DirFnode, ULONG ParentFnode, ULONG Now, PULONG Dnode)
{
    PNODE N;
    HPFS_STATUS Status;
    UCHAR E[64];

    N = NewNode();
    if (N == NULL) return HPFS_NO_MEMORY;
    Status = HpfsAllocDnode(Vol, DirFnode, &N->Sector);
    if (!NT_SUCCESS(Status)) {
        HpfsHostFree(N);
        return Status;
    }
    N->Up = DirFnode;
    N->RootFlag = HPFS_DNODE_ROOT;

    /* ".." : special entry pointing at the parent fnode */
    HpfsZero(E, sizeof(E));
    DE(E)->Length = 36;
    DE(E)->Flags = HPFS_DE_SPECIAL;
    DE(E)->Attributes = HPFS_ATTR_DIRECTORY;
    DE(E)->Fnode = ParentFnode;
    DE(E)->WriteTime = DE(E)->AccessTime = DE(E)->CreateTime = Now;
    DE(E)->NameLength = 2;
    DE(E)->Name[0] = 1;
    DE(E)->Name[1] = 1;
    Status = NodeInsertAt(N, 0, E, FALSE, 0);

    /* end entry */
    HpfsZero(E, sizeof(E));
    DE(E)->Length = 32;
    DE(E)->Flags = HPFS_DE_SPECIAL | HPFS_DE_END;
    DE(E)->WriteTime = DE(E)->AccessTime = DE(E)->CreateTime = Now;
    DE(E)->NameLength = 1;
    DE(E)->Name[0] = 0xFF;
    if (NT_SUCCESS(Status)) Status = NodeInsertAt(N, 1, E, FALSE, 0);
    if (NT_SUCCESS(Status)) Status = StoreNode(Vol, N);
    if (NT_SUCCESS(Status)) {
        *Dnode = N->Sector;
    } else {
        HpfsFreeDnode(Vol, N->Sector);
    }
    HpfsHostFree(N);
    return Status;
}

/* Frees every dnode of a directory (it must be empty but for "..") */
HPFS_STATUS
HpfsFreeDirDnodes(PHPFS_VOL Vol, ULONG Root)
{
    PNODE N;
    HPFS_STATUS Status;
    ULONG Dn = Root, Depth, Next;

    N = NewNode();
    if (N == NULL) return HPFS_NO_MEMORY;
    for (Depth = 0; Depth < 32; Depth++) {
        Status = LoadNode(Vol, Dn, N);
        if (!NT_SUCCESS(Status)) break;
        Next = 0;
        if (N->Count == 2 && HAS_DOWN(N->Ent[1]) && RealCount(N) == 1) {
            Next = 0;       /* ".." with a right subtree: not empty, refuse */
            Status = HPFS_NOT_EMPTY;
            break;
        }
        if (RealCount(N) > 1) {
            Status = HPFS_NOT_EMPTY;
            break;
        }
        if (N->Count >= 1 && HAS_DOWN(N->Ent[0])) {
            Next = GetDown(N->Ent[0]);
        }
        Status = HpfsFreeDnode(Vol, Dn);
        if (!NT_SUCCESS(Status) || Next == 0) break;
        Dn = Next;
    }
    HpfsHostFree(N);
    return Status;
}
