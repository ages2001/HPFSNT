/*
 * HPFSNT - IRP_MJ_SET_INFORMATION
 *
 * Every class but FilePositionInformation takes the volume exclusive and
 * the file exclusive; the entry set is rewritten before returning.
 */

#include "hpfs.h"

/* Cuts FileSize and VDL and drops the cached data past them */
static VOID
HpfsShrinkFileSize (
    PHPFS_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG NewSize
    )
{
    LARGE_INTEGER Page;
    IO_STATUS_BLOCK Iosb;

    /* The cached page holding the new end keeps the old bytes after it.
       Should the file grow again they would show through, so that page
       goes to the disk and out of the cache, to come back zeroed past VDL. */
    if ((NewSize & (HPFS_PAGE_SIZE - 1)) != 0 &&
        NewSize < Fcb->Header.ValidDataLength.QuadPart &&
        Fcb->SectionObjectPointers.DataSectionObject != NULL) {

        Page.QuadPart = NewSize & ~(LONGLONG)(HPFS_PAGE_SIZE - 1);

        CcFlushCache(&Fcb->SectionObjectPointers, &Page, HPFS_PAGE_SIZE, &Iosb);
        (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &Page, HPFS_PAGE_SIZE, FALSE);
    }

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    Fcb->Header.FileSize.QuadPart = NewSize;

    if (Fcb->Header.ValidDataLength.QuadPart > NewSize) {
        Fcb->Header.ValidDataLength.QuadPart = NewSize;
    }

    __try {

        if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        } else if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {
            /* mapped but not cached: drop the pages past the new end */
            LARGE_INTEGER From;
            From.QuadPart = (NewSize + HPFS_PAGE_SIZE - 1) & ~(LONGLONG)(HPFS_PAGE_SIZE - 1);
            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &From, 0, FALSE);
        }

    } __finally {

        HpfsRelease(&Fcb->PagingIoResource);
    }
}

/*
 * Changes FileSize, and with it VDL and the allocation when they no
 * longer fit. Called with the volume and the file exclusive; the caller
 * rewrites the entry set.
 */
NTSTATUS
HpfsSetFileSize (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG NewSize
    )
{
    PHPFS_VCB Vcb = Fcb->Vcb;
    LARGE_INTEGER Size;
    BOOLEAN CacheMapInitialized = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(Ctx);

    if (NewSize < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewSize == Fcb->Header.FileSize.QuadPart) {
        return STATUS_SUCCESS;
    }

    Size.QuadPart = NewSize;

    if (NewSize < Fcb->Header.FileSize.QuadPart &&
        !MmCanFileBeTruncated(&Fcb->SectionObjectPointers, &Size)) {

        return STATUS_USER_MAPPED_FILE;
    }

    __try {

        /*
         * A file mapped by Mm but without a cache map gets no temporary cache
         * map here: with the volume and the file held, CcInitializeCacheMap
         * can wait for an earlier cache map's teardown whose flush needs the
         * file. Dropping the pages past a smaller end needs no cache map
         * (HpfsShrinkFileSize uses CcPurgeCacheSection).
         */

        if (NewSize > Fcb->Header.FileSize.QuadPart) {

            if (NewSize > Fcb->Header.AllocationSize.QuadPart) {

                Status = HpfsSetAllocation(Vcb, Fcb, (ULONGLONG)NewSize);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Fcb->Header.FileSize.QuadPart = NewSize;

            /* The Memory Manager never relies on the file system to zero a
             * paging file past valid data: the whole file counts as valid, so
             * a paging write never starts a huge zero-fill in the modified
             * page writer. */
            if (Fcb->FcbState & FCB_STATE_PAGING_FILE) {
                Fcb->Header.ValidDataLength.QuadPart = NewSize;
            }

            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }

        } else {

            HpfsShrinkFileSize(Fcb, FileObject, NewSize);

            Status = HpfsSetAllocation(Vcb, Fcb, (ULONGLONG)NewSize);
        }

        Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;

    } __finally {

        if (CacheMapInitialized) {
            CcUninitializeCacheMap(FileObject, NULL, NULL);
        }
    }

    return Status;
}

static NTSTATUS
HpfsSetBasicInfo (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    PHPFS_CCB Ccb,
    PFILE_BASIC_INFORMATION Info
    )
{
    ULONG Filter = 0;
    ULONG Attributes;
    UCHAR NewAttributes;

    if (Info->FileAttributes != 0) {

        Attributes = Info->FileAttributes;

        if (HpfsIsDcb(Fcb)) {
            if (Attributes & FILE_ATTRIBUTE_TEMPORARY) {
                return STATUS_INVALID_PARAMETER;
            }
        } else if (Attributes & FILE_ATTRIBUTE_DIRECTORY) {
            return STATUS_INVALID_PARAMETER;
        }

        NewAttributes = (UCHAR)((Attributes & HPFS_ATTR_SETTABLE) |
                                (Fcb->Attributes & (HPFS_ATTR_DIRECTORY | HPFS_ATTR_LONGNAME)));

        if (NewAttributes != Fcb->Attributes) {
            Fcb->Attributes = NewAttributes;
            Filter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
        }
    }

    /* Zero leaves a time alone; so does -1, as later Windows versions use it */
    if (Info->CreationTime.QuadPart != 0 && Info->CreationTime.QuadPart != -1) {
        Fcb->CreationTime = Info->CreationTime;
        Filter |= FILE_NOTIFY_CHANGE_CREATION;
    }

    if (Info->LastAccessTime.QuadPart != 0 && Info->LastAccessTime.QuadPart != -1) {
        Fcb->LastAccessTime = Info->LastAccessTime;
        Ccb->Flags |= CCB_FLAG_USER_SET_ACCESS;
        Filter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
    }

    if (Info->LastWriteTime.QuadPart != 0 && Info->LastWriteTime.QuadPart != -1) {
        Fcb->LastWriteTime = Info->LastWriteTime;
        Ccb->Flags |= CCB_FLAG_USER_SET_WRITE;
        Filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
    }

    if (Filter == 0) {
        return STATUS_SUCCESS;
    }

    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
    HpfsNotifyChange(Vcb, Fcb, Filter, FILE_ACTION_MODIFIED);

    return HpfsUpdateDirent(Vcb, Fcb);
}

static NTSTATUS
HpfsSetDispositionInfo (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    PFILE_OBJECT FileObject,
    PFILE_DISPOSITION_INFORMATION Info
    )
{
    BOOLEAN Empty;
    NTSTATUS Status;

    if (!Info->DeleteFile) {
        Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;
        FileObject->DeletePending = FALSE;
        return STATUS_SUCCESS;
    }

    if (Fcb->Attributes & HPFS_ATTR_READONLY) {
        return STATUS_CANNOT_DELETE;
    }

    if (HpfsIsDcb(Fcb)) {

        HpfsEngineEnter(Vcb);
        Status = HpfsEngineLeave(Vcb, HpfsDirIsEmpty(&Vcb->Vol, Fcb->Fnode, &Empty));
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        if (!Empty) {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }

    } else if (!MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForDelete)) {

        /* A running program */
        return STATUS_CANNOT_DELETE;
    }

    Fcb->FcbState |= FCB_STATE_DELETE_PENDING;
    FileObject->DeletePending = TRUE;

    return STATUS_SUCCESS;
}

/*
 * The rename target exists and may be replaced: remove it. It must be a
 * file nobody has open.
 */
static NTSTATUS
HpfsReplaceTarget (
    PHPFS_VCB Vcb,
    PHPFS_FCB TargetDcb,
    PHPFS_ENTRY Target
    )
{
    PHPFS_FCB Existing;
    NTSTATUS Status;

    if (Target->Attributes & (HPFS_ATTR_DIRECTORY | HPFS_ATTR_READONLY)) {
        return STATUS_ACCESS_DENIED;
    }

    Existing = HpfsFindFcbByFnode(Vcb, Target->Fnode);

    if (Existing != NULL) {

        if (Existing->UncleanCount != 0 ||
            !MmFlushImageSection(&Existing->SectionObjectPointers, MmFlushForDelete)) {

            return STATUS_ACCESS_DENIED;
        }

        /* Only the cache holds it: drop that and delete through the FCB */
        Existing->RefCount++;

        (VOID)ExAcquireResourceExclusiveLite(&Existing->Resource, TRUE);

        (VOID)ExAcquireResourceExclusiveLite(&Existing->PagingIoResource, TRUE);
        Existing->Header.FileSize.QuadPart = 0;
        Existing->Header.ValidDataLength.QuadPart = 0;
        HpfsRelease(&Existing->PagingIoResource);

        (VOID)CcPurgeCacheSection(&Existing->SectionObjectPointers, NULL, 0, FALSE);

        Status = HpfsDeleteFromDisk(Vcb, Existing);

        HpfsRelease(&Existing->Resource);

        HpfsNotifyChange(Vcb, Existing, FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_REMOVED);

        (VOID)MmForceSectionClosed(&Existing->SectionObjectPointers, TRUE);
        HpfsDereferenceFcb(Vcb, Existing);

        return Status;
    }

    HpfsEngineEnter(Vcb);
    Status = HpfsDelete(&Vcb->Vol, TargetDcb->Fnode, Target->Name, Target->NameLength);
    return HpfsEngineLeave(Vcb, Status);
}

static VOID
HpfsReportName (
    PHPFS_VCB Vcb,
    PUNICODE_STRING FullName,
    USHORT NameOffset,
    ULONG Filter,
    ULONG Action
    )
{
    FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                &Vcb->DirNotifyList,
                                (PSTRING)FullName,
                                NameOffset,
                                NULL,
                                NULL,
                                Filter,
                                Action,
                                NULL);
}

/* Sectors a file brings along to another directory, for HPFS386 DASD limits */
static LONG
HpfsDasdSectors (
    PHPFS_FCB Fcb
    )
{
    return HpfsIsFcb(Fcb) ? (LONG)(Fcb->AllocSectors + 1) : 0;
}

static NTSTATUS
HpfsSetRenameInfo (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    PFILE_RENAME_INFORMATION Info
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT TargetObject = IrpSp->Parameters.SetFile.FileObject;
    BOOLEAN ReplaceIfExists = IrpSp->Parameters.SetFile.ReplaceIfExists;
    PHPFS_FCB TargetDcb;
    PHPFS_FCB OldParent = Fcb->ParentDcb;
    PHPFS_FCB Walk;
    PLIST_ENTRY Entry;
    UNICODE_STRING NewName;
    UNICODE_STRING NewUnicode;
    UNICODE_STRING OldFull;
    USHORT OldNameOffset = 0;
    PHPFS_ENTRY Found = NULL;
    UCHAR Oem[HPFS_MAX_NAME + 1];
    ULONG OemLength = 0;
    LONG Dasd = 0;
    BOOLEAN Charged = FALSE;
    ULONG Filter;
    NTSTATUS Status;

    RtlZeroMemory(&OldFull, sizeof(OldFull));
    RtlZeroMemory(&NewUnicode, sizeof(NewUnicode));

    if (TargetObject != NULL) {

        /* The I/O manager opened the target directory for us */
        TargetDcb = (PHPFS_FCB)TargetObject->FsContext;

        if (TargetDcb == NULL || !HpfsIsDcb(TargetDcb)) {
            return STATUS_INVALID_PARAMETER;
        }

        if (TargetDcb->Vcb != Vcb) {
            return STATUS_NOT_SAME_DEVICE;
        }

        NewName = TargetObject->FileName;

    } else {

        TargetDcb = OldParent;

        if (Info->FileNameLength > 0xFFFE) {
            return STATUS_OBJECT_NAME_INVALID;
        }

        NewName.Buffer = Info->FileName;
        NewName.Length = (USHORT)Info->FileNameLength;
        NewName.MaximumLength = NewName.Length;
    }

    if (!HpfsIsValidUnicodeName(&NewName)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    Status = HpfsUnicodeToOem(&NewName, Oem, &OemLength);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Oem[OemLength] = 0;

    if (!HpfsIsLegalName(Oem, OemLength)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    if (Fcb->FcbState & FCB_STATE_DELETE_PENDING) {
        return STATUS_DELETE_PENDING;
    }

    if (TargetDcb->FcbState & (FCB_STATE_DELETE_PENDING | FCB_STATE_DELETED)) {
        return STATUS_DELETE_PENDING;
    }

    if (HpfsIsDcb(Fcb)) {

        /* Not into itself or below */
        if (HpfsIsAncestor(Fcb, TargetDcb)) {
            return STATUS_INVALID_PARAMETER;
        }

        /* Nothing below it may be open */
        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Walk = CONTAINING_RECORD(Entry, HPFS_FCB, FcbLinks);

            if (Walk != Fcb && Walk->UncleanCount != 0 && HpfsIsAncestor(Fcb, Walk)) {
                return STATUS_ACCESS_DENIED;
            }
        }
    }

    /* Exactly the same name: nothing to do */
    if (TargetDcb == OldParent && OemLength == Fcb->OemNameLength &&
        RtlCompareMemory(Oem, Fcb->OemName, OemLength) == OemLength) {

        return STATUS_SUCCESS;
    }

    Found = (PHPFS_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(HPFS_ENTRY), HPFS_TAG_BUFFER);
    if (Found == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        HpfsEngineEnter(Vcb);
        Status = HpfsEngineLeave(Vcb, HpfsLookup(&Vcb->Vol, TargetDcb->Fnode, Oem, OemLength, Found));

        if (NT_SUCCESS(Status)) {

            /* Another spelling of its own name (a change of case) is fine */
            if (Found->Fnode != Fcb->Fnode) {

                if (!ReplaceIfExists) {
                    Status = STATUS_OBJECT_NAME_COLLISION;
                    __leave;
                }

                Status = HpfsReplaceTarget(Vcb, TargetDcb, Found);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

        } else if (Status != STATUS_OBJECT_NAME_NOT_FOUND) {
            __leave;
        }

        Status = HpfsOemToUnicode(Oem, OemLength, &NewUnicode);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* Keep the old name for the notification */
        if (NT_SUCCESS(HpfsBuildFullName(Fcb))) {

            OldFull.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Fcb->FullName.Length, HPFS_TAG_NAME);

            if (OldFull.Buffer != NULL) {
                RtlCopyMemory(OldFull.Buffer, Fcb->FullName.Buffer, Fcb->FullName.Length);
                OldFull.Length = Fcb->FullName.Length;
                OldFull.MaximumLength = OldFull.Length;
                OldNameOffset = (USHORT)(Fcb->FullName.Length - Fcb->Name.Length);
            }
        }

        /* The entry on the disk must carry the FCB's current size and times */
        if (Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) {
            Status = HpfsUpdateDirent(Vcb, Fcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        HpfsEngineEnter(Vcb);

        if (TargetDcb != OldParent) {
            Dasd = HpfsDasdSectors(Fcb);
        }
        Status = STATUS_SUCCESS;
        if (Dasd != 0) {
            Status = HpfsDasdCharge(&Vcb->Vol, TargetDcb->Fnode, Dasd);
            Charged = (BOOLEAN)NT_SUCCESS(Status);
        }
        if (NT_SUCCESS(Status)) {
            Status = HpfsRename(&Vcb->Vol, Fcb->DirFnode, Fcb->OemName, Fcb->OemNameLength,
                                TargetDcb->Fnode, Oem, OemLength);
        }
        if (Charged) {
            if (NT_SUCCESS(Status)) {
                HpfsDasdCharge(&Vcb->Vol, Fcb->DirFnode, -Dasd);
            } else {
                HpfsDasdCharge(&Vcb->Vol, TargetDcb->Fnode, -Dasd);
            }
        }
        if (NT_SUCCESS(Status)) {
            /* The long-name attribute follows the new name */
            Status = HpfsLookup(&Vcb->Vol, TargetDcb->Fnode, Oem, OemLength, Found);
        }

        Status = HpfsEngineLeave(Vcb, Status);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* The FCB follows */
        HpfsForgetNames(Vcb, Fcb);

        if (Fcb->Name.Buffer != NULL) {
            ExFreePool(Fcb->Name.Buffer);
        }
        Fcb->Name = NewUnicode;
        RtlZeroMemory(&NewUnicode, sizeof(NewUnicode));

        RtlCopyMemory(Fcb->OemName, Found->Name, Found->NameLength);
        Fcb->OemNameLength = Found->NameLength;
        Fcb->Attributes = Found->Attributes;

        if (TargetDcb != OldParent) {
            TargetDcb->RefCount++;
            Fcb->ParentDcb = TargetDcb;
            Fcb->DirFnode = TargetDcb->Fnode;
            HpfsDereferenceFcb(Vcb, OldParent);
        }

        Filter = HpfsIsDcb(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME;

        if (OldFull.Buffer != NULL) {
            HpfsReportName(Vcb, &OldFull, OldNameOffset, Filter,
                           (TargetDcb == OldParent) ? FILE_ACTION_RENAMED_OLD_NAME : FILE_ACTION_REMOVED);
        }

        HpfsNotifyChange(Vcb, Fcb, Filter,
                         (TargetDcb == OldParent) ? FILE_ACTION_RENAMED_NEW_NAME : FILE_ACTION_ADDED);

    } __finally {

        ExFreePool(Found);

        if (NewUnicode.Buffer != NULL) {
            ExFreePool(NewUnicode.Buffer);
        }
        if (OldFull.Buffer != NULL) {
            ExFreePool(OldFull.Buffer);
        }
    }

    return Status;
}

static NTSTATUS
HpfsSetAllocationInfo (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    PFILE_OBJECT FileObject,
    PFILE_ALLOCATION_INFORMATION Info
    )
{
    LONGLONG NewAllocation = Info->AllocationSize.QuadPart;
    NTSTATUS Status;

    if (!HpfsIsFcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewAllocation < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NewAllocation < Fcb->Header.FileSize.QuadPart) {

        /* Shrinking below the data cuts the file */
        Status = HpfsSetFileSize(Ctx, Fcb, FileObject, NewAllocation);

        if (NT_SUCCESS(Status)) {
            FileObject->Flags |= FO_FILE_MODIFIED;
            HpfsNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_SIZE, FILE_ACTION_MODIFIED);
        }

    } else {

        Status = HpfsSetAllocation(Vcb, Fcb, (ULONGLONG)NewAllocation);

        if (NT_SUCCESS(Status) &&
            Fcb->Header.AllocationSize.QuadPart >
                HpfsRoundUp(Fcb->Header.FileSize.QuadPart, HPFS_SECTOR_SIZE)) {

            /* Allocation past the data is given back when the file closes */
            Fcb->FcbState |= FCB_STATE_TRUNCATE_ON_CLOSE;
        }

        if (NT_SUCCESS(Status) && Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        }
    }

    if (NT_SUCCESS(Status)) {
        Status = HpfsUpdateDirent(Vcb, Fcb);
    }

    return Status;
}

static NTSTATUS
HpfsSetEndOfFileInfo (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    PFILE_OBJECT FileObject,
    PFILE_END_OF_FILE_INFORMATION Info
    )
{
    NTSTATUS Status;

    if (!HpfsIsFcb(Fcb)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (Info->EndOfFile.QuadPart == Fcb->Header.FileSize.QuadPart) {
        return STATUS_SUCCESS;
    }

    Status = HpfsSetFileSize(Ctx, Fcb, FileObject, Info->EndOfFile.QuadPart);

    if (NT_SUCCESS(Status)) {

        FileObject->Flags |= FO_FILE_MODIFIED;
        HpfsNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_SIZE, FILE_ACTION_MODIFIED);

        Status = HpfsUpdateDirent(Vcb, Fcb);
    }

    return Status;
}

NTSTATUS
HpfsCommonSetInformation (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;
    PHPFS_VCB Vcb = Ctx->Vcb;
    FILE_INFORMATION_CLASS Class = IrpSp->Parameters.SetFile.FileInformationClass;
    PVOID Buffer = Ctx->Irp->AssociatedIrp.SystemBuffer;
    PFILE_POSITION_INFORMATION Position;
    BOOLEAN FcbAcquired = FALSE;
    NTSTATUS Status;

    /* The lazy writer reporting what reached the disk (possibly through a file
     * object whose handle is already closed, so with no CCB): the entry set is
     * rewritten at cleanup and close anyway */
    if (Class == FileEndOfFileInformation && IrpSp->Parameters.SetFile.AdvanceOnly) {
        return STATUS_SUCCESS;
    }

    if (Fcb == NULL || Ccb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Class == FilePositionInformation) {

        if (!HpfsIsVfcb(Fcb)) {
            Status = HpfsVerifyVcb(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
        }

        Position = (PFILE_POSITION_INFORMATION)Buffer;

        if ((FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&
            ((ULONG)Position->CurrentByteOffset.QuadPart & (HPFS_SECTOR_SIZE - 1)) != 0) {

            return STATUS_INVALID_PARAMETER;
        }

        FileObject->CurrentByteOffset = Position->CurrentByteOffset;
        return STATUS_SUCCESS;
    }

    if (HpfsIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* The lazy writer reporting what reached the disk: the entry set is
       rewritten at cleanup and close anyway */
    if (Class == FileEndOfFileInformation && IrpSp->Parameters.SetFile.AdvanceOnly) {
        return STATUS_SUCCESS;
    }

    if (Fcb->FcbState & FCB_STATE_ROOT) {
        return (Class == FileDispositionInformation) ? STATUS_CANNOT_DELETE : STATUS_INVALID_PARAMETER;
    }

    if (Class == FileLinkInformation) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
        FcbAcquired = TRUE;

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        /*
         * A paging file in use must not be deleted or renamed, but the Memory
         * Manager DOES size it (FileAllocationInformation / FileEndOfFileInformation)
         * when it sets the paging file up and when it grows it - those must be
         * allowed, or Mm reports "no properly sized paging file" and runs with
         * almost no virtual memory. (Truncation at open is already prevented by
         * forcing FILE_OPEN for paging opens in hpfscrea.c.)
         */
        if ((Fcb->FcbState & FCB_STATE_PAGING_FILE) &&
            (Class == FileDispositionInformation || Class == FileRenameInformation)) {

            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        switch ((ULONG)Class) {

        case FileBasicInformation:
            Status = HpfsSetBasicInfo(Vcb, Fcb, Ccb, (PFILE_BASIC_INFORMATION)Buffer);
            break;

        case FileDispositionInformation:
            Status = HpfsSetDispositionInfo(Vcb, Fcb, FileObject, (PFILE_DISPOSITION_INFORMATION)Buffer);
            break;

        case FileRenameInformation:
            Status = HpfsSetRenameInfo(Ctx, Vcb, Fcb, (PFILE_RENAME_INFORMATION)Buffer);
            break;

        case FileAllocationInformation:
            Status = HpfsSetAllocationInfo(Ctx, Vcb, Fcb, FileObject,
                                          (PFILE_ALLOCATION_INFORMATION)Buffer);
            break;

        case FileEndOfFileInformation:
            Status = HpfsSetEndOfFileInfo(Ctx, Vcb, Fcb, FileObject,
                                         (PFILE_END_OF_FILE_INFORMATION)Buffer);
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (HpfsIsFcb(Fcb)) {
            Fcb->Header.IsFastIoPossible = HpfsIsFastIoPossible(Fcb);
        }

    } __finally {

        if (FcbAcquired) {
            HpfsRelease(&Fcb->Resource);
        }

        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}
