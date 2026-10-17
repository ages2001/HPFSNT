/*
 * HPFSNT - IRP_MJ_CLEANUP, IRP_MJ_CLOSE, dismount and volume teardown
 *
 * Cleanup of the last handle is where a file is deleted, extra
 * allocation is given back and the entry set gets the final sizes and
 * times. A dismounted volume stays in memory until its last file object
 * is closed. Then the VPB is detached (so the next access mounts afresh)
 * and the volume device object is deleted.
 */

#include "hpfs.h"

VOID
HpfsUnlockVcb (
    PHPFS_VCB Vcb
    )
{
    KIRQL Irql;

    Vcb->VcbState &= ~VCB_STATE_LOCKED;
    Vcb->LockFileObject = NULL;

    IoAcquireVpbSpinLock(&Irql);
    Vcb->Vpb->Flags &= ~VPB_LOCKED;
    IoReleaseVpbSpinLock(Irql);
}

/*
 * Drops cached data of files nobody has open, so that file objects held
 * only by the cache manager or memory manager get closed. Callers that
 * want the data kept flush first. The list is rescanned after each purge
 * because closes can arrive while it runs.
 */
VOID
HpfsPurgeCachedFiles (
    PHPFS_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PHPFS_FCB Fcb;
    BOOLEAN Restart;

    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, HPFS_FCB, FcbLinks);

            if (Fcb->FcbState & FCB_STATE_VISITED) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;

            if (!HpfsIsFcb(Fcb) ||
                Fcb->UncleanCount != 0 ||
                (Fcb->SectionObjectPointers.DataSectionObject == NULL &&
                 Fcb->SectionObjectPointers.ImageSectionObject == NULL)) {

                continue;
            }

            Fcb->RefCount++;

            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
            (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);

            HpfsDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, HPFS_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }
}

/*
 * Called with Vcb->Resource held exclusive. Callers that can still reach
 * the disk flush the volume first. Returns TRUE when the VCB can be
 * deleted right away; the caller then releases the resource and calls
 * HpfsDeleteVcb.
 */
BOOLEAN
HpfsDismountVcb (
    PHPFS_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PHPFS_FCB Fcb;

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FALSE;
    }

    HPFS_DBG((HPFS_PFX "Dismounting volume %08lX\n", Vcb->SerialNumber));

    Vcb->VcbState |= VCB_STATE_DISMOUNTED | VCB_STATE_IN_DISMOUNT;
    Vcb->VcbState &= ~VCB_STATE_MOUNTED;

    /* Send cached I/O on open files back through the IRP path */
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, HPFS_FCB, FcbLinks);
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    }

    HpfsPurgeCachedFiles(Vcb);

    /* Metadata: write what is pending, then forget it (re-read on next use) */
    HpfsEngineEnter(Vcb);
    (VOID)HpfsMetaFlush(Vcb);
    HpfsMetaPurge(Vcb);
    HpfsRelease(&Vcb->EngineResource);

    Vcb->VcbState &= ~VCB_STATE_IN_DISMOUNT;

    if (HpfsTryTeardown(Vcb, 0)) {
        return TRUE;
    }

    /*
     * The volume still has open references (user files - e.g. the running
     * system volume while autochk runs), so it cannot
     * be torn down and handed back to the I/O manager for a fresh mount: on NT
     * 3.1 that remount path cannot complete while the old VPB is still mounted,
     * and the verify that a later open triggers would hang. The caches have
     * already been purged above, so instead of leaving the volume
     * dismounted-but-alive we keep it mounted and re-read the on-disk
     * structures, which is exactly the fresh state a remount would produce.
     */
    Vcb->VcbState &= ~VCB_STATE_DISMOUNTED;
    Vcb->VcbState |= VCB_STATE_MOUNTED;
    Vcb->VcbState &= ~VCB_STATE_DASD_WRITTEN;

    /* Re-read the volume's structures as a fresh mount would */
    if (!(Vcb->VcbState & VCB_STATE_REMOVED) && !NT_SUCCESS(HpfsLoadVolume(Vcb))) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    return FALSE;
}

/*
 * Called with Vcb->Resource held exclusive. InFlightReferences is the
 * VPB reference the current request itself holds (1 for a create, 0 for
 * a close).
 */
BOOLEAN
HpfsTryTeardown (
    PHPFS_VCB Vcb,
    ULONG InFlightReferences
    )
{
    PVPB Vpb = Vcb->Vpb;
    KIRQL Irql;
    BOOLEAN FreeVpb = FALSE;

    if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Vcb->VcbState & (VCB_STATE_IN_DISMOUNT | VCB_STATE_DELETE_PENDING)) ||
        Vcb->OpenCount != 0) {

        return FALSE;
    }

    IoAcquireVpbSpinLock(&Irql);

    if (Vpb->ReferenceCount != InFlightReferences) {
        IoReleaseVpbSpinLock(Irql);
        return FALSE;
    }

    if (Vpb->RealDevice->Vpb == Vpb) {

        Vpb->DeviceObject = NULL;
        Vpb->Flags &= ~(VPB_MOUNTED | VPB_LOCKED);

    } else if (InFlightReferences == 0) {

        /* The I/O manager replaced this VPB during a verify */
        FreeVpb = TRUE;
    }

    IoReleaseVpbSpinLock(Irql);

    Vcb->VcbState |= VCB_STATE_DELETE_PENDING;

    if (FreeVpb) {
        Vcb->VcbState |= VCB_STATE_FREE_VPB;
    }

    return TRUE;
}

/* Called without any locks held */
VOID
HpfsDeleteVcb (
    PHPFS_VCB Vcb
    )
{
    PHPFS_FCB Fcb;
    PVPB FreeVpb = NULL;

    HPFS_DBG((HPFS_PFX "Deleting volume %08lX\n", Vcb->SerialNumber));

    (VOID)ExAcquireResourceExclusiveLite(&HpfsData.Resource, TRUE);
    RemoveEntryList(&Vcb->VcbLinks);
    HpfsRelease(&HpfsData.Resource);

    while (!IsListEmpty(&Vcb->FcbList)) {
        Fcb = CONTAINING_RECORD(Vcb->FcbList.Flink, HPFS_FCB, FcbLinks);
        HpfsDeleteFcb(Fcb);
    }

    if (Vcb->VolumeFcb != NULL) {
        HpfsDeleteFcb(Vcb->VolumeFcb);
    }

    HpfsMetaPurge(Vcb);
    HpfsUnmountFree(&Vcb->Vol);

    FsRtlNotifyUninitializeSync(&Vcb->NotifySync);

    if (Vcb->VcbState & VCB_STATE_FREE_VPB) {
        FreeVpb = Vcb->Vpb;
    }

    ExDeleteResourceLite(&Vcb->EngineResource);
    ExDeleteResourceLite(&Vcb->Resource);

    ObDereferenceObject(Vcb->TargetDeviceObject);
    IoDeleteDevice(Vcb->VolumeDeviceObject);

    if (FreeVpb != NULL) {
        ExFreePool(FreeVpb);
    }
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

/* Last handle of a delete-pending file or directory: remove it */
static VOID
HpfsDeleteOnCleanup (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    BOOLEAN Empty = TRUE;
    NTSTATUS Status;

    if (HpfsIsDcb(Fcb)) {

        /* Entries may have come back since the delete was requested */
        HpfsEngineEnter(Vcb);
        Status = HpfsEngineLeave(Vcb, HpfsDirIsEmpty(&Vcb->Vol, Fcb->Fnode, &Empty));
        if (!NT_SUCCESS(Status)) {
            Empty = FALSE;
        }

        if (!Empty) {
            Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;
            return;
        }

    } else {

        /* Nothing cached may reach the sectors about to be freed */
        (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        Fcb->Header.FileSize.QuadPart = 0;
        Fcb->Header.ValidDataLength.QuadPart = 0;
        HpfsRelease(&Fcb->PagingIoResource);

        (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
    }

    Status = HpfsDeleteFromDisk(Vcb, Fcb);

    if (!NT_SUCCESS(Status)) {
        HPFS_DBG((HPFS_PFX "Delete of %wZ failed %lX\n", &Fcb->Name, Status));
    }

    HpfsNotifyChange(Vcb, Fcb,
                    HpfsIsDcb(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                    FILE_ACTION_REMOVED);
}

NTSTATUS
HpfsCommonCleanup (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;
    PHPFS_VCB Vcb = Ctx->Vcb;
    LARGE_INTEGER Now;
    LARGE_INTEGER Zero;
    PLARGE_INTEGER TruncateSize = NULL;
    ULONG Filter = 0;
    BOOLEAN FcbAcquired = FALSE;
    BOOLEAN Writable;
    BOOLEAN Modified;

    /* IoCreateStreamFileObject cleans up before FsContext is set */
    if (Fcb == NULL || Ccb == NULL) {
        return STATUS_SUCCESS;
    }

    Zero.QuadPart = 0;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Writable = (BOOLEAN)!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_READ_ONLY |
                                               VCB_STATE_SHUTDOWN));

        if (HpfsIsVfcb(Fcb)) {

            if (Vcb->LockFileObject == FileObject) {

                HpfsUnlockVcb(Vcb);

                /* Metadata may have been rewritten under us */
                if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                    (VOID)HpfsDismountVcb(Vcb);
                }
            }

        } else {

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;

            if (HpfsIsDcb(Fcb)) {
#ifdef HPFS_NT31
                HpfsNotifyCleanupList(Vcb, Ccb);
#else
                FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->DirNotifyList, Ccb);
#endif
            } else {
                FsRtlFastUnlockAll(&Fcb->FileLock, FileObject,
                                   IoGetRequestorProcess(Ctx->Irp), NULL);
            }

            if (Ccb->Flags & CCB_FLAG_DELETE_ON_CLOSE) {
                Fcb->FcbState |= FCB_STATE_DELETE_PENDING;
            }

            Modified = (BOOLEAN)((FileObject->Flags & FO_FILE_MODIFIED) != 0);

            if (Writable && !(Fcb->FcbState & FCB_STATE_DELETED)) {

                if (Modified) {

                    KeQuerySystemTime(&Now);

                    if (!(Ccb->Flags & CCB_FLAG_USER_SET_WRITE)) {
                        Fcb->LastWriteTime = Now;
                        Filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
                    }

                    if (!(Ccb->Flags & CCB_FLAG_USER_SET_ACCESS)) {
                        Fcb->LastAccessTime = Now;
                    }

                    if (!(Fcb->Attributes & HPFS_ATTR_ARCHIVE) && HpfsIsFcb(Fcb)) {
                        Fcb->Attributes |= HPFS_ATTR_ARCHIVE;
                        Filter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
                    }

                    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
                }

                if (FileObject->Flags & FO_FILE_SIZE_CHANGED) {
                    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
                    Filter |= FILE_NOTIFY_CHANGE_SIZE;
                }

                if (Fcb->UncleanCount == 1 && (Fcb->FcbState & FCB_STATE_DELETE_PENDING)) {

                    HpfsDeleteOnCleanup(Vcb, Fcb);

                    if (Fcb->FcbState & FCB_STATE_DELETED) {
                        TruncateSize = &Zero;
                        Filter = 0;
                    }

                } else if (Fcb->UncleanCount == 1 &&
                           (Fcb->FcbState & FCB_STATE_TRUNCATE_ON_CLOSE)) {

                    (VOID)HpfsSetAllocation(Vcb, Fcb, (ULONGLONG)Fcb->Header.FileSize.QuadPart);
            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
                    Fcb->FcbState &= ~FCB_STATE_TRUNCATE_ON_CLOSE;
                }

                if (Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) {
                    (VOID)HpfsUpdateDirent(Vcb, Fcb);
                }

                if (Filter != 0) {
                    HpfsNotifyChange(Vcb, Fcb, Filter, FILE_ACTION_MODIFIED);
                }

                /* Removable media: this file is on the disk once closed */
                if ((Vcb->VcbState & VCB_STATE_FLUSH_ON_CLOSE) && HpfsIsFcb(Fcb) &&
                    (Modified || (FileObject->Flags & FO_FILE_SIZE_CHANGED)) &&
                    !(Fcb->FcbState & FCB_STATE_DELETED)) {

                    (VOID)HpfsFlushFile(Vcb, Fcb);
                }
            }

            if (HpfsIsFcb(Fcb)) {

                CcUninitializeCacheMap(FileObject, TruncateSize, NULL);

                /* On a dismounted volume let the last handle take the cache
                   with it, so the file objects close and the VCB can go */
                if ((Vcb->VcbState & VCB_STATE_DISMOUNTED) && Fcb->UncleanCount == 1) {
                    (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
                    (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);
                }
            }

            if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
                Fcb->NonCachedUncleanCount--;
            }
        }

        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

        Fcb->UncleanCount--;
        Vcb->UncleanCount--;

        if (Ccb->Flags & CCB_FLAG_WRITE_HANDLE) {

            Ccb->Flags &= ~CCB_FLAG_WRITE_HANDLE;
            Vcb->WriteCount--;

            /* Removable media is left clean once nothing writes to it */
            if (Vcb->WriteCount == 0 && Writable && (Vcb->VcbState & VCB_STATE_FLUSH_ON_CLOSE)) {
                (VOID)HpfsFlushVolume(Vcb, TRUE);
            }
        }

        if (HpfsIsFcb(Fcb)) {
            Fcb->Header.IsFastIoPossible = HpfsIsFastIoPossible(Fcb);
        }

        FileObject->Flags |= FO_CLEANUP_COMPLETE;

    } __finally {

        if (FcbAcquired) {
            HpfsRelease(&Fcb->Resource);
        }

        HpfsRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Close                                                               */
/* ------------------------------------------------------------------ */

/* Called with Vcb->Resource exclusive; TRUE: the VCB can be deleted */
static BOOLEAN
HpfsCloseFcb (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    PHPFS_CCB Ccb
    )
{
    if (Ccb == NULL) {

        /* no internal streams in HPFSNT */
        UNREFERENCED_PARAMETER(Fcb);

    } else {

        HpfsDeleteCcb(Ccb);

        /* Mapped writes after the last cleanup can still move VDL */
        if (Fcb->RefCount == 1 && (Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) &&
            !(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_READ_ONLY))) {

            (VOID)HpfsUpdateDirent(Vcb, Fcb);
        }

        HpfsDereferenceFcb(Vcb, Fcb);
        Vcb->OpenCount--;
    }

    return HpfsTryTeardown(Vcb, 0);
}

static VOID
NTAPI
HpfsCloseWorker (
    PVOID Context
    )
{
    PHPFS_CLOSE_ITEM Item = (PHPFS_CLOSE_ITEM)Context;
    PHPFS_VCB Vcb = Item->Vcb;
    BOOLEAN Delete;

    FsRtlEnterFileSystem();

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Delete = HpfsCloseFcb(Vcb, Item->Fcb, Item->Ccb);

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    if (Delete) {
        HpfsDeleteVcb(Vcb);
    }

    FsRtlExitFileSystem();

    ExFreePool(Item);
}

/*
 * A close can arrive from inside the cache or memory manager while this
 * thread already holds the volume shared; waiting for it exclusive would
 * then deadlock. Such closes are finished by a worker thread.
 */
NTSTATUS
HpfsCommonClose (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;
    PHPFS_VCB Vcb = Ctx->Vcb;
    PHPFS_CLOSE_ITEM Item;
    BOOLEAN Delete;

    if (Fcb == NULL) {
        return STATUS_SUCCESS;
    }

    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;

    if (!ExAcquireResourceExclusiveLite(&Vcb->Resource, FALSE)) {

        Item = NULL;

        if (IoGetTopLevelIrp() != Ctx->Irp) {
            Item = (PHPFS_CLOSE_ITEM)ExAllocatePoolWithTag(NonPagedPool, sizeof(HPFS_CLOSE_ITEM),
                                                          HPFS_TAG_WORK);
        }

        if (Item != NULL) {

            Item->Vcb = Vcb;
            Item->Fcb = Fcb;
            Item->Ccb = Ccb;

            /* Deprecated in the WDK, but the only work queue NT 3.51/4.0 have */
#if defined(_MSC_VER) && _MSC_VER >= 1300
#pragma warning(push)
#pragma warning(disable: 4995 4996)
#endif
            ExInitializeWorkItem(&Item->Item, HpfsCloseWorker, Item);
            ExQueueWorkItem(&Item->Item, DelayedWorkQueue);
#if defined(_MSC_VER) && _MSC_VER >= 1300
#pragma warning(pop)
#endif

            return STATUS_SUCCESS;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    }

    __try {

        Delete = HpfsCloseFcb(Vcb, Fcb, Ccb);

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    if (Delete) {
        HpfsDeleteVcb(Vcb);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Deleting                                                            */
/* ------------------------------------------------------------------ */

/*
 * Removes the file or (empty) directory from the disk: entry, data, EAs,
 * fnode. The FCB stays (marked deleted) for Cc and Mm. Vcb->Resource
 * exclusive.
 */
NTSTATUS
HpfsDeleteFromDisk (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    NTSTATUS Status;

    if (Fcb->FcbState & FCB_STATE_DELETED) {
        return STATUS_SUCCESS;
    }

    /* Nothing may reach the sectors about to be freed through the run list */
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    HpfsEngineEnter(Vcb);
    Status = HpfsDelete(&Vcb->Vol, Fcb->DirFnode, Fcb->OemName, Fcb->OemNameLength);
    Status = HpfsEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        Fcb->FcbState |= FCB_STATE_DELETED;
        Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;
        if (Fcb->Runs != NULL) {
            ExFreePool(Fcb->Runs);
            Fcb->Runs = NULL;
        }
        Fcb->RunCount = 0;
        Fcb->AllocSectors = 0;
        Fcb->Header.AllocationSize.QuadPart = 0;
        Fcb->Header.FileSize.QuadPart = 0;
        Fcb->Header.ValidDataLength.QuadPart = 0;
    }

    HpfsRelease(&Fcb->PagingIoResource);
    return Status;
}
