/*
 * HPFSNT - the engine's host in the driver: the metadata cache
 *
 * HPFS scatters its structures (fnodes, anodes, dnodes, bitmaps, EA runs)
 * between the data of the files, often in the same 4 KB page. A page-based
 * cache of the whole volume would write a stale copy of a file's data
 * sector back over the file whenever a neighbouring structure in that page
 * changed. So metadata is cached here instead, block by block, exactly as
 * the engine maps it (a block is the run of sectors one HpfsHostMap call
 * names: an fnode, a dnode, a bitmap ...):
 *
 *   - A block mapped again finds its cached copy.
 *   - A new block that overlaps cached ones writes and drops them first.
 *   - Freed sectors are dropped from the cache (HpfsHostFreed), so a block
 *     that turns into file data is never written over it later.
 *   - Changed blocks are written when the engine call that changed them
 *     finishes (HpfsEngineLeave), in one batch.
 *   - Clean blocks stay cached up to HPFS_META_MAX_BYTES, least recently
 *     used first out.
 *
 * Everything here runs with Vcb->EngineResource held exclusive.
 */

#include "hpfs.h"

#define VCB_OF(Vol)     ((PHPFS_VCB)(Vol)->HostContext)

static ULONG
MetaHash (
    ULONG Sector
    )
{
    return (Sector ^ (Sector >> 8)) & (HPFS_META_HASH - 1);
}

VOID
HpfsMetaInitialize (
    PHPFS_VCB Vcb
    )
{
    ULONG i;

    InitializeListHead(&Vcb->MetaLru);
    for (i = 0; i < HPFS_META_HASH; i++) {
        Vcb->MetaHash[i] = NULL;
    }
    Vcb->MetaBytes = 0;
    Vcb->MetaDirty = 0;
    Vcb->MetaWriteStatus = STATUS_SUCCESS;
}

static VOID
MetaUnhash (
    PHPFS_VCB Vcb,
    PHPFS_META M
    )
{
    PHPFS_META *P = &Vcb->MetaHash[MetaHash(M->Sector)];

    while (*P != NULL) {
        if (*P == M) {
            *P = M->HashNext;
            break;
        }
        P = &(*P)->HashNext;
    }
}

static VOID
MetaFree (
    PHPFS_VCB Vcb,
    PHPFS_META M
    )
{
    MetaUnhash(Vcb, M);
    RemoveEntryList(&M->Links);
    Vcb->MetaBytes -= M->Count * HPFS_SECTOR_SIZE;
    if (M->Dirty) {
        Vcb->MetaDirty--;
    }
    ExFreePool(M->Data);
    ExFreePool(M);
}

static NTSTATUS
MetaWrite (
    PHPFS_VCB Vcb,
    PHPFS_META M
    )
{
    NTSTATUS Status;

    Status = HpfsWriteDisk(Vcb, M->Sector, M->Count, M->Data);
    if (NT_SUCCESS(Status)) {
        M->Dirty = FALSE;
        Vcb->MetaDirty--;
    } else if (NT_SUCCESS(Vcb->MetaWriteStatus)) {
        Vcb->MetaWriteStatus = Status;
    }
    return Status;
}

/* Writes every changed block */
NTSTATUS
HpfsMetaFlush (
    PHPFS_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PHPFS_META M;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    if (Vcb->MetaDirty == 0) {
        Result = Vcb->MetaWriteStatus;
        Vcb->MetaWriteStatus = STATUS_SUCCESS;
        return Result;
    }

    for (Entry = Vcb->MetaLru.Flink; Entry != &Vcb->MetaLru; Entry = Entry->Flink) {
        M = CONTAINING_RECORD(Entry, HPFS_META, Links);
        if (M->Dirty) {
            Result = MetaWrite(Vcb, M);
            if (!NT_SUCCESS(Result) && NT_SUCCESS(Status)) {
                Status = Result;
            }
        }
    }

    if (NT_SUCCESS(Status)) {
        Status = Vcb->MetaWriteStatus;
    }
    Vcb->MetaWriteStatus = STATUS_SUCCESS;
    return Status;
}

/* Drops everything (the volume is going away or was changed under us) */
VOID
HpfsMetaPurge (
    PHPFS_VCB Vcb
    )
{
    PHPFS_META M;

    while (!IsListEmpty(&Vcb->MetaLru)) {
        M = CONTAINING_RECORD(Vcb->MetaLru.Flink, HPFS_META, Links);
        MetaFree(Vcb, M);
    }
    Vcb->MetaDirty = 0;
}

/* Keeps the clean cache within its budget */
static VOID
MetaTrim (
    PHPFS_VCB Vcb
    )
{
    PLIST_ENTRY Entry, Prev;
    PHPFS_META M;

    for (Entry = Vcb->MetaLru.Blink;
         Entry != &Vcb->MetaLru && Vcb->MetaBytes > HPFS_META_MAX_BYTES;
         Entry = Prev) {

        Prev = Entry->Blink;
        M = CONTAINING_RECORD(Entry, HPFS_META, Links);
        if (M->Refs == 0 && !M->Dirty) {
            MetaFree(Vcb, M);
        }
    }
}

/*
 * Drops cached blocks overlapping [Sector, Sector+Count) other than Keep.
 * Unless Discard, changed ones are written first. Fails if one is in use.
 */
static NTSTATUS
MetaDropOverlaps (
    PHPFS_VCB Vcb,
    ULONG Sector,
    ULONG Count,
    PHPFS_META Keep,
    BOOLEAN Discard
    )
{
    PLIST_ENTRY Entry, Next;
    PHPFS_META M;
    NTSTATUS Status;

    for (Entry = Vcb->MetaLru.Flink; Entry != &Vcb->MetaLru; Entry = Next) {

        Next = Entry->Flink;
        M = CONTAINING_RECORD(Entry, HPFS_META, Links);

        if (M == Keep || M->Sector >= Sector + Count || Sector >= M->Sector + M->Count) {
            continue;
        }

        if (M->Refs != 0) {
            if (Discard) {
                continue;           /* still mapped by a caller: leave it */
            }
            HPFS_DBG((HPFS_PFX "overlapping metadata maps %lX+%lu / %lX+%lu\n",
                      Sector, Count, M->Sector, M->Count));
            return STATUS_FILE_CORRUPT_ERROR;
        }

        if (M->Dirty && !Discard) {
            Status = MetaWrite(Vcb, M);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
        }
        MetaFree(Vcb, M);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* The engine's host interface                                         */
/* ------------------------------------------------------------------ */

HPFS_STATUS
HpfsHostMap (
    PHPFS_VOL Vol,
    ULONG Sector,
    ULONG Count,
    ULONG Mode,
    PHPFS_BUF Buf
    )
{
    PHPFS_VCB Vcb = VCB_OF(Vol);
    PHPFS_META M;
    NTSTATUS Status;

    Buf->Host = NULL;
    Buf->Data = NULL;

    if (Count == 0 || Count > 128 || Sector >= Vol->TotalSectors ||
        Count > Vol->TotalSectors - Sector) {
        return HPFS_DISK_CORRUPT;
    }

    if ((Mode & HPFS_MAP_WRITE) && !Vcb->DirtyMarked && !Vcb->InMarkDirty &&
        !(Vcb->VcbState & VCB_STATE_READ_ONLY)) {
        HpfsMarkVolumeDirty(Vcb);
    }

    for (M = Vcb->MetaHash[MetaHash(Sector)]; M != NULL; M = M->HashNext) {
        if (M->Sector == Sector && M->Count == Count) {
            break;
        }
    }

    if (M == NULL) {

        /* Overlapping blocks of other shapes go to the disk and out first */
        Status = MetaDropOverlaps(Vcb, Sector, Count, NULL, FALSE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        M = (PHPFS_META)ExAllocatePoolWithTag(NonPagedPool, sizeof(HPFS_META), HPFS_TAG_META);
        if (M == NULL) {
            return HPFS_NO_MEMORY;
        }
        M->Data = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Count * HPFS_SECTOR_SIZE, HPFS_TAG_META);
        if (M->Data == NULL) {
            ExFreePool(M);
            return HPFS_NO_MEMORY;
        }
        M->Sector = Sector;
        M->Count = Count;
        M->Refs = 0;
        M->Dirty = FALSE;
        M->Valid = FALSE;

        if (Mode == HPFS_MAP_NEW) {
            RtlZeroMemory(M->Data, Count * HPFS_SECTOR_SIZE);
            M->Valid = TRUE;
        } else {
            Status = HpfsReadDisk(Vcb, Sector, Count, M->Data);
            if (!NT_SUCCESS(Status)) {
                ExFreePool(M->Data);
                ExFreePool(M);
                return Status;
            }
            M->Valid = TRUE;
        }

        M->HashNext = Vcb->MetaHash[MetaHash(Sector)];
        Vcb->MetaHash[MetaHash(Sector)] = M;
        InsertHeadList(&Vcb->MetaLru, &M->Links);
        Vcb->MetaBytes += Count * HPFS_SECTOR_SIZE;

    } else {

        RemoveEntryList(&M->Links);
        InsertHeadList(&Vcb->MetaLru, &M->Links);
        if (Mode == HPFS_MAP_NEW) {
            RtlZeroMemory(M->Data, Count * HPFS_SECTOR_SIZE);
        }
    }

    M->Refs++;
    Buf->Data = M->Data;
    Buf->Sector = Sector;
    Buf->Count = Count;
    Buf->Host = M;
    Buf->Dirty = FALSE;
    return HPFS_OK;
}

VOID
HpfsHostSetDirty (
    PHPFS_VOL Vol,
    PHPFS_BUF Buf
    )
{
    PHPFS_VCB Vcb = VCB_OF(Vol);
    PHPFS_META M = (PHPFS_META)Buf->Host;

    if (M != NULL && !M->Dirty) {
        M->Dirty = TRUE;
        Vcb->MetaDirty++;
    }
    Buf->Dirty = TRUE;
}

VOID
HpfsHostUnmap (
    PHPFS_VOL Vol,
    PHPFS_BUF Buf
    )
{
    PHPFS_VCB Vcb = VCB_OF(Vol);
    PHPFS_META M = (PHPFS_META)Buf->Host;

    if (M == NULL) {
        return;
    }
    Buf->Host = NULL;
    Buf->Data = NULL;
    M->Refs--;
    if (M->Refs == 0 && Vcb->MetaBytes > HPFS_META_MAX_BYTES) {
        MetaTrim(Vcb);
    }
}

VOID
HpfsHostFreed (
    PHPFS_VOL Vol,
    ULONG Sector,
    ULONG Count
    )
{
    (VOID)MetaDropOverlaps(VCB_OF(Vol), Sector, Count, NULL, TRUE);
}

PVOID
HpfsHostAlloc (
    ULONG Bytes
    )
{
    return ExAllocatePoolWithTag(PagedPool, Bytes ? Bytes : 1, HPFS_TAG_ENGINE);
}

VOID
HpfsHostFree (
    PVOID P
    )
{
    ExFreePool(P);
}

ULONG
HpfsHostNow (
    VOID
    )
{
    LARGE_INTEGER Now;

    KeQuerySystemTime(&Now);
    return HpfsFromNtTime(Now);
}

/* ------------------------------------------------------------------ */
/* Engine calls                                                        */
/* ------------------------------------------------------------------ */

VOID
HpfsEngineEnter (
    PHPFS_VCB Vcb
    )
{
    (VOID)ExAcquireResourceExclusiveLite(&Vcb->EngineResource, TRUE);
}

/*
 * Writes what the engine call changed and releases the engine. Returns
 * Status, or the write error if the call itself succeeded.
 */
NTSTATUS
HpfsEngineLeave (
    PHPFS_VCB Vcb,
    NTSTATUS Status
    )
{
    NTSTATUS Flush;

    Flush = HpfsMetaFlush(Vcb);

    /*
     * After the system's shutdown notice the volume must stay clean: closes
     * and entry updates still arrive (the last handles, the lazy writer), so
     * each change is followed by clearing the dirty bit again.
     */
    if (NT_SUCCESS(Flush) && (Vcb->VcbState & VCB_STATE_SHUTDOWN) && Vcb->DirtyMarked &&
        !Vcb->InMarkDirty && !(Vcb->VcbState & (VCB_STATE_KEEP_DIRTY | VCB_STATE_READ_ONLY))) {
        Vcb->InMarkDirty = TRUE;
        if (NT_SUCCESS(HpfsMarkDirty(&Vcb->Vol, FALSE)) && NT_SUCCESS(HpfsMetaFlush(Vcb))) {
            Vcb->DirtyMarked = FALSE;
        }
        Vcb->InMarkDirty = FALSE;
    }

    HpfsRelease(&Vcb->EngineResource);

    if (NT_SUCCESS(Status) && !NT_SUCCESS(Flush)) {
        Status = Flush;
    }
    return Status;
}
