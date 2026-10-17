/*
 * HPFSNT - the checker core: verifies every structure of a volume.
 *
 * Walks the directory tree from the root fnode and checks each dnode
 * (signature, self/up links, order, end entry, down pointers), each fnode
 * (back links, name, size against allocation, the allocation tree and its
 * anodes, EAs, ACL areas) and builds a map of every sector in use, which
 * is then compared with the band bitmaps and the directory band bitmap.
 *
 * With HPFS_CHECK_FIX it also repairs what can be repaired safely: entries
 * whose fnode or allocation is damaged are removed, sizes that disagree are
 * made to agree, wrong parent links, entry flags and attributes, stale
 * checksums, and finally the bitmaps (sectors in use are always marked
 * used; lost sectors are freed only when the directory tree is sound).
 */

#include "hpfsusr.h"
#include "hpfssup.h"
#include "hpfschk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* A repair noted during the walk, applied after it */
#define FIX_REMOVE      1       /* remove the entry */
#define FIX_SIZE        2       /* entry and fnode size := Value */
#define FIX_PARENT      3       /* fnode parent := Dir */
#define FIX_ENTRY       4       /* entry attributes := Value, entry flags := Value2 */
#define FIX_DOTDOT      5       /* ".." of directory Fnode := Dir */

typedef struct _FIX {
    ULONG   Type;
    ULONG   Dir;
    ULONG   Fnode;
    ULONG   Value;
    ULONG   Value2;
    ULONG   NameLength;
    UCHAR   Name[256];
} FIX;

typedef struct _CHK {
    PHPFS_VOL       Vol;
    ULONG           Flags;
    FIX            *Fixes;
    ULONG           FixCount;
    ULONG           FixCap;
    ULONG           StructErrors;   /* errors other than in the bitmaps */
    BOOLEAN         InBitmaps;
    PHPFS_CHECK_RESULT R;
    PUCHAR          Used;           /* 1 bit per sector: referenced */
    void          (*Report)(void *Ctx, const char *Msg);
    void           *Ctx;
    ULONG           MaxDepth;
    ULONG           MinLeafDepth;
    ULONG           MaxLeafDepth;
} CHK;

static void
Err(CHK *C, const char *Fmt, ...)
{
    char Buf[512];
    va_list Ap;
    va_start(Ap, Fmt);
    vsnprintf(Buf, sizeof(Buf), Fmt, Ap);
    va_end(Ap);
    C->R->Errors++;
    if (!C->InBitmaps) C->StructErrors++;
    if (C->Report && C->R->Errors <= 200) C->Report(C->Ctx, Buf);
}

static void
AddFix(CHK *C, ULONG Type, ULONG Dir, ULONG Fnode, const UCHAR *Name, ULONG NameLength, ULONG Value, ULONG Value2)
{
    FIX *F;
    if (!(C->Flags & HPFS_CHECK_FIX)) return;
    if (C->FixCount == C->FixCap) {
        ULONG Cap = C->FixCap ? C->FixCap * 2 : 64;
        FIX *N = (FIX *)realloc(C->Fixes, Cap * sizeof(FIX));
        if (N == NULL) return;
        C->Fixes = N;
        C->FixCap = Cap;
    }
    F = &C->Fixes[C->FixCount++];
    F->Type = Type; F->Dir = Dir; F->Fnode = Fnode; F->Value = Value; F->Value2 = Value2;
    F->NameLength = NameLength > 255 ? 255 : NameLength;
    if (Name) memcpy(F->Name, Name, F->NameLength);
}

static void
Warn(CHK *C, const char *Fmt, ...)
{
    char Buf[512];
    va_list Ap;
    va_start(Ap, Fmt);
    vsnprintf(Buf, sizeof(Buf), Fmt, Ap);
    va_end(Ap);
    C->R->Warnings++;
    if (C->Report && C->R->Warnings <= 50) C->Report(C->Ctx, Buf);
}

static BOOLEAN
Claim(CHK *C, ULONG First, ULONG Count, const char *What, ULONG Owner)
{
    ULONG S;
    BOOLEAN Ok = TRUE;
    if (First >= C->Vol->TotalSectors || Count > C->Vol->TotalSectors - First) {
        Err(C, "%s of %08lX: sectors %08lX+%lu out of range", What, (unsigned long)Owner,
            (unsigned long)First, (unsigned long)Count);
        return FALSE;
    }
    for (S = First; S < First + Count; S++) {
        if (C->Used[S >> 3] & (1 << (S & 7))) {
            if (Ok) Err(C, "%s of %08lX: sector %08lX used twice", What, (unsigned long)Owner, (unsigned long)S);
            Ok = FALSE;
        }
        C->Used[S >> 3] |= (UCHAR)(1 << (S & 7));
    }
    return Ok;
}

static void
NameStr(const UCHAR *N, ULONG L, char *Out)
{
    ULONG I;
    for (I = 0; I < L && I < 254; I++) Out[I] = (N[I] >= 32 && N[I] < 127) ? (char)N[I] : '?';
    Out[I] = 0;
}

/* ------------------------------------------------------------------ */
/* Allocation trees                                                    */
/* ------------------------------------------------------------------ */

static ULONG
CheckTree(CHK *C, ULONG Sector, BOOLEAN IsFnode, ULONG Up, ULONG Owner, ULONG *Next, ULONG Depth)
{
    HPFS_BUF Buf;
    PHPFS_BTREE_HEADER H;
    PUCHAR Arr;
    ULONG LeafCap, NodeCap, I, N, Leaves = 0;
    ULONG Downs[HPFS_ANODE_NODES], Bounds[HPFS_ANODE_NODES];

    if (Depth > 10) {
        Err(C, "fnode %08lX: allocation tree too deep", (unsigned long)Owner);
        return 0;
    }
    if (!NT_SUCCESS(HpfsHostMap(C->Vol, Sector, 1, HPFS_MAP_READ, &Buf))) {
        Err(C, "fnode %08lX: cannot read %08lX", (unsigned long)Owner, (unsigned long)Sector);
        return 0;
    }
    if (IsFnode) {
        PHPFS_FNODE F = (PHPFS_FNODE)Buf.Data;
        H = &F->Header; Arr = (PUCHAR)&F->u;
        LeafCap = HPFS_FNODE_LEAVES; NodeCap = HPFS_FNODE_NODES;
    } else {
        PHPFS_ANODE A = (PHPFS_ANODE)Buf.Data;
        if (A->Magic != HPFS_ANODE_MAGIC) {
            Err(C, "fnode %08lX: anode %08lX bad signature", (unsigned long)Owner, (unsigned long)Sector);
            HpfsHostUnmap(C->Vol, &Buf);
            return 0;
        }
        if (A->Self != Sector) Err(C, "anode %08lX: self %08lX", (unsigned long)Sector, (unsigned long)A->Self);
        if (A->Up != Up) Err(C, "anode %08lX: up %08lX, expected %08lX", (unsigned long)Sector,
                             (unsigned long)A->Up, (unsigned long)Up);
        if (Depth == 1 && Up == Owner && !(A->Header.Flags & HPFS_BT_FNODE_PARENT) && IsFnode == FALSE) {
            /* child of the fnode root should carry the flag (OS/2 sets it) */
        }
        H = &A->Header; Arr = (PUCHAR)&A->u;
        LeafCap = HPFS_ANODE_LEAVES; NodeCap = HPFS_ANODE_NODES;
        C->R->Anodes++;
    }
    N = H->UsedCount;
    if (H->Flags & HPFS_BT_INTERNAL) {
        if (N > NodeCap || N == 0) {
            Err(C, "%08lX: bad internal node count %lu", (unsigned long)Sector, (unsigned long)N);
            HpfsHostUnmap(C->Vol, &Buf);
            return 0;
        }
        if (H->FreeCount != NodeCap - N || H->FirstFree != 8 + N * 8) {
            Warn(C, "%08lX: btree header counts %u/%u/%u", (unsigned long)Sector, H->FreeCount, H->UsedCount, H->FirstFree);
        }
        for (I = 0; I < N; I++) {
            Downs[I] = ((PHPFS_BTREE_NODE)Arr)[I].Down;
            Bounds[I] = ((PHPFS_BTREE_NODE)Arr)[I].FileSector;
        }
        HpfsHostUnmap(C->Vol, &Buf);
        for (I = 0; I < N; I++) {
            if (!Claim(C, Downs[I], 1, "anode", Owner)) continue;
            Leaves += CheckTree(C, Downs[I], FALSE, Sector, Owner, Next, Depth + 1);
            if (I + 1 < N && Bounds[I] != *Next) {
                Err(C, "%08lX: node bound %08lX, subtree ends at %08lX", (unsigned long)Sector,
                    (unsigned long)Bounds[I], (unsigned long)*Next);
            }
            if (I + 1 == N && Bounds[I] != 0xFFFFFFFF && Bounds[I] < *Next) {
                Err(C, "%08lX: last bound %08lX below %08lX", (unsigned long)Sector,
                    (unsigned long)Bounds[I], (unsigned long)*Next);
            }
        }
        return Leaves;
    }
    if (N > LeafCap) {
        Err(C, "%08lX: bad leaf count %lu", (unsigned long)Sector, (unsigned long)N);
        HpfsHostUnmap(C->Vol, &Buf);
        return 0;
    }
    if (H->FreeCount != LeafCap - N || H->FirstFree != 8 + N * 12) {
        if (!(IsFnode && N == 0)) {
            Warn(C, "%08lX: btree header counts %u/%u/%u", (unsigned long)Sector, H->FreeCount, H->UsedCount, H->FirstFree);
        }
    }
    {
        HPFS_EXTENT E[HPFS_ANODE_LEAVES];
        memcpy(E, Arr, N * sizeof(HPFS_EXTENT));
        HpfsHostUnmap(C->Vol, &Buf);
        for (I = 0; I < N; I++) {
            if (E[I].FileSector != *Next) {
                Err(C, "fnode %08lX: extent at file sector %08lX, expected %08lX", (unsigned long)Owner,
                    (unsigned long)E[I].FileSector, (unsigned long)*Next);
            }
            if (E[I].Length == 0) Warn(C, "fnode %08lX: empty extent", (unsigned long)Owner);
            Claim(C, E[I].DiskSector, E[I].Length, "data", Owner);
            *Next = E[I].FileSector + E[I].Length;
            C->R->Extents++;
        }
    }
    return N;
}

static void
CheckStore(CHK *C, ULONG Sector, BOOLEAN Anode, ULONG Length, ULONG Owner, const char *What)
{
    if (Length == 0) {
        return;
    }
    if (!Anode) {
        Claim(C, Sector, (Length + 511) / 512, What, Owner);
    } else {
        ULONG Next = 0;
        if (Claim(C, Sector, 1, What, Owner)) {
            CheckTree(C, Sector, FALSE, Owner, Owner, &Next, 1);
            if ((ULONGLONG)Next * 512 < Length) {
                Err(C, "fnode %08lX: %s tree holds %lu sectors for %lu bytes", (unsigned long)Owner, What,
                    (unsigned long)Next, (unsigned long)Length);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Fnodes and directories                                              */
/* ------------------------------------------------------------------ */

static void CheckDir(CHK *C, ULONG DirFnode, ULONG Parent, ULONG Depth);

static void
CheckFnode(CHK *C, ULONG Fnode, ULONG DirFnode, PHPFS_DIRENT De)
{
    HPFS_BUF Buf;
    PHPFS_FNODE F;
    char Name[260];
    ULONG I, Next = 0, Alloc;
    BOOLEAN IsDir = (BOOLEAN)((De->Attributes & HPFS_ATTR_DIRECTORY) != 0);
    ULONG EaSec, EaExt, EaIn, AclSec, AclExt, AclIn, Size, Flags, EaOff, Before;
    BOOLEAN AclAn;

    NameStr(De->Name, De->NameLength, Name);
    if (!Claim(C, Fnode, 1, "fnode", Fnode)) {
        AddFix(C, FIX_REMOVE, DirFnode, Fnode, De->Name, De->NameLength, 0, 0);
        return;
    }
    if (!NT_SUCCESS(HpfsHostMap(C->Vol, Fnode, 1, HPFS_MAP_READ, &Buf))) {
        Err(C, "%s: cannot read fnode %08lX", Name, (unsigned long)Fnode);
        AddFix(C, FIX_REMOVE, DirFnode, Fnode, De->Name, De->NameLength, 0, 0);
        return;
    }
    F = (PHPFS_FNODE)Buf.Data;
    if (F->Magic != HPFS_FNODE_MAGIC) {
        Err(C, "%s: fnode %08lX bad signature", Name, (unsigned long)Fnode);
        HpfsHostUnmap(C->Vol, &Buf);
        AddFix(C, FIX_REMOVE, DirFnode, Fnode, De->Name, De->NameLength, 0, 0);
        return;
    }
    if (F->Parent != DirFnode) {
        Err(C, "%s: fnode %08lX parent %08lX, expected %08lX", Name, (unsigned long)Fnode,
            (unsigned long)F->Parent, (unsigned long)DirFnode);
        AddFix(C, FIX_PARENT, DirFnode, Fnode, De->Name, De->NameLength, 0, 0);
    }
    if (F->NameLength != De->NameLength && !(F->NameLength == 15 && De->NameLength > 15)) {
        Warn(C, "%s: fnode name length %u", Name, F->NameLength);
    }
    for (I = 0; I < 15 && I < De->NameLength; I++) {
        if (F->Name[I] != De->Name[I]) {
            Warn(C, "%s: fnode name differs", Name);
            break;
        }
    }
    if (((F->Flags & HPFS_FNODE_DIR) != 0) != IsDir) {
        Err(C, "%s: fnode directory flag does not match the entry", Name);
        HpfsHostUnmap(C->Vol, &Buf);
        AddFix(C, FIX_REMOVE, DirFnode, Fnode, De->Name, De->NameLength, 0, 0);
        return;
    }
    EaSec = F->EaSector; EaExt = F->EaSizeExternal; EaIn = F->EaSizeInternal;
    AclSec = F->AclSector; AclExt = F->AclSizeExternal; AclIn = F->AclSizeInternal;
    AclAn = (BOOLEAN)(F->AclAnode != 0);
    Size = F->FileSize;
    Flags = F->Flags;
    EaOff = F->EaOffset;
    if (EaOff != 0 && (EaOff < HPFS_FNODE_EA_OFFSET || EaOff + AclIn + EaIn > 512)) {
        Err(C, "%s: fnode EA area %lu+%lu+%lu", Name, (unsigned long)EaOff, (unsigned long)AclIn, (unsigned long)EaIn);
    }
    if (AclIn || AclExt) C->R->Acls++;
    HpfsHostUnmap(C->Vol, &Buf);

    Before = C->R->Errors;
    if (IsDir) {
        C->R->Directories++;
    } else {
        C->R->Files++;
        CheckTree(C, Fnode, TRUE, 0, Fnode, &Next, 0);
        Alloc = Next;
        C->R->DataSectors += Alloc;
    }

    /* EAs */
    CheckStore(C, EaSec, (BOOLEAN)((Flags & HPFS_FNODE_EA_ANODE) != 0), EaExt, Fnode, "EA");
    CheckStore(C, AclSec, AclAn, AclExt, Fnode, "ACL");

    /* A file whose allocation or EA store is damaged goes */
    if (C->R->Errors != Before && !IsDir) {
        AddFix(C, FIX_REMOVE, DirFnode, Fnode, De->Name, De->NameLength, 0, 0);
        return;
    }

    if (!IsDir) {
        ULONG Good = Size;
        if ((ULONGLONG)Alloc * 512 < Size) {
            Err(C, "%s: %lu bytes but only %lu sectors allocated", Name, (unsigned long)Size, (unsigned long)Alloc);
            Good = Alloc * 512;
        }
        if (Size != De->FileSize) {
            Err(C, "%s: size %lu in the entry, %lu in the fnode", Name, (unsigned long)De->FileSize, (unsigned long)Size);
        }
        if (Good != Size || Size != De->FileSize) {
            AddFix(C, FIX_SIZE, DirFnode, Fnode, De->Name, De->NameLength, Good, 0);
        }
    }
    if (EaIn || EaExt) {
        PUCHAR List; ULONG Len;
        if (!NT_SUCCESS(HpfsReadEasRaw(C->Vol, Fnode, &List, &Len))) {
            Err(C, "%s: EA list unreadable", Name);
        } else {
            ULONG Off = 0;
            C->R->EaFiles++;
            while (List && Off + 4 <= Len) {
                PHPFS_EA E = (PHPFS_EA)(List + Off);
                if ((E->Flags & HPFS_EA_INDIRECT) && HpfsEaValueLength(E) == 8) {
                    PUCHAR V = List + Off + 4 + E->NameLength + 1;
                    ULONG VL = V[0] | (V[1] << 8) | ((ULONG)V[2] << 16) | ((ULONG)V[3] << 24);
                    ULONG VS = V[4] | (V[5] << 8) | ((ULONG)V[6] << 16) | ((ULONG)V[7] << 24);
                    CheckStore(C, VS, (BOOLEAN)((E->Flags & HPFS_EA_ANODE) != 0), VL, Fnode, "EA value");
                }
                Off += HpfsEaSize(E);
            }
            if (List) HpfsHostFree(List);
        }
        if (!(De->Flags & HPFS_DE_EA)) {
            Warn(C, "%s: has EAs but the entry lacks the EA flag", Name);
            AddFix(C, FIX_ENTRY, DirFnode, Fnode, De->Name, De->NameLength, De->Attributes,
                   De->Flags | HPFS_DE_EA);
        }
        if (De->EaSize != EaExt + EaIn && De->EaSize != EaExt) {
            Warn(C, "%s: entry EA size %lu, fnode %lu+%lu", Name, (unsigned long)De->EaSize,
                 (unsigned long)EaIn, (unsigned long)EaExt);
        }
    }

    if (IsDir) {
        CheckDir(C, Fnode, DirFnode, 0);
    }
}

typedef struct _DNSTACK {
    UCHAR   Last[256];
    ULONG   LastLen;
    BOOLEAN HaveLast;
} DNSTACK;

/* Checks dnode Dn and its subtree; Lo/Hi bound the names (NULL = open) */
static void
CheckDnode(CHK *C, ULONG DirFnode, ULONG Dn, ULONG Up, BOOLEAN Root, ULONG Depth, DNSTACK *Order)
{
    HPFS_BUF Buf;
    PHPFS_DNODE D;
    PUCHAR Copy;
    ULONG Off, Count = 0, Downs = 0;
    char Name[260];

    if (Depth > 30) {
        Err(C, "directory %08lX: dnode tree too deep", (unsigned long)DirFnode);
        return;
    }
    if (Depth > C->MaxDepth) C->MaxDepth = Depth;
    if (!Claim(C, Dn, 4, "dnode", DirFnode)) return;
    C->R->Dnodes++;
    if (!NT_SUCCESS(HpfsHostMap(C->Vol, Dn, 4, HPFS_MAP_READ, &Buf))) {
        Err(C, "dnode %08lX unreadable", (unsigned long)Dn);
        return;
    }
    Copy = (PUCHAR)malloc(2048);
    memcpy(Copy, Buf.Data, 2048);
    HpfsHostUnmap(C->Vol, &Buf);
    D = (PHPFS_DNODE)Copy;
    if (D->Magic != HPFS_DNODE_MAGIC) {
        Err(C, "dnode %08lX: bad signature", (unsigned long)Dn);
        free(Copy);
        return;
    }
    if (D->Self != Dn) Err(C, "dnode %08lX: self %08lX", (unsigned long)Dn, (unsigned long)D->Self);
    if (D->Up != Up) Err(C, "dnode %08lX: up %08lX, expected %08lX", (unsigned long)Dn,
                         (unsigned long)D->Up, (unsigned long)Up);
    if (((D->RootFlag & 1) != 0) != Root) Err(C, "dnode %08lX: root flag wrong", (unsigned long)Dn);
    if (D->FirstFree > 2048 || D->FirstFree < 20 + 32) {
        Err(C, "dnode %08lX: first free %lu", (unsigned long)Dn, (unsigned long)D->FirstFree);
        free(Copy);
        return;
    }

    Off = 20;
    for (;;) {
        PHPFS_DIRENT De = (PHPFS_DIRENT)(Copy + Off);
        BOOLEAN End, HasDown;
        if (Off + 32 > D->FirstFree || De->Length < 32 || (De->Length & 3) || Off + De->Length > D->FirstFree) {
            Err(C, "dnode %08lX: bad entry at %lu", (unsigned long)Dn, (unsigned long)Off);
            break;
        }
        End = (BOOLEAN)((De->Flags & HPFS_DE_END) != 0);
        HasDown = (BOOLEAN)((De->Flags & HPFS_DE_DOWN) != 0);
        if (De->Length != HpfsDirentSize(De->NameLength, HasDown)) {
            Err(C, "dnode %08lX: entry length %u for a %u byte name", (unsigned long)Dn, De->Length, De->NameLength);
        }
        if (HasDown) Downs++;
        if (HasDown) {
            CheckDnode(C, DirFnode, HpfsDirentDown(De), Dn, FALSE, Depth + 1, Order);
        } else if (Depth + 1 < C->MinLeafDepth || C->MinLeafDepth == 0) {
            /* tracked below */
        }
        if (End) {
            Count++;
            Off += De->Length;
            break;
        }
        NameStr(De->Name, De->NameLength, Name);
        if (Order->HaveLast &&
            HpfsCompareNames(C->Vol, Order->Last, Order->LastLen, De->Name, De->NameLength) >= 0) {
            Err(C, "dnode %08lX: \"%s\" out of order", (unsigned long)Dn, Name);
        }
        memcpy(Order->Last, De->Name, De->NameLength);
        Order->LastLen = De->NameLength;
        Order->HaveLast = TRUE;

        if (De->Flags & HPFS_DE_SPECIAL) {
            if (De->NameLength != 2 || De->Name[0] != 1 || De->Name[1] != 1) {
                Err(C, "dnode %08lX: odd special entry", (unsigned long)Dn);
            } else {
                C->R->DotDots++;
                if (C->R->CurrentParent != 0 && De->Fnode != C->R->CurrentParent) {
                    Err(C, "directory %08lX: \"..\" points at %08lX, parent is %08lX", (unsigned long)DirFnode,
                        (unsigned long)De->Fnode, (unsigned long)C->R->CurrentParent);
                    AddFix(C, FIX_DOTDOT, C->R->CurrentParent, DirFnode, NULL, 0, 0, 0);
                }
            }
        } else {
            if (!HpfsIsLegalName(De->Name, De->NameLength)) {
                Warn(C, "dnode %08lX: odd name \"%s\"", (unsigned long)Dn, Name);
            }
            if (((De->Attributes & HPFS_ATTR_LONGNAME) != 0) != !HpfsIsDos83(De->Name, De->NameLength)) {
                Warn(C, "\"%s\": long-name attribute is %s", Name,
                    (De->Attributes & HPFS_ATTR_LONGNAME) ? "set" : "missing");
                AddFix(C, FIX_ENTRY, DirFnode, De->Fnode, De->Name, De->NameLength,
                       De->Attributes ^ HPFS_ATTR_LONGNAME, De->Flags);
            }
            if (De->CodePage != 0 && (De->CodePage & 0x7F) >= 31) {
                Err(C, "\"%s\": code page index %u", Name, De->CodePage);
            }
            {
                ULONG SaveParent = C->R->CurrentParent;
                CheckFnode(C, De->Fnode, DirFnode, De);
                C->R->CurrentParent = SaveParent;
            }
        }
        Count++;
        Off += De->Length;
    }
    if (Off != D->FirstFree) {
        Err(C, "dnode %08lX: entries end at %lu, first free %lu", (unsigned long)Dn, (unsigned long)Off,
            (unsigned long)D->FirstFree);
    }
    if (Downs != 0 && Downs != Count) {
        Err(C, "dnode %08lX: %lu of %lu entries have down pointers", (unsigned long)Dn,
            (unsigned long)Downs, (unsigned long)Count);
    }
    if (Count == 1 && !Root) {
        Err(C, "dnode %08lX: empty (only the end entry)", (unsigned long)Dn);
    }
    if (Downs == 0) {
        if (C->MinLeafDepth == 0 || Depth + 1 < C->MinLeafDepth) C->MinLeafDepth = Depth + 1;
        if (Depth + 1 > C->MaxLeafDepth) C->MaxLeafDepth = Depth + 1;
    }
    free(Copy);
}

static void
CheckDir(CHK *C, ULONG DirFnode, ULONG Parent, ULONG Depth)
{
    ULONG Root;
    DNSTACK *Order;

    if (!NT_SUCCESS(HpfsDirRoot(C->Vol, DirFnode, &Root))) {
        Err(C, "directory %08lX: no root dnode", (unsigned long)DirFnode);
        return;
    }
    ULONG SaveMin = C->MinLeafDepth, SaveMax = C->MaxLeafDepth, SaveDepth = C->MaxDepth;
    Order = (DNSTACK *)calloc(1, sizeof(DNSTACK));
    C->R->CurrentParent = Parent;
    C->MinLeafDepth = 0;
    C->MaxLeafDepth = 0;
    C->MaxDepth = 0;
    CheckDnode(C, DirFnode, Root, DirFnode, TRUE, 0, Order);
    free(Order);
    if (C->MinLeafDepth != C->MaxLeafDepth) {
        C->R->Unbalanced++;
    }
    if (C->MaxDepth + 1 > C->R->DnodeDepth) C->R->DnodeDepth = C->MaxDepth + 1;
    C->MinLeafDepth = SaveMin;
    C->MaxLeafDepth = SaveMax;
    C->MaxDepth = SaveDepth;
    (void)Depth;
}

/* ------------------------------------------------------------------ */
/* Whole volume                                                        */
/* ------------------------------------------------------------------ */

/* One walk of the volume: fills C->Used and C->R */
static int
CheckPass(CHK *Cp)
{
#define C (*Cp)
    PHPFS_VOL Vol = Cp->Vol;
    PHPFS_CHECK_RESULT R = Cp->R;
    HPFS_BUF Buf;
    ULONG I, S, Band, Diff = 0, Lost = 0;
    PHPFS_SUPERBLOCK Super;
    PHPFS_SPAREBLOCK Spare;
    ULONG HotfixMap, HotfixMax, CpDir, BadList, Uid, ListSectors;

    memset(R, 0, sizeof(*R));
    C.StructErrors = 0;
    C.InBitmaps = FALSE;
    C.MaxDepth = C.MinLeafDepth = C.MaxLeafDepth = 0;
    C.Used = (PUCHAR)calloc((Vol->TotalSectors + 7) / 8 + 1, 1);
    if (C.Used == NULL) return -1;

    /* Fixed structures */
    Claim(&C, 0, 20, "boot/super/spare", 0);
    if (!NT_SUCCESS(HpfsHostMap(Vol, HPFS_SUPER_SECTOR, 1, HPFS_MAP_READ, &Buf))) return -1;
    Super = (PHPFS_SUPERBLOCK)Buf.Data;
    BadList = Super->BadBlockList;
    Uid = Super->UserIdTable;
    {
        UCHAR Sb[512];
        HPFS_BUF B2;
        memcpy(Sb, Buf.Data, 512);
        HpfsHostUnmap(Vol, &Buf);
        if (!NT_SUCCESS(HpfsHostMap(Vol, HPFS_SPARE_SECTOR, 1, HPFS_MAP_READ, &B2))) return -1;
        Spare = (PHPFS_SPAREBLOCK)B2.Data;
        HotfixMap = Spare->HotfixMap;
        HotfixMax = Spare->HotfixMax;
        CpDir = Spare->CodePageDir;
        if (Spare->SuperChecksum != HpfsChecksum(Sb, 512)) Warn(&C, "superblock checksum stale");
        {
            UCHAR Sp[512];
            memcpy(Sp, B2.Data, 512);
            memset(Sp + 0x2C, 0, 4);
            if (Spare->SpareChecksum != HpfsChecksum(Sp, 512)) Warn(&C, "spareblock checksum stale");
        }
        if (Spare->Flags & HPFS_SPARE_DIRTY) {
            R->Dirty = 1;
        }
        Claim(&C, HotfixMap, 4, "hotfix map", 0);
        {
            HPFS_BUF HB;
            if (NT_SUCCESS(HpfsHostMap(Vol, HotfixMap, 4, HPFS_MAP_READ, &HB))) {
                for (I = 0; I < HotfixMax && I < HPFS_MAX_HOTFIX; I++) {
                    ULONG Sp2 = ((PULONG)HB.Data)[HotfixMax + I];
                    if (Sp2) Claim(&C, Sp2, 1, "hotfix spare", 0);
                }
                HpfsHostUnmap(Vol, &HB);
            }
        }
        for (I = 0; I < Spare->SpareDnodesFree && I < 101; I++) {
            Claim(&C, Spare->SpareDnode[I], 4, "spare dnode", 0);
        }
        HpfsHostUnmap(Vol, &B2);
    }
    ListSectors = ((Vol->BandCount * 4 + 511) / 512 + 3) & ~3UL;
    Claim(&C, Vol->BitmapListSector, ListSectors, "bitmap list", 0);
    for (I = 0; I < Vol->BandCount; I++) Claim(&C, Vol->BandBitmap[I], 4, "bitmap", I);
    if (BadList) Claim(&C, BadList, 4, "bad block list", 0);
    if (Uid) Claim(&C, Uid, 8, "user id table", 0);
    if (Vol->DirBandDnodes) {
        Claim(&C, Vol->DirBandBitmap, 4, "dirband bitmap", 0);
    }
    if (CpDir) {
        HPFS_BUF CB;
        Claim(&C, CpDir, 1, "code page directory", 0);
        if (NT_SUCCESS(HpfsHostMap(Vol, CpDir, 1, HPFS_MAP_READ, &CB))) {
            PHPFS_CPDIR D = (PHPFS_CPDIR)CB.Data;
            ULONG Seen[31], NSeen = 0, J;
            for (I = 0; I < D->Count && I < 31; I++) {
                ULONG DS = D->Entry[I].DataSector;
                for (J = 0; J < NSeen && Seen[J] != DS; J++) ;
                if (J == NSeen) { Seen[NSeen++] = DS; Claim(&C, DS, 1, "code page data", 0); }
            }
            HpfsHostUnmap(Vol, &CB);
        }
    }

    /* Root fnode and the tree */
    {
        HPFS_DIRENT RootDe;
        memset(&RootDe, 0, sizeof(RootDe));
        RootDe.Attributes = HPFS_ATTR_DIRECTORY;
        Claim(&C, Vol->RootFnode, 1, "root fnode", 0);
        R->Directories++;
        CheckDir(&C, Vol->RootFnode, Vol->RootFnode, 0);
    }

    /* Bitmaps against the map of used sectors */
    C.InBitmaps = TRUE;
    for (Band = 0; Band < Vol->BandCount; Band++) {
        ULONG Base = Band * HPFS_BAND_SECTORS;
        ULONG Lim = Vol->TotalSectors - Base < HPFS_BAND_SECTORS ? Vol->TotalSectors - Base : HPFS_BAND_SECTORS;
        if (!NT_SUCCESS(HpfsHostMap(Vol, Vol->BandBitmap[Band], 4, HPFS_MAP_READ, &Buf))) {
            Err(&C, "bitmap of band %lu unreadable", (unsigned long)Band);
            continue;
        }
        for (I = 0; I < Lim; I++) {
            S = Base + I;
            {
                BOOLEAN Free = (BOOLEAN)((Buf.Data[I >> 3] >> (I & 7)) & 1);
                BOOLEAN InUse = (BOOLEAN)((C.Used[S >> 3] >> (S & 7)) & 1);
                BOOLEAN InDirBand = (BOOLEAN)(Vol->DirBandDnodes && S >= Vol->DirBandStart &&
                                              S < Vol->DirBandStart + Vol->DirBandDnodes * 4);
                if (InDirBand) {
                    if (Free) { if (Diff++ < 20) Err(&C, "dirband sector %08lX free in the bitmap", (unsigned long)S); }
                    continue;
                }
                if (Free && InUse) {
                    if (Diff++ < 20) Err(&C, "sector %08lX in use but free in the bitmap", (unsigned long)S);
                } else if (!Free && !InUse) {
                    Lost++;
                } else if (Free) {
                    R->FreeSectors++;
                }
            }
        }
        HpfsHostUnmap(Vol, &Buf);
    }
    if (Diff > 20) Err(&C, "... %lu sectors in use but free in the bitmap", (unsigned long)Diff);
    if (Lost) {
        Err(&C, "%lu sectors marked used but not referenced (lost)", (unsigned long)Lost);
        R->LostSectors = Lost;
    }

    /* Directory band bitmap against the dnodes found */
    if (Vol->DirBandDnodes) {
        if (NT_SUCCESS(HpfsHostMap(Vol, Vol->DirBandBitmap, 4, HPFS_MAP_READ, &Buf))) {
            ULONG Bad = 0;
            for (I = 0; I < Vol->DirBandDnodes; I++) {
                ULONG Dn = Vol->DirBandStart + I * 4;
                BOOLEAN Free = (BOOLEAN)((Buf.Data[I >> 3] >> (I & 7)) & 1);
                BOOLEAN InUse = (BOOLEAN)((C.Used[Dn >> 3] >> (Dn & 7)) & 1);
                if (Free == InUse) {
                    if (Bad++ < 10) Err(&C, "dirband dnode %08lX: bitmap says %s", (unsigned long)Dn, Free ? "free" : "used");
                } else if (Free) {
                    R->FreeDnodes++;
                }
            }
            HpfsHostUnmap(Vol, &Buf);
        }
    }

    C.InBitmaps = FALSE;
    return (int)R->Errors;
#undef C
}

static void
Say(CHK *C, const char *Fmt, ...)
{
    char Buf[512];
    va_list Ap;
    if (C->Report == NULL) return;
    va_start(Ap, Fmt);
    vsnprintf(Buf, sizeof(Buf), Fmt, Ap);
    va_end(Ap);
    C->Report(C->Ctx, Buf);
}

/* Applies the repairs noted by the walk; returns how many took */
static ULONG
ApplyFixes(CHK *C)
{
    PHPFS_VOL Vol = C->Vol;
    HPFS_ENTRY E;
    ULONG I, Pass, Done = 0;
    HPFS_STATUS St;
    char Name[260];

    /* Everything else before removals (a removal can move entries between dnodes) */
    for (Pass = 0; Pass < 2; Pass++) {
        for (I = 0; I < C->FixCount; I++) {
            FIX *F = &C->Fixes[I];
            if ((F->Type == FIX_REMOVE) != (Pass == 1)) continue;
            NameStr(F->Name, F->NameLength, Name);
            St = HPFS_OK;
            switch (F->Type) {
            case FIX_REMOVE:
                St = HpfsDirRemove(Vol, F->Dir, F->Name, F->NameLength);
                if (NT_SUCCESS(St)) Say(C, "removed the damaged entry \"%s\"", Name);
                break;
            case FIX_SIZE:
                St = HpfsLookup(Vol, F->Dir, F->Name, F->NameLength, &E);
                if (NT_SUCCESS(St)) {
                    E.FileSize = F->Value;
                    St = HpfsDirUpdate(Vol, F->Dir, &E);
                }
                if (NT_SUCCESS(St)) St = HpfsSetFnodeSize(Vol, F->Fnode, F->Value);
                if (NT_SUCCESS(St)) Say(C, "\"%s\": size set to %lu", Name, (unsigned long)F->Value);
                break;
            case FIX_PARENT:
                St = HpfsSetFnodeName(Vol, F->Fnode, F->Name, F->NameLength, F->Dir);
                if (NT_SUCCESS(St)) Say(C, "\"%s\": fnode links corrected", Name);
                break;
            case FIX_ENTRY:
                St = HpfsLookup(Vol, F->Dir, F->Name, F->NameLength, &E);
                if (NT_SUCCESS(St)) {
                    E.Attributes = (UCHAR)F->Value;
                    E.Flags = (UCHAR)F->Value2;
                    St = HpfsDirUpdate(Vol, F->Dir, &E);
                }
                if (NT_SUCCESS(St)) Say(C, "\"%s\": entry flags corrected", Name);
                break;
            case FIX_DOTDOT:
                St = HpfsSetDotDot(Vol, F->Fnode, F->Dir);
                if (NT_SUCCESS(St)) Say(C, "directory %08lX: \"..\" corrected", (unsigned long)F->Fnode);
                break;
            }
            if (NT_SUCCESS(St)) {
                Done++;
            } else {
                Say(C, "could not repair \"%s\" (status %08lX)", Name, (unsigned long)St);
            }
        }
    }
    return Done;
}

/* Rewrites the band bitmaps and the directory band bitmap from C->Used */
static ULONG
WriteBitmaps(CHK *C, BOOLEAN FreeLost)
{
    PHPFS_VOL Vol = C->Vol;
    HPFS_BUF Buf;
    ULONG Band, I, S, Changed = 0;

    for (Band = 0; Band < Vol->BandCount; Band++) {
        ULONG Base = Band * HPFS_BAND_SECTORS;
        ULONG Lim = Vol->TotalSectors - Base < HPFS_BAND_SECTORS ? Vol->TotalSectors - Base : HPFS_BAND_SECTORS;
        BOOLEAN Dirty = FALSE;
        if (!NT_SUCCESS(HpfsHostMap(Vol, Vol->BandBitmap[Band], 4, HPFS_MAP_WRITE, &Buf))) continue;
        for (I = 0; I < Lim; I++) {
            BOOLEAN Free, InUse, Want;
            S = Base + I;
            Free = (BOOLEAN)((Buf.Data[I >> 3] >> (I & 7)) & 1);
            InUse = (BOOLEAN)((C->Used[S >> 3] >> (S & 7)) & 1);
            if (Vol->DirBandDnodes && S >= Vol->DirBandStart && S < Vol->DirBandStart + Vol->DirBandDnodes * 4) {
                InUse = TRUE;
            }
            Want = InUse ? FALSE : (FreeLost ? TRUE : Free);
            if (Want != Free) {
                Buf.Data[I >> 3] ^= (UCHAR)(1 << (I & 7));
                Dirty = TRUE;
                Changed++;
            }
        }
        if (Dirty) HpfsSetDirty(Vol, &Buf);
        HpfsHostUnmap(Vol, &Buf);
    }
    if (Vol->DirBandDnodes && NT_SUCCESS(HpfsHostMap(Vol, Vol->DirBandBitmap, 4, HPFS_MAP_WRITE, &Buf))) {
        BOOLEAN Dirty = FALSE;
        for (I = 0; I < Vol->DirBandDnodes; I++) {
            ULONG Dn = Vol->DirBandStart + I * 4;
            BOOLEAN Free = (BOOLEAN)((Buf.Data[I >> 3] >> (I & 7)) & 1);
            BOOLEAN InUse = (BOOLEAN)((C->Used[Dn >> 3] >> (Dn & 7)) & 1);
            BOOLEAN Want = InUse ? FALSE : (FreeLost ? TRUE : Free);
            if (Want != Free) {
                Buf.Data[I >> 3] ^= (UCHAR)(1 << (I & 7));
                Dirty = TRUE;
                Changed++;
            }
        }
        if (Dirty) HpfsSetDirty(Vol, &Buf);
        HpfsHostUnmap(Vol, &Buf);
    }
    Vol->FreeKnown = FALSE;
    for (Band = 0; Band < Vol->BandCount; Band++) Vol->BandFree[Band] = HPFS_UNKNOWN;
    Vol->DirBandFree = HPFS_UNKNOWN;
    return Changed;
}

/* Rewrites both checksums (and with Clean, clears the dirty bit) */
static void
WriteSuperSpare(PHPFS_VOL Vol, BOOLEAN Clean)
{
    HPFS_BUF Sb, Sp;
    if (Clean) {
        HpfsMarkDirty(Vol, FALSE);
        return;
    }
    if (!NT_SUCCESS(HpfsHostMap(Vol, HPFS_SUPER_SECTOR, 1, HPFS_MAP_READ, &Sb))) return;
    if (NT_SUCCESS(HpfsHostMap(Vol, HPFS_SPARE_SECTOR, 1, HPFS_MAP_WRITE, &Sp))) {
        HpfsUpdateChecksums((PHPFS_SUPERBLOCK)Sb.Data, (PHPFS_SPAREBLOCK)Sp.Data);
        HpfsSetDirty(Vol, &Sp);
        HpfsHostUnmap(Vol, &Sp);
    }
    HpfsHostUnmap(Vol, &Sb);
}

int
HpfsCheckVolumeEx(PHPFS_VOL Vol, PHPFS_CHECK_RESULT R, void (*Report)(void *, const char *), void *Ctx,
                  ULONG Flags)
{
    CHK C;
    int Errors;
    ULONG Fixed, Changed;
    BOOLEAN Sound;

    memset(&C, 0, sizeof(C));
    C.Vol = Vol;
    C.R = R;
    C.Report = Report;
    C.Ctx = Ctx;
    C.Flags = Flags;

    Errors = CheckPass(&C);
    if (Errors < 0 || !(Flags & HPFS_CHECK_FIX) || Vol->ReadOnly) {
        free(C.Used);
        free(C.Fixes);
        return Errors;
    }
    if (Errors == 0 && R->Warnings == 0 && !R->Dirty) {
        free(C.Used);
        free(C.Fixes);
        return 0;
    }

    /* 1. entries */
    Fixed = ApplyFixes(&C);
    free(C.Fixes);
    C.Fixes = NULL;
    C.FixCount = C.FixCap = 0;

    /* 2. walk again (quietly) for the sectors in use, then the bitmaps */
    free(C.Used);
    C.Report = NULL;
    C.Flags = 0;
    if (CheckPass(&C) < 0) {
        free(C.Used);
        return -1;
    }
    Sound = (BOOLEAN)(C.StructErrors == 0);
    Changed = WriteBitmaps(&C, Sound);
    if (Changed) {
        C.Report = Report;
        Say(&C, "bitmaps: %lu entries corrected%s", (unsigned long)Changed,
            Sound ? "" : " (lost space kept: the directory tree has damage this tool cannot repair)");
        C.Report = NULL;
    }
    WriteSuperSpare(Vol, Sound);
    free(C.Used);

    /* 3. what is left */
    Errors = CheckPass(&C);
    free(C.Used);
    C.Report = Report;
    if (Fixed || Changed) {
        Say(&C, "%lu repair(s) made; %d error(s) remain", (unsigned long)(Fixed + (Changed ? 1 : 0)), Errors);
    }
    return Errors;
}

int
HpfsCheckVolume(PHPFS_VOL Vol, PHPFS_CHECK_RESULT R, void (*Report)(void *, const char *), void *Ctx)
{
    return HpfsCheckVolumeEx(Vol, R, Report, Ctx, 0);
}
